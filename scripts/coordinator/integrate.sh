#!/usr/bin/env bash
# Integrate one worker branch into main, linearly, with verification.
#
#   integrate.sh stuxnet147/w1-semantic-c-frontend
#
# fetch -> rebase the branch onto main -> ff-only merge -> build -> ctest ->
# push. Any failure stops right there: a broken rebase leaves the temp branch
# for inspection, a red test leaves main unpushed.
#
# pipefail is load-bearing: ctest is piped through tail, and without it the
# pipeline reported tail's exit status, so a red suite got pushed once
# (2026-08-10) before this line existed.
set -euo pipefail

branch="${1:?usage: integrate.sh <remote-branch-name>}"
preset="${PRESET:-windows-clang}"

git fetch origin --prune

ahead="$(git log --oneline "origin/main..origin/$branch" | wc -l)"
if [ "$ahead" -eq 0 ]; then
    echo "origin/$branch has nothing beyond origin/main; nothing to do"
    exit 0
fi
echo "== integrating $ahead commit(s) from origin/$branch"
git log --oneline "origin/main..origin/$branch" | cat

git checkout -q -B integ-tmp "origin/$branch"
if ! git rebase -q main; then
    echo "!! rebase stopped with conflicts. Resolve on integ-tmp, then:" >&2
    echo "   git checkout main && git merge --ff-only integ-tmp" >&2
    exit 1
fi
git checkout -q main
git merge --ff-only integ-tmp
git branch -q -D integ-tmp

echo "== building ($preset)"
warnings="$(cmake --build --preset "$preset" --parallel 2>&1 | grep -cE 'error|warning' || true)"
echo "   warnings/errors: $warnings"

echo "== testing ($preset)"
ctest --preset "$preset" 2>&1 | tail -3

echo "== pushing"
git push origin main
git log --oneline -1 | cat
