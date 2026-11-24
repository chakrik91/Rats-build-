# Vendored TensorFlow Lite Micro audio frontend (Apache-2.0).
# Used to generate the 40-dim features MicroWakeWord was trained on.

set(FE_DIR tensorflow/lite/experimental/microfrontend/lib)

idf_component_register(
    SRCS
        "${FE_DIR}/fft.cc"
        "${FE_DIR}/fft_util.cc"
        "${FE_DIR}/filterbank.c"
        "${FE_DIR}/filterbank_util.c"
        "${FE_DIR}/frontend.c"
        "${FE_DIR}/frontend_util.c"
        "${FE_DIR}/kiss_fft_int16.cc"
        "${FE_DIR}/log_lut.c"
        "${FE_DIR}/log_scale.c"
        "${FE_DIR}/log_scale_util.c"
        "${FE_DIR}/noise_reduction.c"
        "${FE_DIR}/noise_reduction_util.c"
        "${FE_DIR}/pcan_gain_control.c"
        "${FE_DIR}/pcan_gain_control_util.c"
        "${FE_DIR}/window.c"
        "${FE_DIR}/window_util.c"
    INCLUDE_DIRS
        "."
        "third_party/kissfft"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-maybe-uninitialized
)
