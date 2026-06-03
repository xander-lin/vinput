asr_provider_lib = static_library('asr_provider',
  'asr_provider.h',
  'asr_provider.cpp',
  'mock_provider.h',
  'mock_provider.cpp',
  'zipformer_provider.h',
  'zipformer_provider.cpp',
  'doubao_provider.h',
  'doubao_provider.cpp',
  install: false)

asr_provider_dep = declare_dependency(
  include_directories: include_directories('.'),
  link_whole: asr_provider_lib)
