#!/bin/bash
set -euo pipefail
CC=clang CXX=clang cmake -B build_test -DTEST_BUILD=1 -GNinja
ninja -C build_test
ctest --test-dir build_test --output-on-failure