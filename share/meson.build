# share/ — sounds and CA certificate bundle.

install_data(
  files(
    'sounds/alert.wav',
    'sounds/login.wav',
    'sounds/logout.wav',
    'sounds/receive.wav',
    'sounds/send.wav',
  ),
  install_dir : datadir / 'sounds' / 'purple',
)

if get_option('install-ssl-certs')
  install_data(
    files(
      'ca-certs/CAcert_Root.pem',
      'ca-certs/CAcert_Class3.pem',
      'ca-certs/mozilla.pem',
    ),
    install_dir : datadir / 'purple' / 'ca-certs',
  )
endif
