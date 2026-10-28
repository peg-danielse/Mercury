# scripts

Helpers for the Mercury repository itself. The benchmark scripts live with their benchmarks (`benchmarks/`).

**backup.sh** takes a snapshot of the repository into `data/backups/<timestamp>/`. It records the commit, the uncommitted
changes (staged and unstaged), the tracked and untracked files that git does not ignore (archived as `source.tar.gz`,
excluding `data/`), and a git bundle of each initialised submodule, so the full submodule history is kept without its
working tree. It prints the snapshot directory on stdout and its status on stderr. Set `KEEP=N` to keep only the newest N
snapshots. `src/test/compare_systems.sh` runs it at the start of every experiment, so each experiment can be traced back
to the exact state it ran from. There is no schedule.

**restore.sh** rebuilds a snapshot into a new directory, `data/restore/<name>` by default, or the directory given as the
second argument. The result is a git repository at the snapshot's commit, with the snapshot's files in the working tree,
its staged changes in the index, and the submodules cloned from their bundles at the recorded commits. It never touches
the live tree, so an experiment can be inspected or rerun from a snapshot without risking current work. Copy individual
files back by hand when you need them in the live tree.
