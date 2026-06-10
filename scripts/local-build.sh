#!/usr/bin/env bash
# Local probe build using the west workspace in this repo (.venv + zmk/ + zephyr/).
# Mirrors the CI invocation in .github/workflows/build.yml — same ZMK_CONFIG,
# SHIELD, snippets, and ZMK_EXTRA_MODULES (repo root is itself a ZMK module).
# NOT for release artifacts — those always come from CI (reproducible container).
# Usage: scripts/local-build.sh [R|L|reset|R_debug]   (default R)
set -eu
T="${1:-R}"
WS="$(cd "$(dirname "$0")/.." && pwd)"
# venv first: nanopb's protoc-gen-nanopb plugin resolves python3 from PATH, and
# the venv carries the protobuf runtime matching brew's protoc gencode.
export PATH="$WS/.venv/bin:/opt/homebrew/bin:$PATH"
WEST="$WS/.venv/bin/west"
# west extension commands (build) resolve only from inside the workspace topdir
cd "$WS"
EXTRA="$WS;$WS/vendor/zmk-feature-charge-indicator;$WS/vendor/zmk-rgbled-widget"

case "$T" in
R)
    SHIELD="monokey_R rgbled_adapter"
    SNIPPET="studio-rpc-usb-uart"
    ;;
R_debug)
    SHIELD="monokey_R rgbled_adapter"
    SNIPPET="studio-rpc-usb-uart zmk-usb-logging"
    ;;
L)
    SHIELD="monokey_L rgbled_adapter"
    SNIPPET=""
    ;;
reset)
    SHIELD="settings_reset"
    SNIPPET=""
    ;;
*)
    echo "usage: $0 [R|L|reset|R_debug]" >&2
    exit 2
    ;;
esac

if [ -n "$SNIPPET" ]; then
    set -- -S "$SNIPPET"
else
    set --
fi

"$WEST" build -s "$WS/zmk/app" -d "$WS/build/$T" -b "xiao_ble//zmk" "$@" -- \
    -DZMK_CONFIG="$WS/config" \
    -DSHIELD="$SHIELD" \
    -DZMK_EXTRA_MODULES="$EXTRA"

ls -la "$WS/build/$T/zephyr/zmk.uf2"
