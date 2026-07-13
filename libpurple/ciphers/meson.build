# libpurple/ciphers — static convenience library linked into libpurple.

libpurple_ciphers = static_library('purple-ciphers',
  files(
    'des.c',
    'gchecksum.c',
    'hmac.c',
    'md4.c',
    'md5.c',
    'rc4.c',
    'sha1.c',
    'sha256.c',
  ),
  include_directories : [include_directories('..'), config_h_include],
  c_args : common_cflags,
  dependencies : [glib_dep],
  pic : true,
  install : false,
)
