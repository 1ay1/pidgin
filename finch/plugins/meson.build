# finch/plugins — GNT UI plugins.

finch_plugin_cflags = common_cflags + [
  '-DDATADIR="@0@"'.format(abs_datadir),
]
finch_plugin_inc = [
  include_directories('..', '../../libpurple'),
  config_h_include,
]

# name -> {sources, extra deps}
finch_plugins = {
  'gntclipboard' : {'src' : ['gntclipboard.c'], 'x11' : true},
  'gntgf'        : {'src' : ['gntgf.c'],        'x11' : true},
  'gnthistory'   : {'src' : ['gnthistory.c']},
  'gntlastlog'   : {'src' : ['lastlog.c']},
  'gnttinyurl'   : {'src' : ['gnttinyurl.c']},
  'grouping'     : {'src' : ['grouping.c']},
}

foreach name, spec : finch_plugins
  deps = [finch_dep, gnt_dep, glib_dep]
  if spec.get('x11', false) and have_x11
    deps += x11_dep
  endif
  shared_library(name,
    files(spec['src']),
    include_directories : finch_plugin_inc,
    c_args : finch_plugin_cflags + (spec.get('x11', false) and have_x11 ? ['-DHAVE_X11'] : []),
    dependencies : deps,
    name_prefix : '',
    override_options : ['b_lundef=false'],
    install : true,
    install_dir : finch_plugindir,
  )
endforeach
