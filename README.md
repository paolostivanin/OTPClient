# OTPClient
GTK4/libadwaita application for managing TOTP and HOTP two-factor authentication tokens.

## Features

### Supported standards
- TOTP and HOTP
- Custom digits (4–10) and custom period (10–120s)
- SHA1, SHA256, and SHA512 algorithms
- Steam guard codes ([details](https://github.com/paolostivanin/OTPClient/wiki/Steam-Support))

### Organization
- Token grouping: assign tokens to groups (e.g. "Work", "Personal") for quick filtering
- Header bar dropdown to filter by group, or use `group:<name>` / `#<name>` in the search bar
- Groups can be assigned via the right-click context menu, the edit dialog, or when adding a token
- Groups are preserved during Aegis, AuthenticatorPro, and 2FAS import/export

### Import & Export
- [Aegis](https://github.com/beemdevelopment/Aegis) (encrypted and plain)
- [AuthenticatorPro](https://github.com/jamie-mh/AuthenticatorPro) (encrypted and plain)
- [2FAS](https://github.com/twofas) (encrypted and plain)
- [FreeOTPPlus](https://github.com/helloworld1/FreeOTPPlus) (plain, key URI format)
- Google migration QR codes (import only)

### Security
- Local database encrypted with AES256-GCM
- Key derived via Argon2id (default: 4 iterations, 128 MiB memory, parallelism 4)
- Decrypted content held in libgcrypt secure memory, never written to disk
- Integration with the OS secret service via libsecret

## Installation
OTPClient is available as a Flatpak and in several distro repositories. See the [packages list](https://github.com/paolostivanin/OTPClient/wiki/Tested-OS-&-Packages#packages) for details.

### Building from source
1. Install all the libraries listed under [requirements](#requirements).
2. Clone and build:
```
git clone https://github.com/paolostivanin/OTPClient.git
cd OTPClient
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make
sudo make install
```

## Requirements
| Name                                                | Min Version |
|-----------------------------------------------------|-------------|
| GTK                                                 | 4.10.0      |
| libadwaita                                          | 1.5.0       |
| Glib                                                | 2.74.0      |
| jansson                                             | 2.13        |
| libgcrypt                                           | 1.10.1      |
| libpng                                              | 1.6.0       |
| [libcotp](https://github.com/paolostivanin/libcotp) | 4.0.0       |
| zbar                                                | 0.20        |
| protobuf-c                                          | 1.3.0       |
| uuid                                                | 2.30        |
| libsecret                                           | 0.20        |
| qrencode                                            | 4.0.0       |

**Note:** The system memlock limit should be at least 64 MB. Lower values may cause issues when handling many tokens, especially when importing third-party backups. See the [wiki](https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations) for how to check and adjust this.

## Wiki
For screenshots, roadmap, and usage guides, see the [project wiki](https://github.com/paolostivanin/OTPClient/wiki).

## License
This software is released under the GPLv3 license. See the [LICENSE](LICENSE) file for details.
