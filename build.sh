#!/bin/bash

echo "Compiling pastebinc.c"

ARGS=""

if [[ "${1}" == "1" ]]; then
  ARGS="${ARGS} -DPRINTPASTE"
fi

gcc ${ARGS} \
  -lcurl \
  `pkg-config --cflags --libs glib-2.0` \
  pastebinc.c \
  -o ./pastebinc

echo "Done"

if [[ "${1}" == "1" ]]; then
  echo "Testing"
  date | ./pastebinc
fi
