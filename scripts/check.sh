#!/bin/sh

set -eu

REPO_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)

cd "$REPO_DIR"
./gradlew test lint assembleDebug
./helper/tests/run.sh
./scripts/build-helper.sh
./scripts/check-helper-reproducibility.sh
./scripts/check-system-app-reproducibility.sh
