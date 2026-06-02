fcitx5_core = dependency('Fcitx5Core')
fcitx5_module = dependency('Fcitx5Module')
libcurl = dependency('libcurl')
pulse_simple = dependency('libpulse-simple')

asr_provider = asr_provider_dep

sherpa_onnx_lib = sherpa_onnx_dir / 'lib'
sherpa_onnx_cxx = declare_dependency(
  link_args: ['-L' + sherpa_onnx_lib, '-lsherpa-onnx-cxx-api', '-lsherpa-onnx-c-api',
              '-lonnxruntime', '-Wl,-rpath,' + sherpa_onnx_lib])

subdir('src')
