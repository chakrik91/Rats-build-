#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  NINO_MUSIC_STATE_IDLE = 0,
  NINO_MUSIC_STATE_BUFFERING,
  NINO_MUSIC_STATE_PLAYING,
  NINO_MUSIC_STATE_PAUSED,
} nino_music_state_t;

typedef struct {
  nino_music_state_t state;
  bool session;
  bool paused;
  uint32_t rate_hz;
  uint32_t buffered_ms;
  uint32_t position_ms;
} nino_music_status_t;

esp_err_t nino_music_init(void);
esp_err_t nino_music_start(const char *url); /* copies url, starts puller */
esp_err_t nino_music_push_start(uint32_t rate_hz, uint16_t channels, uint16_t bits);
esp_err_t nino_music_push_pcm(const uint8_t *data, size_t len);
esp_err_t nino_music_pause(void);
esp_err_t nino_music_resume(void);
void nino_music_stop(void); /* idempotent */
bool nino_music_is_playing(void);
/** True when the user explicitly paused (HTTP/WS); voice must not auto-resume. */
bool nino_music_is_user_paused(void);
void nino_music_pause_for_speech(bool paused); /* duck for wake/TTS */
void nino_music_get_status(nino_music_status_t *out);
const char *nino_music_state_str(nino_music_state_t state);
