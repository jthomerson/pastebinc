#!/bin/bash

echo "Compiling pastebinc.c"

ARGS=""

gcc ${ARGS} \
  -lcurl \
  `pkg-config --cflags --libs glib-2.0` \
  pastebinc.c \
  -o ./pastebinc

echo "Done"

if [[ "${1}" == "1" ]]; then
  echo "Testing"
  echo "--"
  echo ""
  date | ./pastebinc
fi
