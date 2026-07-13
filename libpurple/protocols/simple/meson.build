libsimple = shared_library('simple',
  files('simple.c', 'sipmsg.c'),
  include_directories : prpl_inc,
  c_args : prpl_cflags,
  dependencies : [libpurple_dep, glib_dep],
  name_prefix : 'lib',
  install : true,
  install_dir : prpl_install_dir,
)
