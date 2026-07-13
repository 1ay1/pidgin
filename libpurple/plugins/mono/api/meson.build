# PurpleAPI.dll — managed API assembly.

purple_api_dll = custom_target('PurpleAPI.dll',
  input : files(
    'BlistNode.cs',
    'BuddyList.cs',
    'Buddy.cs',
    'Contact.cs',
    'Debug.cs',
    'Event.cs',
    'PurplePlugin.cs',
    'Group.cs',
    'Signal.cs',
    'Status.cs',
  ),
  output : 'PurpleAPI.dll',
  command : [mcs_prog, '-t:library', '-out:@OUTPUT@', '@INPUT@'],
  install : true,
  install_dir : purple_plugindir,
)
