# musicmessaging — needs a D-Bus binding stub generated from its own source.

mm_bindings = custom_target('music-messaging-bindings.c',
  input : files('musicmessaging.c'),
  output : 'music-messaging-bindings.ci',
  command : [python3, files('../../../libpurple/dbus-codegen.py'),
             files('../../../libpurple/dbus-analyze-functions.py'),
             '--export-only', '--', '@INPUT@'],
  capture : true,
)

shared_library('musicmessaging',
  files('musicmessaging.c'),
  include_directories : pidgin_plugin_inc,
  c_args : pidgin_plugin_cflags,
  dependencies : [pidgin_dep, gtk_dep, dbus_dep, dbus_glib_dep,
                  declare_dependency(sources : mm_bindings)],
  name_prefix : '',
  override_options : ['b_lundef=false'],
  install : true,
  install_dir : pidgin_plugindir,
)

install_data(files('music.png'),
  install_dir : datadir / 'pixmaps' / 'pidgin' / 'buttons',
)
