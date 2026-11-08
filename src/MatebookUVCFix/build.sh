#!/bin/bash
set -eu
cd "$(dirname "$0")"
SDK="$(pwd)/deps/MacKernelSDK-master"
LILU="$(pwd)/deps/Lilu"
mkdir -p build
FLAGS=(-arch x86_64 -std=gnu++14 -mkernel -fapple-kext -nostdinc -nostdinc++ -isystem "$SDK/Headers" -I "$LILU" -I . -DKERNEL -DKERNEL_PRIVATE -DPRODUCT_NAME=MatebookUVCFix -DMODULE_VERSION=0.3.0 -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector -fno-common -fvisibility=hidden -mno-red-zone -fno-asynchronous-unwind-tables -mmacosx-version-min=10.9 -Os)
clang++ "${FLAGS[@]}" -c kern_start.cpp -o build/kern_start.o
clang++ "${FLAGS[@]}" -c "$LILU/Library/plugin_start.cpp" -o build/plugin_start.o
clang++ "${FLAGS[@]}" -c kmod.cpp -o build/kmod.o
mkdir -p build/MatebookUVCFix.kext/Contents/MacOS
cp Info.plist build/MatebookUVCFix.kext/Contents/Info.plist
ld -arch x86_64 -kext -undefined dynamic_lookup -o build/MatebookUVCFix.kext/Contents/MacOS/MatebookUVCFix build/kern_start.o build/plugin_start.o build/kmod.o "$SDK/Library/x86_64/libkmod.a"
