from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


def _decode_key(value: str, expected_bytes: int, label: str) -> bytes:
    try:
        decoded = base64.b64decode(value, validate=True)
    except Exception as error:  # pragma: no cover - library exception varies
        raise RuntimeError(f"{label} is not valid base64") from error
    if len(decoded) != expected_bytes:
        raise RuntimeError(f"{label} must decode to {expected_bytes} bytes")
    return decoded


def canonical_json_bytes(document: dict) -> bytes:
    return json.dumps(
        document,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")


class RevisionSigner:
    def __init__(self, private_key_b64: str):
        raw = _decode_key(private_key_b64, 32, "configuration signing key")
        self._key = Ed25519PrivateKey.from_private_bytes(raw)

    def sign(self, document: dict) -> tuple[str, str]:
        canonical = canonical_json_bytes(document)
        digest = hashlib.sha256(canonical).hexdigest()
        signature = base64.b64encode(self._key.sign(canonical)).decode("ascii")
        return digest, signature

    def public_key_b64(self) -> str:
        public = self._key.public_key().public_bytes_raw()
        return base64.b64encode(public).decode("ascii")


class DataCipher:
    def __init__(self, key_b64: str):
        self._cipher = AESGCM(_decode_key(key_b64, 32, "data encryption key"))

    def encrypt(self, plaintext: str) -> str:
        nonce = os.urandom(12)
        encrypted = self._cipher.encrypt(nonce, plaintext.encode("utf-8"), b"ivrdroid-v1")
        return base64.b64encode(nonce + encrypted).decode("ascii")

    def decrypt(self, encoded: str) -> str:
        payload = base64.b64decode(encoded, validate=True)
        if len(payload) < 29:
            raise ValueError("encrypted value is truncated")
        return self._cipher.decrypt(payload[:12], payload[12:], b"ivrdroid-v1").decode("utf-8")


def digest_pairing_code(code: str, hmac_key_b64: str) -> str:
    key = _decode_key(hmac_key_b64, 32, "pairing HMAC key")
    return hmac.new(key, code.encode("ascii"), hashlib.sha256).hexdigest()


def digest_device_token(token: str) -> str:
    return hashlib.sha256(token.encode("ascii")).hexdigest()


def mask_phone(phone: str | None) -> str:
    if not phone:
        return "Unknown"
    if len(phone) <= 7:
        return "***"
    return f"{phone[:3]}-***-{phone[-4:]}"
