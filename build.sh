#!/bin/sh
set -eu
cc -O2 -std=c11 -Wall -Wextra -pthread rookzero.c -o rookzero
printf 'Built RookZero 1.1.1 -> ./rookzero\n'
