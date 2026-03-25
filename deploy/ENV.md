# Environment variables for the Agora Server Gateway repro

All variables are optional unless noted. Use `deploy/.env` (or `--env-file deploy/.env` with plain docker) so compose and docker run get the same values.

## Channel and credentials

| Variable | Description | Default |
|----------|-------------|---------|
| `AGORA_APP_ID` | Agora App ID (required for init; used as token if `AGORA_TOKEN` is not set) | `dummy_app_id_for_repro` |
| `AGORA_CHANNEL_ID` | Channel name to join. If set, the app connects after init. | (empty, no join) |
| `AGORA_TOKEN` | Token for authenticated join. If unset, app uses App ID (when token auth disabled in project). | same as App ID |
| `AGORA_UID` | Local user ID: numeric string (e.g. `0`, `12345`) in default mode, or **user account string** when `AGORA_USE_STRING_UID=1` (e.g. alphanumeric + allowed punctuation per Agora rules). | `0` |
| `AGORA_USE_STRING_UID` | Set to `1` to enable **string user account** mode. Must match how the channel/token were set up (enable “user account / string UID” in Agora Console if your project requires it). Generate tokens for the **same** user identifier string. | `0` (off) |
| `AGORA_SET_CLIENT_ROLE_TYPE` | `1` = set client role on connection config; `0` = do not set role field. | `1` |
| `AGORA_CLIENT_ROLE_TYPE` | Client role value when enabled: `AUDIENCE` or `BROADCASTER` (or `2`/`1`). | `AUDIENCE` |
| `AGORA_AREA_CODE` | Service area: `GLOB`, `OVS`, or hex (e.g. `0xFFFFFFFF`). | `GLOB` |
| `AGORA_SET_SERVICE_CHANNEL_PROFILE` | `1` = set `channelProfile` on **service** init (`AgoraServiceConfiguration` / `agora_service_config`); `0` = leave constructor defaults. | `0` |
| `AGORA_SERVICE_CHANNEL_PROFILE` | Value when service profile is enabled: `COMMUNICATION` or `LIVE_BROADCASTING` (or `0`/`1`). | `COMMUNICATION` |
| `AGORA_SET_SERVICE_AUDIO_SCENARIO` | `1` = set `audioScenario` on service init; `0` = leave default. | `0` |
| `AGORA_SERVICE_AUDIO_SCENARIO` | Service audio scenario: `DEFAULT`, `CHATROOM`, `GAME_STREAMING`, … or numeric `0`–`10`. | `DEFAULT` |
| `AGORA_SET_CHANNEL_PROFILE` | `1` = set channel profile on **RtcConnection** config; `0` = do not set profile field. | `0` |
| `AGORA_CHANNEL_PROFILE` | Connection-level channel profile when enabled: `COMMUNICATION` or `LIVE_BROADCASTING` (or `0`/`1`). | `COMMUNICATION` |
| `AGORA_REGISTER_CONN_OBSERVER` | v2 only: `1` = register `rtc_conn_observer` callbacks; `0` = do not register (default, more stable). | `0` |
| `AGORA_REGISTER_LOCAL_USER_OBSERVER` | `1` = register local-user observer callbacks (C++ LL and v2); `0` = disable registration. | `1` |
| `AGORA_LU_CB_AUDIO_SUB` | When local-user observer is enabled, gate audio-track-subscribed callback. | `1` |
| `AGORA_LU_CB_VIDEO_SUB` | When local-user observer is enabled, gate video-track-subscribed callback. | `1` |
| `AGORA_LU_CB_VOLUME_IND` | When local-user observer is enabled, gate audio-volume-indication callback. C++ default `1`; v2 default `0` (crash-prone in some setups). | `1` (C++), `0` (v2) |
| `AGORA_LU_CB_USER_INFO_UPDATED` | When local-user observer is enabled, wire `onUserInfoUpdated` / `on_user_info_updated`. | `0` |
| `AGORA_VOLUME_INDICATION_INTERVAL_MS` | Interval for `setAudioVolumeIndicationParameters` / v2 volume indication (ms). | `1000` |
| `AGORA_VOLUME_INDICATION_SMOOTH` | Smooth parameter for volume indication. | `3` |
| `AGORA_VOLUME_INDICATION_VAD` | Report VAD: `0` or `1`. | `0` |
| `AGORA_SET_LOCAL_USER_AUDIO_SCENARIO` | `1` = call `ILocalUser::setAudioScenario` / `agora_local_user_set_audio_scenario` after join setup. | `0` |
| `AGORA_LOCAL_USER_AUDIO_SCENARIO` | Scenario for local-user `setAudioScenario` (same names/numbers as service). | `DEFAULT` |

**String UID example** (in `deploy/.env`):

```bash
AGORA_USE_STRING_UID=1
AGORA_UID=myserver_NJKERNJ34MKPS3P0S
```

## Join duration and media

| Variable | Description | Default |
|----------|-------------|---------|
| `AGORA_JOIN_DURATION_SEC` | Seconds to stay in channel; `0` = until Ctrl+C. | `60` |
| `AGORA_DUMP_BEFORE_MIXING_PCM` | **v2 only:** `1` = append raw PCM from `on_playback_audio_frame_before_mixing` to `before_mixing_<uid>.pcm` under `AGORA_DUMP_PCM_DIR` (implies audio observer on). | `0` |
| `AGORA_DUMP_PCM_DIR` | Directory for per-UID `.pcm` files (created if missing). | `/tmp/agora_pcm_dump` |
| `AGORA_RECEIVE_VIDEO` | `1` = subscribe to remote video; `0` = audio only. | `0` |
| `AGORA_REGISTER_AUDIO_OBSERVER` | `1` = register playback audio frame observer (logs remote audio callback frames); `0` = disable observer registration. | `1` |
| `AGORA_ENABLE_AUDIO_VOLUME_INDICATION` | `1` = enable SDK audio volume indication callback (`onAudioVolumeIndication` / `on_audio_volume_indication`); `0` = disable. | `1` |
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
| `AGORA_REPRO_IMAGE` | **Host / `run_with_logs.sh` only:** Docker image to run (default `servergateway-repro`, same as root `docker compose build repro`). |

## Bisect / debug

| Variable | Description |
|----------|-------------|
| `AGORA_REPRO_STOP_AFTER` | Stop after step: `init`, `create_local_audio_track`, `connect`, or `publish`. Used to bisect which API triggers the assert. |
| `AGORA_REPRO_IMPL` | **`run_with_rtprio0` entrypoint:** set to `v2` for `repro_v2_full` (v2 C API); anything else runs `repro_pthread_init` (C++). |
