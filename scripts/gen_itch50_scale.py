#!/usr/bin/env python3
"""Large synthetic ITCH 5.0 file (real NASDAQ framing) for scale/throughput testing of
tools/replay_itch50. NOT real market data -- a never-crossing random order flow, packed with
the third-party oracle's own format strings. Usage: gen_itch50_scale.py OUT.itch [N_EVENTS] [CAP_RESTING_PER_SYMBOL=40000]"""
import json, random, struct, sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from itch import messages as M
from gen_itch50_oracle import build, frame

out_path = sys.argv[1]; n = int(sys.argv[2]) if len(sys.argv) > 2 else 1_500_000
cap = int(sys.argv[3]) if len(sys.argv) > 3 else 40_000   # max resting orders per symbol (engine capacity is 65,536)
rnd = random.Random(7); ts = 25_200_000_000_000
syms = {1: b'SYNA    ', 2: b'SYNB    ', 3: b'SYNC    '}
pool = {l: [] for l in syms}; info = {}   # per-locate list of refs (swap-remove), ref -> [loc, side, px, qty]
ref_ctr, match = 1000, 9000
rng = {'B': (995000, 999900), 'S': (1000000, 1005000)}
def rpx(s): lo, hi = rng[s]; return lo + 100 * rnd.randint(0, (hi - lo) // 100)
def drop(l, r):
    p = pool[l]; i = p.index(r) if len(p) < 64 else None
    if i is None: i = idx[r]
    last = p[-1]; p[i] = last; idx[last] = i; p.pop(); idx.pop(r, None); info.pop(r, None)
idx = {}
def add_to_pool(l, r): pool[l].append(r); idx[r] = len(pool[l]) - 1
with open(out_path, 'wb') as f:
    for l, st in syms.items():
        ts += 100; f.write(frame(build(M.StockDirectoryMessage, l, 0, ts, st, b'Q', b'N', 100, b'N', b'C', b'Z ', b'P', b'N', b'N', b'1', b'N', 0, b'N')))
    for _ in range(n):
        l = rnd.choice(list(syms)); ts += rnd.randint(1, 50); x = rnd.random(); p = pool[l]
        if (x < 0.42 and len(p) < cap) or not p:
            s = rnd.choice('BS'); px = rpx(s); sh = rnd.randint(1, 500); r = ref_ctr; ref_ctr += 1
            f.write(frame(build(M.AddOrderNoMPIAttributionMessage, l, 0, ts, r, s.encode(), sh, syms[l], px))); info[r] = [l, s, px, sh]; add_to_pool(l, r)
        else:
            r = p[rnd.randrange(len(p))]; _, s, px, q = info[r]
            if x < 0.62:
                ex = rnd.randint(1, q); f.write(frame(build(M.OrderExecutedMessage, l, 0, ts, r, ex, match))); match += 1
                if ex == q: drop(l, r)
                else: info[r][3] -= ex
            elif x < 0.74 and q > 1:
                c = rnd.randint(1, q - 1); f.write(frame(build(M.OrderCancelMessage, l, 0, ts, r, c))); info[r][3] -= c
            elif x < 0.90:
                f.write(frame(build(M.OrderDeleteMessage, l, 0, ts, r))); drop(l, r)
            else:
                nr = ref_ctr; ref_ctr += 1; nsh = rnd.randint(1, 500); npx = rpx(s)
                f.write(frame(build(M.OrderReplaceMessage, l, 0, ts, r, nr, nsh, npx))); drop(l, r); info[nr] = [l, s, npx, nsh]; add_to_pool(l, nr)
exp = {}
for l, st in syms.items():
    b, a = {}, {}
    for r in pool[l]:
        _, s, px, q = info[r]; d = b if s == 'B' else a; d[px] = d.get(px, 0) + q
    bb = max(b) if b else 0; ba = min(a) if a else 0
    exp[st.decode().strip()] = dict(orders=len(pool[l]), bid_px=bb, bid_qty=b.get(bb, 0), ask_px=ba, ask_qty=a.get(ba, 0))
json.dump(exp, open(out_path + '.expected.json', 'w'))
print(json.dumps(exp))
