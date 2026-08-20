#!/bin/sh
# Turn the runtime header into a C string constant, so cubc carries it.
printf '/* Generated from %s -- do not edit. */\n' "$1"
printf '#include "cub.h"\n\n'
printf 'const char *CUB_RUNTIME_SRC =\n'
sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$/\\n"/' "$1"
printf ';\n'
