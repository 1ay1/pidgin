project('pidgin', 'c',
  version : '2.15.0dev',
  license : 'GPL-2.0-or-later',
  meson_version : '>=0.56.0',
  default_options : [
    'c_std=gnu11',
    'warning_level=1',
    'buildtype=debugoptimized',
  ],
)

# ---------------------------------------------------------------------------
# Versioning (mirrors configure.ac m4_define block)
# ---------------------------------------------------------------------------
version_arr = meson.project_version().split('.')
purple_major_version = version_arr[0].to_int()
purple_minor_version = version_arr[1].to_int()
# micro carries the "dev" suffix in the project version string; strip it.
_micro_raw = version_arr[2]
_micro_digits = ''
foreach ch : ['0','1','2','3','4','5','6','7','8','9']
  if _micro_raw.startswith(ch)
    _micro_digits = ch
  endif
endforeach
purple_micro_version = _micro_digits == '' ? 0 : _micro_raw.substring(0, 1).to_int()

# libtool -version-info current for the shared libraries (see configure.ac).
purple_lt_current = 15
purple_soversion = '@0@.0.@1@'.format(purple_lt_current - purple_lt_current, purple_lt_current)

# ---------------------------------------------------------------------------
# Compiler / platform basics
# ---------------------------------------------------------------------------
cc = meson.get_compiler('c')
host_os = host_machine.system()

add_project_arguments('-DHAVE_CONFIG_H=1', language : 'c')

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
prefix     = get_option('prefix')
libdir     = get_option('libdir')
datadir    = get_option('datadir')
sysconfdir = get_option('sysconfdir')
localedir  = get_option('localedir')

abs_libdir     = prefix / libdir
abs_datadir    = prefix / datadir
abs_sysconfdir = prefix / sysconfdir
abs_localedir  = prefix / localedir

purple_plugindir = abs_libdir / 'purple-@0@'.format(purple_major_version)
pidgin_plugindir = abs_libdir / 'pidgin'
finch_plugindir  = abs_libdir / 'finch'

# ---------------------------------------------------------------------------
# Mandatory dependencies
# ---------------------------------------------------------------------------
glib_dep    = dependency('glib-2.0', version : '>=2.28.0')
gobject_dep = dependency('gobject-2.0')
gmodule_dep = dependency('gmodule-2.0')
gthread_dep = dependency('gthread-2.0')
gio_dep     = dependency('gio-2.0')
libxml_dep  = dependency('libxml-2.0', version : '>=2.6.0')
libm_dep    = cc.find_library('m', required : false)

glib_deps = [glib_dep, gobject_dep, gmodule_dep, gthread_dep, gio_dep]

# gio-unix (optional, for unix socket helpers)
gio_unix_dep = dependency('gio-unix-2.0', required : false)

glib_genmarshal = find_program('glib-genmarshal')
i18n = import('i18n')
pkgconf = import('pkgconfig')

# ---------------------------------------------------------------------------
# Optional dependencies
# ---------------------------------------------------------------------------
# SSL / TLS
ssl_backend = get_option('ssl')
want_gnutls = ssl_backend in ['auto', 'gnutls']
want_nss    = ssl_backend in ['auto', 'nss']
gnutls_dep = want_gnutls ? dependency('gnutls', required : ssl_backend == 'gnutls') : dependency('', required : false)
nss_dep    = want_nss    ? dependency('nss',    required : ssl_backend == 'nss')    : dependency('', required : false)
have_gnutls = gnutls_dep.found()
have_nss    = nss_dep.found() and not have_gnutls
have_ssl    = have_gnutls or have_nss

# Cyrus SASL (for jabber / SASL auth)
sasl_dep = dependency('libsasl2', required : get_option('sasl'))
have_sasl = sasl_dep.found()

# GNU libidn (stringprep / IDN)
idn_dep = dependency('libidn', required : get_option('idn'))
have_idn = idn_dep.found()

# Avahi (bonjour)
avahi_client_dep = dependency('avahi-client', required : get_option('avahi'))
avahi_glib_dep   = dependency('avahi-glib',   required : get_option('avahi'))
have_avahi = avahi_client_dep.found() and avahi_glib_dep.found()

# GStreamer (sound / voice-video)
gst_dep = dependency('gstreamer-1.0', required : get_option('gstreamer'))
have_gstreamer = gst_dep.found()

# NetworkManager (2.x uses the legacy dbus-glib + libnm-util API)
nm_feat = get_option('networkmanager')
dbusglib_dep = dependency('dbus-glib-1', required : false)
nm_dep = dependency('libnm', required : false)
have_nm = nm_feat.enabled() or (nm_feat.auto() and nm_dep.found() and dbusglib_dep.found())
have_nm = have_nm and nm_dep.found() and dbusglib_dep.found()

# GTK+ 2 (Pidgin)
gtk_dep = dependency('gtk+-2.0', version : '>=2.14.0', required : get_option('gtk'))
have_gtk = gtk_dep.found()

# GtkSpell (spell checking in Pidgin)
gtkspell_dep = dependency('gtkspell-2.0', required : get_option('gtkspell'))
have_gtkspell = gtkspell_dep.found() and have_gtk

# X11 (screensaver / session mgmt in Pidgin)
x11_dep = dependency('x11', required : false)
have_x11 = x11_dep.found() and have_gtk

# libgnt (Finch)
gnt_dep = dependency('gnt', required : get_option('gnt'))
have_gnt = gnt_dep.found()

# D-Bus
dbus_dep      = dependency('dbus-1',      version : '>=0.90', required : get_option('dbus'))
dbus_glib_dep = dependency('dbus-glib-1',  version : '>=0.70', required : get_option('dbus'))
have_dbus = dbus_dep.found() and dbus_glib_dep.found()
python3 = import('python').find_installation('python3', required : have_dbus)

# Perl plugin loader
perl_prog = find_program('perl', required : get_option('perl'))
have_perl = false
perl_cflags = []
perl_libs = []
perl_archlib = ''
if perl_prog.found()
  perl_ccopts = run_command(perl_prog, '-MExtUtils::Embed', '-e', 'ccopts', check : false)
  perl_ldopts = run_command(perl_prog, '-MExtUtils::Embed', '-e', 'ldopts', check : false)
  if perl_ccopts.returncode() == 0 and perl_ldopts.returncode() == 0
    perl_cflags = perl_ccopts.stdout().strip().split()
    _perl_ld = perl_ldopts.stdout().strip().split()
    perl_libs = []
    foreach a : _perl_ld
      if not a.startswith('-Wl,-rpath') and a != '-Wl,-rpath'
        perl_libs += a
      endif
    endforeach
    perl_archlib = run_command(perl_prog, '-MConfig', '-e', 'print $Config{installsitearch}', check : false).stdout().strip()
    if cc.has_function('perl_run', args : perl_cflags + perl_libs, prefix : '#include <EXTERN.h>\n#include <perl.h>')
      have_perl = true
      conf_perl = true
    endif
  endif
endif
if get_option('perl').enabled() and not have_perl
  error('Perl loader requested but a usable libperl was not found')
endif

# Tcl / Tk plugin loader
tcl_dep = dependency('tcl', required : false)
if not tcl_dep.found()
  tcl_dep = cc.find_library('tcl', required : get_option('tcl'))
endif
have_tcl = tcl_dep.found()
tk_dep = dependency('tk', required : false)
if not tk_dep.found()
  tk_dep = cc.find_library('tk', required : false)
endif
have_tk = tk_dep.found() and have_tcl and not get_option('tk').disabled()

# Mono plugin loader
mono_dep = dependency('mono-2', required : get_option('mono'))
mcs_prog = find_program('mcs', 'csc', required : get_option('mono'))
have_mono = mono_dep.found() and mcs_prog.found()

# SQLite3 (CAP plugin)
sqlite3_dep = dependency('sqlite3', required : get_option('sqlite3'))
have_sqlite3 = sqlite3_dep.found()

# Unity / Evolution (optional Pidgin plugins)
unity_dep     = dependency('unity',       required : get_option('unity-integration'))
messaging_dep = dependency('messaging-menu', required : get_option('unity-integration'))
have_unity = unity_dep.found() and messaging_dep.found()
evo_dep = dependency('libebook-1.2', required : get_option('evolution'))
have_evolution = evo_dep.found()

# gettext / i18n
have_nls = get_option('nls')
if have_nls
  intl_dep = cc.find_library('intl', required : false)
else
  intl_dep = dependency('', required : false)
endif

# ---------------------------------------------------------------------------
# config.h
# ---------------------------------------------------------------------------
conf = configuration_data()

conf.set_quoted('PACKAGE', 'pidgin')
conf.set_quoted('PACKAGE_NAME', 'pidgin')
conf.set_quoted('VERSION', meson.project_version())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('DISPLAY_VERSION', meson.project_version())
conf.set_quoted('CONFIG_ARGS', 'meson')

conf.set('PURPLE_PLUGINS', get_option('plugins') ? 1 : false)
conf.set('DEBUG', get_option('debug') ? 1 : false)
conf.set('ENABLE_NLS', get_option('nls') ? 1 : false)

# Header checks
check_headers = [
  ['fcntl.h',                'HAVE_FCNTL_H'],
  ['sys/time.h',             'HAVE_SYS_TIME_H'],
  ['unistd.h',               'HAVE_UNISTD_H'],
  ['locale.h',               'HAVE_LOCALE_H'],
  ['signal.h',               'HAVE_SIGNAL_H'],
  ['stdint.h',               'HAVE_STDINT_H'],
  ['string.h',               'HAVE_STRING_H'],
  ['regex.h',                'HAVE_REGEX_H'],
  ['alloca.h',               'HAVE_ALLOCA_H'],
  ['endian.h',               'HAVE_ENDIAN_H'],
  ['langinfo.h',             'HAVE_LANGINFO_CODESET'],
  ['arpa/nameser_compat.h',  'HAVE_ARPA_NAMESER_COMPAT_H'],
  ['sys/param.h',            'HAVE_SYS_PARAM_H'],
  ['sys/socket.h',           'HAVE_SYS_SOCKET_H'],
  ['sys/sysctl.h',           'HAVE_SYS_SYSCTL_H'],
  ['sys/utsname.h',          'HAVE_SYS_UTSNAME_H'],
]
foreach h : check_headers
  if cc.has_header(h[0])
    conf.set(h[1], 1)
  endif
endforeach

# Function checks
check_funcs = [
  ['strdup',      'HAVE_STRDUP'],
  ['strstr',      'HAVE_STRSTR'],
  ['atexit',      'HAVE_ATEXIT'],
  ['setlocale',   'HAVE_SETLOCALE'],
  ['getaddrinfo', 'HAVE_GETADDRINFO'],
  ['inet_ntop',   'HAVE_INET_NTOP'],
  ['getifaddrs',  'HAVE_GETIFADDRS'],
  ['timegm',      'HAVE_TIMEGM'],
  ['gethostid',   'HAVE_GETHOSTID'],
]
foreach f : check_funcs
  if cc.has_function(f[0])
    conf.set(f[1], 1)
  endif
endforeach

# sizeof checks (AC_CHECK_SIZEOF)
conf.set('SIZEOF_TIME_T', cc.sizeof('time_t', prefix : '#include <time.h>'))

# struct tm.tm_gmtoff
if cc.has_member('struct tm', 'tm_gmtoff', prefix : '#include <time.h>')
  conf.set('HAVE_TM_GMTOFF', 1)
endif
if cc.has_member('struct tm', 'tm_zone', prefix : '#include <time.h>')
  conf.set('HAVE_STRUCT_TM_TM_ZONE', 1)
endif

# iconv
if cc.has_function('iconv') or cc.has_header('iconv.h')
  conf.set('HAVE_ICONV', 1)
endif

# Feature toggles into config.h
if have_ssl
  conf.set('HAVE_SSL', 1)
endif
if have_gnutls
  conf.set('HAVE_GNUTLS', 1)
endif
if have_nss
  conf.set('HAVE_NSS', 1)
endif
if have_sasl
  conf.set('HAVE_CYRUS_SASL', 1)
endif
if have_idn
  conf.set('USE_IDN', 1)
endif
if have_gstreamer
  conf.set('USE_GSTREAMER', 1)
endif
if have_nm
  conf.set('HAVE_NETWORKMANAGER', 1)
endif
if have_gtkspell
  conf.set('USE_GTKSPELL', 1)
endif
if have_x11
  conf.set('HAVE_X11', 1)
endif
if gio_unix_dep.found()
  conf.set('HAVE_GIOUNIX', 1)
endif
if have_dbus
  conf.set('HAVE_DBUS', 1)
endif
if have_perl
  conf.set('HAVE_PERL', 1)
  conf.set('HAVE_PERL_EVAL_PV', 1)
endif
if have_tcl
  conf.set('HAVE_TCL', 1)
endif
if have_tk
  conf.set('HAVE_TK', 1)
endif
if have_mono
  conf.set('ENABLE_MONO', 1)
endif
if have_evolution
  conf.set('HAVE_EVOLUTION_ADDRESSBOOK', 1)
endif

# Static protocol init (STATIC_PROTO_INIT). We build all prpls dynamically,
# so the static init function is a no-op.
conf.set('STATIC_PROTO_INIT', 'static void static_proto_init(void) { }')

configure_file(output : 'config.h', configuration : conf)

# package_revision.h — carries the VCS revision string (REVISION).
package_revision_h = vcs_tag(
  command : ['git', 'rev-parse', '--short', 'HEAD'],
  input : 'package_revision.h.in',
  output : 'package_revision.h',
  fallback : 'unknown',
  replace_string : '@VCS_TAG@',
)

config_h_include = include_directories('.')

# ---------------------------------------------------------------------------
# version.h and purple.h (generated from *.in templates)
# ---------------------------------------------------------------------------
version_conf = configuration_data()
version_conf.set('PURPLE_MAJOR_VERSION', purple_major_version)
version_conf.set('PURPLE_MINOR_VERSION', purple_minor_version)
version_conf.set('PURPLE_MICRO_VERSION', purple_micro_version)
version_conf.set('PLUGINS_DEFINE', get_option('plugins') ? '#define PURPLE_PLUGINS 1' : '')

# common cflags for everything
common_cflags = cc.get_supported_arguments([
  '-DHAVE_CONFIG_H',
  '-Wno-deprecated-declarations',
])

subdir('libpurple')

if have_gtk
  subdir('pidgin')
endif

if have_gnt
  subdir('finch')
endif

# data + i18n
subdir('share')
if have_nls
  subdir('po')
endif

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
summary({
  'prefix'        : prefix,
  'GTK Pidgin'    : have_gtk,
  'GNT Finch'     : have_gnt,
  'SSL (GnuTLS)'  : have_gnutls,
  'SSL (NSS)'     : have_nss,
  'Cyrus SASL'    : have_sasl,
  'GNU libidn'    : have_idn,
  'GStreamer'     : have_gstreamer,
  'Avahi/Bonjour' : have_avahi,
  'NetworkManager': have_nm,
  'GtkSpell'      : have_gtkspell,
  'Plugins'       : get_option('plugins'),
}, section : 'Core')

summary({
  'D-Bus'         : have_dbus,
  'Perl loader'   : have_perl,
  'Tcl loader'    : have_tcl,
  'Tk'            : have_tk,
  'Mono loader'   : have_mono,
  'SQLite3 (CAP)' : have_sqlite3,
  'Unity'         : have_unity,
  'Evolution'     : have_evolution,
  'NLS/gettext'   : have_nls,
}, section : 'Extras')
