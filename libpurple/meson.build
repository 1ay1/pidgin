# ---------------------------------------------------------------------------
# libpurple
# ---------------------------------------------------------------------------

purple_inc = include_directories('.', 'media')

# --- generated version.h / purple.h from templates -------------------------
version_h = configure_file(
  input : 'version.h.in',
  output : 'version.h',
  configuration : version_conf,
)

# purple.h.in only substitutes plugin/version bits; feed it the same data.
purple_h = configure_file(
  input : 'purple.h.in',
  output : 'purple.h',
  configuration : version_conf,
)

# --- GLib marshallers ------------------------------------------------------
marshallers_h = custom_target('marshallers.h',
  input : 'marshallers.list',
  output : 'marshallers.h',
  command : [glib_genmarshal, '--prefix=purple_smarshal', '--header', '@INPUT@'],
  capture : true,
)

marshallers_c = custom_target('marshallers.c',
  input : 'marshallers.list',
  output : 'marshallers.c',
  command : [glib_genmarshal, '--prefix=purple_smarshal', '--body', '@INPUT@'],
  capture : true,
)

# ---------------------------------------------------------------------------
# ciphers (static convenience library)
# ---------------------------------------------------------------------------
subdir('ciphers')

# ---------------------------------------------------------------------------
# core sources
# ---------------------------------------------------------------------------
purple_coresources = files(
  'account.c',
  'accountopt.c',
  'blist.c',
  'buddyicon.c',
  'certificate.c',
  'cipher.c',
  'circbuffer.c',
  'cmds.c',
  'connection.c',
  'connhealth.c',
  'conversation.c',
  'core.c',
  'debug.c',
  'desktopitem.c',
  'eventloop.c',
  'ft.c',
  'idle.c',
  'imgstore.c',
  'log.c',
  'media/backend-fs2.c',
  'media/backend-iface.c',
  'media/candidate.c',
  'media/codec.c',
  'media/enum-types.c',
  'media.c',
  'mediamanager.c',
  'mime.c',
  'msgqueue.c',
  'nat-pmp.c',
  'network.c',
  'ntlm.c',
  'notify.c',
  'plugin.c',
  'pluginpref.c',
  'pounce.c',
  'prefs.c',
  'privacy.c',
  'protocols.c',
  'proxy.c',
  'prpl.c',
  'ratelimit.c',
  'reconnect.c',
  'request.c',
  'roomlist.c',
  'savedstatuses.c',
  'server.c',
  'signals.c',
  'smiley.c',
  'dnsquery.c',
  'dnssrv.c',
  'status.c',
  'stringref.c',
  'stun.c',
  'sound.c',
  'sound-theme.c',
  'sound-theme-loader.c',
  'sslconn.c',
  'theme.c',
  'theme-loader.c',
  'theme-manager.c',
  'upnp.c',
  'util.c',
  'value.c',
  'version.c',
  'xmlnode.c',
  'whiteboard.c',
)

purple_built = [marshallers_c, marshallers_h, version_h, purple_h]

# ---------------------------------------------------------------------------
# D-Bus server + generated bindings
# ---------------------------------------------------------------------------
dbus_sources = []
dbus_built = []
dbus_deps = []
dbus_lib_only_deps = []

if have_dbus
  codegen = files('dbus-codegen.py')
  analyze_types = files('dbus-analyze-types.py')
  analyze_funcs = files('dbus-analyze-functions.py')
  analyze_signals = files('dbus-analyze-signals.py')

  # Core headers consumed by the type/function analyzers.
  dbus_coreheaders = files(
    'account.h', 'accountopt.h', 'blist.h', 'buddyicon.h', 'certificate.h',
    'cipher.h', 'circbuffer.h', 'cmds.h', 'connection.h', 'connhealth.h',
    'conversation.h',
    'core.h', 'dbus-maybe.h', 'debug.h', 'desktopitem.h', 'eventloop.h',
    'ft.h', 'gaim-compat.h', 'glibcompat.h', 'idle.h', 'imgstore.h', 'log.h',
    'media.h', 'media-gst.h', 'mediamanager.h', 'mime.h', 'msgqueue.h',
    'nat-pmp.h',
    'network.h', 'notify.h', 'ntlm.h', 'plugin.h', 'pluginpref.h', 'pounce.h',
    'prefs.h', 'privacy.h', 'protocols.h', 'proxy.h', 'prpl.h', 'ratelimit.h', 'reconnect.h', 'request.h', 'roomlist.h',
    'savedstatuses.h', 'server.h', 'signals.h', 'smiley.h', 'dnsquery.h',
    'dnssrv.h', 'status.h', 'stringref.h', 'stun.h', 'sound.h',
    'sound-theme.h', 'sound-theme-loader.h', 'sslconn.h', 'theme.h',
    'theme-loader.h', 'theme-manager.h', 'upnp.h', 'util.h', 'value.h',
    'xmlnode.h', 'whiteboard.h',
    'media/backend-iface.h', 'media/candidate.h', 'media/codec.h',
    'media/enum-types.h',
  )

  # Exported headers for the dbus function bindings.
  dbus_exported = files(
    'dbus-useful.h', 'dbus-define-api.h', 'account.h', 'blist.h',
    'buddyicon.h', 'connection.h', 'conversation.h', 'core.h', 'ft.h',
    'log.h', 'notify.h', 'prefs.h', 'roomlist.h', 'savedstatuses.h',
    'smiley.h', 'status.h', 'server.h', 'util.h', 'xmlnode.h', 'prpl.h',
  )

  # Headers whose enums are extracted verbatim into the client bindings.
  dbus_all_headers = dbus_coreheaders + [version_h, purple_h]

  dbus_types_h = custom_target('dbus-types.h',
    input : dbus_all_headers,
    output : 'dbus-types.h',
    command : [python3, codegen, analyze_types, '--pattern=PURPLE_DBUS_DECLARE_TYPE(%s)', '--', '@INPUT@'],
    capture : true,
  )
  dbus_types_c = custom_target('dbus-types.c',
    input : dbus_all_headers,
    output : 'dbus-types.ci',
    command : [python3, codegen, analyze_types, '--pattern=PURPLE_DBUS_DEFINE_TYPE(%s)', '--', '@INPUT@'],
    capture : true,
  )
  dbus_bindings_c = custom_target('dbus-bindings.c',
    input : dbus_exported,
    output : 'dbus-bindings.ci',
    command : [python3, codegen, analyze_funcs, '--', '@INPUT@'],
    capture : true,
  )

  # dbus-signals.c reads all core sources + a couple of prpl sources.
  dbus_signal_inputs = purple_coresources + files(
    'protocols/irc/irc.c',
    'protocols/jabber/libxmpp.c',
  )
  dbus_signals_c = custom_target('dbus-signals.c',
    input : dbus_signal_inputs,
    output : 'dbus-signals.ci',
    command : [python3, codegen, analyze_signals, '--', '@INPUT@'],
    capture : true,
  )

  dbus_sources = files('dbus-server.c', 'dbus-useful.c')
  # dbus-server.c #includes the generated .c files, so they must be generated
  # (tracked as sources) but NOT compiled independently. Only dbus-types.h is
  # a standalone header. We attach the include-only .c targets to dbus-server
  # via an explicit dependency so ninja builds them first.
  dbus_built = [dbus_types_h]
  dbus_gen_includes = [dbus_types_c, dbus_bindings_c, dbus_signals_c]
  # A sources-only dependency enforces generation ordering without compiling
  # the #included .c files as standalone translation units.
  dbus_gen_dep = declare_dependency(sources : dbus_gen_includes)
  dbus_deps = [dbus_dep, dbus_glib_dep]
  dbus_lib_only_deps = [dbus_gen_dep]
endif

purple_cflags = common_cflags + [
  '-DDATADIR="@0@"'.format(abs_datadir),
  '-DLIBDIR="@0@"'.format(purple_plugindir),
  '-DLOCALEDIR="@0@"'.format(abs_localedir),
  '-DSYSCONFDIR="@0@"'.format(abs_sysconfdir),
]

purple_deps = glib_deps + [libxml_dep, libm_dep] + dbus_deps
if have_gstreamer
  purple_deps += gst_dep
endif
if have_nm
  purple_deps += [nm_dep, dbusglib_dep]
endif
if have_idn
  purple_deps += idn_dep
endif
if gio_unix_dep.found()
  purple_deps += gio_unix_dep
endif

libpurple = library('purple',
  purple_coresources, purple_built, dbus_sources, dbus_built,
  include_directories : [purple_inc, config_h_include],
  c_args : purple_cflags,
  dependencies : purple_deps + dbus_lib_only_deps,
  link_with : libpurple_ciphers,
  version : '@0@.0.0'.format(purple_lt_current),
  install : true,
)

libpurple_dep_dbus_gen = have_dbus ? [dbus_types_h] : []
libpurple_dep = declare_dependency(
  link_with : libpurple,
  include_directories : [purple_inc, config_h_include],
  dependencies : glib_deps + dbus_deps,
  sources : [version_h, purple_h, marshallers_h] + libpurple_dep_dbus_gen,
)

# public headers
install_headers([
    'account.h', 'accountopt.h', 'blist.h', 'buddyicon.h', 'certificate.h',
    'cipher.h', 'circbuffer.h', 'cmds.h', 'connection.h', 'connhealth.h',
    'conversation.h',
    'core.h', 'dbus-maybe.h', 'debug.h', 'desktopitem.h', 'eventloop.h',
    'ft.h', 'gaim-compat.h', 'glibcompat.h', 'idle.h', 'imgstore.h', 'log.h',
    'media.h', 'media-gst.h', 'mediamanager.h', 'mime.h', 'msgqueue.h',
    'nat-pmp.h',
    'network.h', 'notify.h', 'ntlm.h', 'plugin.h', 'pluginpref.h', 'pounce.h',
    'prefs.h', 'privacy.h', 'protocols.h', 'proxy.h', 'prpl.h', 'ratelimit.h', 'reconnect.h', 'request.h', 'roomlist.h',
    'savedstatuses.h', 'server.h', 'signals.h', 'smiley.h', 'dnsquery.h',
    'dnssrv.h', 'status.h', 'stringref.h', 'stun.h', 'sound.h',
    'sound-theme.h', 'sound-theme-loader.h', 'sslconn.h', 'theme.h',
    'theme-loader.h', 'theme-manager.h', 'upnp.h', 'util.h', 'value.h',
    'xmlnode.h', 'whiteboard.h',
  ],
  subdir : 'libpurple',
)
install_headers(version_h, purple_h, subdir : 'libpurple')
install_headers([
    'media/backend-iface.h', 'media/candidate.h', 'media/codec.h',
    'media/enum-types.h',
  ],
  subdir : 'libpurple/media',
)

# pkg-config
pkgconf.generate(libpurple,
  name : 'libpurple',
  description : 'libpurple is a GLib-based instant messenger library.',
  version : meson.project_version(),
  requires : ['glib-2.0'],
  subdirs : 'libpurple',
  variables : [
    'plugindir=@0@'.format(purple_plugindir),
  ],
)

# ---------------------------------------------------------------------------
# D-Bus client library, service file, helper scripts
# ---------------------------------------------------------------------------
if have_dbus
  # purple-client-bindings.{c,h} for the thin client library.
  client_bindings_c = custom_target('purple-client-bindings.c',
    input : dbus_exported,
    output : 'purple-client-bindings.ci',
    command : [python3, codegen, analyze_funcs, '--client', '--', '@INPUT@'],
    capture : true,
  )
  client_bindings_h = custom_target('purple-client-bindings.h',
    input : dbus_all_headers + dbus_exported,
    output : 'purple-client-bindings.h',
    command : [python3, files('gen-client-bindings-h.py'),
               analyze_types, analyze_funcs,
               '@OUTPUT@', '@PRIVATE_DIR@',
               dbus_all_headers.length().to_string(), '@INPUT@'],
  )

  # purple-client.c #includes purple-client-bindings.ci; the .h is a real
  # header. Order generation via a sources-only dependency.
  client_gen_dep = declare_dependency(sources : [client_bindings_c, client_bindings_h])

  libpurple_client = library('purple-client',
    files('purple-client.c'),
    include_directories : [purple_inc, config_h_include],
    c_args : common_cflags,
    dependencies : [glib_dep, dbus_dep, dbus_glib_dep, client_gen_dep],
    version : '@0@.0.0'.format(purple_lt_current),
    install : true,
  )

  # D-Bus session service activation file.
  dbus_service_conf = configuration_data()
  dbus_service_conf.set('bindir', prefix / get_option('bindir'))
  configure_file(
    input : 'purple.service.in',
    output : 'im.pidgin.purple.PurpleService.service',
    configuration : dbus_service_conf,
    install : true,
    install_dir : abs_datadir / 'dbus-1' / 'services',
  )

  # purple-client-example
  executable('purple-client-example',
    files('purple-client-example.c'),
    include_directories : [purple_inc, config_h_include],
    c_args : common_cflags,
    dependencies : [glib_dep, dbus_dep, dbus_glib_dep, client_gen_dep],
    link_with : libpurple_client,
    install : true,
  )

  # Helper scripts (installed to bindir).
  install_data(
    files('purple-remote', 'purple-send', 'purple-send-async', 'purple-url-handler'),
    install_dir : get_option('bindir'),
    install_mode : 'rwxr-xr-x',
  )
endif

# ---------------------------------------------------------------------------
# protocol plugins & bundled plugins
# ---------------------------------------------------------------------------
subdir('protocols')

if get_option('plugins')
  subdir('plugins')
endif
