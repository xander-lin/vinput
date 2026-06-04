ebur128 = dependency('libebur128')
speexdsp = dependency('speexdsp')
pulse_simple = dependency('libpulse-simple')
libcurl = dependency('libcurl')
soxr = dependency('soxr')

test_audio_pipeline = executable('test_audio_pipeline',
  'test_audio_pipeline.cpp',
  dependencies: [asr_provider_dep, ebur128, speexdsp, pulse_simple, libcurl, soxr],
  install: false)

test_audio_file = executable('test_audio_file',
  'test_audio_file.cpp',
  dependencies: [asr_provider_dep, ebur128, speexdsp, libcurl, soxr, pulse_simple],
  install: false)
