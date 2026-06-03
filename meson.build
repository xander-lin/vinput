project('fcitx5-vinput', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++20', 'buildtype=debug'])

sherpa_onnx_dir = get_option('sherpa_onnx_dir')
if sherpa_onnx_dir == ''
  sherpa_onnx_dir = run_command('sh', '-c',
    'echo ${XDG_DATA_HOME:-$HOME/.local/share}/vinput/sherpa-onnx',
    check: true).stdout().strip()
endif
message('sherpa-onnx dir: ' + sherpa_onnx_dir)

subdir('ASR_provider/src')
subdir('adapter')
