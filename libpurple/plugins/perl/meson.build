# Perl plugin loader.
#
# Two pieces:
#   1. perl.so  — the C loader that embeds a Perl interpreter.
#   2. Purple   — the XS bindings module (built via its own Makefile.PL, the
#                 canonical way to compile Perl XS extensions).

shared_library('perl',
  files(
    'perl.c',
    'perl-common.c',
    'perl-handlers.c',
  ),
  include_directories : [include_directories('..', '../..', '../../../'), config_h_include],
  # Our config.h must be found before Perl's CORE/config.h, so force the build
  # root onto the include path ahead of the perl_cflags -I entries.
  c_args : ['-I' + meson.project_build_root()] + common_cflags + perl_cflags + [
    '-DLIBDIR="@0@"'.format(purple_plugindir),
  ],
  link_args : perl_libs,
  dependencies : [libpurple_dep, glib_dep],
  name_prefix : '',
  install : true,
  install_dir : purple_plugindir,
)

# The Purple XS module is compiled + installed by ExtUtils::MakeMaker. We drive
# it with a wrapper so Meson tracks it as a build+install step.
message('Perl XS bindings (common/Purple) are built via Makefile.PL at install time')

meson.add_install_script('build-perl-module.sh',
  perl_prog.full_path(),
  meson.current_source_dir() / 'common',
)
