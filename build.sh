#!/bin/bash

echo "Compiling pastebinc.c"

ARGS=""

if [[ "${1}" == "1" ]]; then
  ARGS="${ARGS} -DPRINTPASTE"
fi

gcc -lcurl ${ARGS} pastebinc.c -o ./pastebinc
echo "Done"

if [[ "${1}" == "1" ]]; then
  echo "Testing"
  date | ./pastebinc
fi
