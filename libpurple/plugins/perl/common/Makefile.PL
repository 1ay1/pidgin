use 5.006;
use ExtUtils::MakeMaker;
use Config;

# Static Makefile.PL for the Purple Perl XS module.
#
# Historically this file was generated from Makefile.PL.in by the Autotools
# configure script. The Autotools build has been removed; the Meson build
# copies this directory to a work tree and runs `perl Makefile.PL` there
# (see ../build-perl-module.sh), so every path is resolved relative to the
# current directory and GLib flags are queried from pkg-config at run time.

# Locate the top of the source tree (…/libpurple/plugins/perl/common → repo).
use Cwd qw(abs_path);
my $srcdir      = '.';
my $top_srcdir  = abs_path('../../../..');   # repository root
my $libpurple   = "$top_srcdir/libpurple";

# GLib compile flags via pkg-config (fall back to empty on failure).
my $glib_cflags = `pkg-config --cflags glib-2.0 2>/dev/null`;
chomp $glib_cflags;

# Allow the caller (Meson install script) to override the install prefix.
my $prefix = $ENV{PREFIX} || '/usr/local';

WriteMakefile(
    'NAME'              => 'Purple',
    'VERSION_FROM'      => "$srcdir/Purple.pm", # finds $VERSION
    'PREREQ_PM'         => {},
    ($] >= 5.005 ?
      (ABSTRACT_FROM    => "$srcdir/Purple.pm", # finds $ABSTRACT
       AUTHOR           => 'Purple <http://pidgin.im/>') : ()),
    'DEFINE'            => '',
    'INC'               => "-I. -I$srcdir -I$top_srcdir -I$libpurple $glib_cflags",
    'OBJECT'            => '$(O_FILES)', # link all the C files too
    'INSTALLDIRS'          => 'vendor',
    'INSTALL_BASE'         => $prefix,
    'INSTALLVENDORARCH'    => "$prefix/lib/purple-2/perl",
    'INSTALLVENDORMAN3DIR' => "$prefix/share/man/man3",
);
