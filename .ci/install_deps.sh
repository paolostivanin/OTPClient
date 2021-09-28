#!/bin/bash

set -e

__compile_and_install() {
  cmake .. -DCMAKE_INSTALL_PREFIX=/usr
  make -j2
  make install
}

git clone https://github.com/paolostivanin/libbaseencode.git
cd libbaseencode && mkdir build && cd "$_"
  __compile_and_install
cd ../..
git clone https://github.com/paolostivanin/libcotp.git
cd libcotp && mkdir build && cd "$_"
  __compile_and_install
cd ../..
