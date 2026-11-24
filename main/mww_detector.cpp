#include "mww_detector.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern const uint8_t ok_nino_v6_tflite_start[] asm("_binary_ok_nino_v6_tflite_start");
extern const uint8_t ok_nino_v6_tflite_end[] asm("_binary_ok_nino_v6_tflite_end");

static const char *TAG = "mww";

/* Must match ok_nino_v6.json and the MicroWakeWord training frontend. */
static const int kSampleRateHz = 16000;
static const int kFeatureSize = 40;
static const int kFeatureDurationMs = 30;
static const int kFeatureStepMs = 10;
static const size_t kSlidingWindow = 5;
static const uint8_t kProbabilityCutoff = 102; /* 0.4 * 255 */
static const size_t kManifestArenaBytes = 50000;
static const size_t kVariableArenaBytes = 2048;
static const int16_t kMinSlicesBeforeDetection = 100;

static const float kFilterbankLowerHz = 125.0f;
static const float kFilterbankUpperHz = 7500.0f;
static const uint8_t kNoiseSmoothingBits = 10;
static const float kNoiseEvenSmoothing = 0.025f;
static const float kNoiseOddSmoothing = 0.06f;
static const float kNoiseMinSignalRemaining = 0.05f;
static const float kPcanStrength = 0.95f;
static const float kPcanOffset = 80.0f;
static const uint8_t kPcanGainBits = 21;
static const uint8_t kLogScaleShift = 6;

using OpResolver = tflite::MicroMutableOpResolver<20>;

static struct FrontendConfig s_frontend_cfg;
static struct FrontendState s_frontend_state;
static bool s_frontend_ready = false;

static OpResolver s_ops;
static tflite::MicroAllocator *s_var_allocator = nullptr;
static tflite::MicroResourceVariables *s_resources = nullptr;
static tflite::MicroInterpreter *s_interpreter = nullptr;
static uint8_t *s_tensor_arena = nullptr;
static uint8_t *s_var_arena = nullptr;
static size_t s_tensor_arena_size = 0;
static int s_stride = 1;
static uint8_t s_stride_step = 0;
static uint8_t s_window[kSlidingWindow];
static size_t s_window_index = 0;
static int16_t s_ignore_windows = -kMinSlicesBeforeDetection;
static bool s_ready = false;

static uint8_t *alloc_arena(size_t bytes) {
  uint8_t *p = (uint8_t *)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr) {
    p = (uint8_t *)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_8BIT);
  }
  return p;
}

static bool register_ops(OpResolver *ops) {
  return ops->AddCallOnce() == kTfLiteOk && ops->AddVarHandle() == kTfLiteOk &&
         ops->AddReshape() == kTfLiteOk && ops->AddReadVariable() == kTfLiteOk &&
         ops->AddStridedSlice() == kTfLiteOk && ops->AddConcatenation() == kTfLiteOk &&
         ops->AddAssignVariable() == kTfLiteOk && ops->AddConv2D() == kTfLiteOk &&
         ops->AddMul() == kTfLiteOk && ops->AddAdd() == kTfLiteOk && ops->AddMean() == kTfLiteOk &&
         ops->AddFullyConnected() == kTfLiteOk && ops->AddLogistic() == kTfLiteOk &&
         ops->AddQuantize() == kTfLiteOk && ops->AddDepthwiseConv2D() == kTfLiteOk &&
         ops->AddAveragePool2D() == kTfLiteOk && ops->AddMaxPool2D() == kTfLiteOk &&
         ops->AddPad() == kTfLiteOk && ops->AddPack() == kTfLiteOk && ops->AddSplitV() == kTfLiteOk;
}

static void reset_probabilities(void) {
  std::memset(s_window, 0, sizeof(s_window));
  s_window_index = 0;
  s_ignore_windows = -kMinSlicesBeforeDetection;
  s_stride_step = 0;
}

static bool generate_features(const int16_t *samples, size_t available, int8_t *out_features,
                              size_t *consumed) {
  *consumed = 0;
  struct FrontendOutput fe =
      FrontendProcessSamples(&s_frontend_state, samples, available, consumed);
  if (fe.size == 0) {
    return false;
  }

  /* Scale 16-bit frontend bins to int8 the same way ESPHome / MicroWakeWord does. */
  constexpr int32_t value_scale = 256;
  constexpr int32_t value_div = 666;
  for (size_t i = 0; i < fe.size && i < (size_t)kFeatureSize; ++i) {
    int32_t value = ((fe.values[i] * value_scale) + (value_div / 2)) / value_div;
    value += INT8_MIN;
    out_features[i] = static_cast<int8_t>(std::clamp(value, (int32_t)INT8_MIN, (int32_t)INT8_MAX));
  }
  return true;
}

static bool load_interpreter(void) {
  const tflite::Model *model = tflite::GetModel(ok_nino_v6_tflite_start);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    ESP_LOGE(TAG, "Unsupported TFLite schema %lu (need %d)", (unsigned long)model->version(),
             TFLITE_SCHEMA_VERSION);
    return false;
  }

  if (!register_ops(&s_ops)) {
    ESP_LOGE(TAG, "Failed to register TFLM ops");
    return false;
  }

  s_var_arena = alloc_arena(kVariableArenaBytes);
  if (s_var_arena == nullptr) {
    ESP_LOGE(TAG, "Variable arena alloc failed");
    return false;
  }
  s_var_allocator = tflite::MicroAllocator::Create(s_var_arena, kVariableArenaBytes);
  s_resources = tflite::MicroResourceVariables::Create(s_var_allocator, 20);
  if (s_var_allocator == nullptr || s_resources == nullptr) {
    ESP_LOGE(TAG, "Resource variable setup failed");
    return false;
  }

  const size_t attempts[] = {kManifestArenaBytes, kManifestArenaBytes * 2};
  for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); ++i) {
    size_t bytes = (attempts[i] + 15) & ~((size_t)15);
    uint8_t *arena = alloc_arena(bytes);
    if (arena == nullptr) {
      continue;
    }
    auto *interp = new tflite::MicroInterpreter(model, s_ops, arena, bytes, s_resources);
    if (interp->AllocateTensors() == kTfLiteOk) {
      s_tensor_arena = arena;
      s_tensor_arena_size = bytes;
      s_interpreter = interp;
      break;
    }
    delete interp;
    heap_caps_free(arena);
  }

  if (s_interpreter == nullptr) {
    ESP_LOGE(TAG, "AllocateTensors failed (tried %u and %u bytes)", (unsigned)kManifestArenaBytes,
             (unsigned)(kManifestArenaBytes * 2));
    return false;
  }

  TfLiteTensor *input = s_interpreter->input(0);
  TfLiteTensor *output = s_interpreter->output(0);
  if (input == nullptr || output == nullptr || input->dims == nullptr || output->dims == nullptr) {
    ESP_LOGE(TAG, "Missing input/output tensor");
    return false;
  }
  if (input->dims->size != 3 || input->dims->data[0] != 1 || input->dims->data[2] != kFeatureSize ||
      input->type != kTfLiteInt8) {
    ESP_LOGE(TAG, "Unexpected input tensor (want [1, stride, 40] int8)");
    return false;
  }
  if (output->dims->size != 2 || output->dims->data[0] != 1 || output->dims->data[1] != 1 ||
      output->type != kTfLiteUInt8) {
    ESP_LOGE(TAG, "Unexpected output tensor (want [1, 1] uint8)");
    return false;
  }

  s_stride = input->dims->data[1];
  if (s_stride < 1) {
    s_stride = 1;
  }

  ESP_LOGI(TAG, "ok_nino loaded: input [1,%d,%d] arena=%u stride=%d cutoff=%.2f", s_stride,
           kFeatureSize, (unsigned)s_tensor_arena_size, s_stride, kProbabilityCutoff / 255.0f);
  reset_probabilities();
  return true;
}

static bool setup_frontend(void) {
  std::memset(&s_frontend_cfg, 0, sizeof(s_frontend_cfg));
  s_frontend_cfg.window.size_ms = kFeatureDurationMs;
  s_frontend_cfg.window.step_size_ms = kFeatureStepMs;
  s_frontend_cfg.filterbank.num_channels = kFeatureSize;
  s_frontend_cfg.filterbank.lower_band_limit = kFilterbankLowerHz;
  s_frontend_cfg.filterbank.upper_band_limit = kFilterbankUpperHz;
  s_frontend_cfg.noise_reduction.smoothing_bits = kNoiseSmoothingBits;
  s_frontend_cfg.noise_reduction.even_smoothing = kNoiseEvenSmoothing;
  s_frontend_cfg.noise_reduction.odd_smoothing = kNoiseOddSmoothing;
  s_frontend_cfg.noise_reduction.min_signal_remaining = kNoiseMinSignalRemaining;
  s_frontend_cfg.pcan_gain_control.enable_pcan = 1;
  s_frontend_cfg.pcan_gain_control.strength = kPcanStrength;
  s_frontend_cfg.pcan_gain_control.offset = kPcanOffset;
  s_frontend_cfg.pcan_gain_control.gain_bits = kPcanGainBits;
  s_frontend_cfg.log_scale.enable_log = 1;
  s_frontend_cfg.log_scale.scale_shift = kLogScaleShift;

  if (!FrontendPopulateState(&s_frontend_cfg, &s_frontend_state, kSampleRateHz)) {
    ESP_LOGE(TAG, "FrontendPopulateState failed");
    return false;
  }
  s_frontend_ready = true;
  return true;
}

static bool infer_feature(const int8_t features[kFeatureSize], bool *detected) {
  *detected = false;
  TfLiteTensor *input = s_interpreter->input(0);
  int8_t *input_data = tflite::GetTensorData<int8_t>(input);
  std::memmove(input_data + (kFeatureSize * s_stride_step), features, kFeatureSize);
  ++s_stride_step;

  if (s_stride_step < s_stride) {
    return true;
  }
  s_stride_step = 0;

  if (s_interpreter->Invoke() != kTfLiteOk) {
    ESP_LOGW(TAG, "Invoke failed");
    return false;
  }

  const uint8_t prob = s_interpreter->output(0)->data.uint8[0];
  ++s_window_index;
  if (s_window_index == kSlidingWindow) {
    s_window_index = 0;
  }
  s_window[s_window_index] = prob;

  if (prob < kProbabilityCutoff) {
    s_ignore_windows = std::min<int16_t>(static_cast<int16_t>(s_ignore_windows + 1), 0);
  }

  if (s_ignore_windows < 0) {
    return true;
  }

  uint32_t sum = 0;
  for (size_t i = 0; i < kSlidingWindow; ++i) {
    sum += s_window[i];
  }
  if (sum > (uint32_t)kProbabilityCutoff * kSlidingWindow) {
    ESP_LOGI(TAG, "Ok Nino detected (avg=%.2f last=%u)", (sum / (float)kSlidingWindow) / 255.0f,
             (unsigned)prob);
    reset_probabilities();
    *detected = true;
  }
  return true;
}

extern "C" bool mww_detector_init(void) {
  if (s_ready) {
    return true;
  }
  const size_t model_bytes = (size_t)(ok_nino_v6_tflite_end - ok_nino_v6_tflite_start);
  ESP_LOGI(TAG, "Loading ok_nino_v6.tflite (%u bytes)", (unsigned)model_bytes);

  if (!setup_frontend()) {
    return false;
  }
  if (!load_interpreter()) {
    return false;
  }
  s_ready = true;
  return true;
}

extern "C" bool mww_detector_ready(void) { return s_ready; }

extern "C" void mww_detector_reset(void) {
  if (s_frontend_ready) {
    FrontendReset(&s_frontend_state);
  }
  reset_probabilities();
}

extern "C" bool mww_detector_process(const int16_t *samples, size_t sample_count, bool *detected) {
  *detected = false;
  if (!s_ready || samples == nullptr || sample_count == 0) {
    return s_ready;
  }

  size_t offset = 0;
  int8_t features[kFeatureSize];
  while (offset < sample_count) {
    size_t consumed = 0;
    const bool have_feature =
        generate_features(samples + offset, sample_count - offset, features, &consumed);
    if (consumed == 0) {
      break;
    }
    offset += consumed;
    if (!have_feature) {
      continue;
    }
    bool hit = false;
    if (!infer_feature(features, &hit)) {
      return false;
    }
    if (hit) {
      *detected = true;
      return true;
    }
  }
  return true;
}
