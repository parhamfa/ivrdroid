#!/bin/sh

set -eu

REPO_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
DIST_DIR="$REPO_DIR/app/dist"
APK="$REPO_DIR/app/build/outputs/apk/debug/app-debug.apk"
APP_VERSION=$(sed -n 's/^[[:space:]]*versionName = "\([^"]*\)"/\1/p' \
    "$REPO_DIR/app/build.gradle.kts" | head -n 1)

if [ -z "$APP_VERSION" ]; then
    echo "Android app version is missing." >&2
    exit 1
fi

PACKAGE="$DIST_DIR/IVRdroid-system-app-$APP_VERSION.zip"

cd "$REPO_DIR"
./gradlew assembleDebug >/dev/null

python3 "$REPO_DIR/scripts/package-system-app.py" \
    "$REPO_DIR" \
    "$APK" \
    "$PACKAGE"

unzip -t "$PACKAGE" >/dev/null
sh -n "$REPO_DIR/app/magisk/customize.sh"
unzip -p "$PACKAGE" customize.sh | cmp - "$REPO_DIR/app/magisk/customize.sh"
unzip -p "$PACKAGE" \
    system/etc/permissions/privapp-permissions-ai.rx1.ivrdroid.xml \
    | cmp - "$REPO_DIR/app/magisk/privapp-permissions-ai.rx1.ivrdroid.xml"
unzip -p "$PACKAGE" \
    system/etc/sysconfig/ai.rx1.ivrdroid.xml \
    | cmp - "$REPO_DIR/app/magisk/sysconfig-ai.rx1.ivrdroid.xml"
shasum -a 256 "$APK" "$PACKAGE"
