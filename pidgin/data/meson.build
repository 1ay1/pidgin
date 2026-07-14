# pidgin/data — pkg-config, .desktop, appdata, all with i18n merge.

# pidgin.pc
pkgconf.generate(
  name : 'Pidgin',
  filebase : 'pidgin',
  description : 'Pidgin is a GTK3-based instant messenger application.',
  version : meson.project_version(),
  requires : ['gtk+-3.0', 'purple'],
  subdirs : 'pidgin',
  variables : ['plugindir=@0@'.format(pidgin_plugindir)],
)

if have_nls
  # Step 1: resolve @USES_MM_CHAT_SECTION@ -> pidgin.desktop.in
  desktop_conf = configuration_data()
  desktop_conf.set('USES_MM_CHAT_SECTION', '')
  desktop_in = configure_file(
    input : 'pidgin.desktop.in.in',
    output : 'pidgin.desktop.in',
    configuration : desktop_conf,
  )

  # Step 2: intltool-style translation merge.
  i18n.merge_file(
    input : desktop_in,
    output : 'pidgin.desktop',
    type : 'desktop',
    po_dir : meson.project_source_root() / 'po',
    install : true,
    install_dir : datadir / 'applications',
  )

  i18n.merge_file(
    input : 'pidgin.appdata.xml.in',
    output : 'pidgin.appdata.xml',
    type : 'xml',
    po_dir : meson.project_source_root() / 'po',
    install : true,
    install_dir : datadir / 'metainfo',
  )
endif
