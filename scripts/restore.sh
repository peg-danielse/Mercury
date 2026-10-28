#!/bin/bash
# Restore a snapshot made by scripts/backup.sh into a NEW directory. The live tree is never touched.
#
#   scripts/restore.sh SNAPSHOT [DEST]
#
# DEST defaults to data/restore/<snapshot name>. The result is a git repository at the snapshot's commit, with:
#   - the snapshot's files (tracked and untracked) in the working tree, and its staged changes in the index;
#   - the submodules cloned from their bundles at their recorded commits.
# To compare or resume an experiment, run from DEST; to put a snapshot back into the live tree, copy the files you need.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SNAP=$(cd "${1:?usage: restore.sh SNAPSHOT [DEST]}" && pwd)
DEST=${2:-$ROOT/data/restore/$(basename "$SNAP")}
[ -e "$DEST" ] && { echo "$DEST already exists: choose another DEST"; exit 1; }

mkdir -p "$(dirname "$DEST")"
git clone -q --no-checkout "$ROOT" "$DEST"
git -C "$DEST" checkout -q "$(cat "$SNAP/HEAD")"

# The working tree of the snapshot (tracked and untracked files) goes over the checkout.
tar -xzf "$SNAP/source.tar.gz" -C "$DEST"

# The staged changes are restored into the index only: the working tree already holds them.
if [ -s "$SNAP/staged.patch" ]; then
  git -C "$DEST" apply --cached "$SNAP/staged.patch"
fi

# Files the snapshot had deleted (still present in the commit): gone again, from the working tree and the index.
while IFS= read -r line; do
  st=${line:0:2}; path=${line:3}
  case $st in
    " D") rm -f -- "$DEST/$path" ;;                        # deleted, not staged: the index keeps the file
    "D "|"DD"|"AD") rm -f -- "$DEST/$path"; git -C "$DEST" rm -q --cached --ignore-unmatch -- "$path" ;;
  esac
done < "$SNAP/status.txt"

# Each submodule: its bundle, checked out at the commit the superproject recorded.
sed 's/^.//' "$SNAP/submodules.txt" | while read -r sha path _; do
  name=${path//\//-}
  [ -f "$SNAP/$name.bundle" ] || { echo "no bundle for $path in the snapshot" >&2; continue; }
  rm -rf -- "$DEST/$path"
  git clone -q "$SNAP/$name.bundle" "$DEST/$path"
  git -C "$DEST/$path" checkout -q "$sha"
done

echo "restored $SNAP into $DEST"
echo "  at $(git -C "$DEST" rev-parse --short HEAD), $(git -C "$DEST" status --short | wc -l) changes in the working tree"
