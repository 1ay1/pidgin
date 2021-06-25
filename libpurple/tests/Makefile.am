if HAVE_CHECK
TESTS=check_libpurple

clean-local:
	-rm -rf libpurple..


check_PROGRAMS=check_libpurple

if FUZZ
fuzz_programs=\
	fuzz_html_to_xhtml \
	fuzz_jabber_caps \
	fuzz_jabber_id_new \
	fuzz_markup_strip_html \
	fuzz_mime \
	fuzz_xmlnode
check_PROGRAMS+=$(fuzz_programs)
endif


check_libpurple_SOURCES=\
        check_libpurple.c \
	    tests.h \
		test_cipher.c \
		test_jabber_caps.c \
		test_jabber_digest_md5.c \
		test_jabber_jutil.c \
		test_jabber_scram.c \
		test_util.c \
		test_xmlnode.c \
		$(top_builddir)/libpurple/util.h

check_libpurple_CFLAGS=\
        @CHECK_CFLAGS@ \
		$(GLIB_CFLAGS) \
		$(DEBUG_CFLAGS) \
		$(LIBXML_CFLAGS) \
		-I.. \
		-I$(top_srcdir)/libpurple \
		-DBUILDDIR=\"$(top_builddir)\"

check_libpurple_LDADD=\
		$(top_builddir)/libpurple/protocols/jabber/libjabber.la \
		$(top_builddir)/libpurple/libpurple.la \
        @CHECK_LIBS@ \
		$(GLIB_LIBS)


if FUZZ
fuzz_xmlnode_SOURCES=fuzz_xmlnode.c
fuzz_xmlnode_LDADD=$(check_libpurple_LDADD)
fuzz_xmlnode_CFLAGS=-fsanitize=fuzzer,address $(check_libpurple_CFLAGS)

fuzz_jabber_id_new_SOURCES=fuzz_jabber_id_new.c
fuzz_jabber_id_new_LDADD=$(check_libpurple_LDADD)
fuzz_jabber_id_new_CFLAGS=-fsanitize=fuzzer,address $(check_libpurple_CFLAGS)

fuzz_jabber_caps_SOURCES=fuzz_jabber_caps.c
fuzz_jabber_caps_LDADD=$(check_libpurple_LDADD)
fuzz_jabber_caps_CFLAGS=-fsanitize=fuzzer,address $(check_libpurple_CFLAGS)

fuzz_mime_SOURCES=fuzz_mime.c
fuzz_mime_LDADD=$(check_libpurple_LDADD)
fuzz_mime_CFLAGS=-fsanitize=fuzzer,address $(check_libpurple_CFLAGS)

fuzz_html_to_xhtml_SOURCES=fuzz_html_to_xhtml.c
fuzz_html_to_xhtml_LDADD=$(check_libpurple_LDADD)
fuzz_html_to_xhtml_CFLAGS=-fsanitize=fuzzer,address $(check_libpurple_CFLAGS)

fuzz_markup_strip_html_SOURCES=fuzz_markup_strip_html.c
fuzz_markup_strip_html_LDADD=$(check_libpurple_LDADD)
fuzz_markup_strip_html_CFLAGS=-fsanitize=fuzzer,address $(check_libpurple_CFLAGS)
endif

endif
