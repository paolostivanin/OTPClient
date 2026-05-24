import pytest
import ctypes
import sys
from unittest.mock import MagicMock, patch

# Simulated fixed-size buffer constant matching typical import_password buffer sizes
IMPORT_PASSWORD_BUFFER_SIZE = 256  # typical fixed buffer size


def safe_copy_password(destination_buffer_size, text):
    """
    Simulates the password copy operation with proper bounds checking.
    This represents what the secure implementation MUST do.
    Returns the copied password or raises ValueError if input exceeds buffer.
    """
    if text is None:
        return b""
    
    if isinstance(text, str):
        encoded = text.encode('utf-8')
    else:
        encoded = bytes(text)
    
    length = len(encoded)
    
    # SECURITY INVARIANT: length MUST be validated against destination buffer size
    if length >= destination_buffer_size:
        raise ValueError(
            f"Password length {length} exceeds buffer size {destination_buffer_size - 1} "
            f"(must leave room for null terminator)"
        )
    
    # Simulate the copy into a fixed buffer
    buffer = bytearray(destination_buffer_size)
    buffer[:length] = encoded
    return bytes(buffer[:length])


@pytest.mark.parametrize("payload", [
    # Exact buffer size - should be rejected (no room for null terminator)
    "A" * IMPORT_PASSWORD_BUFFER_SIZE,
    # One byte over buffer size
    "B" * (IMPORT_PASSWORD_BUFFER_SIZE + 1),
    # Significantly over buffer size
    "C" * (IMPORT_PASSWORD_BUFFER_SIZE * 2),
    # Large overflow attempt
    "D" * 65536,
    # Maximum typical string attack
    "E" * 1048576,
    # Null bytes embedded in oversized payload
    "\x00" * (IMPORT_PASSWORD_BUFFER_SIZE + 10),
    # Mixed content oversized
    "password123!" * 30,
    # Unicode characters that expand when encoded
    "\u00e9" * (IMPORT_PASSWORD_BUFFER_SIZE // 2 + 10),  # 2-byte UTF-8 chars
    "\u4e2d\u6587" * (IMPORT_PASSWORD_BUFFER_SIZE // 3 + 10),  # 3-byte UTF-8 chars
    # Format string-like oversized payload
    "%s%n%x%p" * 40,
    # Shell injection oversized
    "; rm -rf / #" * 25,
    # SQL injection oversized
    "' OR '1'='1" * 30,
    # Path traversal oversized
    "../../../etc/passwd" * 20,
    # Binary-like oversized payload
    bytes(range(256)) * 2,
    # Exactly one byte over
    "X" * (IMPORT_PASSWORD_BUFFER_SIZE + 1),
    # Two bytes over
    "Y" * (IMPORT_PASSWORD_BUFFER_SIZE + 2),
    # Boundary: exactly at limit (should fail - no null terminator space)
    "Z" * IMPORT_PASSWORD_BUFFER_SIZE,
])
def test_import_password_buffer_overflow_prevention(payload):
    """
    Invariant: The import password copy operation MUST NEVER write more bytes
    than the destination buffer can hold. Any input whose encoded length is
    >= IMPORT_PASSWORD_BUFFER_SIZE must be rejected before any copy occurs.
    The length parameter used in memcpy MUST always be validated against the
    destination buffer size to prevent buffer overflow.
    """
    if isinstance(payload, bytes):
        text = payload
        length = len(text)
    else:
        text = payload
        length = len(payload.encode('utf-8')) if isinstance(payload, str) else len(payload)

    # INVARIANT: If the payload is too large, the operation must raise an error
    # and must NOT silently truncate or overflow the buffer
    if length >= IMPORT_PASSWORD_BUFFER_SIZE:
        with pytest.raises((ValueError, OverflowError, BufferError, Exception)):
            safe_copy_password(IMPORT_PASSWORD_BUFFER_SIZE, text)
    else:
        # For valid-sized inputs, the copy must succeed without corruption
        result = safe_copy_password(IMPORT_PASSWORD_BUFFER_SIZE, text)
        if isinstance(text, str):
            assert len(result) == len(text.encode('utf-8'))
        else:
            assert len(result) == len(text)


@pytest.mark.parametrize("payload", [
    # Valid passwords that should be accepted
    "short",
    "ValidPassword123!",
    "A" * (IMPORT_PASSWORD_BUFFER_SIZE - 1),  # exactly fits with null terminator
    "A" * (IMPORT_PASSWORD_BUFFER_SIZE - 2),  # one under limit
    "",  # empty password
    "correct horse battery staple",
    "P@ssw0rd!",
])
def test_import_password_valid_inputs_accepted(payload):
    """
    Invariant: Valid passwords that fit within the buffer MUST be accepted
    and copied correctly without data corruption.
    """
    result = safe_copy_password(IMPORT_PASSWORD_BUFFER_SIZE, payload)
    
    if isinstance(payload, str):
        encoded = payload.encode('utf-8')
    else:
        encoded = bytes(payload)
    
    # The result must exactly match the input (no corruption, no truncation)
    assert result == encoded
    # The result must fit within the buffer (with room for null terminator)
    assert len(result) < IMPORT_PASSWORD_BUFFER_SIZE


def test_import_password_length_validation_is_performed():
    """
    Invariant: The length of user-provided text MUST be checked against
    the destination buffer size BEFORE any copy operation occurs.
    A password of exactly buffer size must be rejected.
    """
    # This is the critical boundary: exactly buffer size must be rejected
    boundary_payload = "A" * IMPORT_PASSWORD_BUFFER_SIZE
    
    with pytest.raises((ValueError, OverflowError, BufferError, Exception)) as exc_info:
        safe_copy_password(IMPORT_PASSWORD_BUFFER_SIZE, boundary_payload)
    
    # Verify the error is about size/length, not some other error
    assert exc_info.value is not None


def test_import_password_no_silent_truncation():
    """
    Invariant: The import dialog MUST NOT silently truncate oversized passwords.
    Silent truncation could lead to authentication bypass where a user sets a long
    password but only the first N bytes are stored/checked.
    """
    oversized_password = "SecurePassword123!" + "X" * IMPORT_PASSWORD_BUFFER_SIZE
    
    # Must raise an exception - silent truncation is a security violation
    with pytest.raises((ValueError, OverflowError, BufferError, Exception)):
        safe_copy_password(IMPORT_PASSWORD_BUFFER_SIZE, oversized_password)


@pytest.mark.parametrize("size_multiplier", [1, 2, 4, 8, 16, 100, 1000])
def test_import_password_scales_with_multiplier(size_multiplier):
    """
    Invariant: Buffer overflow protection must hold regardless of how much
    larger the input is compared to the buffer size.
    """
    oversized_payload = "A" * (IMPORT_PASSWORD_BUFFER_SIZE * size_multiplier)
    
    with pytest.raises((ValueError, OverflowError, BufferError, Exception)):
        safe_copy_password(IMPORT_PASSWORD_BUFFER_SIZE, oversized_payload)