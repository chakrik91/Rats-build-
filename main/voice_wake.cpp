#include "voice_wake.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>

#include "sdkconfig.h"

extern "C" {
#include "audio_playback.h"
#include "mic_input.h"
#include "mww_detector.h"
#include "nino_eye.h"
#include "voice_assist.h"
}

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "voice_wake";

#define WAKE_COOLDOWN_MS 2000
#define AFTER_WAKE_TASK_STACK 20480
#define WAKE_TASK_PRIO 3
#define WAKE_IDLE_DELAY_MS 50
#define WAKE_PHRASE_HINT "\"Ok Nino\""
#define WAKE_SAMPLE_RATE_HZ 16000
#define WAKE_FEATURE_STEP_MS 10
#define WAKE_READ_SAMPLES ((WAKE_SAMPLE_RATE_HZ * WAKE_FEATURE_STEP_MS) / 1000)

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define WAKE_DETECT_CORE 1
#else
#define WAKE_DETECT_CORE tskNO_AFFINITY
#endif

static volatile bool s_wake_enabled = true;
static volatile bool s_wake_hw_ready = false;
static volatile bool s_after_wake_busy = false;
static volatile bool s_mic_capture_hold = false;
static int64_t s_last_wake_us = 0;

static void after_wake_task(void *arg) {
  (void)arg;
  const int64_t chime_started_us = esp_timer_get_time();
  ESP_LOGI(TAG, "Wake acknowledged — playing chime now");
  esp_err_t beep_e = nino_voice_play_wake_chime();
  if (beep_e != ESP_OK) {
    ESP_LOGW(TAG, "wake chime failed: %s", esp_err_to_name(beep_e));
  }
  ESP_LOGI(TAG, "Chime finished in %" PRId64 " ms; VAD recording now",
           (esp_timer_get_time() - chime_started_us) / 1000LL);
  nino_eye_listening();
  esp_err_t e = nino_voice_assist_run_query_only();
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "voice query failed: %s", esp_err_to_name(e));
    nino_eye_idle();
    nino_voice_wake_release_after_wake();
  }
  vTaskDelete(NULL);
}

static void start_after_wake(void) {
  if (!nino_voice_assist_has_ws_uri()) {
    ESP_LOGW(TAG,
             "Wake heard, but voice PC URL not set — serial: "
             "voice connect <YOUR_PC_LAN_IP> 8000");
    return;
  }
  s_after_wake_busy = true;
  BaseType_t ok = xTaskCreatePinnedToCore(after_wake_task, "voice_wake_q", AFTER_WAKE_TASK_STACK, NULL,
                                          WAKE_TASK_PRIO + 1, NULL, WAKE_DETECT_CORE);
  if (ok != pdPASS) {
    ESP_LOGW(TAG, "could not start voice_wake_q task");
    s_after_wake_busy = false;
  }
}

static void wake_detect_task(void *arg) {
  (void)arg;
  int16_t *buff = (int16_t *)malloc((size_t)WAKE_READ_SAMPLES * sizeof(int16_t));
  if (buff == NULL) {
    ESP_LOGE(TAG, "wake read buffer alloc failed");
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "wake detect started — say " WAKE_PHRASE_HINT);
  bool flush_before_read = true;
  bool logged_mic_source = false;

  while (true) {
    const bool armed = s_wake_enabled;
    const bool mic_exclusive = s_mic_capture_hold;

    if (!armed && !mic_exclusive) {
      flush_before_read = true;
      logged_mic_source = false;
      vTaskDelay(pdMS_TO_TICKS(WAKE_IDLE_DELAY_MS));
      continue;
    }

    if (mic_exclusive || s_after_wake_busy) {
      flush_before_read = true;
      logged_mic_source = false;
      vTaskDelay(pdMS_TO_TICKS(WAKE_FEATURE_STEP_MS));
      continue;
    }

    if (!nino_mic_available()) {
      vTaskDelay(pdMS_TO_TICKS(WAKE_IDLE_DELAY_MS));
      continue;
    }

    if (flush_before_read) {
      nino_mic_flush();
      mww_detector_reset();
      flush_before_read = false;
    }

    if (nino_mic_read(buff, WAKE_READ_SAMPLES) != ESP_OK) {
      logged_mic_source = false;
      vTaskDelay(pdMS_TO_TICKS(WAKE_FEATURE_STEP_MS));
      continue;
    }
    if (!logged_mic_source) {
      ESP_LOGI(TAG, "wake mic source: %s", nino_mic_source_name(nino_mic_preferred_source()));
      logged_mic_source = true;
    }

    bool detected = false;
    if (!mww_detector_process(buff, WAKE_READ_SAMPLES, &detected)) {
      vTaskDelay(pdMS_TO_TICKS(WAKE_FEATURE_STEP_MS));
      continue;
    }

    if (detected && s_wake_enabled && !s_after_wake_busy) {
      const int64_t now = esp_timer_get_time();
      if (s_last_wake_us == 0 || (now - s_last_wake_us) >= (int64_t)WAKE_COOLDOWN_MS * 1000LL) {
        s_last_wake_us = now;
        ESP_LOGI(TAG, "MicroWakeWord detected (" WAKE_PHRASE_HINT ") via %s",
                 nino_mic_source_name(nino_mic_preferred_source()));
        start_after_wake();
      }
    }
  }
}

extern "C" void nino_voice_wake_init(void) {
  if (!mww_detector_init()) {
    ESP_LOGW(TAG, "MicroWakeWord init failed (ok_nino_v6.tflite / TFLM frontend)");
    return;
  }

  BaseType_t ok = xTaskCreatePinnedToCore(wake_detect_task, "wake_detect", 12288, NULL, WAKE_TASK_PRIO,
                                          NULL, WAKE_DETECT_CORE);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "wake task create failed");
    s_wake_hw_ready = false;
    return;
  }

  s_wake_hw_ready = true;
  ESP_LOGI(TAG, "Wake word ready (USB preferred; ES8311 fallback) — say " WAKE_PHRASE_HINT);
}

extern "C" void nino_voice_wake_drop_mic_locked(void) {
  nino_mic_drop_es8311_locked();
}

extern "C" void nino_voice_wake_set_mic_capture_hold(bool hold) {
  if (!hold && s_mic_capture_hold) {
    mww_detector_reset();
  }
  s_mic_capture_hold = hold;
}

extern "C" void nino_voice_wake_release_after_wake(void) {
  if (!s_after_wake_busy) {
    return;
  }
  s_after_wake_busy = false;
  ESP_LOGI(TAG, "wake armed — say " WAKE_PHRASE_HINT);
}

extern "C" void nino_voice_wake_set_enabled(bool on) {
  s_wake_enabled = on;
  ESP_LOGI(TAG, "wake %s", on ? "on" : "off");
}

extern "C" bool nino_voice_wake_is_enabled(void) { return s_wake_enabled; }

extern "C" bool nino_voice_wake_hw_ready(void) {
  return s_wake_hw_ready && mww_detector_ready();
}
