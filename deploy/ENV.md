# Environment variables for the Agora Server Gateway repro

All variables are optional unless noted. Use `deploy/.env` (or `--env-file deploy/.env` with plain docker) so compose and docker run get the same values.

## Channel and credentials

| Variable | Description | Default |
|----------|-------------|---------|
| `AGORA_APP_ID` | Agora App ID (required for init; used as token if `AGORA_TOKEN` is not set) | `dummy_app_id_for_repro` |
| `AGORA_CHANNEL_ID` | Channel name to join. If set, the app connects after init. | (empty, no join) |
| `AGORA_TOKEN` | Token for authenticated join. If unset, app uses App ID (when token auth disabled in project). | same as App ID |
| `AGORA_UID` | Local user ID. | `0` |

## Join duration and media

| Variable | Description | Default |
|----------|-------------|---------|
| `AGORA_JOIN_DURATION_SEC` | Seconds to stay in channel; `0` = until Ctrl+C. | `60` |
| `AGORA_RECEIVE_VIDEO` | `1` = subscribe to remote video; `0` = audio only. | `0` |
| `AGORA_SEND_AUDIO` | `1` = publish local audio (440 Hz tone); `0` = no publish. | `0` |
| `AGORA_SEND_VIDEO` | `1` = publish local video (720p @ 15 fps); `0` = no publish. | `0` |

## Thread priority (SDK config)

| Variable | Description | Default |
|----------|-------------|---------|
| `AGORA_THREAD_PRIORITY` | Deprecated SDK thread priority: `0`=LOWEST, `2`=NORMAL, `5`=CRITICAL. | unset (SDK default) |

## Built-in media encryption

Enable with `AGORA_ENCRYPTION_ENABLE=1`. All users in the same channel must use the same mode, secret, and salt.

| Variable | Description | Required when |
|----------|-------------|----------------|
| `AGORA_ENCRYPTION_ENABLE` | `0` = off, `1` = on. | — |
| `AGORA_ENCRYPTION_MODE` | Encryption mode (see below). | encryption enabled |
| `AGORA_ENCRYPTION_SECRET` | Encryption key (string; Agora recommends 32-byte). | encryption enabled |
| `AGORA_ENCRYPTION_SALT` | Base64-encoded 32-byte salt. | **AES-128-GCM2** or **AES-256-GCM2** only |

**Encryption modes:** Use the **number 1–8** (SDK enum) or a name.  
| Number | Name |
|--------|------|
| 1 | AES-128-XTS |
| 2 | AES-128-ECB |
| 3 | AES-256-XTS |
| 4 | SM4-128-ECB |
| 5 | AES-128-GCM |
| 6 | AES-256-GCM |
| 7 | AES-128-GCM2 |
| 8 | AES-256-GCM2 |

GCM2 (7 and 8) require salt. Generate salt with: `openssl rand -base64 32`.

Example `.env` for encryption (GCM2 with salt):

```bash
AGORA_ENCRYPTION_ENABLE=1
AGORA_ENCRYPTION_MODE=AES-128-GCM2
AGORA_ENCRYPTION_SECRET=your_32_byte_hex_or_ascii_key_here
AGORA_ENCRYPTION_SALT=your_salt_here
```

## Agora SDK log file

| Variable | Description |
|----------|-------------|
| `AGORA_LOG_FILE` | Path for Agora SDK log file **inside the container** (default `/app/agora_sdk.log`). The repro configures the SDK to write logs here so you can inspect decode/encryption errors. Pull from the container (`docker cp <container>:$AGORA_LOG_FILE ./`) or mount a volume. |
| `AGORA_HOST_LOG_FILE` | Optional **host-only** path used by the helper script `deploy/run_with_logs.sh`. When set (default `./logs/agora_sdk.log`), the script copies `AGORA_LOG_FILE` from the container to this host path after the run finishes. |

## Bisect / debug

| Variable | Description |
|----------|-------------|
| `AGORA_REPRO_STOP_AFTER` | Stop after step: `init`, `create_local_audio_track`, `connect`, or `publish`. Used to bisect which API triggers the assert. |
