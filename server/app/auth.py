from __future__ import annotations

from functools import lru_cache
from urllib.parse import urlparse

import jwt
from fastapi import Depends, Header, HTTPException, Request, status
from jwt import PyJWKClient
from sqlalchemy import select
from sqlalchemy.orm import Session

from .crypto import digest_device_token
from .database import get_session
from .models import Device


@lru_cache(maxsize=8)
def _jwk_client(team_domain: str) -> PyJWKClient:
    return PyJWKClient(f"https://{team_domain}/cdn-cgi/access/certs", cache_keys=True)


def require_admin(
    request: Request,
    cf_access_jwt: str | None = Header(default=None, alias="Cf-Access-Jwt-Assertion"),
) -> str:
    settings = request.app.state.settings
    if settings.environment != "production" and not cf_access_jwt:
        return settings.development_admin_email
    if not cf_access_jwt:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Access login required")
    try:
        signing_key = _jwk_client(settings.cf_access_team_domain).get_signing_key_from_jwt(
            cf_access_jwt,
        )
        claims = jwt.decode(
            cf_access_jwt,
            signing_key.key,
            algorithms=["RS256"],
            audience=settings.cf_access_audience,
            options={"require": ["exp", "iat", "aud"]},
        )
        email = claims.get("email")
        if not isinstance(email, str) or not email:
            raise ValueError("email claim missing")
        return email
    except Exception as error:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid Access assertion",
        ) from error


def require_admin_write(
    request: Request,
    actor: str = Depends(require_admin),
    origin: str | None = Header(default=None),
) -> str:
    settings = request.app.state.settings
    if settings.environment == "production":
        if not origin:
            raise HTTPException(status_code=403, detail="Origin header required")
        expected = urlparse(settings.public_base_url)
        observed = urlparse(origin)
        if (observed.scheme, observed.netloc) != (expected.scheme, expected.netloc):
            raise HTTPException(status_code=403, detail="Cross-origin write rejected")
    return actor


def require_device(
    authorization: str | None = Header(default=None),
    session: Session = Depends(get_session),
) -> Device:
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="Device credential required")
    token = authorization.removeprefix("Bearer ").strip()
    if len(token) < 32 or len(token) > 256:
        raise HTTPException(status_code=401, detail="Invalid device credential")
    device = session.scalar(
        select(Device).where(Device.token_hash == digest_device_token(token)),
    )
    if not device or device.revoked_at is not None:
        raise HTTPException(status_code=401, detail="Device credential revoked")
    return device
