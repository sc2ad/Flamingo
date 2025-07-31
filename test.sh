#!/bin/sh
CC=clang CXX=clang cmake -B build -DTEST_BUILD=1 -GNinja
ninja -C build
ctest --test-dir build --output-on-failure
