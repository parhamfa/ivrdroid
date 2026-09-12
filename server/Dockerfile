FROM --platform=linux/amd64 python:3.13.9-slim-bookworm@sha256:8cdd496996889eef0a8a7921c152c0a4a9f18a26c7a5e7257d3c90b431891894

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1

RUN apt-get update \
    && apt-get install -y --no-install-recommends ffmpeg ca-certificates gosu \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 ivrdroid

WORKDIR /app
COPY pyproject.toml alembic.ini ./
COPY alembic ./alembic
COPY app ./app
COPY docker-entrypoint.sh /usr/local/bin/ivrdroid-entrypoint
RUN pip install --no-cache-dir . \
    && mkdir -p /data/prompts /data/recordings \
    && chown -R ivrdroid:ivrdroid /data/prompts /data/recordings /app \
    && chmod 0700 /data/recordings \
    && chmod 0755 /usr/local/bin/ivrdroid-entrypoint

ARG SOURCE_COMMIT=unknown
ENV IVRDROID_SOURCE_COMMIT=${SOURCE_COMMIT}
LABEL org.opencontainers.image.version="0.9.0" org.opencontainers.image.revision=${SOURCE_COMMIT}
EXPOSE 8000
ENTRYPOINT ["ivrdroid-entrypoint"]
CMD ["sh", "-c", "alembic upgrade head && exec uvicorn app.main:app --host 0.0.0.0 --port 8000 --proxy-headers --forwarded-allow-ips=172.16.0.0/12"]
