#!/usr/bin/env bash
# Vendor only the Android app sources, fonts, screenshots, READMEs, and all
# license/copying/copyright/notice files of Sloop Go, the companion app whose UI
# this project follows. The copy is a read-only reference and is never built.
# Git metadata is excluded; license files are always retained. The regenerated
# manifest keeps every sloop-fm1/ line of the existing one. The default channel
# is SSH, matching tools/fetch_upstream.sh.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
THIRD_PARTY="$ROOT/third_party"
DEST="$THIRD_PARTY/sloop-fm1-go"
MANIFEST="$THIRD_PARTY/MANIFEST.sha256"
DEFAULT_REF=ed941b5853f551d1628bc3aecce4fa2010cd6f67
DEFAULT_REPO=git@github.com:jahlib/sloop-fm1-go.git
REF=$DEFAULT_REF
MIRROR=

usage() {
    cat <<'EOF'
Usage: tools/fetch_sloop_go.sh [--ref <full-commit-sha>] [--mirror <prefix>]

Clone jahlib/sloop-fm1-go over SSH by default, check out the pinned commit,
replace third_party/sloop-fm1-go, and regenerate third_party/MANIFEST.sha256
with the sloop-fm1/ lines of the existing manifest kept.
--mirror selects an alternate repository prefix, for example:
  --mirror https://github.com/
EOF
}

fail() {
    echo "fetch_sloop_go: $*" >&2
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
    REPO="${MIRROR%/}/jahlib/sloop-fm1-go.git"
fi

mkdir -p "$THIRD_PARTY"
if [[ -e $DEST || -e $MANIFEST ]]; then
    [[ -f $MANIFEST ]] || fail "existing vendored tree has no manifest"
    (cd "$THIRD_PARTY" && sha256sum -c MANIFEST.sha256)
fi

WORK=$(mktemp -d "$THIRD_PARTY/.fetch-sloop-go.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
CHECKOUT="$WORK/upstream"
STAGE="$WORK/sloop-fm1-go"

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
    android/app/src fonts screenshots README.md android/README.md LICENSE NOTICE.md; do
    copy_path "$path"
done

while IFS= read -r path; do
    base=${path##*/}
    case "${base,,}" in
        *licens*|*copying*|*copyright*|*notice*)
            copy_path "$path"
            ;;
    esac
done < <(git -C "$CHECKOUT" ls-files)

(
    cd "$WORK"
    find sloop-fm1-go -type f -print0 | sort -z | xargs -0 sha256sum > MANIFEST.sha256
)
if [[ -f $MANIFEST ]]; then
    awk '$2 ~ /^sloop-fm1\//' "$MANIFEST" >> "$WORK/MANIFEST.sha256"
fi
LC_ALL=C sort -k2 -o "$WORK/MANIFEST.sha256" "$WORK/MANIFEST.sha256"

rm -rf "$DEST"
mv "$STAGE" "$DEST"
mv "$WORK/MANIFEST.sha256" "$MANIFEST"
[[ -z $(find "$DEST" -name .git -print -quit) ]] || fail "vendored tree unexpectedly contains git metadata"
(cd "$THIRD_PARTY" && sha256sum -c MANIFEST.sha256)
printf 'Vendored %s at %s (%s files)\n' "$REPO" "$ACTUAL" "$(find "$DEST" -type f | wc -l)"
