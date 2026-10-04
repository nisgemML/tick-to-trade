#!/usr/bin/env python3
"""End-to-end check of tools/replay_itch50 against tests/data/synth_day.expected.json.

The expected books come from two independent Python models (see gen_itch50_oracle.py), one
built from generator intent and one from what the third-party itchfeed parser reads out of the
bytes. For each symbol this runs the real C++ replay -- real NASDAQ file framing, real pipeline,
real matching engine -- both from the start of the day and from a mid-day snapshot, and checks:
reference book, per-type counts, translator counters, and that the ENGINE'S final book equals
the expected book with zero fills (the synthetic book never crosses, so any fill is a bug).
"""
import json, os, subprocess, sys, tempfile

replay, datadir = sys.argv[1], sys.argv[2]
exp = json.load(open(os.path.join(datadir, 'synth_day.expected.json')))
itch = os.path.join(datadir, 'synth_day.itch')
fails = 0

def run(sym, extra):
    with tempfile.TemporaryDirectory() as td:
        p = subprocess.run([replay, itch, '--symbol', sym, '--json', '--log', os.path.join(td, 'fills.log')] + extra,
                           capture_output=True, text=True, timeout=180)
    js = [l for l in p.stdout.splitlines() if l.startswith('{')]
    return p.returncode, (json.loads(js[-1]) if js else None), p.stderr

def check(cond, msg):
    global fails
    if not cond:
        fails += 1; print(f"  FAIL: {msg}")

for sym, e in exp['symbols'].items():
    for mode, extra in [('from start', []), ('snapshot at mid-day', ['--engine-start-ns', str(exp['engine_start_ns'])]), ('reference only', ['--no-engine'])]:
        rc, r, err = run(sym, extra)
        print(f"{sym} [{mode}]: rc={rc}")
        check(r is not None and rc == 0, f"replay failed rc={rc}\n{err}")
        if r is None: continue
        pt = {k: v for k, v in r['per_type'].items() if v}
        check(r['found'] and r['target_locate'] == e['locate'], "wrong locate")
        check(r['messages_read'] == exp['messages_total'], f"messages_read {r['messages_read']} != {exp['messages_total']}")
        check(r['truncated'] == 0 and r['malformed'] == 0, "truncated/malformed frames")
        check(pt == e['per_type'], f"per-type counts {pt} != {e['per_type']}")
        ref = r['ref']
        for k in ('orders', 'bid_px', 'bid_qty', 'ask_px', 'ask_qty', 'bid_levels', 'ask_levels'):
            check(ref[k] == e[k], f"ref.{k} {ref[k]} != expected {e[k]}")
        check(ref['crossed_events'] == 0 and ref['unknown_ref'] == 0 and ref['over_reduce'] == 0 and ref['duplicate_ref'] == 0, f"reference-book anomalies: {ref}")
        t = r['translator']; c = e['per_type']
        check(t['adds'] == c.get('A', 0) + c.get('F', 0) and t['replaces'] == c.get('U', 0) and t['deletes'] == c.get('D', 0)
              and t['execs'] == c.get('E', 0) + c.get('C', 0) and t['partial_cancels'] == c.get('X', 0) and t['unknown_ref'] == 0, f"translator counters {t}")
        en = r['engine']
        if mode == 'reference only':
            check(not en['ran'], "engine ran with --no-engine")
        else:
            check(en['ran'] and r['agree'], "engine book disagrees with reference")
            check(en['fills'] == 0, f"engine produced {en['fills']} fills on a never-crossing book")
            check(en['sent'] == en['processed'] and en['sent'] > 0, f"engine processed {en['processed']} of {en['sent']} sent")
            for k in ('bid_px', 'bid_qty', 'ask_px', 'ask_qty'):
                check(en[k] == e[k], f"engine.{k} {en[k]} != expected {e[k]}")
            dp = en['depth']   # FULL book: every level's price, total quantity and order count, engine vs reference
            check(dp['mismatched_levels'] == 0, f"{dp['mismatched_levels']} price level(s) differ between engine and reference")
            check(dp['bid_levels_compared'] == e['bid_levels'] and dp['ask_levels_compared'] == e['ask_levels'],
                  f"compared {dp['bid_levels_compared']}+{dp['ask_levels_compared']} levels, expected {e['bid_levels']}+{e['ask_levels']}")

# capacity overflow must be detected and reported distinctly (exit 4), never silently compared
rc, r, err = run('SYNB', ['--assume-engine-capacity', '100'])
print(f"SYNB [forced capacity 100]: rc={rc}")
check(rc == 4 and r is not None and r['engine']['capacity_exceeded'] and r['agree'] is False and 'ENGINE CAPACITY EXCEEDED' in err,
      f"capacity overflow not reported: rc={rc} engine={r and r['engine']}")
rc, r, _ = run('SYNB', ['--assume-engine-capacity', '100000'])
check(rc == 0 and r['engine']['capacity_exceeded'] is False and r['engine']['peak_resting'] > 0, "peak_resting not reported correctly under normal capacity")

rc, r, _ = run('NOPE', ['--no-engine'])
print(f"NOPE [unknown symbol]: rc={rc}")
check(rc == 3 and r is not None and r['found'] is False, "unknown symbol must exit 3 with found=false")

print("PASS: replay matches the independently derived books" if fails == 0 else f"FAIL: {fails} check(s) failed")
sys.exit(1 if fails else 0)
