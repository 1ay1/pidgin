if FUZZ
check_PROGRAMS=\
	fuzz_html_to_xhtml \
	fuzz_jabber_caps \
	fuzz_jabber_id_new \
	fuzz_markup_strip_html \
	fuzz_mime \
	fuzz_xmlnode

common_CFLAGS=\
	$(GLIB_CFLAGS) \
	$(DEBUG_CFLAGS) \
	$(LIBXML_CFLAGS) \
	-I.. \
	-I$(top_srcdir)/libpurple \
	-DBUILDDIR=\"$(top_builddir)\"

common_LDADD=\
	$(top_builddir)/libpurple/protocols/jabber/libjabber.la \
	$(top_builddir)/libpurple/libpurple.la \
	$(GLIB_LIBS)

fuzz_xmlnode_SOURCES=fuzz_xmlnode.c
fuzz_xmlnode_LDADD=$(common_LDADD)
fuzz_xmlnode_CFLAGS=-fsanitize=fuzzer,address $(common_CFLAGS)

fuzz_jabber_id_new_SOURCES=fuzz_jabber_id_new.c
fuzz_jabber_id_new_LDADD=$(common_LDADD)
fuzz_jabber_id_new_CFLAGS=-fsanitize=fuzzer,address $(common_CFLAGS)

fuzz_jabber_caps_SOURCES=fuzz_jabber_caps.c
fuzz_jabber_caps_LDADD=$(common_LDADD)
fuzz_jabber_caps_CFLAGS=-fsanitize=fuzzer,address $(common_CFLAGS)

fuzz_mime_SOURCES=fuzz_mime.c
fuzz_mime_LDADD=$(common_LDADD)
fuzz_mime_CFLAGS=-fsanitize=fuzzer,address $(common_CFLAGS)

fuzz_html_to_xhtml_SOURCES=fuzz_html_to_xhtml.c
fuzz_html_to_xhtml_LDADD=$(common_LDADD)
fuzz_html_to_xhtml_CFLAGS=-fsanitize=fuzzer,address $(common_CFLAGS)

fuzz_markup_strip_html_SOURCES=fuzz_markup_strip_html.c
fuzz_markup_strip_html_LDADD=$(common_LDADD)
fuzz_markup_strip_html_CFLAGS=-fsanitize=fuzzer,address $(common_CFLAGS)

endif
