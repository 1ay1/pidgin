acp_sources = files('acp.c', 'acp_rpc.c', 'acp_render.c', 'acp_stream.c')

# acp_stream.c drives the GtkIMHtml widget directly for live in-place Markdown
# streaming, so the plugin links GTK + Pidgin UI headers. Pidgin exports the
# gtk_imhtml_* / PidginConversation symbols to plugins loaded in its process.
acp_inc = [prpl_inc, include_directories('../../../pidgin')]

libacp = shared_library('acp',
  acp_sources,
  include_directories : acp_inc,
  c_args : prpl_cflags,
  dependencies : [libpurple_dep, glib_dep, json_glib_dep, gtk_dep],
  name_prefix : 'lib',
  # acp_stream.c calls gtk_imhtml_* / PidginConversation which are exported by
  # the pidgin executable at runtime (not a linkable .so), so allow undefined
  # symbols at link time -- exactly like pidgin's own GTK plugins.
  override_options : ['b_lundef=false'],
  install : true,
  install_dir : prpl_install_dir,
)
