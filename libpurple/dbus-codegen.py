#!/usr/bin/env python3
# Meson wrapper: concatenate input header/source files and pipe them to one of
# the dbus-analyze-*.py generators on stdin, writing the result to stdout.
#
# Usage: dbus-codegen.py <analyzer.py> [analyzer-args...] -- <input files...>
import subprocess
import sys

argv = sys.argv[1:]
sep = argv.index('--')
analyzer_and_args = argv[:sep]
inputs = argv[sep + 1:]

data = b''
for path in inputs:
    with open(path, 'rb') as fh:
        data += fh.read()
        data += b'\n'

proc = subprocess.run(
    [sys.executable] + analyzer_and_args,
    input=data,
    stdout=subprocess.PIPE,
)
sys.stdout.buffer.write(proc.stdout)
sys.exit(proc.returncode)
