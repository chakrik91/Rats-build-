#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Load ok_nino.tflite + TFLM audio frontend. */
bool mww_detector_init(void);

/** True after a successful init. */
bool mww_detector_ready(void);

/** Clear frontend + sliding-window state (call after VAD releases the mic). */
void mww_detector_reset(void);

/**
 * Consume 16 kHz mono PCM. Sets *detected when the sliding-window average
 * exceeds the model cutoff. Returns false only on a hard inference error.
 */
bool mww_detector_process(const int16_t *samples, size_t sample_count, bool *detected);

#ifdef __cplusplus
}
#endif
