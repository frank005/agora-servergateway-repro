#!/usr/bin/env bash
# Run agora-repro container, wait for exit, copy SDK log(s) to the host, then remove container.
#
# Usage (from repo root OR any directory):
#   ./deploy/run_with_logs.sh [extra docker run args...]
#
# Host log destination (in order of precedence):
#   1. AGORA_HOST_LOG_FILE set in your shell environment
#   2. AGORA_HOST_LOG_FILE set in deploy/.env
#   3. Default: <repo-root>/logs/agora_sdk.log
#
# After the run, the script also copies every log-like file under /app (*.log, *.log.*, any depth)
# into AGORA_HOST_LOG_DIR (default: <dirname of AGORA_HOST_LOG_FILE>/agora_sdk_logs_all).
#
# In-container primary log path defaults to /app/agora_sdk.log; override with AGORA_LOG_FILE
# (shell or deploy/.env).
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

# Primary SDK log path inside the container: shell > deploy/.env > default
if [[ -n "${AGORA_LOG_FILE:-}" ]]; then
  IN_CONTAINER_LOG="${AGORA_LOG_FILE}"
else
  IN_CONTAINER_LOG="$(read_env_var AGORA_LOG_FILE "/app/agora_sdk.log")"
fi

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

# Directory on the host for all log-like files copied from /app (recursive)
if [[ -n "${AGORA_HOST_LOG_DIR:-}" ]]; then
  HOST_LOG_DIR="${AGORA_HOST_LOG_DIR}"
else
  HOST_LOG_DIR="$(dirname "${HOST_LOG_PATH}")/agora_sdk_logs_all"
fi
if [[ "${HOST_LOG_DIR}" != /* ]]; then
  HOST_LOG_DIR="${REPO_ROOT}/${HOST_LOG_DIR}"
fi

# Default matches root docker-compose.yml image (docker compose build repro).
IMAGE_NAME="${AGORA_REPRO_IMAGE:-servergateway-repro}"
CONTAINER_NAME="agora-repro-logs-$$"

echo "============================================================"
echo "  image:         ${IMAGE_NAME}"
echo "  container:     ${CONTAINER_NAME}"
echo "  log (in):      ${IN_CONTAINER_LOG}"
echo "  log (host):    ${HOST_LOG_PATH}"
echo "  logs dir:      ${HOST_LOG_DIR}  (all *.log under /app)"
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
echo "Container exited (status=${RUN_STATUS}). Copying SDK log(s)..."

mkdir -p "$(dirname "${HOST_LOG_PATH}")"
if docker cp "${CONTAINER_NAME}:${IN_CONTAINER_LOG}" "${HOST_LOG_PATH}" 2>/dev/null; then
  echo "Primary SDK log copied -> ${HOST_LOG_PATH}"
  ls -lh "${HOST_LOG_PATH}"
else
  echo "WARNING: could not copy ${IN_CONTAINER_LOG} from container (file may not exist)." >&2
fi

# Copy every log-like file from /app (recursive): noisy SDK output often splits across files / rotation.
TMPDIR_APP=""
cleanup_tmpdir() {
  if [[ -n "${TMPDIR_APP}" && -d "${TMPDIR_APP}" ]]; then
    rm -rf "${TMPDIR_APP}"
  fi
}
trap cleanup_tmpdir EXIT

if [[ "${AGORA_HOST_COPY_ALL_APP_LOGS:-1}" != "0" ]]; then
  TMPDIR_APP="$(mktemp -d)"
  if docker cp "${CONTAINER_NAME}:/app/." "${TMPDIR_APP}/" 2>/dev/null; then
    mkdir -p "${HOST_LOG_DIR}"
    found=0
    while IFS= read -r -d '' f; do
      rel="${f#"${TMPDIR_APP}/"}"
      dest="${HOST_LOG_DIR}/${rel}"
      mkdir -p "$(dirname "${dest}")"
      cp -f "${f}" "${dest}"
      found=1
    done < <(find "${TMPDIR_APP}" -type f \( -name '*.log' -o -name '*.log.*' \) -print0 2>/dev/null || true)
    if [[ "${found}" -eq 1 ]]; then
      echo "All /app log files (*.log / *.log.*) copied -> ${HOST_LOG_DIR}"
      find "${HOST_LOG_DIR}" -type f -print | sort
    else
      echo "No *.log / *.log.* files found under /app (or tree was empty)." >&2
    fi
  else
    echo "WARNING: could not copy /app from container for multi-file log gather." >&2
  fi
fi

if [[ "${AGORA_HOST_COPY_APP_DIR:-0}" == "1" ]]; then
  APP_SNAP="$(dirname "${HOST_LOG_PATH}")/agora_app_full_snapshot"
  mkdir -p "${APP_SNAP}"
  if docker cp "${CONTAINER_NAME}:/app/." "${APP_SNAP}/" 2>/dev/null; then
    echo "Full /app snapshot copied -> ${APP_SNAP}"
  else
    echo "WARNING: could not copy full /app snapshot." >&2
  fi
fi

docker rm "${CONTAINER_NAME}" >/dev/null 2>&1 || true
echo "Container removed."

exit "${RUN_STATUS}"
