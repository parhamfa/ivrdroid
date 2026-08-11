#!/bin/sh

set -eu

load_secret() {
    variable_name=$1
    eval "file_path=\${${variable_name}_FILE:-}"
    if [ -z "$file_path" ]; then
        return
    fi
    if [ ! -f "$file_path" ]; then
        echo "Required secret file for $variable_name is unavailable." >&2
        exit 1
    fi
    secret_value=$(tr -d '\r\n' < "$file_path")
    if [ -z "$secret_value" ]; then
        echo "Required secret file for $variable_name is empty." >&2
        exit 1
    fi
    export "$variable_name=$secret_value"
    unset "${variable_name}_FILE"
}

load_secret IVRDROID_CONFIG_SIGNING_PRIVATE_KEY_B64
load_secret IVRDROID_DATA_ENCRYPTION_KEY_B64
load_secret IVRDROID_PAIRING_HMAC_KEY_B64
load_secret IVRDROID_RECORDING_ENCRYPTION_KEY_B64
load_secret IVRDROID_RECORDING_ENCRYPTION_PREVIOUS_KEYS_JSON
load_secret IVRDROID_CF_DEVICE_SERVICE_CLIENT_SECRET

if [ -n "${IVRDROID_DATABASE_PASSWORD_FILE:-}" ]; then
    if [ ! -f "$IVRDROID_DATABASE_PASSWORD_FILE" ]; then
        echo "Required database password file is unavailable." >&2
        exit 1
    fi
    database_password=$(tr -d '\r\n' < "$IVRDROID_DATABASE_PASSWORD_FILE")
    if [ -z "$database_password" ]; then
        echo "Required database password file is empty." >&2
        exit 1
    fi
    export IVRDROID_DATABASE_URL="postgresql+psycopg://ivrdroid:${database_password}@db:5432/ivrdroid"
    unset IVRDROID_DATABASE_PASSWORD_FILE
fi

exec gosu ivrdroid "$@"
