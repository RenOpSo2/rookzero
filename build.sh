#!/bin/sh
set -eu
cc -O2 -std=c11 -Wall -Wextra -pthread rookzero.c -o rookzero
printf 'Built RookZero %s -> ./rookzero\n' "$(sed -n 's/^#define VERSION "\(.*\)"$/\1/p' rookzero.c)"