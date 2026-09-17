#!/usr/bin/env python3
"""Build context for a prior API with only additive audit read compatibility backported.

The old control plane remains unchanged. Disable/acknowledge auditing before rollback.
The copied recording serializer/player understands preserved session media; its default list
continues to show only voicemail/conversations for the older dashboard.
"""
import argparse
import io
import json
import subprocess
import tarfile
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('base_commit')
parser.add_argument('output', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
base = subprocess.check_output(['git', 'rev-parse', args.base_commit + '^{commit}'], cwd=root, text=True).strip()
head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
if args.output.exists():
    raise SystemExit('Rollback context must be a new directory.')
args.output.mkdir(parents=True, mode=0o700)
archive = subprocess.check_output(['git', 'archive', base, 'server'], cwd=root)
with tarfile.open(fileobj=io.BytesIO(archive)) as source:
    source.extractall(args.output, filter='data')
paths = ['app/models.py', 'app/schemas.py', 'app/recording_api.py', 'app/recording_service.py', 'alembic/versions/0006_session_audit.py', 'alembic/versions/0007_call_safety.py', 'alembic/versions/0008_continuous_recordings.py']
for path in paths:
    target = args.output / 'server' / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes((root / 'server' / path).read_bytes())
main = args.output / 'server/app/main.py'
code = main.read_text()
needle = '.where(Recording.call_id.in_(call_ids), Recording.status != "deleted")'
replacement = '.where(Recording.call_id.in_(call_ids), Recording.status != "deleted", Recording.kind != "session_audit")'
if needle not in code and replacement not in code:
    raise SystemExit('Prior call-count query changed; review the compatibility patch.')
main.write_text(code.replace(needle, replacement))
dockerfile = args.output / 'server/Dockerfile'
code = dockerfile.read_text()
lines = code.splitlines()
lines.append(f'LABEL org.opencontainers.image.revision="{base}" ai.rx1.audit-compatibility-source="{head}"')
dockerfile.write_text('\n'.join(lines) + '\n')
(args.output / 'rollback-source.json').write_text(json.dumps({'base_commit': base, 'audit_compatibility_commit': head, 'backports': paths}, indent=2) + '\n')
print(args.output)
