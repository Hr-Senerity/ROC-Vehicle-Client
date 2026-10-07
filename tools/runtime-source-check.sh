#!/usr/bin/env bash
set -euo pipefail
[[ $# -eq 1 ]] || exit 2
repo=$1
git_cmd=(git -c "safe.directory=$repo" -C "$repo")
[[ -z $("${git_cmd[@]}" status --porcelain) ]] || exit 2
# Reject private/document paths in the actual commit, not just the worktree.
! "${git_cmd[@]}" ls-tree -r --name-only HEAD | grep -E '^(docs|contracts|Connect|out|runtime-private)/' || exit 2
