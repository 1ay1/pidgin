libirc = shared_library('irc',
  files('cmds.c', 'dcc_send.c', 'irc.c', 'msgs.c', 'parse.c'),
  include_directories : prpl_inc,
  c_args : prpl_cflags,
  dependencies : [libpurple_dep, glib_dep] + (have_sasl ? [sasl_dep] : []),
  name_prefix : 'lib',
  install : true,
  install_dir : prpl_install_dir,
)
