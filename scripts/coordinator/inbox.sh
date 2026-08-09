#!/usr/bin/env bash
# Read the coordinator's orchestration inbox in a readable form.
#
#   inbox.sh            oldest unacknowledged delivery (consumable batch)
#   inbox.sh --peek     read-only look at unread mail, consumes nothing
#   inbox.sh --ack ID   acknowledge a delivery, then show what remains
set -u

mode_args=()
case "${1:-}" in
    --peek) mode_args=(--peek) ;;
    --ack)  [ -n "${2:-}" ] || { echo "usage: inbox.sh --ack <deliveryId>" >&2; exit 2; }
            mode_args=(--ack "$2") ;;
    "") ;;
    *) echo "usage: inbox.sh [--peek | --ack <deliveryId>]" >&2; exit 2 ;;
esac

orca orchestration check "${mode_args[@]}" --json 2>&1 | python -c "
import sys, json
d = json.load(sys.stdin)
r = d.get('result', {})
if r.get('acknowledged'):
    print('acknowledged:', r.get('acknowledged'))
print('delivery: %s | count: %s' % (r.get('deliveryId'), r.get('count')))
for m in r.get('messages', []):
    frm = (m.get('from_handle') or '?')[:28]
    print()
    print('=== %s | %s | from %s' % (m.get('id'), m.get('type'), frm))
    subject = (m.get('subject') or '').strip()
    if subject:
        print('subject:', subject)
    body = (m.get('body') or '').strip()
    if body:
        print(body[:1500])
        if len(body) > 1500:
            print('... (%d bytes total)' % len(body))
    payload = m.get('payload')
    if payload:
        print('payload:', json.dumps(payload, ensure_ascii=False)[:300])
"
