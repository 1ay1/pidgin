# ---------------------------------------------------------------------------
# Pidgin — GTK+2 client
# ---------------------------------------------------------------------------

pidgin_sources = files(
  'pidginstock.c',
  'gtkaccount.c',
  'gtkblist.c',
  'gtkblist-theme.c',
  'gtkblist-theme-loader.c',
  'gtkcellrendererexpander.c',
  'gtkcertmgr.c',
  'gtkconn.c',
  'gtkconv.c',
  'gtkdebug.c',
  'gtkdialogs.c',
  'gtkdnd-hints.c',
  'gtkdocklet.c',
  'gtkdocklet-gtk.c',
  'gtkeventloop.c',
  'gtkft.c',
  'gtkicon-theme.c',
  'gtkicon-theme-loader.c',
  'gtkidle.c',
  'gtkimhtml.c',
  'gtkimhtmltoolbar.c',
  'gtklog.c',
  'gtkmain.c',
  'gtkmedia.c',
  'gtkmenutray.c',
  'gtknotify.c',
  'gtkplugin.c',
  'gtkpluginpref.c',
  'gtkpounce.c',
  'gtkprefs.c',
  'gtkprivacy.c',
  'gtkrequest.c',
  'gtkroomlist.c',
  'gtksavedstatuses.c',
  'gtkscrollbook.c',
  'gtksession.c',
  'gtksmiley.c',
  'gtksound.c',
  'gtksourceiter.c',
  'gtksourceundomanager.c',
  'gtksourceview-marshal.c',
  'gtkstatus-icon-theme.c',
  'gtkstatusbox.c',
  'gtkstyle.c',
  'gtkthemes.c',
  'gtkutils.c',
  'gtkwhiteboard.c',
  'minidialog.c',
  'pidgintooltip.c',
)

pidgin_cflags = common_cflags + [
  '-DDATADIR="@0@"'.format(abs_datadir),
  '-DLIBDIR="@0@"'.format(pidgin_plugindir),
  '-DLOCALEDIR="@0@"'.format(abs_localedir),
  '-DSYSCONFDIR="@0@"'.format(abs_sysconfdir),
]

pidgin_deps = [libpurple_dep, gtk_dep, libxml_dep, libm_dep] + glib_deps
if have_gstreamer
  pidgin_deps += gst_dep
endif
if have_gtkspell
  pidgin_deps += gtkspell_dep
endif
if have_x11
  pidgin_deps += x11_dep
endif
if gio_unix_dep.found()
  pidgin_deps += gio_unix_dep
endif

pidgin = executable('pidgin',
  pidgin_sources, package_revision_h,
  include_directories : [include_directories('.'), config_h_include],
  c_args : pidgin_cflags,
  dependencies : pidgin_deps,
  export_dynamic : true,
  install : true,
)

install_headers([
    'gtkaccount.h', 'gtkblist.h', 'gtkblist-theme.h', 'gtkblist-theme-loader.h',
    'gtkcellrendererexpander.h', 'gtkcertmgr.h', 'gtkconn.h', 'gtkconv.h',
    'gtkconvwin.h', 'gtkdebug.h', 'gtkdialogs.h', 'gtkdnd-hints.h',
    'gtkdocklet.h', 'gtkeventloop.h', 'gtkft.h', 'gtkicon-theme.h',
    'gtkicon-theme-loader.h', 'gtkidle.h', 'gtkgaim-compat.h', 'gtkimhtml.h',
    'gtkimhtmltoolbar.h', 'gtklog.h', 'gtkmedia.h', 'gtkmenutray.h',
    'gtknickcolors.h', 'gtknotify.h', 'gtkplugin.h', 'gtkpluginpref.h',
    'gtkprefs.h', 'gtkprivacy.h', 'gtkpounce.h', 'gtkrequest.h',
    'gtkroomlist.h', 'gtksavedstatuses.h', 'gtkscrollbook.h', 'gtksession.h',
    'gtksmiley.h', 'gtksound.h', 'gtksourceiter.h', 'gtksourceundomanager.h',
    'gtksourceview-marshal.h', 'gtkstatus-icon-theme.h', 'gtkstatusbox.h',
    'gtkstyle.h', 'pidginstock.h', 'gtkthemes.h', 'gtkutils.h',
    'gtkwhiteboard.h', 'minidialog.h', 'pidgintooltip.h', 'pidgin.h',
  ],
  subdir : 'pidgin',
)

# expose the pidgin build dep for its own plugins
pidgin_dep = declare_dependency(
  include_directories : [include_directories('.'), config_h_include],
  dependencies : [libpurple_dep, gtk_dep],
)

subdir('pixmaps')
subdir('data')
if get_option('plugins')
  subdir('plugins')
endif
