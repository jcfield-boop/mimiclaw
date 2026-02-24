#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_VERSION="${IDF_VERSION:-v5.5.2}"
ESP_ROOT="${ESP_ROOT:-$HOME/.espressif}"
TARGET="${TARGET:-esp32c6}"

# Locate ESP-IDF: check IDF_DIR/IDF_PATH override first, then common install paths
DEFAULT_IDF_DIR="$ESP_ROOT/esp-idf-$IDF_VERSION"
if [[ -z "${IDF_DIR:-}" && -z "${IDF_PATH:-}" ]]; then
  if [[ -f "$HOME/esp/esp-idf/export.sh" ]]; then
    DEFAULT_IDF_DIR="$HOME/esp/esp-idf"
  fi
fi
IDF_DIR="${IDF_DIR:-${IDF_PATH:-$DEFAULT_IDF_DIR}}"

if [[ ! -f "$IDF_DIR/export.sh" ]]; then
  echo "ESP-IDF not found at: $IDF_DIR" >&2
  echo "Run scripts/setup_idf_macos.sh first, or set IDF_DIR/IDF_PATH." >&2
  exit 1
fi

# shellcheck source=/dev/null
. "$IDF_DIR/export.sh"

cd "$PROJECT_ROOT"

# Only call set-target if the current sdkconfig target differs (avoids wiping config)
CURRENT_TARGET=""
if [[ -f sdkconfig ]]; then
  CURRENT_TARGET=$(grep -m1 '^CONFIG_IDF_TARGET=' sdkconfig 2>/dev/null | cut -d'"' -f2 || true)
fi
if [[ "$CURRENT_TARGET" != "$TARGET" ]]; then
  echo "Setting target to $TARGET (was: ${CURRENT_TARGET:-unset})"
  idf.py set-target "$TARGET"
fi

idf.py build
