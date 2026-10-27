"""Password hashing helpers using versioned PBKDF2-HMAC-SHA256 hashes."""

import base64
import hashlib
import hmac
import os

ALGORITHM = "pbkdf2_sha256"
ITERATIONS = 600_000
SALT_BYTES = 16
KEY_BYTES = 32
MAX_ACCEPTED_ITERATIONS = 2_000_000


def hash_password(password: str) -> str:
    salt = os.urandom(SALT_BYTES)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, ITERATIONS, dklen=KEY_BYTES)
    encode = lambda value: base64.urlsafe_b64encode(value).decode("ascii").rstrip("=")
    return f"{ALGORITHM}${ITERATIONS}${encode(salt)}${encode(digest)}"


def verify_password(password: str, encoded_hash: str) -> bool:
    try:
        algorithm, iterations_text, salt_text, expected_text = encoded_hash.split("$", 3)
        iterations = int(iterations_text)
        if algorithm != ALGORITHM or not 100_000 <= iterations <= MAX_ACCEPTED_ITERATIONS:
            return False
        decode = lambda value: base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
        salt, expected = decode(salt_text), decode(expected_text)
        if len(salt) != SALT_BYTES or len(expected) != KEY_BYTES:
            return False
        actual = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iterations, dklen=KEY_BYTES)
        return hmac.compare_digest(actual, expected)
    except (ValueError, TypeError, UnicodeError):
        return False
