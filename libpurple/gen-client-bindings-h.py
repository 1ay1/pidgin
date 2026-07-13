#!/usr/bin/env python3
# Meson wrapper to generate purple-client-bindings.h, which the autotools
# build produced with two appended commands:
#   dbus-analyze-types.py --keyword=enum --verbatim   < coreheaders   > out
#   dbus-analyze-functions.py --client --headers      < exported      >> out
#
# Usage:
#   gen-client-bindings-h.py <types.py> <functions.py> <output> <unused> \
#       <num_core_headers> <all inputs...>
# The first <num_core_headers> inputs are the core headers; the rest are the
# exported headers.
import subprocess
import sys

types_py = sys.argv[1]
funcs_py = sys.argv[2]
output = sys.argv[3]
# argv[4] is @PRIVATE_DIR@ (unused, reserved)
num_core = int(sys.argv[5])
inputs = sys.argv[6:]

core_headers = inputs[:num_core]
exported = inputs[num_core:]


def cat(paths):
    data = b''
    for p in paths:
        with open(p, 'rb') as fh:
            data += fh.read() + b'\n'
    return data


part1 = subprocess.run(
    [sys.executable, types_py, '--keyword=enum', '--verbatim'],
    input=cat(core_headers), stdout=subprocess.PIPE, check=True).stdout
part2 = subprocess.run(
    [sys.executable, funcs_py, '--client', '--headers'],
    input=cat(exported), stdout=subprocess.PIPE, check=True).stdout

with open(output, 'wb') as fh:
    fh.write(part1)
    fh.write(part2)
