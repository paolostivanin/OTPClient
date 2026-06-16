# Tests

OTPClient's automated tests, run via CTest after a normal build:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CI also runs an ASan + UBSan pass (`-DSANITIZE=ON`) over the same set.

Each binary uses the GLib `g_test` framework and lives in its own file. The
sections below describe what each one is meant to catch.

## OTP code generation

**`test_otp_generation`** anchors the user-visible output of the app to the
IETF reference vectors. It calls libcotp directly with the secrets and
timestamps published in RFC 6238 (TOTP, SHA1/SHA256/SHA512) and RFC 4226
(HOTP) and asserts the exact codes. If a libcotp upgrade, a build-flag
change, or a refactor in the secret-handling path ever shifts the generated
digits, these tests fail immediately.

## Database lifecycle

**`test_db_roundtrip`** exercises the happy path of the encrypted database:
save the file, reload it, and check that the JSON survives the round-trip.
It also verifies that the wrong password is rejected, that a single corrupted
ciphertext byte trips the GCM authentication tag, that changing the password
works (and that the old password no longer unlocks), and that updated Argon2
KDF parameters are persisted in the header.

**`test_db_transaction`** covers the failure paths that complement the
round-trip tests: when encryption or the atomic file write is forced to
fail, the in-memory state must roll back to exactly what it was before the
transaction. Password and KDF-parameter changes that fail must restore the
previous key and parameters. A stale snapshot (a second handle modifying
the file in between) must be detected and rejected rather than silently
clobbering the newer save.

**`test_malformed_db`** feeds the loader hand-crafted bad files: truncated
headers, future version numbers, garbled Argon2 parameters, payloads above
the secure-memory cap. None of them should crash or load partial state.

## Import formats

**`test_malformed_aegis`** and **`test_malformed_importers`** poke the
backup importers (Aegis, Authenticator Pro, 2FAS, FreeOTP+) with
deliberately broken JSON: missing required fields, unknown enum values for
algorithm and token type, malformed encrypted payloads, mixed-validity
URI files. Valid entries should still come through; invalid ones should be
skipped or rejected cleanly rather than crashing on a NULL lookup.

## URI parsing

**`test_parse_uri`** covers the basic `otpauth://` parser. **`test_parse_uri_extra`**
adds the branches the first file does not touch: HOTP URIs, SHA256 and
SHA512 algorithms, out-of-bounds period/digits/counter values, malformed
schemes, and the parse -> emit -> parse round-trip (a token built in code,
serialised to a URI, and parsed back must keep all its fields intact).

## QR codes

**`test_qrcode_parser`** scans QR codes generated in-memory at test time:
valid textures, valid JPEG files (skipped if gdk-pixbuf in the CI image
lacks the JPEG writer), images that contain no QR, corrupt image bytes,
and oversized textures that should be refused before decoding.

## Common helpers and validation

**`test_otp_validation`** and **`test_common_utils`** cover the small
utilities that everything else builds on: the JSON validator for stored
token records, the hex string helpers, `secure_strdup`, and the algorithm
name -> integer mapping. The validator tests pin every required-field and
range check so a refactor of the validation policy can't silently relax it.
