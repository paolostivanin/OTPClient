# OTPClient
<a href="https://circleci.com/gh/paolostivanin/OTPClient">
  <img alt="CircleCI" src="https://circleci.com/gh/paolostivanin/OTPClient.svg?style=svg"/>
</a>
<a href="https://scan.coverity.com/projects/paolostivanin-otpclient">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/12749/badge.svg"/>
</a>


Highly secure and easy to use GTK+ software for two-factor authentication that supports both Time-based One-time Passwords (TOTP) and HMAC-Based One-Time Passwords (HOTP).

## Requirements
| Name                                               | Min Version |
|----------------------------------------------------|-------------|
| GTK+                                               | 3.24        |
| Glib                                               | 2.68.0      |
| jansson                                            | 2.12        |
| libgcrypt                                          | 1.10.1      |
| libpng                                             | 1.6.30      |
| [libcotp](https://github.com/paolostivanin/libcotp) | 3.0.0      |
| zbar                                               | 0.20        |
| protobuf-c                                         | 1.3.0       |
| protobuf                                           | 3.6.0       |
| uuid                                               | 2.34        |
| libsecret                                          | 0.20        |
| qrencode                                           | 4.0.2       |

:warning: Please note that the memlock value should be `>= 64 MB`. Any value less than this may cause issues when dealing with tens of tokens (especially when importing from third parties backups).
See this [wiki section](https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations) for info on how to check the current value and set, if needed, a higher one.

## Features
- integration with the OS' secret service provider via libsecret
- support both TOTP and HOTP
- support setting custom digits (between 4 and 10 inclusive)
- support setting a custom period (between 10 and 120 seconds inclusive)
- support SHA1, SHA256 and SHA512 algorithms
- support for Steam codes (please read [THIS PAGE](https://github.com/paolostivanin/OTPClient/wiki/Steam-Support))
- import and export encrypted/plain [Aegis](https://github.com/beemdevelopment/Aegis) backup
- import and export plain [FreeOTPPlus](https://github.com/helloworld1/FreeOTPPlus) backup (key URI format only)
- import and export encrypted/plain [AuthenticatorPro](https://github.com/jamie-mh/AuthenticatorPro) backup
- import and export encrypted/plain [2FAS](https://github.com/twofas) backup
- import of Google's migration QR codes
- local database is encrypted using AES256-GCM
  - key is derived using Argon2id with the following default parameters: 4 iterations, 128 MiB memory cost, 4 parallelism, 32 taglen. The first three parameters can be changed by the user.
  - decrypted file is never saved (and hopefully never swapped) to disk. While the app is running, the decrypted content resides in a "secure memory" buffer allocated by Gcrypt
- GNOME Shell search provider and KDE KRunner integration (requires secret service-enabled key storage)

## Testing
* Before each release, I run PVS Studio and Coverity in order to catch even more bugs.
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

## IDE
Developed using JetBrains CLion 
<a href="https://www.jetbrains.com" style="display: inline-block; vertical-align: middle;">
  <img alt="CLion logo"
       src="https://resources.jetbrains.com/storage/products/company/brand/logos/CLion_icon.svg"
       style="width: 25px; height: 25px; margin-left: 10px;" />
</a>
