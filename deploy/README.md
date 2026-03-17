# Reproduce Agora Linux SDK pthread scheduling assertion in Docker/Kubernetes

This directory contains a minimal repro and deployment files to reproduce the **glibc assertion** when running the Agora RTC C/C++ SDK (Linux) in a restricted container. **They reproduce with `docker run --cap-drop=ALL`**; we use `cap_drop: [ALL]` and `ulimits.rtprio: 0`.

## Issue summary

- **Symptom:** Fatal glibc error in `tpp.c:83 (__pthread_tpp_change_priority)`: assertion  
  `new_prio == -1 || (new_prio >= fifo_min_prio && new_prio <= fifo_max_prio)`  
  and **SIGABRT**.
- **Cause:** The SDK uses real-time thread scheduling (`SCHED_FIFO`/`SCHED_RR`) and `PTHREAD_PRIO_INHERIT` mutexes. When the process lacks `CAP_SYS_NICE` and `RLIMIT_RTPRIO` is 0, `pthread_setschedparam` cannot set RT priority, threads stay at `SCHED_OTHER` with priority 0, and locking a priority-protocol mutex triggers the glibc assertion.
- **Goal:** Reproduce this in Docker/Kubernetes (no `CAP_SYS_NICE`, default RT limit) to validate the behavior and test any SDK or runtime workarounds.

## What’s included

| Item | Description |
|------|-------------|
| `repro_pthread_init.cpp` | Minimal C++ program that calls `createAgoraService()`, `initialize()`, and `createLocalAudioTrack()` to hit the same code paths that spin up audio threads. |
| `Makefile.repro` / `CMakeLists.txt` | Build the repro against the SDK in `../agora_rtc_sdk/agora_sdk`. |
| `Dockerfile` | Multi-stage build: compile repro + `run_with_rtprio0` entrypoint wrapper; runtime image runs **without** `CAP_SYS_NICE`. |
| `docker-compose.yml` (root) | Runs with `cap_drop: [ALL]` and `ulimits.rtprio: 0` (matches `docker run --cap-drop=ALL`). |
| `kubernetes/deployment.yaml` | Drops all capabilities (`ALL`); entrypoint sets `RLIMIT_RTPRIO=0`. |
| `fake_setschedparam.c` | LD_PRELOAD shim that makes `pthread_setschedparam` report success without changing the thread; use to **force** the glibc assert when you can’t reproduce. |
| **Docs** | COMMANDS.md, REPRO_NOTES.md, BISECT.md, SDK_PRIORITY.md, and full env reference (including encryption) live in the **docs** folder (e.g. `docs/` at repo root or `servergateway_bak/docs`). |

## Prerequisites

- **Docker** — Docker Desktop (or Docker Engine) must be **installed and running**.
- **SDK**: `agora_rtc_sdk` in the repo root. If you see:
  ```text
  ERROR: Cannot connect to the Docker daemon at unix:///.../docker.sock. Is the docker daemon running?
  ```
  start **Docker Desktop** from your Applications (or install it from [docker.com](https://www.docker.com/products/docker-desktop/)), wait until it says it’s running, then try again.
- The **Agora Linux SDK** must be present at repo root: `agora_rtc_sdk/` with an `agora_sdk` subdir containing headers and `.so` files.

## Build and run (Docker)

From the **repository root** (parent of `deploy/`):

```bash
# Build image
docker build -f deploy/Dockerfile -t agora-repro .

# Run and crash (assert). Image sets LD_PRELOAD so plain docker with cap-drop=ALL runs to the assert.
# Use --env-file so you get the same App ID / channel / token as compose (needed to join a channel):
docker run --rm --cap-drop=ALL --ulimit rtprio=0 --env-file deploy/.env agora-repro
```

Or with Docker Compose (from repo root):

```bash
docker compose build repro
docker compose run --rm repro
```

(SDK: `agora_rtc_sdk/` at repo root.)

To add `CAP_SYS_NICE` for a successful run: `docker compose run --rm --cap-add SYS_NICE repro`.

Expected output when the assert is triggered:

- `Creating Agora service...`
- `Calling service->initialize()...`
- Then glibc assertion and **Signal 6 (SIGABRT)**.

To confirm it works when RT is allowed (e.g. on a host with permissions):

```bash
docker run --rm --cap-add=SYS_NICE agora-repro
```

### Joining a channel (App ID, token, channel)

You can pass your Agora credentials via **environment variables** so the repro joins a real channel after init:

| Env var | Description |
|--------|-------------|
| `AGORA_APP_ID` | Your Agora App ID (required for init; also used as token if `AGORA_TOKEN` is not set) |
| `AGORA_CHANNEL_ID` | Channel name to join. If set, the app connects after init. |
| `AGORA_TOKEN` | Token for authenticated join (optional). If unset, the app uses the App ID (works only if the project has token auth disabled in Agora Console). |
| `AGORA_UID` | Local user ID (optional; default `"0"`). |
| `AGORA_JOIN_DURATION_SEC` | Seconds to stay in the channel; `0` = run until Ctrl+C (default `60`). |
| `AGORA_RECEIVE_VIDEO` | Set to `1` to subscribe to and process remote video frames. |
| `AGORA_SEND_AUDIO` | Set to `1` to publish local audio (440 Hz tone). |
| `AGORA_SEND_VIDEO` | Set to `1` to publish local video (720p badge + “A” image @ 15 fps). |

**Logging:** Receive logs (`[audio] received remote frame ...`, `[video] received remote frame ...`) are from the **SDK callbacks** — they report the actual raw frames delivered by the SDK (samples, dimensions, etc.). Send logs (`[audio] sent chunk ...`, `[video] sent frame ...`) log what we push to the SDK. Startup logs include `Join duration: N s` (from `AGORA_JOIN_DURATION_SEC`) and every 10 s in channel `[channel] N s remaining`; when the timer expires you get `Duration reached, leaving channel.` Encryption (optional): `AGORA_ENCRYPTION_ENABLE=1`, `AGORA_ENCRYPTION_MODE` (number 1–8 or name, e.g. 7 or AES-128-GCM2), `AGORA_ENCRYPTION_SECRET`, `AGORA_ENCRYPTION_SALT` (for modes 7 and 8). See the **docs** folder for COMMANDS.md (all run commands) and ENV.md (full env reference including encryption). **Agora SDK logs** are written to a file (default `/app/agora_sdk.log`; override with `AGORA_LOG_FILE`) so you can pull them to check decode/encryption errors, e.g. `docker cp <container_id>:/app/agora_sdk.log ./` or run with a volume `-v $(pwd)/logs:/app` and set `AGORA_LOG_FILE=/app/agora_sdk.log`.

Example (join a channel with token):

```bash
docker run --rm --cap-add=SYS_NICE \
  -e AGORA_APP_ID=your_app_id \
  -e AGORA_CHANNEL_ID=my_channel \
  -e AGORA_TOKEN=your_token \
  -e AGORA_UID=server_1 \
  agora-repro
```

Example (join without token auth; testing only):

```bash
docker run --rm --cap-add=SYS_NICE \
  -e AGORA_APP_ID=your_app_id \
  -e AGORA_CHANNEL_ID=my_channel \
  agora-repro
```

### Using an env file

Put your credentials in **`deploy/.env`** (same folder as `docker-compose.yml`). Example:

```bash
# deploy/.env
AGORA_APP_ID=a9a4b25e4e8b4a558aa39780d1a84342
AGORA_CHANNEL_ID=frank
# AGORA_TOKEN=   # omit if token auth is disabled in your project
# AGORA_UID=0    # optional
```

Then run with `--env-file` so the container gets these variables:

```bash
docker run --rm --cap-add=SYS_NICE --env-file deploy/.env agora-repro
```

With Docker Compose (from repo root; root `docker-compose.yml` loads `deploy/.env`):

```bash
docker compose run --rm repro
```

## Build and run (Kubernetes)

1. Build and push (or load into a local cluster) the image, e.g.:

   ```bash
   docker build -f deploy/Dockerfile -t agora-repro:latest .
   kind load docker-image agora-repro:latest   # if using kind
   ```

2. Apply the deployment (drops all capabilities; entrypoint sets `RLIMIT_RTPRIO=0`):

   ```bash
   kubectl apply -f deploy/kubernetes/
   ```

3. Watch logs (expect crash loop / SIGABRT):

   ```bash
   kubectl logs -f deployment/agora-repro
   ```

## Building the repro locally (no Docker)

From `deploy/`:

```bash
cd deploy
make -f Makefile.repro
export LD_LIBRARY_PATH="../agora_rtc_sdk/agora_sdk:$LD_LIBRARY_PATH"
./repro_pthread_init
```

Or with CMake:

```bash
mkdir build && cd build
cmake ..
cmake --build .
LD_LIBRARY_PATH="../agora_rtc_sdk/agora_sdk:$LD_LIBRARY_PATH" ./repro_pthread_init
```

On a host with `CAP_SYS_NICE` and sufficient `RLIMIT_RTPRIO`, the program may succeed; in a restricted container it should hit the assertion.

## Questions for Agora

1. **Configuration:** Is there a (documented or undocumented) option to disable real-time thread scheduling in the SDK (e.g. fall back to `SCHED_OTHER` when RT is unavailable)?
2. **Graceful degradation:** Can the SDK detect `pthread_setschedparam` failure (e.g. `EPERM`) and continue without RT priority instead of crashing?

We are in a cloud game streaming environment where voice importance varies; we’d like the option to run with normal-priority audio threads where acceptable, and only request elevated container permissions where voice is critical.
