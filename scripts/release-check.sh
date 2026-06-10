#!/usr/bin/env bash
# Release version consistency check across the conductor + conductor-studio repos.
# Usage: scripts/release-check.sh <version>   e.g. scripts/release-check.sh 0.5.6
#
# The release version lives in 6 places across 2 repos; this catches the misses
# before tagging (see Knowledge/conductor-v0.5.5-system-review).
set -u
V="${1:?usage: release-check.sh <version, e.g. 0.5.6>}"
CONDUCTOR_DIR="$(cd "$(dirname "$0")/.." && pwd)"
STUDIO_DIR="$HOME/conductor-studio"
fail=0
ok()   { echo "OK   $*"; }
err()  { echo "FAIL $*"; fail=1; }
warn() { echo "WARN $*"; }

# (1) FW version string baked into the binary / shown in Studio
if grep -q "CONFIG_ZMK_STUDIO_FIRMWARE_VERSION=\"$V\"" "$CONDUCTOR_DIR/config/boards/shields/monokey/monokey_R.conf"; then
    ok "monokey_R.conf CONFIG_ZMK_STUDIO_FIRMWARE_VERSION=$V"
else
    err "monokey_R.conf CONFIG_ZMK_STUDIO_FIRMWARE_VERSION is not $V"
fi

# (2) Four CI artifact names (R / L / reset / R_debug)
n=$(grep -cF "conductor_v$V" "$CONDUCTOR_DIR/build.yaml")
if [ "$n" -eq 4 ]; then
    ok "build.yaml has 4 artifact names for v$V"
else
    err "build.yaml artifact names matching conductor_v$V: $n (expected 4)"
fi

# (3) ZMK fork revision (warn-only: a docs/keymap-only release may not cut a new ZMK branch)
if grep -q "revision: release/conductor-$V" "$CONDUCTOR_DIR/config/west.yml"; then
    ok "west.yml zmk revision release/conductor-$V"
else
    warn "west.yml zmk revision is not release/conductor-$V (OK only if this release has no ZMK delta)"
fi

# (4-6) Studio side: package version, firmware card, hosted UF2s
if [ -d "$STUDIO_DIR" ]; then
    if grep -q "\"version\": \"$V\"" "$STUDIO_DIR/package.json"; then
        ok "studio package.json version $V"
    else
        err "studio package.json version is not $V"
    fi
    FW_TSX="$STUDIO_DIR/client/src/pages/Firmware.tsx"
    if grep -q "version: '$V'" "$FW_TSX" && grep -q "/fw/v$V" "$FW_TSX"; then
        ok "Firmware.tsx has the v$V card"
    else
        err "Firmware.tsx is missing the v$V card or its /fw/v$V base"
    fi
    for f in R L reset; do
        if [ -f "$STUDIO_DIR/client/public/fw/v$V/${f}_conductor_v$V.uf2" ]; then
            ok "studio fw/v$V/${f}_conductor_v$V.uf2"
        else
            err "studio missing fw/v$V/${f}_conductor_v$V.uf2"
        fi
    done
else
    warn "studio repo not found at $STUDIO_DIR — studio checks skipped"
fi

if [ "$fail" -eq 0 ]; then
    echo "— all release checks passed for v$V —"
fi
exit "$fail"
