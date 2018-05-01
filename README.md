# OTPClient
<a href="https://scan.coverity.com/projects/paolostivanin-otpclient">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/12749/badge.svg"/>
</a>

Simple GTK+ v3 TOTP/HOTP client that uses [libcotp](https://github.com/paolostivanin/libcotp)

## Requirements
|Name|Min Version|Suggested Version|
|----|-----------|-----------------|
|GTK+|3.22|-|
|Glib|2.50.0|-|
|jansson|2.8.0|-|
|libgcrypt|1.6.0|-|
|libzip|1.1.0|-|
|libpng|1.6.0|-|
|[libcotp](https://github.com/paolostivanin/libcotp)|1.0.10|-|
|zbar|0.10|0.20 ([linuxtv](https://linuxtv.org/downloads/zbar/))    |

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
- when a row is **ticked**, the otp is automatically copied to the clipboard (which is erased before terminating the program)
  - another otp value can still be copied by double clicking it when the **row is ticked**

## Installation
1. install all the needed libraries listed under [requirements](#requirements)
2. clone and install OTPClient:
```
$ git clone https://github.com/paolostivanin/otpclient OTPClient
$ cd OTPClient
$ mkdir build && cd $_
$ cmake -DCMAKE_INSTALL_PREFIX=/usr ..
$ make
$ sudo make install
```

## Screenshots
See the [project's wiki](https://github.com/paolostivanin/OTPClient/wiki)

## How To Use
See the [project's wiki](https://github.com/paolostivanin/OTPClient/wiki)

## Limitations
See the [project's wiki](https://github.com/paolostivanin/OTPClient/wiki)

## Tested OS
See the [project's wiki](https://github.com/paolostivanin/OTPClient/wiki)

## Flatpak
See the [project's wiki](https://github.com/paolostivanin/OTPClient/wiki)

## Packages
Personally, I prefer to spend time on development rather than packaging for the myriads of systems out there. If you want to maintain the package for your favourite/daily driver distro(s), feel free to drop me an email or open a PR with an update for this section :)

|Distro|Link|
|:-:|:---:|
|Archlinux|https://aur.archlinux.org/packages/otpclient|
|Gentoo linux|https://github.com/mPolr/overlay|

## License
This software is released under the GPLv3 license. Please have a look at the [LICENSE](LICENSE) file for more details.
 
