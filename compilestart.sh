#!/bin/bash

#cd to the directory of the script
cd "$(dirname "$0")"

make clean && make native && make native hutfinder && make native structure_finder

echo "./hutfinder"
echo "./structure_finder"