use 5.006;
use ExtUtils::MakeMaker;

# Static Makefile.PL for the Pidgin (GTK UI) Perl XS module.
#
# Previously generated from Makefile.PL.in by Autotools configure. The
# Autotools build has been removed; paths resolve relative to this directory
# and GTK flags come from pkg-config at run time.

use Cwd qw(abs_path);
my $srcdir     = '.';
my $top_srcdir = abs_path('../../../..');   # repository root

my $gtk_cflags = `pkg-config --cflags gtk+-2.0 2>/dev/null`;
chomp $gtk_cflags;

my $prefix = $ENV{PREFIX} || '/usr/local';

WriteMakefile(
    'NAME'             => 'Pidgin',
    'VERSION_FROM'     => "$srcdir/Pidgin.pm", # finds $VERSION
    ($] >= 5.005 ?
      ('ABSTRACT_FROM' => "$srcdir/Pidgin.pm", # finds $ABSTRACT
       'AUTHOR'        => 'Pidgin <http://pidgin.im/>') : ()),
    'DEFINE'        => '',
    'INC'           => "-I. -I$srcdir -I$top_srcdir -I$top_srcdir/libpurple -I$top_srcdir/pidgin $gtk_cflags",
    'OBJECT'        => '$(O_FILES)', # link all the C files too
    'TYPEMAPS'      => ["$top_srcdir/libpurple/plugins/perl/common/typemap"],
    'INSTALLDIRS'          => 'vendor',
    'INSTALL_BASE'         => $prefix,
    'INSTALLVENDORARCH'    => "$prefix/lib/pidgin/perl",
    'INSTALLVENDORMAN3DIR' => "$prefix/share/man/man3",
);
