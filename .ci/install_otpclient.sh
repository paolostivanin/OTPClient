#!/bin/bash

set -e

function __compile_and_install {
  cmake .. -DCMAKE_INSTALL_PREFIX=/usr
  make -j2
  make install
}

mkdir build && cd "$_"
__compile_and_install
