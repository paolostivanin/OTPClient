# OTPClient
<a href="https://scan.coverity.com/projects/paolostivanin-otpclient">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/12749/badge.svg"/>
</a>

Simple GTK+ v3 TOTP/HOTP client that uses [libcotp](https://github.com/paolostivanin/libcotp)

## Requirements
- GTK+          >= 3.22
- Glib          >= 2.50.0
- json-glib     >= 1.2.0
- libgcrypt     >= 1.6.0
- libzip
- libcotp       >= 1.0.10
- libbaseencde  >= 1.0.4

## Features
- import encrypted [Authenticator Plus](https://www.authenticatorplus.com/) backup
- import encrypted [andOTP](https://github.com/flocke/andOTP) backup
- encrypt local file using AES256-GCM
  - key is derived using PBKDF2 with SHA512 and 100k iterations

## Compile
To compile this software you need all the dependencies specified above. The last two (libcotp and libbasenecode) will be downloaded and compiled by the script [install.sh](install.sh).
```
wget https://raw.githubusercontent.com/paolostivanin/OTPClient/master/install.sh > /tmp/install.sh
chmod +x /tmp/install.sh
./tmp/install.sh
```

## Tested OS
Last update: 08/Nov/2017

|OS|Version|Branch|DE|Notes|
|:-:|:----:|:----:|:-:|:--:|
|Archlinux|-|stable|GNOME|-|
|Ubuntu|17.10|-|GNOME|-|
|Debian|9|stable|GNOME|-|
|Debian|-|testing|GNOME|-|
|Solus|-|stable|Budgie|-|
|macOS|10.13|High Sierra|-|Install with brew: `cmake`, `gkt+3`, `gnome-icon-theme`, `libzip`, `libgcrypt`, `json-glib`. Then `ln -s /usr/local/Cellar/libzip/<VERSION>/lib/libzip/include/zipconf.h /usr/local/include/` |

## License
This software is released under the GPLv3 license. Please have a look at the [LICENSE](LICENSE) file for more details.
 
