# ---------------------------------------------------------------------------
# Finch — ncurses / libgnt client
# ---------------------------------------------------------------------------

finch_sources = files(
  'gntaccount.c',
  'gntblist.c',
  'gntcertmgr.c',
  'gntconn.c',
  'gntconv.c',
  'gntdebug.c',
  'gntft.c',
  'finch.c',
  'gntidle.c',
  'gntlog.c',
  'gntmedia.c',
  'gntnotify.c',
  'gntplugin.c',
  'gntpounce.c',
  'gntprefs.c',
  'gntrequest.c',
  'gntroomlist.c',
  'gntsound.c',
  'gntstatus.c',
  'gntui.c',
)

finch_cflags = common_cflags + [
  '-DSTANDALONE',
  '-DGNTSEAL_ENABLE',
  '-DDATADIR="@0@"'.format(abs_datadir),
  '-DLIBDIR="@0@"'.format(finch_plugindir),
  '-DLOCALEDIR="@0@"'.format(abs_localedir),
  '-DSYSCONFDIR="@0@"'.format(abs_sysconfdir),
]

finch_deps = [libpurple_dep, gnt_dep, libxml_dep] + glib_deps
ncursesw_dep = dependency('ncursesw', required : false)
if not ncursesw_dep.found()
  ncursesw_dep = cc.find_library('ncursesw', required : false)
endif
if ncursesw_dep.found()
  finch_deps += ncursesw_dep
endif
if have_gstreamer
  finch_deps += gst_dep
endif

finch = executable('finch',
  finch_sources, package_revision_h,
  include_directories : [include_directories('.'), config_h_include],
  c_args : finch_cflags,
  dependencies : finch_deps,
  export_dynamic : true,
  install : true,
)

install_headers([
    'gntaccount.h', 'gntblist.h', 'gntcertmgr.h', 'gntconn.h', 'gntconv.h',
    'gntdebug.h', 'gntft.h', 'finch.h', 'gntidle.h', 'gntlog.h', 'gntmedia.h',
    'gntnotify.h', 'gntplugin.h', 'gntpounce.h', 'gntprefs.h', 'gntrequest.h',
    'gntroomlist.h', 'gntsound.h', 'gntstatus.h', 'gntui.h',
  ],
  subdir : 'finch',
)

pkgconf.generate(
  name : 'Finch',
  description : 'Finch is an instant messenger application that uses libpurple for protocol support and ncurses (libgnt) for the UI.',
  version : meson.project_version(),
  requires : ['gnt', 'purple'],
  subdirs : 'finch',
)

finch_dep = declare_dependency(
  include_directories : [include_directories('.'), config_h_include],
  dependencies : [libpurple_dep, gnt_dep],
)

if get_option('plugins')
  subdir('plugins')
endif
