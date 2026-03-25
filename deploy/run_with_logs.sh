#!/usr/bin/env bash
# Run agora-repro container, wait for exit, copy SDK log to the host, then remove container.
#
# Usage (from repo root OR any directory):
#   ./deploy/run_with_logs.sh [extra docker run args...]
#
# Host log destination (in order of precedence):
#   1. AGORA_HOST_LOG_FILE set in your shell environment
#   2. AGORA_HOST_LOG_FILE set in deploy/.env
#   3. Default: <repo-root>/logs/agora_sdk.log
#
# In-container log path is always /app/agora_sdk.log (the SDK default).
#
# Container hardening (matches root docker-compose `repro`: no extra caps, no RT priority):
#   --cap-drop=ALL --ulimit rtprio=0
# On Apple Silicon, --platform linux/amd64 matches the amd64 SDK in this image.
set -euo pipefail

# Resolve repo root (two levels up from this script, no matter where it's called from)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ENV_FILE="${SCRIPT_DIR}/.env"

# Helper: read a variable from the .env file, stripping quotes and inline comments
read_env_var() {
  local key="$1"
  local default_val="${2:-}"
  if [[ -f "${ENV_FILE}" ]]; then
    local raw
    raw=$(grep -E "^${key}[[:space:]]*=" "${ENV_FILE}" | tail -1 | cut -d= -f2-)
    # Strip leading/trailing whitespace
    raw="${raw#"${raw%%[![:space:]]*}"}"
    raw="${raw%"${raw##*[![:space:]]}"}"
    # Strip surrounding quotes (single or double)
    raw="${raw#\'}" ; raw="${raw%\'}"
    raw="${raw#\"}" ; raw="${raw%\"}"
    # Strip inline comment
    raw="${raw%%#*}"
    # Strip trailing whitespace again after comment removal
    raw="${raw%"${raw##*[![:space:]]}"}"
    if [[ -n "${raw}" ]]; then
      echo "${raw}"
      return
    fi
  fi
  echo "${default_val}"
}

IN_CONTAINER_LOG="/app/agora_sdk.log"

# Host log path: shell env > .env file > default
if [[ -n "${AGORA_HOST_LOG_FILE:-}" ]]; then
  HOST_LOG_PATH="${AGORA_HOST_LOG_FILE}"
else
  HOST_LOG_PATH="$(read_env_var AGORA_HOST_LOG_FILE "${REPO_ROOT}/logs/agora_sdk.log")"
fi

# Make host path absolute (resolve relative paths against repo root)
if [[ "${HOST_LOG_PATH}" != /* ]]; then
  HOST_LOG_PATH="${REPO_ROOT}/${HOST_LOG_PATH}"
fi

# Default matches root docker-compose.yml image (docker compose build repro).
IMAGE_NAME="${AGORA_REPRO_IMAGE:-servergateway-repro}"
CONTAINER_NAME="agora-repro-logs-$$"

echo "============================================================"
echo "  image:         ${IMAGE_NAME}"
echo "  container:     ${CONTAINER_NAME}"
echo "  log (in):      ${IN_CONTAINER_LOG}"
echo "  log (host):    ${HOST_LOG_PATH}"
echo "============================================================"

DOCKER_PLATFORM=()
if [[ "$(uname -m)" == "arm64" ]] || [[ "$(uname -m)" == "aarch64" ]]; then
  DOCKER_PLATFORM=(--platform linux/amd64)
fi

ENV_FILE_ARGS=()
if [[ -f "${ENV_FILE}" ]]; then
  ENV_FILE_ARGS=(--env-file "${ENV_FILE}")
else
  echo "Note: ${ENV_FILE} not found; running without --env-file (pass env via -e or shell)." >&2
fi

# Run (no --rm so we can docker cp after exit)
set +e
docker run \
  --name "${CONTAINER_NAME}" \
  --cap-drop=ALL \
  --ulimit rtprio=0 \
  "${DOCKER_PLATFORM[@]}" \
  "${ENV_FILE_ARGS[@]}" \
  -e "AGORA_LOG_FILE=${IN_CONTAINER_LOG}" \
  "$@" \
  "${IMAGE_NAME}"
RUN_STATUS=$?
set -e

echo ""
echo "Container exited (status=${RUN_STATUS}). Copying SDK log..."

mkdir -p "$(dirname "${HOST_LOG_PATH}")"
if docker cp "${CONTAINER_NAME}:${IN_CONTAINER_LOG}" "${HOST_LOG_PATH}" 2>/dev/null; then
  echo "SDK log copied -> ${HOST_LOG_PATH}"
  ls -lh "${HOST_LOG_PATH}"
else
  echo "WARNING: could not copy ${IN_CONTAINER_LOG} from container (file may not exist)." >&2
fi

docker rm "${CONTAINER_NAME}" >/dev/null 2>&1 || true
echo "Container removed."

exit "${RUN_STATUS}"
