#!/bin/bash
# End-to-end smoke for otpclient-cli. Designed for sanitizer (ASan+UBSan)
# builds: the CMake SANITIZE option compiles with -fno-sanitize-recover=all,
# so any instrumented failure aborts the binary, which `set -e` here
# propagates as a CI failure.

set -euo pipefail
set -x

OTPCLIENT_CLI="${OTPCLIENT_CLI:-otpclient-cli}"
SMOKE_TMPDIR=$(mktemp -d -t otpclient-smoke-XXXXXX)
trap 'rm -rf "$SMOKE_TMPDIR"' EXIT

# Sanitizer runtime options. Leak detection is off: GLib/GTK global init
# legitimately survives until exit, and curating a suppression list is more
# maintenance than the bug-finding payoff. Re-enable here if you want it.
export ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=0:check_initialization_order=1:strict_init_order=1:detect_stack_use_after_return=1"
export UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stacktrace=1"

# Force libsecret to fail fast: there is no session bus in this container.
# (The GSettings default for secret-service is false anyway, but defence in
# depth: a fallback keyfile could flip it.)
unset DBUS_SESSION_BUS_ADDRESS DBUS_SYSTEM_BUS_ADDRESS

# Use the in-memory GSettings backend so the smoke does not depend on dconf
# being installed/running.
export GSETTINGS_BACKEND=memory

DB_PATH="$SMOKE_TMPDIR/test.db"
DB_PWFILE="$SMOKE_TMPDIR/dbpw"
URI_FILE="$SMOKE_TMPDIR/uris.txt"
EXPORT_DIR="$SMOKE_TMPDIR/export"
mkdir -p "$EXPORT_DIR"

printf 'smokepass-%s' "$$" > "$DB_PWFILE"
chmod 600 "$DB_PWFILE"

cat > "$URI_FILE" <<'EOF'
otpauth://totp/Example:alice@example.com?secret=JBSWY3DPEHPK3PXP&issuer=Example&algorithm=SHA1&digits=6&period=30
otpauth://hotp/AcmeCorp:bob@acme.com?secret=JBSWY3DPEHPK3PXP&issuer=AcmeCorp&algorithm=SHA1&digits=6&counter=0
EOF

# Phase 1: no-DB ops (option parsing, GLib/gcry init, GSettings reads).
"$OTPCLIENT_CLI" --version
"$OTPCLIENT_CLI" --list-types
"$OTPCLIENT_CLI" --list-databases
"$OTPCLIENT_CLI" --export-settings > /dev/null

# Phase 2: bootstrap an encrypted DB via import. DB password is read from the
# --password-file; the import-file password comes from stdin (the
# freeotpplus_plain parser ignores it, but the CLI requires non-empty).
echo "x" | "$OTPCLIENT_CLI" --import -t freeotpplus_plain -f "$URI_FILE" -d "$DB_PATH" -p "$DB_PWFILE"
test -s "$DB_PATH"

# Phase 3: round-trip the DB through read paths.
"$OTPCLIENT_CLI" --list -d "$DB_PATH" -p "$DB_PWFILE"
"$OTPCLIENT_CLI" --show -d "$DB_PATH" -p "$DB_PWFILE" -i "Example" -a "alice@example.com" -m
"$OTPCLIENT_CLI" --export -t freeotpplus_plain -d "$DB_PATH" -p "$DB_PWFILE" -o "$EXPORT_DIR"
test -s "$EXPORT_DIR/freeotpplus-exports.txt"

echo "SMOKE: all steps passed"
