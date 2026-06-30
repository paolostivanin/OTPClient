#!/bin/bash

set -e

# Extra cmake flags can be passed via EXTRA_CMAKE_ARGS (e.g. "-DSANITIZE=ON
# -DCMAKE_BUILD_TYPE=Debug"). Each flag must be a single shell word - values
# containing spaces are not supported.
__compile_and_install() {
  cmake .. -DCMAKE_INSTALL_PREFIX=/usr ${EXTRA_CMAKE_ARGS:-}
  make -j2
  make install
}

mkdir build && cd "$_"
__compile_and_install
