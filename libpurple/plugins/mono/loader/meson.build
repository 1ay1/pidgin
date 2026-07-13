# Mono loader C library (mono.so).

shared_library('mono',
  files(
    'mono.c',
    'mono-helper.c',
    'debug-glue.c',
    'signal-glue.c',
    'blist-glue.c',
    'status-glue.c',
  ),
  include_directories : [include_directories('../../..', '../../../libpurple'), config_h_include],
  c_args : common_cflags,
  dependencies : [libpurple_dep, glib_dep, mono_dep],
  name_prefix : '',
  install : true,
  install_dir : purple_plugindir,
)
