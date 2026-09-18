#!/usr/bin/env python3
"""Record exact device/source outputs. Host image IDs are recorded separately after image build."""
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
if os.environ.get('IVRDROID_SOURCE_COMMIT') != commit:
    raise SystemExit('Build source commit must match HEAD.')
version = '0.10.1'
for path in ('app/magisk/module.prop', 'helper/magisk/module.prop'):
    if f'version={version}\n' not in (root / path).read_text():
        raise SystemExit(f'Inconsistent release metadata: {path}')
app = (root / 'app/build.gradle.kts').read_text()
if f'versionName = "{version}"' not in app:
    raise SystemExit('Inconsistent APK version.')
code = int(re.search(r'versionCode = (\d+)', app)[1])
overlay = int(re.search(r'^versionCode=(\d+)', (root / 'app/magisk/module.prop').read_text(), re.M)[1])
helper = int(re.search(r'^versionCode=(\d+)', (root / 'helper/magisk/module.prop').read_text(), re.M)[1])
if code != overlay or code < 20 or helper < 28:
    raise SystemExit('Version codes must preserve Android upgrade order.')
folder = Path(sys.argv[1]).resolve()
files = ['IVRdroid-0.10.1.apk', 'IVRdroid-0.10.1-acceptance.apk', 'IVRdroid-system-app-0.10.1.zip',
         'IVRdroid-helper-0.10.1-disabled.zip', 'ivrdroid-helper', 'ivrdroid-audio-harness', 'source.tar']
artifacts = {}
for name in files:
    path = folder / name
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    artifacts[name] = {'sha256': digest, 'size_bytes': path.stat().st_size}
manifest = {'release': version, 'source_commit': commit, 'android_version_code': code,
            'overlay_version_code': overlay, 'helper_version_code': helper, 'artifacts': artifacts}
(folder / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(folder / 'SHA256SUMS').write_text(''.join(f'{entry["sha256"]}  {name}\n' for name, entry in artifacts.items()))
print(folder / 'manifest.json')
