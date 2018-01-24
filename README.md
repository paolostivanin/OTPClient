# OTPClient
<a href="https://scan.coverity.com/projects/paolostivanin-otpclient">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/12749/badge.svg"/>
</a>

Simple GTK+ v3 TOTP/HOTP client that uses [libcotp](https://github.com/paolostivanin/libcotp)

## Screenshots
![Main window](/data/screenshots/mainwin.png?raw=true "Main window")
![Add tokens](/data/screenshots/addtokens.png?raw=true "Add new tokens")

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
- double click on a *ticked row* to copy the OTP value

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

## How To Use
On the first run, you will be asked:

1. where to store the database file
2. to type two times the encryption password for the database

Please keep in mind that the **password can't be recovered**. This means that if the password is forgotten, **the data is lost**.

After the first run, every time you start the program you will be asked to enter the password you have previously chosen.


## Limitations
On Ubuntu 16.04 (and maybe other distro), the `memlock` default value is very low (`64 KB`, you can check that with `ulimit -l`).

If you are going to store more than ~130 tokens, each one using very long label and issuer (128 chars) and a long secret (64 chars), then you must have to increase that limit.
To do that, please follow these steps:
* create a file called, for example, `/etc/security/limits.d/memlock.conf` and add the following text:
```
* soft memlock unlimited
* hard memlock unlimited
```
* append to the file `/etc/pam.d/common-session` (could be another file on another distro):
```
session required pam_limits.so
```
reboot the system.

## Tested OS

|OS|Version|Branch|DE|
|:-:|:----:|:----:|:-:|
|Archlinux|-|stable|GNOME|
|Ubuntu|16.04[1], 17.10|-|GNOME|
|Debian|9|stable|GNOME|
|Debian|-|testing (08/nov/2017)|GNOME|
|Solus|-|stable|Budgie|
|Fedora|26, 27|-|GNOME|
|macOS[2]|10.13|High Sierra|-|

[1] OTPClient can be run on Ubuntu 16.04 only with [Flatpak](#flatpak).

[2] For MacOS you need to:
- install brew
- install `cmake`, `gkt+3`, `gnome-icon-theme`, `libzip`, `libgcrypt`, `json-glib`
- create the missing symlink: `ln -s /usr/local/Cellar/libzip/<VERSION>/lib/libzip/include/zipconf.h /usr/local/include/`
- install `libcotp`

## Packages
Personally, I prefer to spend time on development rather than packaging for the myriads of systems out there. If you want to maintain the package for your favourite/daily driver distro(s), feel free to drop me an email or open a PR with an update for this section :)

|Distro|Link|
|:-:|:---:|
|Archlinux|https://aur.archlinux.org/packages/otpclient-git|

## Flatpak
To install Flatpak, follow the [official guide](https://flatpak.org/getting.html). To install OTPClient, open a terminal and the execute:
```
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install flathub com.github.paolostivanin.OTPClient
```
Please note that with the flatpak version you won't be asked where to store the database. Instead, the software will use the app's data directory (`/home/USER/.var/app/com.github.paolostivanin.OTPClient/data`)
This change was necessary in order to restrict the app's permissions to the filesystem (from the initial `filesystem=home` to nothing).

## License
This software is released under the GPLv3 license. Please have a look at the [LICENSE](LICENSE) file for more details.
 
