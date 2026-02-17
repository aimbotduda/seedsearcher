#!/bin/bash

#cd to the directory of the script
cd "$(dirname "$0")"

echo "=== Building cubiomes library ==="
make clean && make native

if [ ! -f libcubiomes.a ]; then
    echo "ERROR: libcubiomes.a was not built. Check for errors above."
    exit 1
fi

echo ""
echo "=== Building structure_finder ==="
cc -O3 -march=native -ffast-math -flto -o structure_finder structure_finder.c libcubiomes.a -lm -pthread
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to build structure_finder"
    exit 1
fi

echo ""
echo "=== Building groupfinder ==="
make -C findgroups
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to build groupfinder"
    exit 1
fi

echo ""
echo "=== Build Complete ==="
echo ""
echo "Run: ./structure_finder"
echo "Run: cd findgroups && ./groupfinder"
