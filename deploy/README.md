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
| `repro_pthread_init.cpp` | C++ repro using the SDK’s C++ API: `createAgoraService()`, `initialize()`, create local audio track, optional connect/publish/subscribe. |
| `repro_v2_full.cpp` | Parallel repro using the **v2 C API** (`agora_service_*`, `agora_rtc_conn_*`, etc.) with **dlopen/dlsym only** (no link to `libagora_rtc_sdk`). Same env vars and behavior as the C++ repro. Set `AGORA_REPRO_IMPL=v2` to run this binary. |
| `CMakeLists.txt` / `Makefile.repro` | Build both repros (CMake builds both; Makefile.repro builds only `repro_pthread_init` for local use). |
| `Dockerfile` | Multi-stage build (Ubuntu 24.04): compiles both repros + `run_with_rtprio0` entrypoint; runtime runs **without** `CAP_SYS_NICE`. |
| `docker-compose.yml` (root) | Main Compose file: `cap_drop: [ALL]`, `ulimits.rtprio: 0`, image `servergateway-repro`. Use from repo root. |
| `deploy/docker-compose.yml` | Alternate Compose (e.g. arm64 / `cap_drop: SYS_NICE`); root file is the one used in this README. |
| `kubernetes/deployment.yaml` | Drops all capabilities (`ALL`); entrypoint sets `RLIMIT_RTPRIO=0`. |
| `fake_setschedparam.c` | LD_PRELOAD shim so `pthread_setschedparam` reports success; use to **force** the glibc assert when you can’t reproduce. |
| `run_with_logs.sh` | Runs the container, then copies the SDK log from the container to a host path (from `.env`: `AGORA_HOST_LOG_FILE`). |
| **Env reference** | **`deploy/ENV.md`** — full list of env vars (credentials, encryption, logging, bisect, etc.). |

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

Compose builds the image as **`servergateway-repro`**. The examples below use **`agora-repro`** (from `docker build -t agora-repro`). If you use Compose, either run `docker run ... servergateway-repro` or set `AGORA_REPRO_IMAGE=servergateway-repro` when using `run_with_logs.sh`.

To run the **v2 C API** repro instead of the C++ one, set `AGORA_REPRO_IMPL=v2` in `deploy/.env` (or pass `-e AGORA_REPRO_IMPL=v2`). The entrypoint will exec `repro_v2_full`.

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

**Logging:** Receive logs (`[audio] received remote frame ...`, `[video] received remote frame ...`) are from the SDK callbacks; send logs (`[audio] sent chunk ...`, `[video] sent frame ...`) log what we push. Startup logs include `Join duration: N s` and every 10 s `[channel] N s remaining`; when the timer expires you get `Duration reached, leaving channel.` **Encryption (optional):** `AGORA_ENCRYPTION_ENABLE=1`, `AGORA_ENCRYPTION_MODE` (1–8 or name, e.g. 7 or AES-128-GCM2), `AGORA_ENCRYPTION_SECRET`, `AGORA_ENCRYPTION_SALT` (for GCM2 modes). See **`deploy/ENV.md`** for the full env reference. **Agora SDK logs** are written inside the container (default `/app/agora_sdk.log`). To copy them to the host after the run, use **`./deploy/run_with_logs.sh`** (reads `AGORA_HOST_LOG_FILE` from `deploy/.env`); or run `docker cp <container_id>:/app/agora_sdk.log ./` manually.

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

Copy the example env file and edit it with your credentials:

```bash
cp deploy/.env.example deploy/.env
# Edit deploy/.env with your App ID, channel, token, etc.
```

Put your credentials in **`deploy/.env`** (same folder as `docker-compose.yml`). Example:

```bash
# deploy/.env
AGORA_APP_ID=your_appid_here
AGORA_CHANNEL_ID=your_channel_name_here
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

From repo root. **CMake** (builds both `repro_pthread_init` and `repro_v2_full`):

```bash
cd deploy
mkdir build && cd build
cmake ..
cmake --build .
export LD_LIBRARY_PATH="../../agora_rtc_sdk/agora_sdk:$LD_LIBRARY_PATH"
./repro_pthread_init
# or: ./repro_v2_full
```

**Make** (builds only `repro_pthread_init`):

```bash
cd deploy
make -f Makefile.repro
export LD_LIBRARY_PATH="../agora_rtc_sdk/agora_sdk:$LD_LIBRARY_PATH"
./repro_pthread_init
```

On a host with `CAP_SYS_NICE` and sufficient `RLIMIT_RTPRIO`, the program may succeed; in a restricted container it should hit the assertion.

## Questions for Agora

1. **Configuration:** Is there a (documented or undocumented) option to disable real-time thread scheduling in the SDK (e.g. fall back to `SCHED_OTHER` when RT is unavailable)?
2. **Graceful degradation:** Can the SDK detect `pthread_setschedparam` failure (e.g. `EPERM`) and continue without RT priority instead of crashing?

We are in a cloud game streaming environment where voice importance varies; we’d like the option to run with normal-priority audio threads where acceptable, and only request elevated container permissions where voice is critical.
