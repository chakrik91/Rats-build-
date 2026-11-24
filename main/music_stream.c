#include "music_stream.h"

#include "audio_playback.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "nino_music";

#define MUSIC_RING_BYTES (256 * 1024)
#define MUSIC_PREBUFFER_PULL (96 * 1024)
#define MUSIC_PREBUFFER_PUSH_MS 150
#define MUSIC_PREBUFFER_PUSH_STEADY 1024
#define MUSIC_MIN_WRITE_MS 20
#define MUSIC_MIN_WRITE_PUSH_MS 8
#define MUSIC_HTTP_CHUNK 4096
#define MUSIC_WRITE_CHUNK 4096
#define MUSIC_TRIGGER_BYTES 512
#define MUSIC_TRIGGER_PUSH_BYTES 1
#define MUSIC_FEED_TASK_STACK 8192
#define MUSIC_FEED_TASK_PRIO 6
#define MUSIC_GATHER_MS 12
#define MUSIC_HTTP_TASK_STACK 10240
#define MUSIC_HTTP_TASK_PRIO 4
#define MUSIC_URL_MAX 384
#define MUSIC_HDR_ACC 512
#define MUSIC_HTTP_TIMEOUT_MS 10000
#define MUSIC_UNDERRUN_LOG_MS 1000

typedef enum {
  MUSIC_SRC_NONE = 0,
  MUSIC_SRC_PULL,
  MUSIC_SRC_PUSH,
} music_src_t;

static StreamBufferHandle_t s_ring;
static uint8_t *s_ring_storage;
static StaticStreamBuffer_t s_ring_ctrl;
static SemaphoreHandle_t s_ctrl;
static uint8_t s_feed_buf[MUSIC_WRITE_CHUNK];
static uint8_t s_http_buf[MUSIC_HTTP_CHUNK];
static size_t s_feed_acc_len;

static char s_url[MUSIC_URL_MAX];
static volatile bool s_inited;
static volatile bool s_session_active;
static volatile bool s_stop;
static volatile bool s_user_stop;
static volatile bool s_user_paused;
static volatile bool s_speech_paused;
static volatile bool s_http_running;
static volatile bool s_http_eof;
static volatile bool s_header_ok;
static volatile bool s_ready_to_play;
static volatile uint32_t s_generation;
static volatile uint32_t s_rate_hz = 32000;
static volatile uint32_t s_position_bytes;
static volatile music_src_t s_src;
static volatile bool s_output_started;
static uint8_t s_push_hdr[MUSIC_HDR_ACC];
static size_t s_push_hdr_len;
static int s_push_hdr_state; /* 0 unknown, 1 wav header, 2 raw PCM */

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

typedef struct {
  uint32_t sample_rate;
  uint16_t channels;
  uint16_t bits;
  bool have_fmt;
  size_t pcm_off;
} wav_stream_hdr_t;

/* 1 = complete, 0 = need more bytes, -1 = invalid. */
static int wav_try_parse(const uint8_t *buf, size_t len, wav_stream_hdr_t *h) {
  memset(h, 0, sizeof(*h));
  if (len < 12) {
    return 0;
  }
  if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
    return -1;
  }
  size_t off = 12;
  while (off + 8 <= len) {
    const uint8_t *chunk = buf + off;
    uint32_t sz = read_le32(chunk + 4);
    if (memcmp(chunk, "fmt ", 4) == 0) {
      if (off + 8 + 16 > len) {
        return 0;
      }
      uint16_t fmt = read_le16(chunk + 8);
      h->channels = read_le16(chunk + 10);
      h->sample_rate = read_le32(chunk + 12);
      h->bits = read_le16(chunk + 8 + 14);
      if (fmt != 1 && fmt != (uint16_t)0xFFFE) {
        return -1;
      }
      h->have_fmt = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      if (!h->have_fmt) {
        return -1;
      }
      if (h->channels != 1 || h->bits != 16) {
        return -1;
      }
      if (h->sample_rate < 8000 || h->sample_rate > 48000) {
        return -1;
      }
      h->pcm_off = off + 8;
      return 1;
    } else if (sz == 0xFFFFFFFFu) {
      return -1;
    }
    uint32_t skip = 8u + sz + (sz & 1u);
    if (off + skip > len) {
      return 0;
    }
    off += skip;
  }
  return 0;
}

static bool url_ok(const char *url) {
  if (url == NULL || url[0] == '\0') {
    return false;
  }
  if (strlen(url) >= MUSIC_URL_MAX) {
    return false;
  }
  if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
    return false;
  }
  return true;
}

static uint32_t rate_or_default(void) {
  uint32_t rate = s_rate_hz;
  return rate >= 8000 ? rate : 16000;
}

static size_t min_write_bytes(void) {
  const uint32_t ms =
      (s_src == MUSIC_SRC_PUSH && s_output_started) ? MUSIC_MIN_WRITE_PUSH_MS
                                                    : MUSIC_MIN_WRITE_MS;
  size_t n = (size_t)((rate_or_default() * 2u * ms) / 1000u);
  if (n < 256) {
    n = 256;
  }
  if (s_src != MUSIC_SRC_PUSH && n < MUSIC_TRIGGER_BYTES) {
    n = MUSIC_TRIGGER_BYTES;
  }
  if (n > MUSIC_WRITE_CHUNK) {
    n = MUSIC_WRITE_CHUNK;
  }
  return n & ~(size_t)1u;
}

static size_t prebuffer_bytes(void) {
  if (s_src == MUSIC_SRC_PULL) {
    return MUSIC_PREBUFFER_PULL;
  }
  if (s_output_started) {
    return MUSIC_PREBUFFER_PUSH_STEADY;
  }
  size_t n = (size_t)((rate_or_default() * 2u * MUSIC_PREBUFFER_PUSH_MS) / 1000u);
  if (n < 4096) {
    n = 4096;
  }
  return n;
}

static uint32_t bytes_to_ms(size_t bytes) {
  uint32_t rate = rate_or_default();
  return (uint32_t)((bytes * 1000u) / (rate * 2u));
}

static void close_codec(void) {
  nino_audio_bus_lock();
  nino_audio_spk_close_locked();
  nino_audio_bus_unlock();
}

static bool ring_push(const uint8_t *data, size_t len, uint32_t gen, TickType_t wait) {
  size_t off = 0;
  while (off < len) {
    if (s_stop || gen != s_generation) {
      return false;
    }
    size_t n = xStreamBufferSend(s_ring, data + off, len - off, wait);
    off += n;
    if (n == 0) {
      if (wait == 0) {
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  return true;
}

static void music_teardown(bool close_spk) {
  s_stop = true;
  s_session_active = false;
  s_ready_to_play = false;
  s_header_ok = false;
  s_http_eof = true;
  s_user_paused = false;
  s_src = MUSIC_SRC_NONE;
  s_generation++;

  int wait = 0;
  while (s_http_running && wait < 50) {
    vTaskDelay(pdMS_TO_TICKS(100));
    wait++;
  }
  vTaskDelay(pdMS_TO_TICKS(80));
  s_feed_acc_len = 0;
  s_position_bytes = 0;
  s_output_started = false;
  if (s_ring != NULL) {
    xStreamBufferReset(s_ring);
    (void)xStreamBufferSetTriggerLevel(s_ring, MUSIC_TRIGGER_BYTES);
  }
  if (close_spk && !s_speech_paused) {
    close_codec();
  }
}

static void begin_session_locked(music_src_t src, uint32_t rate_hz) {
  const bool keep_speech = s_speech_paused;
  s_user_stop = false;
  music_teardown(false);
  s_speech_paused = keep_speech;
  s_user_paused = false;
  s_stop = false;
  s_http_eof = false;
  s_ready_to_play = false;
  s_feed_acc_len = 0;
  s_position_bytes = 0;
  s_rate_hz = rate_hz;
  s_src = src;
  s_push_hdr_len = 0;
  s_push_hdr_state = 0;
  s_session_active = true;
  s_output_started = false;
  (void)xStreamBufferSetTriggerLevel(
      s_ring, src == MUSIC_SRC_PUSH ? MUSIC_TRIGGER_PUSH_BYTES : MUSIC_TRIGGER_BYTES);
}

static void http_task(void *arg) {
  (void)arg;
  const uint32_t gen = s_generation;
  char url[MUSIC_URL_MAX];
  strncpy(url, s_url, sizeof(url) - 1);
  url[sizeof(url) - 1] = '\0';

  s_http_running = true;
  s_http_eof = false;

  esp_http_client_config_t cfg = {
      .url = url,
      .method = HTTP_METHOD_GET,
      .timeout_ms = MUSIC_HTTP_TIMEOUT_MS,
      .buffer_size = MUSIC_HTTP_CHUNK,
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == NULL) {
    ESP_LOGE(TAG, "HTTP client init failed");
    goto done;
  }

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
    goto cleanup;
  }
  int hdr = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  if (hdr < 0 || status != 200) {
    ESP_LOGE(TAG, "HTTP stream status %d (hdr %d)", status, hdr);
    goto cleanup;
  }

  uint8_t acc[MUSIC_HDR_ACC];
  size_t acc_len = 0;
  bool header_done = false;
  size_t pcm_queued = 0;
  wav_stream_hdr_t wav = {};

  while (!s_stop && gen == s_generation) {
    int r = esp_http_client_read(client, (char *)s_http_buf, MUSIC_HTTP_CHUNK);
    if (r < 0) {
      ESP_LOGW(TAG, "HTTP read error %d", r);
      break;
    }
    if (r == 0) {
      if (esp_http_client_is_complete_data_received(client)) {
        break;
      }
      continue;
    }

    const uint8_t *src = s_http_buf;
    size_t n = (size_t)r;

    if (!header_done) {
      size_t room = sizeof(acc) - acc_len;
      size_t take = n < room ? n : room;
      memcpy(acc + acc_len, src, take);
      acc_len += take;
      int pr = wav_try_parse(acc, acc_len, &wav);
      if (pr < 0) {
        ESP_LOGE(TAG, "Invalid WAV header on music stream");
        break;
      }
      if (pr == 0) {
        if (acc_len >= sizeof(acc)) {
          ESP_LOGE(TAG, "WAV header too large");
          break;
        }
        continue;
      }
      header_done = true;
      s_rate_hz = wav.sample_rate;
      s_header_ok = true;
      ESP_LOGI(TAG, "Music stream %u Hz mono 16-bit", (unsigned)wav.sample_rate);
      src = acc + wav.pcm_off;
      n = acc_len - wav.pcm_off;
      /* Bytes that did not fit in acc still belong to PCM. */
      if (take < (size_t)r) {
        if (n > 0 && !ring_push(src, n, gen, pdMS_TO_TICKS(200))) {
          break;
        }
        pcm_queued += n;
        src = s_http_buf + take;
        n = (size_t)r - take;
      }
    }

    if (n > 0) {
      if (!ring_push(src, n, gen, pdMS_TO_TICKS(200))) {
        break;
      }
      pcm_queued += n;
    }
    if (!s_ready_to_play && pcm_queued >= prebuffer_bytes()) {
      s_ready_to_play = true;
      ESP_LOGI(TAG, "Music prebuffer %u bytes", (unsigned)pcm_queued);
    }
  }

  if (!s_ready_to_play && header_done && pcm_queued > 0) {
    s_ready_to_play = true;
  }

cleanup:
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

done:
  s_http_eof = true;
  s_http_running = false;
  if (!s_stop && gen == s_generation) {
    ESP_LOGI(TAG, "Music HTTP stream ended");
  }
  vTaskDelete(NULL);
}

static void feed_task(void *arg) {
  (void)arg;
  int64_t last_underrun_log = 0;

  while (true) {
    if (!s_session_active || s_speech_paused || s_user_paused || !s_header_ok) {
      if (s_session_active && s_src == MUSIC_SRC_PULL && s_http_eof &&
          !s_speech_paused && !s_user_paused && s_feed_acc_len == 0 &&
          (s_ring == NULL || xStreamBufferBytesAvailable(s_ring) == 0)) {
        s_session_active = false;
        close_codec();
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    const size_t min_w = min_write_bytes();
    const size_t avail =
        xStreamBufferBytesAvailable(s_ring) + s_feed_acc_len;
    const bool pull_eof = (s_src == MUSIC_SRC_PULL && s_http_eof);

    if (!s_ready_to_play) {
      if (avail >= prebuffer_bytes() || (pull_eof && avail >= 2)) {
        s_ready_to_play = true;
        ESP_LOGI(TAG, "Music prebuffer %u bytes", (unsigned)avail);
      } else {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
    }

    uint32_t gen = s_generation;
    const TickType_t gather_until =
        xTaskGetTickCount() + pdMS_TO_TICKS(MUSIC_GATHER_MS);
    while (s_feed_acc_len < min_w && xTaskGetTickCount() < gather_until) {
      if (s_feed_acc_len >= MUSIC_WRITE_CHUNK) {
        break;
      }
      size_t got = xStreamBufferReceive(s_ring, s_feed_buf + s_feed_acc_len,
                                        MUSIC_WRITE_CHUNK - s_feed_acc_len,
                                        pdMS_TO_TICKS(MUSIC_GATHER_MS));
      s_feed_acc_len += got;
      if (got == 0 && xStreamBufferBytesAvailable(s_ring) == 0) {
        break;
      }
    }

    if (gen != s_generation || s_stop || !s_session_active || s_speech_paused ||
        s_user_paused) {
      continue;
    }

    if (s_feed_acc_len < min_w) {
      if (pull_eof) {
        if (s_feed_acc_len < 2 && xStreamBufferBytesAvailable(s_ring) == 0) {
          s_session_active = false;
          s_feed_acc_len = 0;
          s_output_started = false;
          close_codec();
          ESP_LOGI(TAG, "Music track finished");
          continue;
        }
      } else {
        /* Hold small WS chunks (e.g. 12 B) until min_write; do not drop back to
         * full prebuffer after playback has started (that caused audible gaps). */
        if (!s_output_started) {
          int64_t now = (int64_t)xTaskGetTickCount() * (int64_t)portTICK_PERIOD_MS;
          if (now - last_underrun_log >= MUSIC_UNDERRUN_LOG_MS) {
            ESP_LOGW(TAG, "Music buffering (need %u B, have %u B)",
                     (unsigned)min_w, (unsigned)s_feed_acc_len);
            last_underrun_log = now;
          }
        }
        continue;
      }
    }

    size_t even = s_feed_acc_len & ~(size_t)1u;
    if (even == 0) {
      continue;
    }

    nino_audio_bus_lock();
    esp_err_t open_err = nino_audio_spk_open_locked(s_rate_hz);
    esp_err_t wr = ESP_FAIL;
    if (open_err == ESP_OK) {
      wr = nino_audio_write_pcm16_mono_locked(s_feed_buf, even);
    }
    nino_audio_bus_unlock();
    if (open_err != ESP_OK || wr != ESP_OK) {
      ESP_LOGW(TAG, "Music write failed (%s)",
               esp_err_to_name(open_err != ESP_OK ? open_err : wr));
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    s_output_started = true;
    s_position_bytes += (uint32_t)even;
    if (even < s_feed_acc_len) {
      s_feed_buf[0] = s_feed_buf[even];
      s_feed_acc_len = 1;
    } else {
      s_feed_acc_len = 0;
    }
  }
}

esp_err_t nino_music_init(void) {
  if (s_inited) {
    return ESP_OK;
  }

  s_ctrl = xSemaphoreCreateMutex();
  if (s_ctrl == NULL) {
    return ESP_ERR_NO_MEM;
  }

  s_ring_storage = (uint8_t *)heap_caps_malloc(
      MUSIC_RING_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (s_ring_storage == NULL) {
    ESP_LOGE(TAG, "PSRAM ring alloc failed");
    return ESP_ERR_NO_MEM;
  }
  s_ring = xStreamBufferCreateStatic(MUSIC_RING_BYTES, MUSIC_TRIGGER_BYTES,
                                     s_ring_storage, &s_ring_ctrl);
  if (s_ring == NULL) {
    free(s_ring_storage);
    s_ring_storage = NULL;
    return ESP_ERR_NO_MEM;
  }

  BaseType_t ok =
      xTaskCreate(feed_task, "music_feed", MUSIC_FEED_TASK_STACK, NULL, MUSIC_FEED_TASK_PRIO, NULL);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "music_feed task failed");
    vStreamBufferDelete(s_ring);
    s_ring = NULL;
    free(s_ring_storage);
    s_ring_storage = NULL;
    return ESP_ERR_NO_MEM;
  }

  s_inited = true;
  ESP_LOGI(TAG, "Music stream ready (ring %u KiB, min write %u ms)",
           (unsigned)(MUSIC_RING_BYTES / 1024), (unsigned)MUSIC_MIN_WRITE_MS);
  return ESP_OK;
}

esp_err_t nino_music_start(const char *url) {
  if (!s_inited) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!url_ok(url)) {
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(s_ctrl, portMAX_DELAY);
  begin_session_locked(MUSIC_SRC_PULL, 32000);
  s_header_ok = false;

  strncpy(s_url, url, sizeof(s_url) - 1);
  s_url[sizeof(s_url) - 1] = '\0';

  BaseType_t ok = xTaskCreate(http_task, "music_http", MUSIC_HTTP_TASK_STACK, NULL,
                              MUSIC_HTTP_TASK_PRIO, NULL);
  if (ok != pdPASS) {
    s_session_active = false;
    s_src = MUSIC_SRC_NONE;
    xSemaphoreGive(s_ctrl);
    ESP_LOGE(TAG, "music_http task failed");
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "Music start %s%s", s_url, s_speech_paused ? " (paused for speech)" : "");
  xSemaphoreGive(s_ctrl);
  return ESP_OK;
}

esp_err_t nino_music_push_start(uint32_t rate_hz, uint16_t channels, uint16_t bits) {
  if (!s_inited) {
    return ESP_ERR_INVALID_STATE;
  }
  if (rate_hz < 8000 || rate_hz > 48000 || channels != 1 || bits != 16) {
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(s_ctrl, portMAX_DELAY);
  begin_session_locked(MUSIC_SRC_PUSH, rate_hz);
  s_header_ok = true;
  ESP_LOGI(TAG, "Music push start %u Hz mono 16-bit%s", (unsigned)rate_hz,
           s_speech_paused ? " (paused for speech)" : "");
  xSemaphoreGive(s_ctrl);
  return ESP_OK;
}

esp_err_t nino_music_push_pcm(const uint8_t *data, size_t len) {
  if (!s_inited || data == NULL || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!s_session_active || s_src != MUSIC_SRC_PUSH || !s_header_ok) {
    return ESP_ERR_INVALID_STATE;
  }
  uint32_t gen = s_generation;

  if (s_push_hdr_state == 0) {
    if (len >= 4 && memcmp(data, "RIFF", 4) == 0) {
      s_push_hdr_state = 1;
    } else {
      s_push_hdr_state = 2;
    }
  }

  if (s_push_hdr_state == 1) {
    size_t room = sizeof(s_push_hdr) - s_push_hdr_len;
    size_t take = len < room ? len : room;
    memcpy(s_push_hdr + s_push_hdr_len, data, take);
    s_push_hdr_len += take;

    if (s_push_hdr_len >= 12 &&
        (memcmp(s_push_hdr, "RIFF", 4) != 0 || memcmp(s_push_hdr + 8, "WAVE", 4) != 0)) {
      s_push_hdr_state = 2;
      if (!ring_push(s_push_hdr, s_push_hdr_len, gen, pdMS_TO_TICKS(40))) {
        return ESP_ERR_NO_MEM;
      }
      if (take < len && !ring_push(data + take, len - take, gen, pdMS_TO_TICKS(40))) {
        return ESP_ERR_NO_MEM;
      }
      return ESP_OK;
    }

    if (s_push_hdr_len > 12) {
      const uint8_t *chunk_id = s_push_hdr + 12;
      const bool looks_like_wav = memcmp(chunk_id, "fmt ", 4) == 0 ||
                                  memcmp(chunk_id, "LIST", 4) == 0 ||
                                  memcmp(chunk_id, "JUNK", 4) == 0 ||
                                  memcmp(chunk_id, "data", 4) == 0;
      if (!looks_like_wav) {
        /* 12-byte RIFF/WAVE prefix, then raw PCM — do not play the prefix. */
        s_push_hdr_state = 2;
        size_t pcm_n = s_push_hdr_len - 12;
        if (pcm_n > 0 && !ring_push(s_push_hdr + 12, pcm_n, gen, pdMS_TO_TICKS(40))) {
          return ESP_ERR_NO_MEM;
        }
        if (take < len && !ring_push(data + take, len - take, gen, pdMS_TO_TICKS(40))) {
          return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Music push skipped 12-byte RIFF prefix");
        return ESP_OK;
      }
    }

    wav_stream_hdr_t wav = {};
    int pr = wav_try_parse(s_push_hdr, s_push_hdr_len, &wav);
    if (pr < 0) {
      s_push_hdr_state = 2;
      if (!ring_push(s_push_hdr, s_push_hdr_len, gen, pdMS_TO_TICKS(40))) {
        return ESP_ERR_NO_MEM;
      }
      if (take < len && !ring_push(data + take, len - take, gen, pdMS_TO_TICKS(40))) {
        return ESP_ERR_NO_MEM;
      }
      return ESP_OK;
    }
    if (pr == 0) {
      if (s_push_hdr_len >= sizeof(s_push_hdr)) {
        return ESP_ERR_INVALID_ARG;
      }
      return ESP_OK;
    }
    s_rate_hz = wav.sample_rate;
    s_push_hdr_state = 2;
    size_t pcm_n = s_push_hdr_len - wav.pcm_off;
    if (pcm_n > 0 && !ring_push(s_push_hdr + wav.pcm_off, pcm_n, gen, pdMS_TO_TICKS(40))) {
      return ESP_ERR_NO_MEM;
    }
    if (take < len && !ring_push(data + take, len - take, gen, pdMS_TO_TICKS(40))) {
      return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Music push skipped WAV header, %u Hz", (unsigned)wav.sample_rate);
    return ESP_OK;
  }

  if (!ring_push(data, len, gen, pdMS_TO_TICKS(40))) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

esp_err_t nino_music_pause(void) {
  if (!s_inited || !s_session_active) {
    return ESP_ERR_INVALID_STATE;
  }
  xSemaphoreTake(s_ctrl, portMAX_DELAY);
  s_user_paused = true;
  s_speech_paused = false;
  close_codec();
  xSemaphoreGive(s_ctrl);
  ESP_LOGI(TAG, "Music user-pause at %u ms", (unsigned)bytes_to_ms(s_position_bytes));
  return ESP_OK;
}

esp_err_t nino_music_resume(void) {
  if (!s_inited || !s_session_active) {
    return ESP_ERR_INVALID_STATE;
  }
  xSemaphoreTake(s_ctrl, portMAX_DELAY);
  s_user_paused = false;
  size_t avail = xStreamBufferBytesAvailable(s_ring) + s_feed_acc_len;
  s_ready_to_play =
      s_output_started ? true : (avail >= prebuffer_bytes());
  xSemaphoreGive(s_ctrl);
  ESP_LOGI(TAG, "Music user-resume at %u ms", (unsigned)bytes_to_ms(s_position_bytes));
  return ESP_OK;
}

void nino_music_stop(void) {
  if (!s_inited) {
    return;
  }
  xSemaphoreTake(s_ctrl, portMAX_DELAY);
  s_user_stop = true;
  s_speech_paused = false;
  music_teardown(true);
  xSemaphoreGive(s_ctrl);
  ESP_LOGI(TAG, "Music stopped");
}

bool nino_music_is_playing(void) {
  return s_session_active && !s_user_paused && !s_speech_paused;
}

bool nino_music_is_user_paused(void) {
  return s_session_active && s_user_paused;
}

void nino_music_pause_for_speech(bool paused) {
  if (paused) {
    if (s_user_paused) {
      return;
    }
    s_speech_paused = true;
    close_codec();
    return;
  }
  s_speech_paused = false;
  if (s_user_stop || !s_session_active || s_user_paused) {
    return;
  }
  if (s_output_started) {
    s_ready_to_play = true;
  }
}

void nino_music_get_status(nino_music_status_t *out) {
  if (out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  out->rate_hz = rate_or_default();
  out->session = s_session_active;
  out->paused = s_session_active && (s_user_paused || s_speech_paused);
  out->position_ms = bytes_to_ms(s_position_bytes);
  size_t buffered = s_feed_acc_len;
  if (s_ring != NULL) {
    buffered += xStreamBufferBytesAvailable(s_ring);
  }
  out->buffered_ms = bytes_to_ms(buffered);

  if (!s_session_active) {
    out->state = NINO_MUSIC_STATE_IDLE;
  } else if (out->paused) {
    out->state = NINO_MUSIC_STATE_PAUSED;
  } else if (!s_ready_to_play) {
    out->state = NINO_MUSIC_STATE_BUFFERING;
  } else {
    out->state = NINO_MUSIC_STATE_PLAYING;
  }
}

const char *nino_music_state_str(nino_music_state_t state) {
  switch (state) {
  case NINO_MUSIC_STATE_BUFFERING:
    return "buffering";
  case NINO_MUSIC_STATE_PLAYING:
    return "playing";
  case NINO_MUSIC_STATE_PAUSED:
    return "paused";
  case NINO_MUSIC_STATE_IDLE:
  default:
    return "idle";
  }
}
