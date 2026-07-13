# Mono/.NET plugin loader.

subdir('loader')
subdir('api')

# Sample managed plugins compiled against PurpleAPI.dll.
foreach cs : ['MPlugin', 'GetBuddyBack']
  custom_target(cs + '.dll',
    input : files(cs + '.cs'),
    output : cs + '.dll',
    command : [mcs_prog, '-t:library', '-lib:@0@'.format(meson.current_build_dir() / 'api'),
               '-out:@OUTPUT@', '-r:PurpleAPI.dll', '@INPUT@'],
    depends : purple_api_dll,
    install : true,
    install_dir : purple_plugindir,
  )
endforeach
