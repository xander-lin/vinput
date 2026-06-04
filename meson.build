project('fcitx5-vinput', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++20', 'warning_level=3', 'werror=true'])

add_project_arguments('-Wno-variadic-macros', language: 'cpp')

subdir('ASR_provider/src')
subdir('tools')
subdir('adapter')

install_man('docs/vinput.1')
