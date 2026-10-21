# OTPClient

A GTK4/libadwaita application for managing TOTP and HOTP two-factor authentication
tokens in encrypted local databases, with a CLI and desktop-search integration.

This README describes the code on this branch, which may include features newer
than the [latest published release](https://github.com/paolostivanin/OTPClient/releases/latest).
For release-specific instructions, read the README at that release's tag.

## Contents

- [Features](#features)
- [Installation](#installation)
- [Usage](#usage)
  - [Getting started](#getting-started)
  - [Tokens and databases](#tokens-and-databases)
  - [Import and export](#import-and-export)
  - [HOTP upgrade compatibility](#hotp-upgrade-compatibility)
  - [Backup and restore](#backup-and-restore)
  - [Command-line interface](#command-line-interface)
  - [Desktop integration](#desktop-integration)
  - [Desktop search](#desktop-search)
- [Security](#security)
- [Building from source](#building-from-source)
  - [Requirements](#requirements)
  - [Build and install](#build-and-install)
  - [Build options](#build-options)
- [Documentation and support](#documentation-and-support)
- [License](#license)

## Features

- TOTP, HOTP, and [Steam Guard codes](https://github.com/paolostivanin/OTPClient/wiki/Steam-Support).
- SHA1, SHA256, and SHA512; manual entry and `otpauth://` URIs support
  4–10 digits and periods of 1–120 seconds.
- Multiple encrypted databases, cross-database search, token groups, and bulk actions.
- QR scanning from image files, webcam, or clipboard, plus QR display for existing tokens.
- Import and export for Aegis, AuthenticatorPro, 2FAS, and FreeOTPPlus;
  Google Authenticator migration QR import.
- Import previews, duplicate detection, and review and repair of invalid tokens.
- Configurable automatic locking, clipboard clearing, and optional system-keyring integration.
- CLI with table, JSON, and CSV output; GNOME Shell and KDE Plasma desktop search.

## Installation

### Flatpak

Install from [Flathub](https://flathub.org/apps/com.github.paolostivanin.OTPClient).
With the Flathub remote already configured:

```sh
flatpak install flathub com.github.paolostivanin.OTPClient
flatpak run com.github.paolostivanin.OTPClient
```

### Distribution packages

OTPClient is also available in several distro repositories. See the
[packages list](https://github.com/paolostivanin/OTPClient/wiki/Tested-OS-&-Packages#packages)
for details. Package versions may differ from the latest upstream release.
To compile it yourself, see [Building from source](#building-from-source).

## Usage

### Getting started

1. Launch OTPClient and create an encrypted database with a password.
2. Add a token by scanning its QR code, entering its secret manually, or importing
   a backup from another authenticator.
3. Use the token's **Action** button to copy a TOTP or generate an HOTP.
4. Save a database backup through **Settings → Backup → Back up tokens**.

Automatic locking is off by default. Enable it in Settings if you want OTPClient
to lock after inactivity or supported desktop lock events; see the
[locking behavior and limitations](SECURITY.md#memory-and-locking).

### Tokens and databases

OTPs are hidden by default. Each row's **Action** button copies a TOTP or generates
an HOTP and briefly reveals it. The row's right-click menu, double-click, Enter,
and Ctrl+C provide the same action when the token list has focus; Enter also
works from search. Selection and keyboard navigation never consume HOTP codes.
Hidden codes leave the value cell empty. The **Validity** countdown follows each
visible TOTP code, regardless of selection. Adjust this in **Settings → Display**.
Tokens can be reordered by drag and drop, and their QR codes displayed for
pairing with another device.

The sidebar lists your databases. The first database you create becomes the
default, marked with a star, and loads at startup. Clicking another database
switches the current one (shown in bold); right-click **Set as Primary** to
change the startup default.

Search can include tokens from other databases. Filter groups with
`group:<name>` or `#<name>`. Assign groups when adding or editing a token, or
through its right-click menu. **Manage Groups** renames a group across its
tokens or removes the group from them. Groups are preserved in Aegis,
AuthenticatorPro, and 2FAS import/export.

Use **Select** in the header bar to assign a group, export, or delete multiple
tokens. Only checked tokens that are visible are acted on; bulk deletion can be
undone. Results from other databases are read-only. An HOTP result from another
database opens that database first; unlock it and explicitly generate the code
there. Both the GUI and CLI save the next HOTP counter before showing a code.

Tokens that fail validation are preserved separately so the database can still
open. The banner opens a review where each token can be repaired or deleted.
Deleting a token from this review requires confirmation and cannot be undone.

### Import and export

Migration to and from other authenticator apps:
- [Aegis](https://github.com/beemdevelopment/Aegis) (encrypted and plain)
- [AuthenticatorPro](https://github.com/jamie-mh/AuthenticatorPro) (encrypted and plain)
- [2FAS](https://github.com/twofas) (encrypted and plain)
- [FreeOTPPlus](https://github.com/helloworld1/FreeOTPPlus) (plain, key URI format)
- Google migration QR codes (import only)

In the GUI, choosing a file shows a preview first: exact duplicates are unchecked, tokens
that share an issuer and account but differ in secret are flagged, and only
checked tokens are imported.

Encrypted exports require a nonempty password. Existing encrypted exports with
empty passwords can still be imported. Partial imports report the number and
reasons for skipped entries; review those warnings before deleting the source.
The CLI keeps a successful exit status when valid entries were imported and
prints warnings to stderr. Entirely invalid input fails without changing tokens.

Plain exports contain token secrets without encryption. See
[Exports and external copies](SECURITY.md#exports-and-external-copies).

### HOTP upgrade compatibility

The stored counter means the next unused code in both interfaces. Older GUI
versions stored the last generated counter, while the CLI stored the next one.
Existing values are preserved because the last writer cannot be inferred. If an
existing HOTP account rejects its first code after upgrading, generate the next
code or resynchronize its counter with the provider. Newly imported counters
are interpreted as the next unused value.

### Backup and restore

**Settings -> Backup** has four buttons covering both your app preferences (saved
as JSON) and your tokens (a byte-for-byte copy of the encrypted database, written
with `0600` permissions). *Restore tokens* opens the saved file as an additional
database in the sidebar - the previously-active database stays on disk untouched,
so restore is non-destructive.

A reminder banner appears when no backup is recorded for the active database,
or when its last recorded backup is more than 30 days old. Backup history and
snoozes are tracked separately for each database. The older global timestamp
could not identify which database it referred to, so it is not carried over:
after upgrading, every database reads "No backup recorded" until you take one,
and any active snooze is reset. Its **Back up Now** button
runs the same flow as *Settings -> Backup -> Back up tokens*. The reminder can be
snoozed for 7 days from the primary menu (**Snooze Backup Reminder**), and it
hides automatically once a backup completes. The Export menu (Ctrl+E) is for
migration to other apps and does **not** count as a backup. CLI migration exports
follow the same rule.

### Command-line interface

`otpclient-cli` uses the same databases as the GUI. For example:

```sh
# List configured databases by name.
otpclient-cli --list-databases

# List accounts and issuers in the default database.
otpclient-cli --list

# Show an exact account/issuer match as JSON.
otpclient-cli --show --account alice --issuer Example --match-exact --output=json
```

Use your token's actual account and issuer in the last command. With Flatpak,
replace `otpclient-cli` with
`flatpak run --command=otpclient-cli com.github.paolostivanin.OTPClient`.

| Option | Purpose |
|--------|---------|
| `--database` | Select a database by path or configured name. |
| `--account`, `--issuer`, `--match-exact` | Select the token for `--show`. |
| `--show-next` | Also show the next TOTP. |
| `--output={table,json,csv}` | Format `--show`, `--list`, or `--list-databases` output. |
| `--import`, `--export`, `--type` | Import or export a migration backup; `--list-types` lists format identifiers. |
| `--file`, `--output-dir` | Set the input file or export directory; `--output-dir` is unavailable in Flatpak. |
| `--password-file` | Read the database password from a regular file with permissions `0600` or stricter; symlinks are rejected. |
| `--export-settings`, `--import-settings` | Back up or restore GSettings preferences as JSON. |

Bash, zsh, and fish completions are installed with the CLI. See
`otpclient-cli --help` and the [CLI manual](man/otpclient-cli.1) for all options.

### Desktop integration

Configure these optional features in **Settings → Integration**:

- **Minimize to tray:** built in by default, but off until enabled; requires a system tray.
- **Start minimized:** also available as `otpclient --start-minimized`. Requires
  minimize-to-tray and an available tray; otherwise the window is shown. The
  database starts locked and asks for its password when you show the window.
- **Start at login:** native builds write
  `~/.config/autostart/com.github.paolostivanin.OTPClient.desktop`. Flatpak uses
  the Background portal; the setting is unavailable when the desktop does not
  support the required portal functionality. It follows the start-minimized preference.

### Desktop search

`otpclient-search-provider` is a separate D-Bus daemon for **GNOME Shell Activities
Search** and **KDE Plasma 6 KRunner**. Enable Secret Service integration and save
the database password in your keyring to use it. The provider must also be
enabled with a nonempty trigger keyword (default: `otp`).

Type the keyword followed by a query, such as `otp github`. Selecting a result
computes the OTP, sends a notification, and copies it to the clipboard. Search
previews show token labels, without OTP values. Keyword changes take effect
without restarting.

On KDE Plasma, clipboard delivery uses Klipper. Elsewhere it needs `wl-copy` on
Wayland or `xclip` / `xsel` on X11. Native installations need these tools installed;
the Flatpak bundles clipboard helpers. Activation from the Plasma application
launcher's Enter key may be slower than clicking a result or using KRunner.

The keyword is a query filter, not a password. Desktop search operates
independently of the GUI lock and does not automatically clear copied codes.
Read the [search-provider security model](SECURITY.md#desktop-search) before enabling it.

## Security

Databases use AES-256-GCM with an authenticated v3 header. Argon2id derives the
key from your password; defaults are 4 iterations, 128 MiB memory, and parallelism
4, configurable per database. Libgcrypt secure memory is used for decrypted
JSON and cached keys, with cleanup on locking subject to ongoing operations.

GUI clipboard clearing defaults to 30 seconds and also runs on lock and exit
while OTPClient still owns the clipboard. Optional keyring integration stores
the database password through the OS Secret Service.

See [SECURITY.md](SECURITY.md) for the full security model, limitations, supported
versions, and [private vulnerability reporting](SECURITY.md#reporting-a-vulnerability).

## Building from source

### Requirements

Install a C11 compiler, CMake **3.25 or newer**, pkg-config, a build tool such as
Make or Ninja, and the development packages for these libraries:

| Name                                                | Min Version |
|-----------------------------------------------------|-------------|
| GTK                                                 | 4.10.0      |
| libadwaita                                          | 1.5.0       |
| gdk-pixbuf                                           | -           |
| GLib                                                | 2.74.0      |
| GIO                                                 | 2.74.0      |
| jansson                                             | 2.13        |
| libgcrypt                                           | 1.10.1      |
| [libcotp](https://github.com/paolostivanin/libcotp) | 4.0.0       |
| zbar                                                | 0.20        |
| protobuf-c                                          | 1.3.0       |
| uuid                                                | 2.30        |
| libsecret                                           | 0.20        |
| qrencode                                            | 4.0.0       |

GTK, libadwaita, gdk-pixbuf, zbar, protobuf-c, and qrencode are only required
when `BUILD_GUI=ON`.

The integration tests (`BUILD_TESTING=ON`, the default) additionally use Python 3,
`dbus-daemon`, and `glib-compile-schemas`; GUI integration tests also use `Xvfb`.
A missing runtime tool skips the test that needs it, and CMake reports which
ones it skipped. Use `-DBUILD_TESTING=OFF` to leave the tests
out altogether. Tests run against private settings, databases, a D-Bus session,
and a virtual display, never the ones you are logged into.

OTPClient uses a secure-memory pool of up to 64 MiB and reserves another 1 MiB
of the memlock budget for other library allocations when possible. With lower
limits, it reduces the pool; large databases or imports may need a higher limit.
See the
[wiki](https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations)
for how to check and adjust this.

### Build and install

```sh
git clone https://github.com/paolostivanin/OTPClient.git
cd OTPClient
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

These commands build the default branch. To build a published release, check out
its tag before configuring. Installation also invokes `glib-compile-schemas`
and `gtk4-update-icon-cache`; install the corresponding GLib and GTK tools.

For automated checks and their runtime dependencies, see [Tests](tests/README.md).

### Build options

The GUI, CLI, search provider, and tests are enabled by default. Pass
`-D<OPTION>=OFF` to disable a component.

| Option                     | Default | Description                                                          |
|----------------------------|---------|----------------------------------------------------------------------|
| `BUILD_GUI`                | `ON`    | Build the GTK4/libadwaita app (`otpclient`)                          |
| `BUILD_CLI`                | `ON`    | Build the command-line interface (`otpclient-cli`) and completions   |
| `BUILD_SEARCH_PROVIDER`    | `ON`    | Build the GNOME Shell / KRunner D-Bus daemon                         |
| `BUILD_TESTING`            | `ON`    | Build tests and register available CTest checks                    |
| `IS_FLATPAK`               | `OFF`   | Use the flatpak app's config folder for the database                 |
| `ENABLE_MINIMIZE_TO_TRAY`  | `ON`    | Offer minimize-to-tray in the GUI (needs a StatusNotifierWatcher)    |
| `SANITIZE`                 | `OFF`   | Build with AddressSanitizer + UndefinedBehaviorSanitizer             |

GNU/Clang builds are position-independent (`-fPIE`, linked with `-pie`), and
Linux links are hardened with `-Wl,-z,relro,-z,now`, `--as-needed`, and
`--no-undefined`. The flags `-fcf-protection=full`,
`-fzero-call-used-regs=used-gpr`, `-fstrict-flex-arrays=2`, and
`-ftrivial-auto-var-init=zero` are added whenever the toolchain supports them,
and `Release` builds enable LTO when supported (except with `SANITIZE=ON`).

## Documentation and support

- [Project wiki](https://github.com/paolostivanin/OTPClient/wiki): usage guides,
  screenshots, and roadmap.
- [Releases](https://github.com/paolostivanin/OTPClient/releases): changes and downloads.
- [GUI manual](man/otpclient.1) and [CLI manual](man/otpclient-cli.1): command options;
  native installations also provide `man otpclient` and `man otpclient-cli`.
- [Tests](tests/README.md): running the automated checks and what they cover.
- [Issue tracker](https://github.com/paolostivanin/OTPClient/issues): bugs and feature requests.
- [Security policy](SECURITY.md#reporting-a-vulnerability): report vulnerabilities privately.

## License

This software is released under the GPLv3 license. See [LICENSE](LICENSE) for details.
