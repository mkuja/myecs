#!/usr/bin/env bash
# Downloads glTF sample models used by examples/tests into assets/.
#
# Licensing is per-model and, for Fox, per-contribution -- check
# Models/<name>/LICENSE.md upstream before adding one here, and record what
# you find in THIRD-PARTY-NOTICES.md:
#
#   BoomBox  CC0-1.0                    public domain, no obligation
#   Fox      CC0-1.0 + CC-BY-4.0        ATTRIBUTION REQUIRED (see below)
#
# DamagedHelmet -- the usual PBR showcase -- is dual-licensed with a
# NON-COMMERCIAL clause, so it is deliberately not used here.
#
# assets/ is gitignored: binaries do not belong in the repository.
set -euo pipefail

cd "$(dirname "$0")/.."
base="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models"
dest="assets/models"
mkdir -p "$dest"

fetch() {
    local name="$1" path="$2"
    if [[ -f "$dest/$name" ]]; then
        printf '  have  %s\n' "$name"
        return
    fi
    printf '  get   %s\n' "$name"
    curl -fsSL "$base/$path" -o "$dest/$name" || {
        printf '  FAILED %s\n' "$name" >&2
        return 1
    }
}

echo "fetching CC0 glTF sample models into $dest"
# Skeletal animation: three cycles (Survey, Walk, Run) driving one rig.
# Mixed licence: the mesh is CC0, but the rig+animation and the glTF
# conversion are CC-BY-4.0, so shipping this file obliges you to credit
# tomkranis, Asobo Studio and scurest.
fetch Fox.glb "Fox/glTF-Binary/Fox.glb"
# PBR: metallic-roughness, normal, emissive and occlusion maps.
fetch BoomBox.glb "BoomBox/glTF-Binary/BoomBox.glb"

printf '\nBoomBox: CC0. Fox: CC-BY-4.0 in part -- see THIRD-PARTY-NOTICES.md\n'
ls -la "$dest"
