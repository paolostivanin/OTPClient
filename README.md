# OTPClient
A highly secure GTK4/libadwaita application for managing TOTP and HOTP two-factor authentication tokens, with CLI and desktop-search integration.

## Features

### Supported standards
- TOTP and HOTP
- Manual entry: digits 4–10, period 1–300 s
- `otpauth://` URIs (import / display / QR): digits 6–8, period 1–300 s (per RFC 6238)
- SHA1, SHA256, and SHA512 algorithms
- Steam guard codes ([details](https://github.com/paolostivanin/OTPClient/wiki/Steam-Support))

### GUI
- Token list with drag-and-drop reordering
- Cross-database search (`group:<name>` / `#<name>` syntax for filtering)
- Token grouping: assign tokens to groups (e.g. "Work", "Personal") via right-click,
  the edit dialog, or when adding a token; groups are preserved during Aegis,
  AuthenticatorPro, and 2FAS import/export
- QR code display for any token (re-pairing or sharing across devices)
- Idle and screensaver auto-lock with configurable timeout
- Configurable clipboard wipe; clipboard is also cleared on lock and app exit
- HOTP "next" button is debounced to prevent accidental double-increments
- Optional minimize-to-tray (build-time opt-in)

### Command-line interface (`otpclient-cli`)
- `--show / --list` for scripting and shell integration
- `--import / --export` against the same database the GUI uses
- `--output={table,json,csv}` for machine-readable output
- `--password-file` to read the master password from a file
- `--export-settings / --import-settings` for GSettings backup/restore
- Bash, zsh, and fish completions installed by default

### Desktop search (`otpclient-search-provider`)
A separate D-Bus daemon that integrates with **GNOME Shell Activities Search**
and **KDE Plasma 6 KRunner**. Type the configurable trigger keyword (default
`otp`) followed by a query — selecting a result computes the OTP and delivers
it via system notification. The OTP value never appears in the search-result
preview, so other processes on the session bus cannot poll for it.

### Import & export
Migration to and from other authenticator apps:
- [Aegis](https://github.com/beemdevelopment/Aegis) (encrypted and plain)
- [AuthenticatorPro](https://github.com/jamie-mh/AuthenticatorPro) (encrypted and plain)
- [2FAS](https://github.com/twofas) (encrypted and plain)
- [FreeOTPPlus](https://github.com/helloworld1/FreeOTPPlus) (plain, key URI format)
- Google migration QR codes (import only)

### Backup & restore
**Settings → Backup** has four buttons covering both your app preferences (saved
as JSON) and your tokens (a byte-for-byte copy of the encrypted database, written
with `0600` perms). *Restore tokens* opens the saved file as an additional
database in the sidebar — the previously-active database stays on disk untouched,
so restore is non-destructive.

A reminder banner appears on the main window when no token backup has ever been
taken, or when the last one is more than 30 days old. Its **Back up Now** button
runs the same flow as *Settings → Backup → Back up tokens*. The reminder can be
snoozed for 7 days from the primary menu (**Snooze Backup Reminder**), and it
hides automatically once a backup completes. The Export menu (Ctrl+E) is for
migration to other apps and does **not** count as a backup.

## Security

- Local database encrypted with AES256-GCM
- Key derived via Argon2id (default: 4 iterations, 128 MiB memory, parallelism 4 — configurable per database)
- Decrypted content held in libgcrypt secure memory, never written to disk
- Integration with the OS secret service via libsecret

### Security model
What is protected:
- **On-disk database**: encrypted with AES256-GCM; the key is derived from
  your password with Argon2id (parameters configurable per database).
- **Secrets in RAM**: derived keys, the decrypted token JSON, and per-token
  secrets all live in libgcrypt secure memory. Those pages are `mlock`'d, so
  they will not be paged to swap or written to a hibernation image.
- **Crash dumps**: `PR_SET_DUMPABLE=0` and `RLIMIT_CORE=0` are set at startup,
  so a crash with secrets in memory will not produce a core file.
- **Clipboard hygiene**: copied OTPs are wiped after a configurable timeout
  (default 30 s), on database lock (manual, idle auto-lock, or screensaver),
  and on app exit (including SIGINT / SIGTERM / SIGHUP).

What is **not** defended against:
- A same-UID attacker with `ptrace` or `/proc/PID/mem` access can read live
  secrets while the database is unlocked. Distro-default
  `kernel.yama.ptrace_scope=1` mitigates this for unrelated processes.
- A cold-boot or DMA attack against a running machine can recover secrets
  from RAM regardless of whether the database is locked. Database lock
  provides UI gating; it does not scrub secrets from process memory.

The search-provider daemon caches its own derived key and entry list with a
60 s TTL plus per-database file-monitor invalidation, independent of the
GUI's lock state.

## Installation
OTPClient is available as a Flatpak and in several distro repositories. See the
[packages list](https://github.com/paolostivanin/OTPClient/wiki/Tested-OS-&-Packages#packages)
for details.

### Building from source
1. Install all the libraries listed under [requirements](#requirements).
2. Configure, build, and install:
```sh
git clone https://github.com/paolostivanin/OTPClient.git
cd OTPClient
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

#### Build options
All targets are built by default; pass `-D<OPTION>=OFF` to skip one.

| Option                     | Default | Description                                                          |
|----------------------------|---------|----------------------------------------------------------------------|
| `BUILD_GUI`                | `ON`    | Build the GTK4/libadwaita app (`otpclient`)                          |
| `BUILD_CLI`                | `ON`    | Build the command-line interface (`otpclient-cli`) and completions   |
| `BUILD_SEARCH_PROVIDER`    | `ON`    | Build the GNOME Shell / KRunner D-Bus daemon                         |
| `IS_FLATPAK`               | `OFF`   | Use the flatpak app's config folder for the database                 |
| `ENABLE_MINIMIZE_TO_TRAY`  | `OFF`   | Enable minimize-to-tray support in the GUI                           |

`Release` builds enable LTO and additional hardening flags
(`-fcf-protection=full`, `-fzero-call-used-regs`, `-fstrict-flex-arrays=2`,
`-ftrivial-auto-var-init=zero`) when the toolchain supports them.

## Requirements
| Name                                                | Min Version |
|-----------------------------------------------------|-------------|
| GTK                                                 | 4.10.0      |
| libadwaita                                          | 1.5.0       |
| Glib                                                | 2.74.0      |
| GIO                                                 | 2.74.0      |
| jansson                                             | 2.13        |
| libgcrypt                                           | 1.10.1      |
| libpng                                              | 1.6.0       |
| [libcotp](https://github.com/paolostivanin/libcotp) | 4.0.0       |
| zbar                                                | 0.20        |
| protobuf-c                                          | 1.3.0       |
| uuid                                                | 2.30        |
| libsecret                                           | 0.20        |
| qrencode                                            | 4.0.0       |

GTK, libadwaita, libpng, zbar, protobuf-c, and qrencode are only required
when `BUILD_GUI=ON`.

**Note:** The system memlock limit should be at least 64 MB. Lower values may
cause issues when handling many tokens, especially when importing third-party
backups. See the
[wiki](https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations)
for how to check and adjust this.

## Wiki
For screenshots, roadmap, and usage guides, see the
[project wiki](https://github.com/paolostivanin/OTPClient/wiki).

## License
This software is released under the GPLv3 license. See the [LICENSE](LICENSE) file for details.
