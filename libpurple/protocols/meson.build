# libpurple/protocols — dynamically loaded protocol (prpl) plugins.

enabled_protocols = get_option('protocols')

prpl_install_dir = purple_plugindir

prpl_cflags = common_cflags
prpl_inc = [include_directories('..'), config_h_include]

foreach proto : ['irc', 'jabber', 'simple', 'bonjour', 'null', 'acp']
  if proto in enabled_protocols
    if proto == 'bonjour' and not have_avahi
      message('bonjour requested but Avahi not found — skipping')
    elif proto == 'acp' and not have_json_glib
      message('acp requested but json-glib not found — skipping')
    else
      subdir(proto)
    endif
  endif
endforeach
