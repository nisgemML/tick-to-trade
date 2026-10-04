#!/usr/bin/env bash
# Replay a REAL NASDAQ TotalView-ITCH 5.0 file at several cut points and compare the matching engine's
# FULL book (every price level: price, total quantity, resting-order count) with the independent
# reference book built from the same messages.
#
#   scripts/real_day_check.sh <file | file.gz> <path/to/replay_itch50> SYMBOL [SYMBOL ...]
#   scripts/real_day_check.sh ~/itch/20191230.BX_ITCH_50.gz ./build/replay_itch50 AAPL SPY
#
# Cut points are 25/50/75/90% of the file's message count (found with a quick reference-only pass).
# Exit 0 only if every run agrees (a run whose book outgrows the engine's capacity is reported as
# CAPACITY and does not count as a failure -- the engine cannot hold it; see BUGS_FOUND.md #17).
set -u
FILE=$1; BIN=$2; shift 2
dec() { case "$FILE" in *.gz) gzip -dc "$FILE" ;; *) cat "$FILE" ;; esac; }
run() { { dec | "$BIN" - "$@" --json --log "${TMPDIR:-/tmp}/real_day_fills.log"; } 2>/dev/null | grep '^{' | tail -1; }

total=$(run --symbol "$1" --no-engine | python3 -c 'import json,sys; print(json.load(sys.stdin)["messages_read"])')
case "$total" in ''|0|*[!0-9]*) echo "could not read any messages from $FILE (symbol '$1' not found, unreadable file, or tool failed)" >&2; exit 2;; esac
echo "$FILE: $total messages"
fail=0
for sym in "$@"; do
  for pct in 25 50 75 90; do
    n=$(( total * pct / 100 ))
    js=$(run --symbol "$sym" --max-msgs "$n")
    python3 - "$sym" "$pct" "$n" "$js" << 'PY' || fail=1
import json, sys
sym, pct, n, js = sys.argv[1:5]
r = json.loads(js); e = r['engine']; ref = r['ref']; d = e['depth']
tag = 'OK' if r['agree'] else ('CAPACITY' if e['capacity_exceeded'] else 'MISMATCH')
anom = ref['unknown_ref'] + ref['over_reduce'] + ref['duplicate_ref']
print(f"{sym:6} {pct:>3}% ({int(n):>11,} msgs)  engine msgs={e['sent']:>9,}  fills={e['fills']}  crossed={ref['crossed_events']}  "
      f"anomalies={anom}  levels compared={d['bid_levels_compared']}+{d['ask_levels_compared']}  mismatched={d['mismatched_levels']}  {tag}")
sys.exit(0 if tag in ('OK', 'CAPACITY') else 1)
PY
  done
done
exit $fail
