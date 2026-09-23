# OTPClient
A highly secure GTK4/libadwaita application for managing TOTP and HOTP two-factor authentication tokens, with CLI and desktop-search integration.

## Features

### Supported standards
- TOTP and HOTP
- Manual entry: digits 4-10, period 1-120 s
- `otpauth://` URIs (import / display / QR): digits 4-10, period 1-120 s
- SHA1, SHA256, and SHA512 algorithms
- Steam guard codes ([details](https://github.com/paolostivanin/OTPClient/wiki/Steam-Support))

### GUI
- Token list with drag-and-drop reordering
- OTPs are hidden by default. Each row's button in the **Action** column copies
  (TOTP) or generates (HOTP) its code and briefly reveals it, and the same action
  heads the row's right-click menu. Double-clicking a row does the same, as do
  Enter and Ctrl+C when the token list has focus; Enter also works from search.
  Selection and keyboard navigation never consume HOTP codes.
  Hidden codes leave the value cell empty, and the **Validity** countdown
  follows the code: every TOTP that is showing one has a countdown, whether or
  not its row is selected. Display behavior is configurable
  in *Settings -> Display*
- Multiple encrypted databases listed in the sidebar. The first database
  you create becomes the default - it loads automatically on startup and is
  marked with a star. Clicking another row switches the currently open
  database (shown in bold) without changing the default; right-click ->
  *Set as Primary* changes which database is loaded on next startup.
- Cross-database search (`group:<name>` / `#<name>` syntax for filtering)
- Token grouping: assign tokens to groups (e.g. "Work", "Personal") via right-click,
  the edit dialog, or when adding a token; groups are preserved during Aegis,
  AuthenticatorPro, and 2FAS import/export
- Add tokens by scanning a QR code from an image file, the webcam, or the clipboard, or by entering the secret manually
- QR code display for any token (re-pairing or sharing across devices)
- Idle and screensaver auto-lock with configurable timeout
- Configurable clipboard wipe, also applied on lock and exit while OTPClient
  still owns the clipboard. Copying something elsewhere cancels the wipe and
  automatic next-code copying.
- HOTP counters are saved before a code is shown or copied. The GUI and CLI
  share the same counter convention. A HOTP result from another database opens
  that database first; unlock it and explicitly generate the code there.
- Locking closes sensitive dialogs and clears their QR codes, secrets, and
  passwords, including when an outstanding file chooser retains the dialog
- Optional minimize-to-tray (built in by default, off until you enable it)
- Optional start-minimized, from *Settings -> Integration* or with
  `otpclient --start-minimized`. Requires minimize-to-tray and a system tray;
  the database is left locked, so the first time you show the window it asks
  for the password
- Optional start-at-login, from *Settings -> Integration*. Native builds write
  `~/.config/autostart/com.github.paolostivanin.OTPClient.desktop`; the Flatpak
  goes through `org.freedesktop.portal.Background`, which a few desktops
  (sway, Hyprland, river, LXQt, COSMIC, plain XFCE) do not implement, and there
  the row is greyed out. The entry inherits the start-minimized preference

### Command-line interface (`otpclient-cli`)
- `--show / --list` for scripting and shell integration; `--list-databases`
  prints the configured databases by name
- `--account` and `--issuer` select the token for `--show`, with `--match-exact`
  for exact matching and `--show-next` for the next TOTP
- `--database` picks a database by path or by name from `--list-databases`
- `--import / --export` against the same database the GUI uses; `--list-types`
  prints the accepted `--type` identifiers, `--file` and `--output-dir` set the
  paths (`--output-dir` is ignored in the Flatpak build)
- `--output={table,json,csv}` for machine-readable output (`--show`, `--list`,
  and `--list-databases` only)
- `--password-file` to read the master password from a file
- `--export-settings / --import-settings` for GSettings backup/restore
- Bash, zsh, and fish completions installed by default

### Desktop search (`otpclient-search-provider`)
A separate D-Bus daemon that integrates with **GNOME Shell Activities Search**
and **KDE Plasma 6 KRunner**. Type the configurable trigger keyword (default
`otp`) followed by a query - selecting a result computes the OTP, delivers it
via system notification, and copies it to the clipboard (via Klipper's D-Bus
interface on KDE Plasma; via `wl-copy` on Wayland or `xclip` / `xsel` on X11
elsewhere - those tools must be installed for the clipboard step to work
outside KDE). The OTP value never appears in the search-result preview, so
the preview does not expose codes. The provider requires Secret Service access
and a saved database password. Disabling the provider, clearing its keyword,
or disabling Secret Service immediately invalidates its caches and activation
IDs. Keyword changes take effect without restarting. The keyword filters
queries; it is not a password or an authentication mechanism.

> **KDE activation latency:** when activating a result from the Plasma
> application launcher (Kickoff, opened with the Meta key) by pressing
> **Enter**, there can be a delay of about a second before the code is copied.
> **Clicking** the result, or using **KRunner** (Alt+Space), is instant. This
> is a Plasma-side delay in how Kickoff dispatches the activation on Enter, not
> in the daemon, which computes and copies the OTP in well under 100 ms.

### Import & export
Migration to and from other authenticator apps:
- [Aegis](https://github.com/beemdevelopment/Aegis) (encrypted and plain)
- [AuthenticatorPro](https://github.com/jamie-mh/AuthenticatorPro) (encrypted and plain)
- [2FAS](https://github.com/twofas) (encrypted and plain)
- [FreeOTPPlus](https://github.com/helloworld1/FreeOTPPlus) (plain, key URI format)
- Google migration QR codes (import only)

Encrypted exports require a nonempty password. Existing encrypted exports with
empty passwords can still be imported. Partial imports report the number and
reasons for skipped entries; review those warnings before deleting the source.
The CLI keeps a successful exit status when valid entries were imported and
prints warnings to stderr. Entirely invalid input fails without changing tokens.

### HOTP upgrade compatibility
The stored counter means the next unused code in both interfaces. Older GUI
versions stored the last generated counter, while the CLI stored the next one.
Existing values are preserved because the last writer cannot be inferred. If an
existing HOTP account rejects its first code after upgrading, generate the next
code or resynchronize its counter with the provider. Newly imported counters
are interpreted as the next unused value.

### Backup & restore
**Settings -> Backup** has four buttons covering both your app preferences (saved
as JSON) and your tokens (a byte-for-byte copy of the encrypted database, written
with `0600` perms). *Restore tokens* opens the saved file as an additional
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

## Security

- Local database encrypted with AES256-GCM (v3 on-disk format with an authenticated header)
- Key derived via Argon2id (default: 4 iterations, 128 MiB memory, parallelism 4, configurable per database)
- Decrypted content held in libgcrypt secure memory, never written to disk
- Integration with the OS secret service via libsecret

### Security model
What is protected:
- **On-disk database**: encrypted with AES256-GCM; the key is derived from
  your password with Argon2id (parameters configurable per database).
- **Secrets in RAM**: derived keys, the decrypted token JSON, and per-token
  secrets all live in libgcrypt secure memory. Those pages are `mlock`'d, so
  they will not be paged to swap or written to a hibernation image. On lock
  (manual, idle auto-lock, screensaver, or system suspend), the master key and
  the decrypted database are wiped from memory; unlocking re-derives the key.
- **Crash dumps**: `PR_SET_DUMPABLE=0` and `RLIMIT_CORE=0` are set at startup,
  so a crash with secrets in memory will not produce a core file.
- **Clipboard hygiene**: in the GUI, copied OTPs are wiped after a configurable
  timeout (default 30 s), on database lock (manual, idle auto-lock, screensaver,
  or system suspend), and on app exit (including SIGINT / SIGTERM / SIGHUP). The
  search-provider daemon does **not** auto-clear: on KDE the OTP would remain
  in Klipper's history regardless (its D-Bus API has no per-entry history
  removal), so a clear timer would give a misleading sense of protection.

What is **not** defended against:
- A same-UID attacker with `ptrace` or `/proc/PID/mem` access can read live
  secrets while the database is unlocked. Distro-default
  `kernel.yama.ptrace_scope=1` mitigates this for unrelated processes.
- A cold-boot or DMA attack against a running machine with the database
  **unlocked** can recover secrets from RAM. Once the database is locked, the
  master key and decrypted database are wiped from process memory, so a
  cold-boot or DMA attack against a locked instance does not recover them.

The search-provider daemon has a 60-second entry-list cache and a derived-key
cache cleared after five minutes of inactivity, with per-database file-monitor
invalidation. Both are independent of the GUI lock and are cleared when provider
access is disabled.

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
| `ENABLE_MINIMIZE_TO_TRAY`  | `ON`    | Offer minimize-to-tray in the GUI (needs a StatusNotifierWatcher)    |
| `SANITIZE`                 | `OFF`   | Build with AddressSanitizer + UndefinedBehaviorSanitizer             |

GNU/Clang builds are position-independent (`-fPIE`, linked with `-pie`), and
Linux links are hardened with `-Wl,-z,relro,-z,now`, `--as-needed`, and
`--no-undefined`. The flags `-fcf-protection=full`,
`-fzero-call-used-regs=used-gpr`, `-fstrict-flex-arrays=2`, and
`-ftrivial-auto-var-init=zero` are added whenever the toolchain supports them,
and `Release` builds enable LTO.

## Requirements
| Name                                                | Min Version |
|-----------------------------------------------------|-------------|
| GTK                                                 | 4.10.0      |
| libadwaita                                          | 1.5.0       |
| gdk-pixbuf                                           | -           |
| Glib                                                | 2.74.0      |
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
`dbus-daemon`, and `glib-compile-schemas`, and the GUI one uses `Xvfb`. None of
these are build requirements: a missing tool skips the test that needs it, and
CMake reports which ones it skipped. Use `-DBUILD_TESTING=OFF` to leave the tests
out altogether. Tests run against private settings, databases, a D-Bus session,
and a virtual display, never the ones you are logged into.

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
