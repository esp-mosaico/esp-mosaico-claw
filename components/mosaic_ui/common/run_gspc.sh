# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
#
# Invoke GSPC through esp-gsp-tools, matching ESP-GSP 1.1+/1.2 CMake:
#   python -m gsp.execute --version <marker> gspc ...
# GSPC_EXECUTABLE remains a manual override.

_mosaic_gspc_common="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_mosaic_gspc_project="$(cd "$_mosaic_gspc_common/../../.." && pwd)"

_mosaic_gspc_version() {
    if [[ -n "${GSPC_VERSION:-}" ]]; then
        printf '%s\n' "$GSPC_VERSION"
        return
    fi
    local marker
    for marker in \
        "${_mosaic_gspc_project}/.gspc_version" \
        "${ESP_GSP_ROOT:-}/.gspc_version" \
        "${GSP_ROOT:-}/.gspc_version"; do
        if [[ -n "$marker" && -f "$marker" ]]; then
            tr -d '[:space:]' < "$marker"
            return
        fi
    done
    echo "error: no .gspc_version found; set GSPC_VERSION or GSPC_EXECUTABLE" >&2
    return 1
}

gspc() {
    local exe="${GSPC_EXECUTABLE:-}"
    if [[ -z "$exe" && -n "${GSPC:-}" && -f "${GSPC}" && -x "${GSPC}" ]]; then
        exe="$GSPC"
    fi
    if [[ -n "$exe" ]]; then
        "$exe" "$@"
        return
    fi
    local py="${PYTHON:-${PYTHON3:-python3}}"
    local ver
    ver="$(_mosaic_gspc_version)"
    "$py" -m gsp.execute --version "$ver" gspc "$@"
}
