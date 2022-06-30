# OTPClient
<a href="https://circleci.com/gh/paolostivanin/OTPClient">
  <img alt="CircleCI" src="https://circleci.com/gh/paolostivanin/OTPClient.svg?style=svg"/>
</a>

Highly secure and easy to use GTK+ software for two-factor authentication that supports both Time-based One-time Passwords (TOTP) and HMAC-Based One-Time Passwords (HOTP).

## Requirements
| Name                                               | Min Version |
|----------------------------------------------------|-------------|
| GTK+                                               | 3.20        |
| Glib                                               | 2.48.0      |
| jansson                                            | 2.6.0       |
| libgcrypt                                          | 1.6.0       |
| libzip                                             | 1.0.0       |
| libpng                                             | 1.2.0       |
| [libcotp](https://github.com/paolostivanin/libcotp) | 1.2.1       |-|
| zbar                                               | 0.20        |
| protobuf-c                                         | 1.30        |
| protobuf                                           | 3.6         |

## Features
- support both TOTP and HOTP
- support setting custom digits (between 4 and 10 inclusive)
- support setting a custom period (between 10 and 120 seconds inclusive)
- support SHA1, SHA256 and SHA512 algorithms
- support for Steam codes (please read [THIS PAGE](https://github.com/paolostivanin/OTPClient/wiki/Steam-Support))
- import encrypted [Authenticator Plus](https://www.authenticatorplus.com/) backup
- import and export encrypted/plain [andOTP](https://github.com/flocke/andOTP) backup
- import and export encrypted/plain [Aegis](https://github.com/beemdevelopment/Aegis) backup
- import and export plain [FreeOTPPlus](https://github.com/helloworld1/FreeOTPPlus) backup (key URI format only)
- local database is encrypted using AES256-GCM
  - key is derived using PBKDF2 with SHA512 and 100k iterations
  - decrypted file is never saved (and hopefully never swapped) to disk. While the app is running, the decrypted content resides in a "secure memory" buffer allocated by Gcrypt

## Testing
* Before each release, I run PVS Studio in order to catch even more errors and/or corner cases
* With every commit to master, OTPClient is compiled in CircleCI against different distros

## Protobuf
The protobuf files needed to decode Google's otpauth-migration qr codes have been generated with `protoc --c_out=src/ proto/google-migration.proto` 

## Wiki
For things like roadmap, screenshots, how to use OTPClient, etc, please have a look at the [project's wiki](https://github.com/paolostivanin/OTPClient/wiki). You'll find a lot of useful information there.

## Manual installation
If OTPClient hasn't been packaged for your distro ([check here](https://github.com/paolostivanin/OTPClient/wiki/Tested-OS-&-Packages#packages)) and your distro doesn't support Flatpak, then you'll have to manually compile and install OTPClient.
1. install all the needed libraries listed under [requirements](#requirements)
2. clone and install OTPClient:
```
git clone https://github.com/paolostivanin/OTPClient.git
cd OTPClient
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make
sudo make install
```

## License
This software is released under the GPLv3 license. Please have a look at the [LICENSE](LICENSE) file for more details.
