# pidgin/plugins — GTK UI plugins.

pidgin_plugin_cflags = common_cflags + [
  '-DDATADIR="@0@"'.format(abs_datadir),
]
pidgin_plugin_inc = [
  include_directories('..', '../../libpurple'),
  config_h_include,
]

# Simple single-file (or fixed multi-file) GTK plugins.
pidgin_plugins = {
  'autoreply'        : ['autoreply.c'],
  'convcolors'       : ['convcolors.c'],
  'extplacement'     : ['extplacement.c'],
  'gtkbuddynote'     : ['gtkbuddynote.c'],
  'history'          : ['history.c'],
  'iconaway'         : ['iconaway.c'],
  'keywordhighlight' : ['highlight.c'],
  'lastseen'         : ['lastseen.c'],
  'messagesplitter'  : ['messagesplitter.c'],
  'markerline'       : ['markerline.c'],
  'notify'           : ['notify.c'],
  'pidginrc'         : ['pidginrc.c'],
  'quickreplies'     : ['quickreplies.c'],
  'relnot'           : ['relnot.c'],
  'sendbutton'       : ['sendbutton.c'],
  'spellchk'         : ['spellchk.c'],
  'themeedit'        : ['themeedit.c', 'themeedit-icon.c'],
  'timestamp'        : ['timestamp.c'],
  'timestamp_format' : ['timestamp_format.c'],
  'transparency'     : ['transparency.c'],
  'xmppconsole'      : ['xmppconsole.c'],
}

foreach name, src : pidgin_plugins
  shared_library(name,
    files(src),
    include_directories : pidgin_plugin_inc,
    c_args : pidgin_plugin_cflags,
    dependencies : [pidgin_dep, gtk_dep],
    name_prefix : '',
    override_options : ['b_lundef=false'],
    install : true,
    install_dir : pidgin_plugindir,
  )
endforeach

# vvconfig needs GStreamer / voice-video.
if have_gstreamer
  shared_library('vvconfig',
    files('vvconfig.c'),
    include_directories : pidgin_plugin_inc,
    c_args : pidgin_plugin_cflags,
    dependencies : [pidgin_dep, gtk_dep, gst_dep],
    name_prefix : '',
    override_options : ['b_lundef=false'],
    install : true,
    install_dir : pidgin_plugindir,
  )
endif

# unity messaging-menu integration.
if have_unity
  shared_library('unity',
    files('unity.c'),
    include_directories : pidgin_plugin_inc,
    c_args : pidgin_plugin_cflags,
    dependencies : [pidgin_dep, gtk_dep, unity_dep, messaging_dep],
    name_prefix : '',
    override_options : ['b_lundef=false'],
    install : true,
    install_dir : pidgin_plugindir,
  )
endif

# --- plugin subdirectories -------------------------------------------------
subdir('disco')
subdir('ticker')
subdir('gestures')

if have_sqlite3
  subdir('cap')
endif
if have_dbus
  subdir('musicmessaging')
endif
if have_evolution
  subdir('gevolution')
endif
