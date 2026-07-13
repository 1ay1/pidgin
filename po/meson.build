# po/ — message catalogs. Domain: pidgin.
#
# Upstream uses intltool without a LINGUAS file, so derive the language list
# from the .po files present.

po_files = run_command(
  find_program('python3'),
  '-c',
  'import glob, os; print(" ".join(sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob("@0@/*.po"))))'.format(meson.current_source_dir()),
  check : true,
).stdout().strip()

languages = po_files == '' ? [] : po_files.split(' ')

i18n.gettext('pidgin',
  languages : languages,
  install : true,
)
