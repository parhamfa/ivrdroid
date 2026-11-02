# Production deployment and rollback

The production targets are **old-mac** and the enrolled **SM-T585**. old-mac is the 2017
MacBook Pro, now running Ubuntu Linux (hostname `parham`). `ssh old-mac` enters as root and Docker
runs natively, so plain `docker` and `docker compose` work. The Compose project is `ivrdroid`,
defined by `/srv/oldmac-docker/projects/ivrdroid/compose.json`. The `cloudflared` systemd service
routes `ivrdroid.rx1.ai` to the loopback-only web port, `127.0.0.1:3200`; the API and database
stay on internal networks. The macOS/Colima layout under `/Users/parhamfatemi/Services/ivrdroid`
and the Hetzner/OpenLiteSpeed instructions in older verification records describe retired
deployments. old-mac also runs TextBee, rx1-dashboard and other projects: do not modify unrelated
hosts, tunnels, applications or volumes during a release.

Layout under `/srv/oldmac-docker` (root-owned, directories 0700, files 0600):

- `projects/ivrdroid/compose.json` is the whole runtime definition. There is no `.env`: the image
  tags (`ivrdroid-api:<version>-<first12commit>`, likewise `ivrdroid-web`) and the api's
  `IVRDROID_SOURCE_COMMIT` environment entry are written in this file, with `pull_policy: never`.
- `source/Users/parhamfatemi/Services/ivrdroid/deploy/secrets/` holds the runtime secrets, a
  restored copy of the macOS tree. They are bind-mounted read-only into the api under
  `/run/secrets/`, and `docker-entrypoint.sh` loads them into the environment.
- `volumes/ivrdroid_postgres_data`, `volumes/ivrdroid_prompt_media` and
  `volumes/ivrdroid_recording_media` are the bind-mounted data.
- `build/<name>-<version>/` holds source extracted for an image build.
- `backups/<name>-<version>-<UTC timestamp>/` holds each deploy's rollback material.

Runtime secrets, signing keys and rollback archives stay private. Preserve every key version
required by existing media. Do not print credentials or copy them into build contexts.

## Server or dashboard release (current procedure)

Releases 0.10.4 to 0.10.6 used these steps. A release that also changes the tablet adds the
Android steps from the 0.9.0 procedure below.

1. Run server pytest, and the dashboard tests and build when it changed. Commit on a branch,
   merge into `main` with `--no-ff` and push both without force. Build only from that merge
   commit.
2. Copy the exact source to old-mac and build there; it caches the pinned `python`, `node` and
   `nginx-unprivileged` base images:

   ```sh
   C=$(git rev-parse HEAD); V=0.10.6; T=$V-${C:0:12}; B=/srv/oldmac-docker/build/<name>-$V
   git archive "$C" server | ssh old-mac "mkdir -p $B && tar -x -C $B"
   ssh old-mac "cd $B/server && DOCKER_BUILDKIT=0 docker build -t ivrdroid-api:$T --build-arg SOURCE_COMMIT=$C ."
   ```

   For the dashboard, archive `dashboard` as well and build `ivrdroid-web:$T` from
   `$B/dashboard` the same way.
3. Create `backups/<name>-<version>-<UTC timestamp>/` with `compose.previous.json` (a copy of the
   live file), `alembic.before` (`select version_num from alembic_version`),
   `database.before.dump` (`docker exec ivrdroid-db-1 pg_dump -U ivrdroid -d ivrdroid -Fc`) and
   `compose.candidate.json`. The candidate changes only the new image tags and
   `IVRDROID_SOURCE_COMMIT`; diff it against the previous file. Record `SHA256SUMS`.
4. Confirm no call is in progress (latest `call_records`, recent API log). Copy the candidate over
   `compose.json` and run `docker compose -p ivrdroid -f compose.json up -d api` (and/or `web`)
   in `projects/ivrdroid`. The api runs `alembic upgrade head` when it starts.
5. Verify that the containers are healthy and that `/health` reports the new version and full
   commit:

   ```sh
   docker exec ivrdroid-api-1 python -c "import urllib.request; print(urllib.request.urlopen('http://127.0.0.1:8000/health').read().decode())"
   ```

   Also check the schema head, the tablet's `POST /api/device/v1/sync` 200 responses and
   `docker logs ivrdroid-api-1` for errors. Without a login the public host answers with the
   Cloudflare Access redirect.
6. Write the deployment record under `ivrdroid-release-artifacts/deployments/`, outside the repo.

To roll back a release without a schema change, copy `compose.previous.json` back over
`compose.json` and run `up -d` for the same services. Keep the previous images. Never run
`alembic downgrade`.

One-off scripts inside the api container must go through the entrypoint so the secrets are
loaded: `docker exec -i ivrdroid-api-1 ivrdroid-entrypoint python -`.

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

## Email notifications

Migration `0011_email_settings` is additive: it creates the `email_settings` singleton and adds
the nullable `ntfy_settings.email_events` column. Per-event email flags live in that column, not
in `ntfy_settings.events`, so an API from before 0011 can still read the ntfy row after a
rollback (it ignores email and keeps sending ntfy alerts). Do not downgrade to remove them.

Production sends through iCloud+ SMTP from an rx1.ir custom-domain address:

- Server `smtp.mail.me.com`, port 587, STARTTLS. Username is the iCloud Mail @icloud.com
  address, even when the Apple Account signs in with another email: a Gmail login is rejected
  with `550 5.1.1 Mailbox does not exist` and the custom-domain address with `535`. The password
  is an app-specific password. The From address must be an rx1.ir
  address added under iCloud Settings → Custom Email Domain. The dashboard stores the password
  encrypted with the data key and never returns it.
- old-mac normally egresses through the `melbi` WireGuard link, which drops outbound 587/465.
  The rush mangle rule "Route: old-mac SMTP via Starlink" (src 192.168.88.49, tcp dst-port 587,
  `mark-routing main`, before "Mark Conn: Google") sends submission traffic out Starlink. If the
  dashboard's test email times out, check that rule and its counters first.
- rx1.ir DNS must keep the iCloud MX, SPF (`include:icloud.com`) and the
  `sig1._domainkey` CNAME. That CNAME must be **DNS only** in Cloudflare; proxying it hides
  the DKIM key and outgoing mail fails DKIM.

Verify with Settings → Email delivery → "Send test email", then check that SPF, DKIM and DMARC
all pass in the received message's headers.

Since 0.10.6 the emails carry no links (`server/app/email_content.py`). A short body around one
link with an opaque recording ID is the shape of voicemail phishing, and Gmail filed those emails
as spam. Recording emails state the call's facts instead and point at the Recordings page. Do not
add a link back; push notifications keep theirs.

## Previous release procedure (0.9.0)

These steps ran on the macOS host. Its `.env` with `IVRDROID_RELEASE_TAG` no longer exists: on the
current host the tags live in `compose.json` (see the current procedure above).

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

- old-mac: `/Users/parhamfatemi/Services/ivrdroid/backups/20260912T162721Z-session-audit` on the
  retired macOS install; it is not in the restored `/srv/oldmac-docker/source` tree.
- Local: `/Users/parhamfatemi/Backups/ivrdroid/20260912T162721Z-session-audit`
- Tablet: `/data/local/tmp/ivrdroid-rollback-20260912T162721Z.tar.gz`

The valid tablet archive is `tablet-packages-and-state.tar.gz`. The explicitly named
`tablet-packages-and-state.failed-pty-stream` is a rejected capture and must never be restored.
The verified archive includes both modules, the active APK and app state. Retain the protected
checksums and exact image IDs with the release evidence.
