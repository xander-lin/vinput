sherpa_dir = get_option('sherpa_onnx_dir')

asr_provider_lib = static_library('asr_provider',
  'asr_provider.h',
  'asr_provider.cpp',
  'mock_provider.h',
  'mock_provider.cpp',
  'qwen3_provider.h',
  'qwen3_provider.cpp',
  'zipformer_provider.h',
  'zipformer_provider.cpp',
  include_directories: include_directories(sherpa_dir / 'include'),
  install: false)

asr_provider_dep = declare_dependency(
  include_directories: [include_directories('.'), include_directories(sherpa_dir / 'include')],
  link_whole: asr_provider_lib)
