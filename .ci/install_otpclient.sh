#!/bin/bash

set -e

__compile_and_install() {
  cmake .. -DENABLE_MINIMIZE_TO_TRAY=ON -DCMAKE_INSTALL_PREFIX=/usr
  make -j2
  make install
}

mkdir build && cd "$_"
__compile_and_install
