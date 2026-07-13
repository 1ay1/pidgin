# Tcl plugin loader.

tcl_deps = [libpurple_dep, glib_dep, tcl_dep]
tcl_defs = []
if have_tk
  tcl_deps += tk_dep
  tcl_defs += '-DHAVE_TK'
endif

shared_library('tcl',
  files(
    'tcl.c',
    'tcl_glib.c',
    'tcl_cmds.c',
    'tcl_signals.c',
    'tcl_ref.c',
    'tcl_cmd.c',
  ),
  include_directories : [include_directories('..', '../..'), config_h_include],
  c_args : common_cflags + tcl_defs,
  dependencies : tcl_deps,
  name_prefix : '',
  install : true,
  install_dir : purple_plugindir,
)
