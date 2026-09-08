#!/usr/bin/env bash
# Package the private full-library regression bundle for CI.
#
# The 72-wallpaper baseline (tests/regression/baseline) references private
# Steam workshop content that can't ship in the repo. This script stages the
# scene.pkg for every baselined ID, tars them deterministically, splits the
# archive into GitHub-release-sized volumes, and (optionally) publishes the
# parts to a private release. The regression-full workflow reassembles the
# parts, verifies SHA256SUMS, renders the corpus and diffs it against the
# committed baseline.
#
# Usage:
#   scripts/package_ci_regression.sh [workshop_dir] [--tag TAG] [--create-release]
#
# Defaults:
#   workshop_dir = ~/.local/share/Steam/steamapps/workshop/content/431960
#   tag          = ci-regression-data-v1
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASELINE_DIR="$REPO_ROOT/tests/regression/baseline"
OUT_DIR="$REPO_ROOT/ci-regression-bundle"
VOL_SIZE="1900M"   # GitHub release assets cap at 2 GiB

workshop="$HOME/.local/share/Steam/steamapps/workshop/content/431960"
tag="ci-regression-data-v1"
create_release=0

while [ $# -gt 0 ]; do
    case "$1" in
        --tag) tag="$2"; shift 2 ;;
        --create-release) create_release=1; shift ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) workshop="$1"; shift ;;
    esac
done

[ -d "$BASELINE_DIR" ] || { echo "baseline dir missing: $BASELINE_DIR" >&2; exit 1; }
[ -d "$workshop" ] || { echo "workshop dir missing: $workshop" >&2; exit 1; }

# 1. Freeze: stage scene.pkg for every baselined ID. A baseline without its
#    source pkg is a broken freeze — fail loudly instead of shipping a
#    partial corpus.
echo "== staging scene.pkgs for baselined IDs =="
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/stage"
count=0
for png in "$BASELINE_DIR"/*.png; do
    id="$(basename "$png" .png)"
    pkg="$workshop/$id/scene.pkg"
    if [ ! -f "$pkg" ]; then
        echo "ERROR: baseline $id has no scene.pkg under $workshop" >&2
        exit 1
    fi
    mkdir -p "$OUT_DIR/stage/$id"
    cp "$pkg" "$OUT_DIR/stage/$id/scene.pkg"
    count=$((count + 1))
done
echo "staged $count scene.pkgs"

# 2. Deterministic tar: sorted names, fixed owner/mtime so re-packaging the
#    same corpus yields identical hashes (parts become cacheable/stable).
echo "== tar (deterministic) =="
tar -C "$OUT_DIR/stage" \
    --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime='UTC 2026-01-01' \
    -cf "$OUT_DIR/regression-corpus.tar" .

echo "== split into ${VOL_SIZE} volumes =="
split -b "$VOL_SIZE" -d "$OUT_DIR/regression-corpus.tar" "$OUT_DIR/regression-corpus.tar.part-"

# 3. Manifest covers the whole tar and each volume so CI can verify both
#    stages of reassembly.
(cd "$OUT_DIR" && sha256sum regression-corpus.tar regression-corpus.tar.part-* > SHA256SUMS)

echo
echo "== bundle at $OUT_DIR =="
ls -lh "$OUT_DIR/regression-corpus.tar"* "$OUT_DIR/SHA256SUMS"
echo "(staged corpus: $(du -sh "$OUT_DIR/stage" | cut -f1))"
echo
echo "SHA256SUMS:"
cat "$OUT_DIR/SHA256SUMS"

if [ "$create_release" -eq 1 ]; then
    echo
    echo "== publishing release $tag =="
    gh release create "$tag" \
        --title "CI regression corpus ($tag)" \
        --notes "Private Wallpaper Engine workshop scene.pkgs for the full-library regression workflow. See SHA256SUMS for integrity hashes. Regenerate with scripts/package_ci_regression.sh." \
        --latest=false
    (cd "$OUT_DIR" && gh release upload "$tag" SHA256SUMS regression-corpus.tar.part-*)
    echo "published: $(gh release view "$tag" --json assets -q '.assets[].name' | tr '\n' ' ')"
else
    echo
    echo "Bundle ready. Publish with:"
    echo "  gh release create $tag --title 'CI regression corpus' --notes '...' --latest=false"
    echo "  cd $OUT_DIR && gh release upload $tag SHA256SUMS regression-corpus.tar.part-*"
fi
