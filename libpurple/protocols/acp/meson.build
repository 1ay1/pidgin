libacp = shared_library('acp',
  files('acp.c', 'acp_rpc.c', 'acp_render.c'),
  include_directories : prpl_inc,
  c_args : prpl_cflags,
  dependencies : [libpurple_dep, glib_dep, json_glib_dep],
  name_prefix : 'lib',
  install : true,
  install_dir : prpl_install_dir,
)
