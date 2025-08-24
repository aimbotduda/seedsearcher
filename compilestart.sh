#!/bin/bash

#cd to the directory of the script
cd "$(dirname "$0")"

make clean && make native

echo "./hutfinder"
echo "./structure_finder"
