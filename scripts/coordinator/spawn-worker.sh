#!/usr/bin/env bash
# Spawn one supervised worker: worktree, Opus 5 medium Claude, task, inject.
#
#   spawn-worker.sh w5-miter-memory docs/workstreams/W5.md
#
# The two launch flags are both measured requirements: --effort medium is the
# user's standing instruction for workers, and without bypass permissions the
# agent stalls on its first tool call. worker-start returns an empty receipt
# on this Orca build, so this uses the low-level path throughout.
set -eu
cd "$(dirname "$0")/../.."
. scripts/coordinator/env.sh

name="${1:?usage: spawn-worker.sh <worktree-name> <brief-path>}"
brief="${2:?usage: spawn-worker.sh <worktree-name> <brief-path>}"
[ -f "$brief" ] || { echo "brief not found: $brief" >&2; exit 1; }

echo "== worktree"
create_json="$(orca worktree create --repo "id:$QL_ORCA_REPO_ID" --name "$name" --no-parent --json)"
wt_id="$(printf '%s' "$create_json" | python -c "import sys,json;print(json.load(sys.stdin)['result']['worktree']['id'])")"
echo "   $wt_id"

echo "== terminal (Opus 5 medium, bypass)"
handle="$(orca terminal create --worktree "id:$wt_id" --title "$name" \
    --command "$QL_WORKER_COMMAND" --json \
    | python -c "import sys,json;print(json.load(sys.stdin)['result']['terminal']['handle'])")"
echo "   $handle"
orca terminal wait --terminal "$handle" --for tui-idle --timeout-ms 60000 --json > /dev/null

echo "== task + dispatch"
spec="docs/workstreams/README.md 와 $brief 를 읽고 그 지시대로 작업합니다. 브랜치 $name 을 만들어 그 위에서만 작업하고 main 에 직접 커밋하거나 푸시하지 않습니다. 진행 기록은 ${brief%.md}-progress.md 에 계속 갱신합니다. 규약: push 마다 status 로 커밋 해시와 요약, 판단 필요시 ask, 지시서 종료 조건이 전부 닫히면 worker_done."
task_id="$(orca orchestration task-create --spec "$spec" --json \
    | python -c "import sys,json;print(json.load(sys.stdin)['result']['task']['id'])")"
dispatch_id="$(orca orchestration dispatch --task "$task_id" --to "$handle" --inject --json \
    | python -c "import sys,json;print(json.load(sys.stdin)['result']['dispatch']['id'])")"

echo
echo "worktree : $wt_id"
echo "terminal : $handle"
echo "task     : $task_id"
echo "dispatch : $dispatch_id"
