#!/bin/sh

set -eu

cd "$(dirname "$0")"
mkdir -p build

: "${CC:=cc}"
: "${CXX:=c++}"

COMMON_FLAGS="-Wall -Wextra -Werror -Wconversion -Wsign-conversion -O0 -g -DCOMBINED_STAGE2_HOST_TEST -I../../../../../include"
CXX_FLAGS="-ffreestanding -fno-builtin -fno-stack-protector -fno-exceptions -fno-rtti -fno-threadsafe-statics"

"$CC" -std=c11 $COMMON_FLAGS -c patchfinder_harness.c -o build/patchfinder_harness.o
"$CXX" -std=c++20 $COMMON_FLAGS $CXX_FLAGS \
	-c ../patchfinder.cpp -o build/patchfinder.o
"$CXX" build/patchfinder.o build/patchfinder_harness.o -o build/patchfinder_harness
