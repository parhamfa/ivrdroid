from __future__ import annotations

import base64

import pytest
from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app


@pytest.fixture()
def settings(tmp_path):
    return Settings(
        environment="test",
        database_url=f"sqlite:///{tmp_path / 'test.db'}",
        media_root=tmp_path / "prompts",
        recording_root=tmp_path / "recordings",
        public_base_url="https://ivrdroid.test",
        auto_create_schema=True,
        config_signing_private_key_b64=base64.b64encode(bytes(range(32))).decode("ascii"),
        data_encryption_key_b64=base64.b64encode(bytes(range(32, 64))).decode("ascii"),
        pairing_hmac_key_b64=base64.b64encode(bytes(range(64, 96))).decode("ascii"),
        recording_encryption_key_b64=base64.b64encode(bytes(range(96, 128))).decode("ascii"),
        cf_device_service_client_id="service-id",
        cf_device_service_client_secret="service-secret",
        development_admin_email="owner@example.com",
    )


@pytest.fixture()
def client(settings):
    with TestClient(create_app(settings)) as value:
        yield value
