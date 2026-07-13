jabber_sources = files(
  'adhoccommands.c',
  'auth.c',
  'auth_digest_md5.c',
  'auth_plain.c',
  'auth_scram.c',
  'buddy.c',
  'bosh.c',
  'caps.c',
  'chat.c',
  'data.c',
  'disco.c',
  'google/gmail.c',
  'google/google.c',
  'google/google_presence.c',
  'google/google_roster.c',
  'google/google_session.c',
  'google/jingleinfo.c',
  'google/relay.c',
  'ibb.c',
  'iq.c',
  'jabber.c',
  'jingle/jingle.c',
  'jingle/content.c',
  'jingle/iceudp.c',
  'jingle/rawudp.c',
  'jingle/rtp.c',
  'jingle/session.c',
  'jingle/transport.c',
  'jutil.c',
  'message.c',
  'oob.c',
  'parser.c',
  'pep.c',
  'ping.c',
  'presence.c',
  'roster.c',
  'si.c',
  'stream_management.c',
  'useravatar.c',
  'usermood.c',
  'usernick.c',
  'usertune.c',
  'xdata.c',
)

jabber_deps = [libpurple_dep, glib_dep, libxml_dep]
if have_sasl
  jabber_sources += files('auth_cyrus.c')
  jabber_deps += sasl_dep
endif
if have_idn
  jabber_deps += idn_dep
endif
if have_gstreamer
  jabber_deps += gst_dep
endif

# libjabber holds the implementation; libxmpp is the loadable plugin wrapper.
libjabber = shared_library('jabber',
  jabber_sources,
  include_directories : prpl_inc,
  c_args : prpl_cflags,
  dependencies : jabber_deps,
  name_prefix : 'lib',
  install : true,
  install_dir : prpl_install_dir,
)

libxmpp = shared_library('xmpp',
  files('libxmpp.c'),
  include_directories : prpl_inc,
  c_args : prpl_cflags,
  dependencies : [libpurple_dep, glib_dep, libxml_dep],
  link_with : libjabber,
  name_prefix : 'lib',
  install : true,
  install_dir : prpl_install_dir,
)
