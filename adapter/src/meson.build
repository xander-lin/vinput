vinput = shared_library('vinput',
  'vinput.cpp',
  dependencies: [fcitx5_core, fcitx5_module, asr_provider, pulse_simple, ebur128],
  name_prefix: '',
  install: true,
  install_dir: fcitx5_core.get_variable(pkgconfig: 'libdir') / 'fcitx5')

addon_conf = configure_file(
  input: 'vinput-addon.conf.in',
  output: 'vinput.conf',
  configuration: {'PROJECT_VERSION': meson.project_version()},
  install: true,
  install_dir: get_option('datadir') / 'fcitx5' / 'addon')
