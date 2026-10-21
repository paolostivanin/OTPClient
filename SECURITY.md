# Security Policy

## Contents

- [Reporting a vulnerability](#reporting-a-vulnerability)
- [Supported versions](#supported-versions)
- [Security architecture](#security-architecture)
  - [Database encryption](#database-encryption)
  - [Memory and locking](#memory-and-locking)
  - [Process hardening](#process-hardening)
  - [Clipboard](#clipboard)
  - [Desktop search](#desktop-search)
- [Limitations](#limitations)
  - [Host and physical access](#host-and-physical-access)
  - [Swap and hibernation](#swap-and-hibernation)
  - [Exports and external copies](#exports-and-external-copies)

## Reporting a vulnerability

Report suspected vulnerabilities through the repository’s
[GitHub security advisories](https://github.com/paolostivanin/OTPClient/security/advisories).
Please do not open a public issue or publish exploit details before coordinated
disclosure.

Include, when available:

- The affected OTPClient version, operating system, and installation method
  (distribution package, Flatpak, or source build).
- Steps to reproduce, expected and observed behavior, and the potential impact.
- A minimal proof of concept or sample using synthetic tokens. Do not send real
  token secrets, database passwords, or your personal database.

We aim to acknowledge reports within **three business days**. This is a response
target, not a guaranteed deadline. After assessing the report, we will coordinate
a fix and disclosure schedule with the reporter according to severity and the
work required. There is no fixed deadline for releasing a patch. Confirmed
vulnerabilities will be documented in a
[GitHub security advisory](https://github.com/paolostivanin/OTPClient/security/advisories)
as part of coordinated disclosure, normally when a fix is available.

## Supported versions

Only the [latest published stable release](https://github.com/paolostivanin/OTPClient/releases/latest)
is supported for security updates. Fixes are delivered in a new release;
older versions and release lines do not receive maintained backports. Upgrade
to the latest stable release to receive fixes. Development branches and
prereleases are not supported stable releases.

Distribution and Flatpak packages may lag behind upstream releases. Check the
installed version and your package provider's update availability.

The architecture below describes the code on this branch. For a particular
release, consult this document at that release's tag.

## Security architecture

### Database encryption

The local database uses **AES-256-GCM** authenticated encryption. Its key is
derived from the password using **Argon2id**, with parameters stored per database:

| Parameter | Default | Accepted range |
|-----------|---------|----------------|
| Iterations | 4 | 1–64 |
| Memory | 128 MiB | 8 MiB–1 GiB |
| Parallelism | 4 | 1–16 |

Parameters are bounds-checked on load; values outside these ranges are rejected.
The v3 format uses a portable big-endian header containing the magic, version,
IV, salt, and Argon2id parameters. The header is authenticated along with the
ciphertext. Legacy v1 and v2 databases can be read and are upgraded to v3 on the
next save.

### Memory and locking

OTPClient uses libgcrypt secure memory for decrypted token JSON, cached derived
keys, and many secret buffers. Secure allocations are cleared on release, and
sensitive cleanup paths also use explicit wiping. This does not mean every
temporary value, UI representation, or library-managed copy is held in locked
memory or can be erased immediately.

Locking disables access through the GUI, closes sensitive dialogs, clears their
fields and QR displays, and requests cleanup of database secrets. Pending dialog
operations receive cancellation requests. Cleanup has these qualifications:

- A change to the password or key derivation parameters already running must finish
  before its database secrets can be purged safely.
- Background unlock, import, or export work may retain sensitive data until
  the worker completes. Cancellation does not interrupt every library call or
  undo an export that has already been written.
- The separate desktop-search process has its own caches and keyring access;
  locking the GUI does not revoke them.

Automatic locking is **off by default**. When enabled, the default inactivity
timeout is five minutes. Desktop locking requires supported screensaver/session
signals. Native builds also monitor logind's `PrepareForSleep` when available;
Flatpak builds do not subscribe to this system-bus signal and rely on supported
session signals and the inactivity timer. Automatic locking therefore depends
on both the settings and desktop integration available.

### Process hardening

During cryptographic initialization, OTPClient attempts to set
`PR_SET_DUMPABLE=0` and `RLIMIT_CORE=0` to restrict process inspection and core
dumps. Failure is reported as a warning and does not stop execution, so these
are attempted protections rather than unconditional guarantees.

Normal builds enable a stack protector and `_FORTIFY_SOURCE=3`, with PIE on
GNU/Clang and full RELRO with BIND_NOW on Linux. Control-flow protection,
register clearing, strict flexible-array bounds, and automatic variable
initialization are enabled when supported by the compiler. Sanitizer builds
change some of these flags; see the [build options](README.md#build-options).

### Clipboard

The GUI clears copied OTPs after a configurable timeout (default **30 seconds**;
zero disables the timer), on database lock, and on normal exit, including its
SIGINT, SIGTERM, and SIGHUP shutdown handlers, while it still owns the clipboard.
Copying something in another application cancels the pending clear and automatic
next-code copying. Forced termination cannot perform this cleanup.

Clipboard managers or other applications may retain their own copies. The
GUI's clearing behavior does not erase those copies, and the desktop-search
provider does not schedule automatic clipboard clearing.

### Desktop search

The provider requires Secret Service integration, a saved database password,
an enabled provider, and a nonempty trigger keyword. Secret Service integration
is off by default; the provider setting is on by default but does not grant
access without these other requirements. Saving a password delegates its
protection and access control to the system keyring.

The keyword filters queries; it is not authentication. Activation IDs are
single-use random 128-bit capability tokens with a 30-second lifetime, bound
to the requesting D-Bus caller and query. OTP delivery is rate-limited **per
D-Bus caller**. These controls do not
establish a boundary against a compromised desktop session.

The entry-list cache has a 60-second freshness limit, and database file monitors
invalidate affected cached data. Derived keys, entries, and activation IDs are
cleared after five minutes of provider inactivity. Changes to the provider's
enabled state, keyword, or Secret Service setting invalidate caches and pending
delivery authorization. These caches and the provider's ability to retrieve
passwords from the keyring are independent of the GUI lock.

Search previews expose token labels, without OTP values. Activating a result
sends the code through a notification and attempts to copy it to the clipboard.
Native notifications request transient display and a five-second expiry;
Flatpak notifications request transient display when supported and are withdrawn
after five seconds. Display and history handling depend on the desktop backend.
The provider does not automatically clear the clipboard, including Klipper's
history on KDE.

## Limitations

### Host and physical access

Database encryption protects stored data subject to the strength of the password
and KDF settings. OTPClient does not defend against a compromised operating
system, a privileged attacker, or an attacker able to inspect its process memory.
Malicious software in the same desktop session may also capture input, read
clipboard contents, or access secrets through other session services.

Locking reduces the lifetime of secrets in memory, subject to the qualifications
above. It is not a guarantee against cold-boot or DMA attacks, nor proof that
every copy has disappeared from the machine.

### Swap and hibernation

Successfully locked memory pages are protected from ordinary swapping. Memory
locking depends on operating-system support and resource limits; see the
[secure-memory requirements](README.md#requirements).

Memory locking does **not** exclude pages from a hibernation image. Linux may
write a copy of RAM to disk regardless of memory locks, as documented in
[mlock(2)](https://man7.org/linux/man-pages/man2/mlock.2.html). Protection of
hibernation images and other system-level copies depends on the host's storage
and encryption configuration.

### Exports and external copies

New encrypted migration exports require a nonempty password. Legacy encrypted
exports with empty passwords remain importable. Plain exports intentionally
write token secrets without encryption; QR codes, copied codes, and redirected
CLI output can also expose sensitive information outside the encrypted database.

Token backups made through Settings copy the encrypted database. Preferences
backups contain settings rather than token backups. OTPClient cannot revoke
external copies after they have been saved, captured, or shared.
