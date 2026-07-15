libacp = shared_library('acp',
  files('acp.c'),
  include_directories : prpl_inc,
  c_args : prpl_cflags,
  dependencies : [libpurple_dep, glib_dep, json_glib_dep],
  name_prefix : 'lib',
  install : true,
  install_dir : prpl_install_dir,
)
