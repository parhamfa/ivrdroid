# Production deployment and rollback

The production targets are **old-mac** and the enrolled **SM-T585**. Runtime configuration is
`/Users/parhamfatemi/Services/ivrdroid/deploy`; the Compose project is `ivrdroid`. old-mac runs
Docker through the `parhamfatemi` Colima instance. The existing `com.cloudflare.cloudflared`
launch service routes `ivrdroid.rx1.ai` to the loopback-only web port, `127.0.0.1:3200`.
Hetzner/OpenLiteSpeed instructions in older verification records describe a retired deployment.
Do not modify unrelated hosts, tunnels, applications or volumes during this release.

For this host, invoke Docker as its Colima owner (SSH currently enters as root):

```sh
sudo -n -H -u parhamfatemi /opt/local/bin/docker \
  --host unix:///Users/parhamfatemi/.colima/default/docker.sock ps
```

Release staging directories must be readable by that owner for Colima bind mounts. Runtime
`.env`, `deploy/secrets/`, signing keys and rollback archives stay private. Preserve every key
version required by existing media. Do not print credentials or copy them into build contexts.

## Release 0.10.1 continuity update

Follow [CALL_CONTINUITY.md](CALL_CONTINUITY.md) for the new protocol and acceptance contract.
Back up pending device audio, the database and encrypted media before updating. Migrate through
`0008_continuous_recordings` and deploy the compatible API first. Update the app (code 20) and
helper (code 28) together while idle, then the dashboard. Keep the published prompt selections
unchanged. Verify desired/applied policy acknowledgment and the 60-minute default on the tablet.

Retain the compatible new API during tablet rollback. The generated rollback API also includes
the additive schemas and recording read/source-retention compatibility, but does not process new
continuous uploads; pending encrypted sources must remain until the compatible worker resumes.
Never downgrade the schema or restore an old database over new calls and recordings.

Physical acceptance and the two-hour soak are required before tagging this release accepted.

## Previous release procedure (0.9.0)

1. Recheck actual image IDs, schema head, device versions/version codes, active revision, idle
   Telecom/audio state, storage, enrollment and signing identity. Preserve original local changes
   and implement on an isolated `codex/` branch/worktree.
2. Back up a PostgreSQL custom-format dump; prompt/recording media; ignored deployment config;
   Android signing material; active APK; both persistent Magisk modules; and application state.
   Protect backups with directory mode 0700/files 0600 and verify archive hashes. Android
   Keystore keys are nonexportable: keep the original application UID, data and signing identity.
3. Run Android unit tests/lint/build, native policy tests and reproducibility checks, server
   pytest, dashboard tests/build and actual browser playback/seek checks. Restore the dump into
   an isolated PostgreSQL database and test migration `0006_session_audit` with legacy rows.
4. Commit all source, tests, migration, documentation and release metadata. Reconcile with
   `origin/main`, rerun affected checks, merge and push without force. Preserve any dirty original
   checkout. Version metadata is 0.9.0; APK/overlay versionCode >=18 and helper versionCode >=26.
5. From the exact clean merge commit, run `scripts/build-release.sh /protected/release/path`.
   The output includes APK, privileged overlay ZIP, disabled helper ZIP, acceptance harnesses,
   exact `source.tar`, `manifest.json` and SHA256SUMS. Keep the same source commit in Android,
   helper, API and dashboard diagnostics. Do not build a production artifact from a dirty tree.
6. Extract the source archive into a new owner-readable release directory on old-mac. Build
   `linux/amd64` API/web images with `SOURCE_COMMIT` set to the full manifest commit. Tag each as
   `0.9.0-<first12commit>` and retain the image IDs in the host release manifest. The Compose
   `IVRDROID_RELEASE_TAG` chooses those immutable tags. Preserve the current image tags too.
7. Prepare a schema-compatible rollback API using `scripts/build-rollback-api.py BASE OUTPUT`.
   This backports only recording read compatibility and the additive migration to the prior
   API, excludes audits from old dashboard counts/inbox, and leaves old call-control routes.
   Build it alongside the retained old web image. Test its health and recording reads against
   the migrated isolated database before cutover. Its provenance includes both commits.
8. Copy only the reviewed Compose definition into the existing deploy directory. Preserve
   runtime secrets, volume names, Access policies and the tunnel. Set the release tag/source
   commit in `.env`. Keep auditing off. Run the explicit Alembic migration, then deploy matching
   API/web containers. Verify migration head, image IDs, health, authenticated public access,
   existing recording playback and tablet synchronization.
9. Wait for an idle call state. During a bounded maintenance window, update the active APK with
   the same signing certificate (`adb install -r`; never uninstall), persistent `ivrdroid_app`
   overlay and `ivrdroid_helper`. The overlay preserves restricted-network permission,
   privileged in-call control and the scoped Doze exemption. Keep device-wide restricted
   networking enabled. The helper ZIP installs disabled; run its self-test before enabling.
   Reboot as needed, then verify active/system APK hashes, helper binary hash, version codes,
   runtime commits, permissions, role, enrollment, active IVR revision and post-boot sync.
   Verify the runtime `READ_PHONE_STATE` grant added for audit recovery's Telecom idle check;
   the app's existing phone-permission setup requests it. Without it recovery fails closed.
10. Run the isolated native/Android acceptance harnesses while idle. Enable auditing for the
    controlled physical-call matrix. Verify setting → applied acknowledgement → full capture
    → encrypted upload acknowledgement → authenticated production playback → clean idle state.
    Exercise voicemail/finish key, connected operator conversation, no-answer, caller/operator
    hangup, screen-off and interruption/recovery. Use synthetic topology fixtures for emergency
    cases; never place a real emergency test call.
11. Confirm expected caller/operator voices and prompts exactly once, no ambient tablet audio,
    and timeline alignment within 250 ms. Verify voicemail/conversation recordings separately.
    Restore the normal caller policy and leave auditing enabled only after acceptance passes.
    Commit/push any fixes, rebuild and redeploy from the new exact commit. Push `v0.9.0` only
    when every intended target and physical acceptance agree with that commit.

Deployment evidence must state what actually passed. A successful synthetic harness does not
prove a carrier conference. If a physical test is unavailable, record the gap and do not tag an
accepted release.

## Rollback without data loss

First disable full-session auditing in Settings. Wait for an idle applied-policy acknowledgement.
Existing pending recordings remain uploadable under their original acknowledged policy. If the
new app/helper is unhealthy, disable the local IVR switch during recovery and restore the known
working app/helper artifacts with the original signer and retained application data. Never
uninstall, re-enroll, delete the audit spool, or reset Android Keystore as a routine rollback.
Android may require an explicit same-signer version downgrade for the preserved prior APK.

Restore the prior persistent Magisk modules and active APK, then reboot and verify permissions,
role, enrollment, revision and clean audio state. The privileged overlay can be lower than the
active data APK; verify both rather than assuming an overlay alone replaces the running app.

For the server, use the prepared compatible rollback API with the retained prior web image, or
keep the accepted new backend with auditing disabled while rolling back the tablet. Preserve
`0006_session_audit`, all call rows, encrypted media and tombstones. Unpatched older APIs may fail
on new recording kinds or an unknown Alembic revision. Do not run `alembic downgrade`, restore a
stale database over live history, remove volumes, or discard audit recordings during routine
rollback. A disaster restore is a separate reviewed operation.

## Protected baseline for this release

The initial 2026-09-12 baseline was API/web 0.8.6, schema `0005_ntfy_settings`, active APK
0.8.6-dev/code17, persistent app overlay 0.8.4-dev/code15, helper 0.8.2-dev/code24 and IVR revision
21. Recheck before installing; do not rely on these dated values after another release.

- old-mac: `/Users/parhamfatemi/Services/ivrdroid/backups/20260912T162721Z-session-audit`
- Local: `/Users/parhamfatemi/Backups/ivrdroid/20260912T162721Z-session-audit`
- Tablet: `/data/local/tmp/ivrdroid-rollback-20260912T162721Z.tar.gz`

The valid tablet archive is `tablet-packages-and-state.tar.gz`. The explicitly named
`tablet-packages-and-state.failed-pty-stream` is a rejected capture and must never be restored.
The verified archive includes both modules, the active APK and app state. Retain the protected
checksums and exact image IDs with the release evidence.
