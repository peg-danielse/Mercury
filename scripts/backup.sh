#!/bin/bash
# Snapshot the Mercury repository, so the state an experiment ran from can be restored (scripts/restore.sh).
# src/test/compare_systems.sh takes one at the start of every experiment. There is no schedule.
#
#   scripts/backup.sh     one snapshot; its directory is the only line on stdout, messages go to stderr
#
# A snapshot is data/backups/<timestamp>/ and holds:
#   source.tar.gz       every tracked and untracked file that git does not ignore, except data/ (results, seeds)
#   HEAD, status.txt    the commit and the state of the work
#   staged.patch, unstaged.patch   the uncommitted changes
#   submodules.txt      the recorded submodule commits
#   <submodule>.bundle  every ref and commit of each initialised submodule (local commits included)
# Ignored files (builds, run outputs, daemon data) are not included: they are reproducible or large.
#
# Environment:
#   BACKUP_DIR   where snapshots go (default: data/backups)
#   KEEP         keep only the newest N snapshots (default 0: keep all)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT_ROOT=${BACKUP_DIR:-$ROOT/data/backups}
KEEP=${KEEP:-0}
TS=$(date +%Y%m%d-%H%M%S)
DEST=$OUT_ROOT/$TS
mkdir -p "$DEST"
cd "$ROOT"

git rev-parse --is-inside-work-tree >/dev/null
git rev-parse HEAD > "$DEST/HEAD"
git status --short > "$DEST/status.txt"
git diff --cached > "$DEST/staged.patch"
git diff > "$DEST/unstaged.patch"
git submodule status > "$DEST/submodules.txt" || true

# Tracked and untracked files that git does not ignore (a submodule is only its bundle, below), except data/ (results and
# seed snapshots, kept where they are; the snapshots live there). A tracked file deleted in the working tree is skipped.
git ls-files -z --cached --others --exclude-standard \
  | grep -zv '^data/' \
  | tar --null --no-recursion --ignore-failed-read -czf "$DEST/source.tar.gz" -T - 2>/dev/null

# Each initialised submodule: its full history as a bundle, and its own uncommitted state.
git submodule foreach --quiet --recursive '
  name=$(echo "$sm_path" | tr "/" "-")
  git bundle create -q "'"$DEST"'/$name.bundle" --all 2>/dev/null || echo "no bundle for $sm_path"
  git status --short > "'"$DEST"'/$name.status.txt"
' >&2 || true

echo "snapshot $DEST ($(du -sh "$DEST" | cut -f1))" >&2

# Optional pruning of the oldest snapshots (timestamped directories only).
if [ "$KEEP" -gt 0 ]; then
  ls -1d "$OUT_ROOT"/[0-9]*-[0-9]* 2>/dev/null | sort | head -n -"$KEEP" | while read -r old; do
    rm -rf -- "$old" && echo "pruned $old" >&2
  done
fi

echo "$DEST"
