# pidgin/pixmaps — image assets.
#
# The bulk of the artwork installs under $datadir/pixmaps/pidgin/, preserving
# the nested layout. The icons/hicolor tree installs under $datadir directly
# (so it lands at $datadir/icons/hicolor/...).

pidgin_pixmapdir = datadir / 'pixmaps' / 'pidgin'

# Nested artwork trees -> $datadir/pixmaps/pidgin/<tree>/...
foreach tree : ['animations', 'dialogs', 'emblems', 'protocols', 'status', 'toolbar', 'tray']
  install_subdir(tree,
    install_dir : pidgin_pixmapdir,
    exclude_files : ['Makefile.mingw'],
    strip_directory : false,
  )
endforeach

# emotes: install everything except build scaffolding.
install_subdir('emotes',
  install_dir : pidgin_pixmapdir,
  exclude_files : ['Makefile.am', 'Makefile.in', 'Makefile.mingw'],
)

# Standalone button / logo bits.
install_data(files('edit.png', 'pause.png', 'info.png'),
  install_dir : pidgin_pixmapdir / 'buttons',
)
install_data(
  files('logo.png', 'arrow-down.xpm', 'arrow-left.xpm', 'arrow-right.xpm', 'arrow-up.xpm'),
  install_dir : pidgin_pixmapdir,
)

# Application icon theme -> $datadir/icons/hicolor/...
install_subdir('icons/hicolor',
  install_dir : datadir / 'icons',
)
