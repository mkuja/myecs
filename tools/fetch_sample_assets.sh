#!/usr/bin/env bash
# Downloads glTF sample models used by examples/tests into assets/.
#
# Only CC0 models are fetched: public domain, no attribution obligation,
# nothing to add to THIRD-PARTY-NOTICES. Other Khronos samples carry CC-BY,
# and DamagedHelmet -- the usual PBR showcase -- is dual-licensed with a
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
fetch Fox.glb "Fox/glTF-Binary/Fox.glb"
# PBR: metallic-roughness, normal, emissive and occlusion maps.
fetch BoomBox.glb "BoomBox/glTF-Binary/BoomBox.glb"

printf '\nboth models are CC0 (public domain)\n'
ls -la "$dest"
