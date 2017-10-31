# OTPClient
<a href="https://scan.coverity.com/projects/paolostivanin-otpclient">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/12749/badge.svg"/>
</a>

Simple GTK+ v3 TOTP/HOTP client that uses [libcotp](https://github.com/paolostivanin/libcotp)

Please note that the software is currently in **ALPHA** version and is not meant to be used (yet).

## Requirements
- GTK+      >= 3.22
- Glib      >= 2.54.0
- json-glib >= 1.2.0
- libgcrypt >= 1.6.0
- libzip

## Features
- import encrypted [Authenticator Plus](https://www.authenticatorplus.com/) backup
- import encrypted [andOTP](https://github.com/flocke/andOTP) backup
- encrypt local file using AES256-GCM
  - key is derived using PBKDF2 with SHA512 and 100k iterations

## Tested Distros
- Archlinux (stable, GNOME)
- Ubuntu 17.10

## License
See license file.
 
