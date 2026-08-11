# Production deployment and rollback

This file preserves the completed V3 production runbook. The not-yet-deployed V4 external-call
rollout and physical acceptance gate are documented separately in
[EXTERNAL_CALL_V4.md](EXTERNAL_CALL_V4.md); do not treat a local V4 build as carrier validation.

This is the production deployment and rollback runbook. V3 was deployed under explicit approval
on 2026-08-09: the control plane API/web are 0.7.1 at migration
`0003_voicemail_recordings`, the tablet runs the 0.7.0 app/helper, and signed V3 revision 13 is
active. A consented physical call proved caller-only capture, finish-key handling, encrypted
upload, acknowledgement, inbox delivery, safe audio-mode restoration, and no ambient or prompt
capture. The first MP3 was too quiet; server 0.7.1 corrected its mono/loudness normalization and
the reprocessed dashboard playback was accepted. One fresh consented call is still required to
prove that the corrected normalization is applied automatically to a new upload. Do not treat
this runbook as standing authorization for later deployments, migrations, revision activation,
or tablet changes.

## Host layout

The authoritative production host is `hetzner`. Runtime configuration is rooted at
`/opt/ivrdroid/deploy`, and the standalone Compose project is named `ivrdroid`. Release build
contexts may be staged under `/opt/ivrdroid/server` and `/opt/ivrdroid/dashboard`; they are not a
production git checkout. IVRdroid does not join or restart unrelated Hetzner services.

Runtime values belong in ignored files:

```text
deploy/.env
deploy/secrets/config-signing-private.b64
deploy/secrets/data-encryption-key.b64
deploy/secrets/pairing-hmac-key.b64
deploy/secrets/recording-encryption-key.b64
deploy/secrets/cf-device-service-client-secret
deploy/secrets/postgres-password
```

The secrets directory is mode 0700 and each file is mode 0600. Only the Ed25519 public
verification key is committed in the Android build. The recording key must be an independent
32-byte AES key, not the configuration/data key. Never copy private keys into the APK, database,
dashboard bundle, filenames, or logs.

## Network boundary

- `ivrdroid-web` publishes `127.0.0.1:3200 -> 8080`.
- `ivrdroid-api` is reachable only from the application Compose network.
- `ivrdroid-db` is reachable only from the internal database Compose network.
- The existing OpenLiteSpeed `ivrdroid.rx1.ai` vhost proxies to `127.0.0.1:3200`.
- There is no production IVRdroid Cloudflare Tunnel. The old-mac tunnel route is retired.
- Cloudflare Access applications, from most specific to least specific:
  - `ivrdroid-device-enroll`: `ivrdroid.rx1.ai/api/device/v1/enroll`, bypass;
  - `ivrdroid-device-api`: `ivrdroid.rx1.ai/api/device/*`, service auth;
  - `ivrdroid-dashboard`: `ivrdroid.rx1.ai`, owner email OTP.

The enrollment bypass is intentional but narrow. FastAPI still enforces an eight-digit
single-use code, ten-minute expiry, five failures per IP per fifteen minutes, and one active
tablet. All post-enrollment requests require both the Cloudflare service credential and the
independent device bearer credential.

## Deployment order

1. Record existing container health, image digests, port listeners, OpenLiteSpeed proxy state,
   installed tablet artifacts, and their hashes. Create rollback copies before any mutation.
2. Preserve a validated PostgreSQL dump, prompt and recording archives, every recording-key
   version needed to decrypt them, encrypted active manifest, installed app/helper packages,
   exact hashes, and the signed active V2 revision before staging V3.
3. Stage only the release build contexts on `hetzner`; retain ignored runtime configuration and
   secrets under `/opt/ivrdroid/deploy`, then validate the Compose model.
4. Build the pinned `linux/amd64` images, start PostgreSQL, run Alembic, then start API and web.
   Migration `0003_voicemail_recordings` adds recording/upload/retention tables. V3 draft
   initialization archives the V2 draft while retaining caller policy and schedules; immutable
   V1/V2 revision rows are not rewritten.
5. Verify `http://127.0.0.1:3200/health`, container health, port bindings, database migration
   head, and unchanged neighboring services.
6. Verify the V2 active revision and tablet acknowledgement remain unchanged. V3 authoring may
   begin, but publish is rejected until every active tablet reports runtime V3 and recording
   capability when the flow contains `record_message`.
7. Keep the OpenLiteSpeed vhost, Cloudflare proxy/Access policy, service token, and path-specific
   Access applications unchanged. Do not create a tunnel or reload unrelated vhosts.
8. Verify the public owner login, device endpoint precedence, and local/public health.
9. Only after separate approval, install/open the Android 0.7.0 app and the 0.7.0 disabled helper
   module, run its self-test, then enable the audited helper. Validate the V3 tree before publishing
   a signed V3 revision and verify a real idle-time acknowledgement. Keep the active V2 revision
   available for immediate rollback.

The audited tablet has Android restricted-networking mode enabled. Install
`app/dist/IVRdroid-system-app-0.7.0-dev.zip` through Magisk with the APK. This systemizes only
IVRdroid and grants only `CONNECTIVITY_USE_RESTRICTED_NETWORKS`; do not disable the device-wide
restriction or add a broad UID firewall exception. After reboot, verify IVRdroid is a privileged
system app, the permission is granted, the call-screening role is still held, and restricted
networking remains enabled.

The boot-only Wi-Fi guardian never issues a Wi-Fi-disable operation or writes
an SSID/password. If the tablet has no usable Wi-Fi address, it may enable an already-disabled
radio and issue at most three framework reconnect requests during the first three minutes after
boot. Every request is deferred while the app, helper, or Android audio mode reports an active
call. Its sanitized outcome is included in the next device heartbeat.

Dashboard upload is the normal prompt path. For recovery or a deployment-time import, copy a
source file into the API container and run the same validation pipeline with:

```sh
ivrdroid-entrypoint python -m app.manage import-prompt \
  --name "Prompt name" --file /tmp/prompt.wav --actor local-deployment
```

The command enforces the same suffix, 25 MiB, five-minute, conversion, duplicate, quota,
versioning, and audit rules as the dashboard API. Remove the temporary source after import.

## Backup and rollback

The database and prompt volumes are named `ivrdroid_postgres_data` and
`ivrdroid_prompt_media`; voicemail uses the separate `ivrdroid_recording_media` volume. A backup
must include a PostgreSQL dump, prompt media, encrypted recording media, ignored runtime
configuration, all required recording-key versions, and hashes. A volume directory alone is not
a portable database backup. Restore validation must decrypt and play a non-sensitive fixture, not
merely confirm that encrypted files exist.

Rollback is scoped to IVRdroid. During V3 cutover, retain exact signed V1/V2 manifests and the V2
app/helper packages until all physical recording tests pass. For an engine rollback, use a V1/V2
row's `Emergency activate` action in Settings, wait for idle activation, and verify desired and
active revision IDs both equal that immutable revision. Returning to V3 must use `Restore as new`,
which publishes a new signed V3 revision instead of rewriting history.

## Recording storage and key rotation

- The local media quota is 5 GiB. Default retention is automatic deletion after 30 days; manual
  mode never silently deletes audio. At quota, the API returns `507`, the tablet retains its
  encrypted spool, the dashboard shows a critical backlog warning, and a new recording follows
  `on_unavailable` only when the bounded tablet spool cannot accept it.
- Abandoned uploads are tombstoned/cleaned after 24 hours and can restart idempotently. Completed
  audio is deleted only through retention or an explicit owner action; the audit tombstone stays.
- Set `IVRDROID_RECORDING_ENCRYPTION_KEY_VERSION` to a monotonically increasing integer on key
  rotation. Mount a one-line JSON object mapping every still-needed old version to its base64 key
  and set `IVRDROID_RECORDING_ENCRYPTION_PREVIOUS_KEYS_JSON_FILE` through a deployment-specific
  Compose override. Verify old and new messages before retiring an old key. Losing an old key makes
  its retained voicemail irrecoverable.

If the control plane or tablet package itself must be rolled back:

1. turn off the tablet's local IVR switch;
2. leave the OpenLiteSpeed vhost and Cloudflare/Access configuration unchanged unless that exact
   layer is the proven fault;
3. restore the previous APK/helper package if the tablet update caused the failure;
4. restore the pinned IVRdroid images or database dump if the control plane caused the failure;
5. leave the helper's previous active revision or built-in menu in place;
6. re-check unrelated Hetzner services, OpenLiteSpeed, and the stock dialer.

Do not reload unrelated OpenLiteSpeed vhosts or prune shared Docker images/volumes as part of this
rollback.
