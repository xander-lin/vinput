pulse_simple = dependency('libpulse-simple')

test_audio_pipeline = executable('test_audio_pipeline',
  'test_audio_pipeline.cpp',
  dependencies: [asr_provider_dep, pulse_simple],
  install: false)

test_audio_file = executable('test_audio_file',
  'test_audio_file.cpp',
  dependencies: [asr_provider_dep],
  install: false)
