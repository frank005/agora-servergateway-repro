# Project context: Agora Server Gateway + pthread repro

This file is a persistent reference so any session (or after context loss) knows what this repo is, what we built, and how it works.

---

## What this repo is

- **Root:** `servergateway` — used to deploy and **reproduce an issue** with the Agora RTC Linux SDK in containerized environments.
- **Agora SDK:** `agora_rtc_sdk/` at repo root (see `PACKAGE_INFO`). Used by Docker service `repro` (platform linux/amd64). Receive/send audio and video, env flags, teardown fix.
- **Deploy / repro:** `deploy/` — minimal repro program, Dockerfile, docker-compose, and Kubernetes manifests to run the SDK **without** `CAP_SYS_NICE` and with `RLIMIT_RTPRIO=0` so we can reliably hit the glibc pthread assertion.

---

## The issue we’re reproducing

**Symptom:** Process aborts with:

- **Fatal glibc error:** `tpp.c:83 (__pthread_tpp_change_priority): assertion failed: new_prio == -1 || (new_prio >= fifo_min_prio && new_prio <= fifo_max_prio)`
- **Signal 6 (SIGABRT)**

**Cause:**

- Agora SDK uses **real-time thread scheduling** (`SCHED_FIFO` / `SCHED_RR`) and **`PTHREAD_PRIO_INHERIT`** mutexes.
- In containers we typically **don’t** grant `CAP_SYS_NICE` and **don’t** raise `RLIMIT_RTPRIO` (it stays 0).
- Then `pthread_setschedparam` can’t set RT priority (e.g. `EPERM`), threads stay at `SCHED_OTHER` with priority 0, and when the SDK locks a priority-inheritance mutex, glibc’s assertion in `tpp.c` fires.

**Where it happens:** During or right after:

- `agora_service_initialize` / `service->initialize(config)`, or
- `agora_service_create_local_audio_track` / `service->createLocalAudioTrack()`

(i.e. when internal audio threads are created and start using those mutexes.)

**What we tried (and didn’t use in this repo):**

- Interposing `pthread_mutexattr_setprotocol` to downgrade to `PTHREAD_PRIO_NONE` → led to deadlock (linker lock with `dlsym(RTLD_NEXT, ...)` during global constructors).
- Eagerly resolving the real function in a constructor before `dlopen` → still failed to start.

**What we’re asking Agora:**

1. Is there a (documented or undocumented) **config option** to disable real-time thread scheduling (e.g. fall back to `SCHED_OTHER`) when RT isn’t available?
2. Can the SDK **gracefully degrade** when `pthread_setschedparam` fails with `EPERM` (e.g. continue with normal priority instead of crashing)?

---

## Repo layout (relevant parts)

```
<repo_root>/
├── CLAUDE.md                    # This file — project context for AI/humans
├── docker-compose.yml           # Main Compose: cap_drop: [ALL], image servergateway-repro
├── agora_rtc_sdk/               # Agora Linux SDK at repo root
│   ├── PACKAGE_INFO             # e.g. Agora-RTC-*-linux-gnu-...
│   ├── agora_sdk/               # Headers + .so (libagora_rtc_sdk.so, libaosl.so, ...)
│   └── example/                 # Official samples (we don’t run these in deploy)
│       ├── common/              # sample_common.cpp has createAndInitAgoraService()
│       ├── mixed_audio/         # sample_receive_mixed_audio.cpp
│       └── scripts/             # env.cmake, check.cmake, os.cmake — SDK path, libs
└── deploy/                      # Everything we added for repro
    ├── README.md                # User-facing instructions (Docker, K8s, build)
    ├── repro_pthread_init.cpp   # C++ repro (default binary)
    ├── repro_v2_full.cpp        # v2 C API repro; use AGORA_REPRO_IMPL=v2
    ├── CMakeLists.txt           # Builds both repros (used by Dockerfile)
    ├── Makefile.repro           # Local build of repro_pthread_init only
    ├── Dockerfile               # Multi-stage Ubuntu 24.04; both repros + entrypoint
    ├── run_with_rtprio0.c       # Entrypoint: RLIMIT_RTPRIO=0, OS version, exec repro
    ├── run_with_logs.sh         # Run container, copy SDK log to host (AGORA_HOST_LOG_FILE)
    ├── ENV.md                   # Full env var reference
    ├── docker-compose.yml       # Alternate Compose (e.g. arm64)
    └── kubernetes/
        └── deployment.yaml      # Drops ALL capabilities; image agora-repro:latest
```

---

## How the repro works

1. **`repro_pthread_init.cpp`**
   - Reads env: `AGORA_APP_ID` (default `dummy_app_id_for_repro`), `AGORA_CHANNEL_ID`, `AGORA_TOKEN` (optional), `AGORA_UID` (default `"0"`).
   - Calls `createAgoraService()`, sets `AgoraServiceConfiguration` with `appId` from env, `enableAudioProcessor=1`, `enableAudioDevice=1`, then `service->initialize(config)` — crash usually happens here in restricted containers.
   - Calls `service->createLocalAudioTrack()` (another path that can assert). Needs `#include "NGIAgoraAudioTrack.h"` so `ILocalAudioTrack` is a complete type for `agora_refptr`.
   - If `AGORA_CHANNEL_ID` is set: creates RTC connection, `subscribeAllAudio()`, `connect(token, channelId, uid)`, stays 60s (or until Ctrl+C), then disconnects. Needs `NGIAgoraRtcConnection.h` and `NGIAgoraLocalUser.h`.
   - Releases the service and exits.

2. **Build**
   - **Make:** From `deploy/`, `make -f Makefile.repro`; needs `AGORA_SDK` (default `../agora_rtc_sdk/agora_sdk`) and `LD_LIBRARY_PATH` set to that dir when running.
   - **CMake:** From `deploy/`, `mkdir build && cd build && cmake .. && cmake --build .`; same `LD_LIBRARY_PATH` at run time.
   - **Docker:** Dockerfile copies `agora_rtc_sdk` and `deploy/`, builds with CMake in a builder stage, then copies the binary and `.so` files into a minimal runtime image. Runtime image does **not** add `CAP_SYS_NICE`.

3. **Docker**
   - `docker compose build repro` then `docker compose run --rm repro` (uses `agora_rtc_sdk`, platform linux/amd64).
   - Run with RT allowed: `docker compose run --rm --cap-add SYS_NICE repro`.
   - Root `docker-compose.yml` defines service `repro` with `cap_drop: [ALL]` and `ulimits.rtprio: 0`; image **servergateway-repro**. For plain `docker build -t agora-repro`, use image **agora-repro**; `run_with_logs.sh` defaults to `agora-repro` (set `AGORA_REPRO_IMAGE=servergateway-repro` if using Compose image).

4. **Kubernetes**
   - Build and load/push image (e.g. `agora-repro:latest`), then:  
     `kubectl apply -f deploy/kubernetes/`  
   - Deployment drops **ALL** capabilities (same as `docker run --cap-drop=ALL`); container should start and then abort when the repro runs.  
   - Logs: `kubectl logs -f deployment/agora-repro` (expect crash/SIGABRT).

---

## Important details

- **SDK:** Single folder `agora_rtc_sdk/` at repo root. Docker build uses platform linux/amd64; ensure the SDK in that folder matches the platform you build for.
- **No license check in repro:** The minimal repro does not use the sample app’s license/certificate flow; it only initializes the service and creates a local audio track.
- **Intent of deploy/**  
  - Reproduce the glibc assertion in a clean, repeatable way (Docker/K8s, no `CAP_SYS_NICE`).  
  - Document the issue and the two questions for Agora (config option + graceful degradation).  
  - Optionally validate that adding `SYS_NICE` avoids the crash (e.g. `docker run --cap-add=SYS_NICE`).

---

## If you (or another session) need to…

- **Understand the crash:** Read “The issue we’re reproducing” and the `deploy/README.md` summary.
- **Run the repro:** Use `deploy/README.md` (Docker, Docker Compose, Kubernetes, or local Make/CMake).
- **Change the repro:** Edit `deploy/repro_pthread_init.cpp` (C++) or `deploy/repro_v2_full.cpp` (v2 C API); rebuild with CMake (or Makefile.repro for C++ only); Docker image rebuild picks up CMake. Set **AGORA_REPRO_IMPL=v2** to run the v2 binary.
- **Adjust container security:** Edit **root** `docker-compose.yml` (cap_drop/ulimits) or `deploy/kubernetes/deployment.yaml` (securityContext.capabilities). Optional alternate: `deploy/docker-compose.yml`.
- **Full env reference:** See `deploy/ENV.md`.
- **Use a different SDK path:** Docker build arg `AGORA_SDK_DIR=...` (default `agora_rtc_sdk`); SDK arch should match image platform (e.g. linux/amd64).

This file is the single place that ties together: what the repo is, why we have `deploy/`, how the issue works, and how the repro and deployments are supposed to be used.
