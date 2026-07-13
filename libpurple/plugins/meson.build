# libpurple/plugins — bundled loadable plugins + SSL backends.

plugin_cflags = common_cflags + [
  '-DDATADIR="@0@"'.format(abs_datadir),
]
plugin_inc = [include_directories('..'), config_h_include]

# --- SSL backend plugins ---------------------------------------------------
# The generic ssl.c dispatcher is always built; the concrete backend
# (gnutls / nss) is built for whichever TLS library was found.
shared_library('ssl',
  files('ssl/ssl.c'),
  include_directories : plugin_inc,
  c_args : plugin_cflags,
  dependencies : [libpurple_dep, glib_dep],
  name_prefix : '',
  install : true,
  install_dir : purple_plugindir,
)

if have_gnutls
  shared_library('ssl-gnutls',
    files('ssl/ssl-gnutls.c'),
    include_directories : plugin_inc,
    c_args : plugin_cflags,
    dependencies : [libpurple_dep, glib_dep, gnutls_dep],
    name_prefix : '',
    install : true,
    install_dir : purple_plugindir,
  )
endif

if have_nss
  shared_library('ssl-nss',
    files('ssl/ssl-nss.c'),
    include_directories : plugin_inc,
    c_args : plugin_cflags,
    dependencies : [libpurple_dep, glib_dep, nss_dep],
    name_prefix : '',
    install : true,
    install_dir : purple_plugindir,
  )
  shared_library('nss-prefs',
    files('ssl/nss-prefs.c'),
    include_directories : plugin_inc,
    c_args : plugin_cflags,
    dependencies : [libpurple_dep, glib_dep, nss_dep],
    name_prefix : '',
    install : true,
    install_dir : purple_plugindir,
  )
endif

# --- bundled, installed feature plugins ------------------------------------
bundled_plugins = [
  'autoaccept',
  'buddynote',
  'idle',
  'joinpart',
  'log_reader',
  'newline',
  'offlinemsg',
  'psychic',
  'statenotify',
]

foreach p : bundled_plugins
  shared_library(p,
    files(p + '.c'),
    include_directories : plugin_inc,
    c_args : plugin_cflags,
    dependencies : [libpurple_dep, glib_dep],
    name_prefix : '',
    install : true,
    install_dir : purple_plugindir,
  )
endforeach

# dbus-example needs the generated dbus bindings.
if have_dbus
  dbus_example_bindings = custom_target('dbus-example-bindings.c',
    input : files('dbus-example.c'),
    output : 'dbus-example-bindings.ci',
    command : [python3, files('../dbus-codegen.py'),
               files('../dbus-analyze-functions.py'),
               '--export-only', '--', '@INPUT@'],
    capture : true,
  )
  shared_library('dbus-example',
    files('dbus-example.c'),
    include_directories : plugin_inc,
    c_args : plugin_cflags,
    dependencies : [libpurple_dep, glib_dep, dbus_dep, dbus_glib_dep,
                    declare_dependency(sources : dbus_example_bindings)],
    name_prefix : '',
    install : true,
    install_dir : purple_plugindir,
  )
endif

# --- scripting-language plugin loaders -------------------------------------
if have_perl
  subdir('perl')
endif
if have_tcl
  subdir('tcl')
endif
if have_mono
  subdir('mono')
endif
