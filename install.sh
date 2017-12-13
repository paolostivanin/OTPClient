#!/bin/bash

echo "==> Downloading libbaseencode..."
pushd /tmp > /dev/null
  git clone https://github.com/paolostivanin/libbaseencode
  cd libbaseencode && mkdir build && cd $_
  cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr ..
  make
popd > /dev/null

echo && echo " => Please open a terminal and run:
cd /tmp/libbaseencode/build
make install (as root)"

echo && echo " => Press ENTER when the previous step has been executed"
read ans

if ! find /usr/lib | grep -q libbaseencode.so; then 
  echo && echo "==> ERROR: libbaseencode has not been installed. Exiting..."
  exit 1
else
  echo "==> libbaseencode has been correctly installed"
fi

echo && echo "==> Downloading libcotp..."
pushd /tmp > /dev/null
  git clone https://github.com/paolostivanin/libcotp
  cd libcotp && mkdir build && cd $_
  cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr ..
  make
popd > /dev/null

echo && echo " => Please open a terminal and run:
cd /tmp/libcotp/build
make install (as root)"

echo && echo " => Press ENTER when the previous step has been executed"
read ans

if ! find /usr/lib | grep -q libcotp.so; then 
  echo && echo "==> ERROR: libcotp has not been installed. Exiting..."
  exit 1
else
  echo "==> libcotp has been correctly installed"
fi

echo && echo "==> Downloading OTPClient..."
pushd /tmp > /dev/null
  git clone https://github.com/paolostivanin/otpclient
  cd otpclient && mkdir build && cd $_
  cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr ..
  make
popd > /dev/null

echo && echo " => Please open a terminal and run:
cd /tmp/libcotp/build
make install (as root)"

echo && echo " => Press ENTER when the previous step has been executed"
read ans

echo "==> Cleaning up files..."
rm -rf /tmp/libbaseencode
rm -rf /tmp/libcotp
rm -rf /tmp/otpclient
