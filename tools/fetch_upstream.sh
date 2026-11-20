#!/usr/bin/env bash
# Vendor only the engine/host/tests/tools, protocol reference, README, and all
# license/copyright/credits/attribution files. Upstream docs, built site output,
# sample WAVs, and git metadata are intentionally excluded; Task 5 fetches CC0 audio.
# The default channel is SSH because GitHub HTTPS is unavailable on this machine.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
THIRD_PARTY="$ROOT/third_party"
DEST="$THIRD_PARTY/sloop-fm1"
MANIFEST="$THIRD_PARTY/MANIFEST.sha256"
DEFAULT_REF=bac9c05ea7627449405aa10e48da21d75b6c8c56
DEFAULT_REPO=git@github.com:isod89/sloop-fm1.git
REF=$DEFAULT_REF
MIRROR=

usage() {
    cat <<'EOF'
Usage: tools/fetch_upstream.sh [--ref <full-commit-sha>] [--mirror <prefix>]

Clone isod89/sloop-fm1 over SSH by default, check out the pinned commit, replace
third_party/sloop-fm1, and regenerate third_party/MANIFEST.sha256.
--mirror selects an alternate repository prefix, for example:
  --mirror https://gitclone.com/github.com/
EOF
}

fail() {
    echo "fetch_upstream: $*" >&2
    exit 1
}

while (($#)); do
    case "$1" in
        --ref)
            (($# >= 2)) || fail "--ref requires a commit SHA"
            REF=$2
            shift 2
            ;;
        --mirror)
            (($# >= 2)) || fail "--mirror requires a repository prefix"
            MIRROR=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown argument: $1"
            ;;
    esac
done

[[ $REF =~ ^[0-9a-fA-F]{40}$ ]] || fail "--ref must be a full 40-character commit SHA"
REF=${REF,,}
REPO=$DEFAULT_REPO
if [[ -n $MIRROR ]]; then
    REPO="${MIRROR%/}/isod89/sloop-fm1.git"
fi

mkdir -p "$THIRD_PARTY"
if [[ -e $DEST || -e $MANIFEST ]]; then
    [[ -d $DEST && -f $MANIFEST ]] || fail "existing vendored tree and manifest must both be present"
    (cd "$THIRD_PARTY" && sha256sum -c MANIFEST.sha256)
fi

WORK=$(mktemp -d "$THIRD_PARTY/.fetch-upstream.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
CHECKOUT="$WORK/upstream"
STAGE="$WORK/sloop-fm1"

git clone --quiet "$REPO" "$CHECKOUT"
git -C "$CHECKOUT" checkout --quiet --detach "$REF"
ACTUAL=$(git -C "$CHECKOUT" rev-parse HEAD)
[[ $ACTUAL == "$REF" ]] || fail "checked out $ACTUAL, expected $REF"
mkdir -p "$STAGE"

copy_path() {
    local path=$1
    [[ -e "$CHECKOUT/$path" ]] || fail "required upstream path is missing: $path"
    if [[ -e "$STAGE/$path" ]]; then
        return
    fi
    mkdir -p "$STAGE/$(dirname "$path")"
    cp -a "$CHECKOUT/$path" "$STAGE/$path"
}

for path in \
    firmware/src firmware/hal firmware/host tests tools \
    web/EDITOR_PROTOCOL.md README.md LICENSE LICENSES; do
    copy_path "$path"
done

while IFS= read -r path; do
    base=${path##*/}
    case "${base,,}" in
        licens*|copying*|copyright*|credit*|notice*|attribution*)
            copy_path "$path"
            ;;
    esac
done < <(git -C "$CHECKOUT" ls-files)

(
    cd "$WORK"
    find sloop-fm1 -type f -print0 | sort -z | xargs -0 sha256sum > MANIFEST.sha256
)

rm -rf "$DEST"
mv "$STAGE" "$DEST"
mv "$WORK/MANIFEST.sha256" "$MANIFEST"
[[ ! -e $DEST/.git ]] || fail "vendored tree unexpectedly contains git metadata"
(cd "$THIRD_PARTY" && sha256sum -c MANIFEST.sha256)
printf 'Vendored %s at %s (%s files)\n' "$REPO" "$ACTUAL" "$(wc -l < "$MANIFEST")"
