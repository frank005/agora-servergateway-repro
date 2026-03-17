# Reproduce Agora Linux SDK in containers.
# Build on Linux x86_64:  docker build -f deploy/Dockerfile -t agora-repro .
# Build on Apple Silicon:  docker build --platform=linux/amd64 -f deploy/Dockerfile -t agora-repro .
# SDK at repo root:  agora_rtc_sdk/agora_sdk (override with --build-arg AGORA_SDK_DIR=...).
FROM ubuntu:22.04 AS builder
ARG AGORA_SDK_DIR=agora_rtc_sdk

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    g++ \
    make \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY ${AGORA_SDK_DIR} /workspace/agora_rtc_sdk
COPY deploy/repro_pthread_init.cpp deploy/repro_v2.cpp deploy/CMakeLists.txt deploy/run_with_rtprio0.c deploy/fake_setschedparam.c /workspace/deploy/

WORKDIR /workspace/build
RUN cmake ../deploy -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build . \
    && g++ -O2 -o run_with_rtprio0 ../deploy/run_with_rtprio0.c \
    && g++ -shared -fPIC -o fake_setschedparam.so ../deploy/fake_setschedparam.c

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libc6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /workspace/build/repro_pthread_init .
COPY --from=builder /workspace/build/run_with_rtprio0 .
COPY --from=builder /workspace/build/fake_setschedparam.so /app/
COPY --from=builder /workspace/agora_rtc_sdk/agora_sdk/*.so* /app/

ENV LD_LIBRARY_PATH=/app
# So plain "docker run --cap-drop=ALL" runs to the assert instead of initialize() -3
ENV LD_PRELOAD=/app/fake_setschedparam.so
# Set RLIMIT_RTPRIO=0 before running repro (ensures K8s matches Docker ulimits.rtprio: 0)
ENTRYPOINT ["/app/run_with_rtprio0"]
