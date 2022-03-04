This directory contains all of the stuff to create our nsis installer. This
file is meant to document what they all do, but we're still determining some of
that.

## available.lst

`available.lst` is used to allow the user to select what spell checking
dictionaries to install. It is generated from the
[pidgin-dictionaries](https://keep.imfreedom.org/pidgin/dictionaries)
repository. There's a CI job for it in TeamCity that should be ran whenever we
want to upgrade the dictionaries. But this should be done sparingly as we don't
want to break old links or have a ton of the files on SourceForge.

The dictionaries as well as this file are deployed to
[SourceForge](https://sourceforge.net/projects/pidgin/files/dictionaries) in a
time stamped directory.

After the dictionaries are updated on SourceForge, you should update the
`available.lst` in this directory with the one from SourceForge.

