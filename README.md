# OTPClient
<a href="https://scan.coverity.com/projects/paolostivanin-otpclient">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/12749/badge.svg"/>
</a>

Simple GTK+ v3 TOTP/HOTP client that uses [libcotp](https://github.com/paolostivanin/libcotp)

## Requirements
|Name|Min Version|
|----|-----------|
|GTK+|3.22|
|Glib|2.50.0|
|json-glib|1.2.0|
|libgcrypt|1.6.0|
|libzip|1.1.0|
|[libcotp](https://github.com/paolostivanin/libcotp)|1.0.10|

## Features
- support for TOTP and HOTP
- support 6 and 8 digits
- support SHA1, SHA256 and SHA2512 algorithms
- import encrypted [Authenticator Plus](https://www.authenticatorplus.com/) backup
- import encrypted [andOTP](https://github.com/flocke/andOTP) backup
- encrypt local file using AES256-GCM
  - key is derived using PBKDF2 with SHA512 and 100k iterations
  - decrypted file is never saved (and hopefully never swapped) to disk. While the app is running, the decrypted content resides in a "secure memory" buffer allocated by Gcrypt 
- auto-refresh TOTP every 30 seconds

## Installation
1. install all the needed libraries listed under [requirements](#requirements)
2. clone and install OTPClient:
```
$ git clone https://github.com/paolostivanin/otpclient OTPClient
$ cd OTPClient
$ mkdir build && cd $_
$ cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr ..
$ make
# make install 
```

## Tested OS

|OS|Version|Branch|DE|
|:-:|:----:|:----:|:-:|
|Archlinux|-|stable|GNOME|
|Ubuntu|17.04, 17.10|-|GNOME|
|Debian|9|stable|GNOME|
|Debian|-|testing (08/nov/2017)|GNOME|
|Solus|-|stable|Budgie|
|Fedora|26, 27|-|GNOME|
|macOS*|10.13|High Sierra|-|

[*] For MacOS you need to:
- install brew
- install `cmake`, `gkt+3`, `gnome-icon-theme`, `libzip`, `libgcrypt`, `json-glib`
- create the missing symlink: `ln -s /usr/local/Cellar/libzip/<VERSION>/lib/libzip/include/zipconf.h /usr/local/include/`
- install `libcotp`
 
## License
This software is released under the GPLv3 license. Please have a look at the [LICENSE](LICENSE) file for more details.
 
