#!/usr/bin/env bash
set -euo pipefail

# Simple helper to run the agora-repro container, wait for it to finish,
# then copy the SDK log file out to a host path.
#
# Usage (from repo root):
#   # Host log path from env (default ./logs/agora_sdk.log):
#   ./deploy/run_with_logs.sh [extra docker args...]
#
#   # Or override host log path just for this run:
#   AGORA_HOST_LOG_FILE=/tmp/agora_sdk.log ./deploy/run_with_logs.sh
#
# The script:
# - Uses AGORA_LOG_FILE inside the container (default /app/agora_sdk.log)
# - Runs `docker run` with a temporary container name
# - After the container exits, copies AGORA_LOG_FILE to the host path
# - Finally removes the container

HOST_LOG_PATH="${AGORA_HOST_LOG_FILE:-./logs/agora_sdk.log}"

IMAGE_NAME="${AGORA_REPRO_IMAGE:-agora-repro}"
CONTAINER_NAME="agora-repro-logs-$$"
IN_CONTAINER_LOG="${AGORA_LOG_FILE:-/app/agora_sdk.log}"

echo "Running container ${CONTAINER_NAME} from image ${IMAGE_NAME}..."
echo "  SDK log inside container: ${IN_CONTAINER_LOG}"
echo "  Host log output: ${HOST_LOG_PATH}"

set +e
docker run --name "${CONTAINER_NAME}" \
  --cap-add SYS_NICE --ulimit rtprio=0 \
  --env-file deploy/.env \
  -e "AGORA_LOG_FILE=${IN_CONTAINER_LOG}" \
  "$@" \
  "${IMAGE_NAME}"
RUN_STATUS=$?
set -e

echo "Container exited with status ${RUN_STATUS}, copying log..."

mkdir -p "$(dirname "${HOST_LOG_PATH}")"
if docker cp "${CONTAINER_NAME}:${IN_CONTAINER_LOG}" "${HOST_LOG_PATH}" 2>/dev/null; then
  echo "Copied SDK log to ${HOST_LOG_PATH}"
else
  echo "WARNING: Failed to copy ${IN_CONTAINER_LOG}" \
       "from container (file may not exist or logging not enabled)." >&2
fi

docker rm "${CONTAINER_NAME}" >/dev/null 2>&1 || true

exit "${RUN_STATUS}"

