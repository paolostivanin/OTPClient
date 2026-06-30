# Security Policy

## Supported Versions

Only the versions listed below receive security updates.

| Version | Status      |
|---------|-------------|
| 5.1.x   | Active      |
| 5.0.x   | Maintenance |
| < 5.0   | End of life |

- **Active** - current release line. Receives new features, bug fixes, and security patches.
- **Maintenance** - previous release line. Receives security patches and critical bug fixes only; no new features.
- **End of life** - no longer supported. No fixes of any kind; please upgrade.

## Security architecture

- **Database at rest:** the database is encrypted with AES-256-GCM (authenticated
  encryption). The key is derived from your password with Argon2id, with parameters
  stored per database (defaults: 4 iterations, 128 MiB memory, parallelism 4). The
  parameters are bounds-checked on load and out-of-range values are rejected
  (iterations 1 to 64, memory 8 MiB to 1 GiB, parallelism 1 to 16). Files use the v3
  on-disk format: a portable big-endian header (magic, version, IV, salt, and the
  Argon2id parameters) that is fully covered by the GCM tag, so tampering with any
  header field is detected. v1 and v2 databases are read transparently and upgraded
  to v3 on the next save.
- **Secrets in memory:** derived keys, the decrypted token JSON, and per-token secrets
  live in libgcrypt secure memory. Those pages are `mlock`'d, so they are not paged to
  swap or written to a hibernation image, and they are wiped with `explicit_bzero`
  before being freed. On lock (manual, idle auto-lock, screensaver, or system suspend
  via logind) the master key and the decrypted database are purged from memory;
  unlocking re-derives the key rather than comparing a resident copy.
- **Process hardening:** `PR_SET_DUMPABLE=0` and `RLIMIT_CORE=0` are set at startup so a
  crash will not produce a core file containing secrets. Builds enable a stack
  protector, `_FORTIFY_SOURCE=3`, full RELRO with BIND_NOW, and PIE, plus optional
  control-flow protection, register clearing, strict flexible-array bounds, and
  automatic variable initialization when the toolchain supports them.
- **Clipboard:** copied OTPs are wiped after a configurable timeout, on database lock,
  and on exit (including SIGINT, SIGTERM, and SIGHUP).
- **Search provider (opt-in):** the desktop search provider is gated behind a trigger
  keyword and is off until configured. Activation IDs are single-use, random 128-bit
  capability tokens with a 30-second TTL; OTP delivery is rate-limited through a single
  global bucket; and the derived key and caches are wiped after a period of inactivity.
- **Out of scope:** a same-UID attacker with `ptrace` or `/proc/PID/mem` access can read
  live secrets while the database is unlocked (distro-default
  `kernel.yama.ptrace_scope=1` mitigates this for unrelated processes), and a cold-boot
  or DMA attack against a running, unlocked machine can recover secrets from RAM. Once
  the database is locked, the master key and decrypted database are no longer resident.

## Reporting a Vulnerability

To report a vulnerability, email [info@paolostivanin.com](mailto:info@paolostivanin.com). Please do not open a public issue.

1. **Acknowledgment** - you will receive a reply within 24 hours confirming receipt and an initial assessment.
2. **Fix** - a patch will be developed and released within 7 days.
3. **Disclosure** - once the fix is published, a [security advisory](https://github.com/paolostivanin/OTPClient/security/advisories) will be opened on GitHub.
