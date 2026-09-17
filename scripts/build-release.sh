#!/bin/sh
# Build immutable device artifacts and source from an explicitly clean committed checkout.
set -eu
REPO_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_DIR"
if [ -n "$(git status --porcelain --untracked-files=normal)" ]; then
    echo 'Release builds require a clean committed checkout.' >&2
    exit 1
fi
IVRDROID_SOURCE_COMMIT=$(git rev-parse HEAD)
export IVRDROID_SOURCE_COMMIT
SOURCE_DATE_EPOCH=$(git show -s --format=%ct HEAD)
export SOURCE_DATE_EPOCH
RELEASE_DIR=${1:-"$REPO_DIR/../ivrdroid-release-artifacts/0.10.0/$IVRDROID_SOURCE_COMMIT"}
mkdir -p "$RELEASE_DIR"
chmod 0700 "$RELEASE_DIR"
RELEASE_DIR=$(cd "$RELEASE_DIR" && pwd)
./gradlew assembleDebug assembleDebugAndroidTest
./scripts/build-helper.sh
cmake -S helper -B helper/build-android-arm64 -DIVRDROID_BUILD_AUDIO_HARNESS=ON
cmake --build helper/build-android-arm64 --parallel
./scripts/package-helper.sh
./scripts/package-system-app.sh
cp app/build/outputs/apk/debug/app-debug.apk "$RELEASE_DIR/IVRdroid-0.10.0.apk"
cp app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk "$RELEASE_DIR/IVRdroid-0.10.0-acceptance.apk"
cp app/dist/IVRdroid-system-app-0.10.0.zip "$RELEASE_DIR/"
cp helper/dist/IVRdroid-helper-0.10.0-disabled.zip "$RELEASE_DIR/"
cp helper/dist/ivrdroid-helper "$RELEASE_DIR/"
cp helper/build-android-arm64/ivrdroid-audio-harness "$RELEASE_DIR/"
git archive --format=tar HEAD > "$RELEASE_DIR/source.tar"
python3 scripts/release-manifest.py "$RELEASE_DIR"
