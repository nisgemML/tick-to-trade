#!/usr/bin/env python3
"""Generate spec-accurate NASDAQ TotalView-ITCH 5.0 test data from an INDEPENDENT oracle.

The oracle is the third-party `itchfeed` package (pip install itchfeed). Bytes are packed
with the oracle's own `message_format` strings, and every expected field value is read back
out of the oracle's own parser -- none of it passes through this repository's C++ code, so
the C++ parser under test (include/hft/itch50.hpp) is checked against an implementation
that shares no code, and no reading of the spec, with it.

Outputs (default dir: ../tests/data):
  itch50_oracle.txt        one line per message: <type> <hex body> k=v k=v ...  (parser test)
  synth_day.itch           framed [2-byte BE length][body] stream, NASDAQ file format
  synth_day.expected.json  expected final book per symbol, from two independent Python models
"""
import json, os, random, struct, sys
from itch import messages as M
from itch.parser import MessageParser

def ts_split(ts): return (ts >> 32) & 0xFFFF, ts & 0xFFFFFFFF

def build(cls, locate, tracking, ts, *fields):
    t1, t2 = ts_split(ts)
    return cls.message_type + struct.pack(cls.message_format, locate, tracking, t1, t2, *fields)

def frame(b): return struct.pack('>H', len(b)) + b

BOOK_TYPES = b'AFECXDU'
ALL_PARSE = b'RAFECXDU'

# ---------------------------------------------------------------- oracle test vectors
def gen_oracle_cases(rnd):
    lines = []
    def rint(bits): return rnd.getrandbits(bits)
    def emit(t, body, cls):
        framed = frame(body)
        msgs = list(MessageParser(message_type=t).parse_stream(framed))
        assert len(msgs) == 1, (t, len(msgs))
        o = msgs[0]
        kv = {'locate': o.stock_locate, 'tracking': o.tracking_number, 'timestamp': o.timestamp}
        tt = t.decode()
        if tt == 'R':   kv['stock'] = o.stock.hex()
        elif tt in 'AF':
            kv.update(order_ref=o.order_reference_number, side=o.buy_sell_indicator.decode(),
                      shares=o.shares, stock=o.stock.hex(), price=o.price)
        elif tt == 'E': kv.update(order_ref=o.order_reference_number, shares=o.executed_shares, match=o.match_number)
        elif tt == 'C': kv.update(order_ref=o.order_reference_number, shares=o.executed_shares, match=o.match_number,
                                  printable=o.printable.decode(), price=o.execution_price)
        elif tt == 'X': kv.update(order_ref=o.order_reference_number, shares=o.cancelled_shares)
        elif tt == 'D': kv.update(order_ref=o.order_reference_number)
        elif tt == 'U': kv.update(order_ref=o.order_reference_number, new_ref=o.new_order_reference_number,
                                  shares=o.shares, price=o.price)
        lines.append(f"{tt} {body.hex()} " + " ".join(f"{k}={v}" for k, v in kv.items()))
    syms = [b'AAPL    ', b'SPY     ', b'A       ', b'ZVZZT   ', b'BRK.A   ']
    for i in range(150):
        loc, trk, ts = rint(16), rint(16), rint(48)
        st = rnd.choice(syms)
        emit(b'R', build(M.StockDirectoryMessage, loc, trk, ts, st, b'Q', b'N', rint(32), b'N', b'C', b'Z ', b'P', b'N', b'N', b'1', b'N', 0, b'N'), M.StockDirectoryMessage)
        emit(b'A', build(M.AddOrderNoMPIAttributionMessage, loc, trk, ts, rint(64), rnd.choice([b'B', b'S']), rint(32), st, rint(32)), None)
        emit(b'F', build(M.AddOrderMPIDAttribution, loc, trk, ts, rint(64), rnd.choice([b'B', b'S']), rint(32), st, rint(32), b'NSDQ'), None)
        emit(b'E', build(M.OrderExecutedMessage, loc, trk, ts, rint(64), rint(32), rint(64)), None)
        emit(b'C', build(M.OrderExecutedWithPriceMessage, loc, trk, ts, rint(64), rint(32), rint(64), rnd.choice([b'Y', b'N']), rint(32)), None)
        emit(b'X', build(M.OrderCancelMessage, loc, trk, ts, rint(64), rint(32)), None)
        emit(b'D', build(M.OrderDeleteMessage, loc, trk, ts, rint(64)), None)
        emit(b'U', build(M.OrderReplaceMessage, loc, trk, ts, rint(64), rint(64), rint(32), rint(32)), None)
    # explicit boundaries: all-zero and all-max for every field width
    for (loc, trk, ts, r, sh, px) in [(0, 0, 0, 0, 0, 0), (0xFFFF, 0xFFFF, (1 << 48) - 1, (1 << 64) - 1, 0xFFFFFFFF, 0xFFFFFFFF)]:
        emit(b'A', build(M.AddOrderNoMPIAttributionMessage, loc, trk, ts, r, b'B', sh, b'AAPL    ', px), None)
        emit(b'F', build(M.AddOrderMPIDAttribution, loc, trk, ts, r, b'S', sh, b'AAPL    ', px, b'NSDQ'), None)
        emit(b'E', build(M.OrderExecutedMessage, loc, trk, ts, r, sh, r), None)
        emit(b'C', build(M.OrderExecutedWithPriceMessage, loc, trk, ts, r, sh, r, b'Y', px), None)
        emit(b'X', build(M.OrderCancelMessage, loc, trk, ts, r, sh), None)
        emit(b'D', build(M.OrderDeleteMessage, loc, trk, ts, r), None)
        emit(b'U', build(M.OrderReplaceMessage, loc, trk, ts, r, r, sh, px), None)
    return lines

# ---------------------------------------------------------------- synthetic trading day
def gen_day(seed=20260101, n_events=3000):
    rnd = random.Random(seed)
    syms = [(1, b'SYNA    '), (2, b'SYNB    '), (3, b'SYNC    ')]
    ts = 25_200_000_000_000
    out, orders = [], {loc: {} for loc, _ in syms}
    gcount, scount = {}, {loc: {} for loc, _ in syms}
    mid_ts = None
    def tick():
        nonlocal ts
        ts += rnd.randint(1, 50_000); return ts
    def rec(t, loc):
        gcount[t] = gcount.get(t, 0) + 1
        scount[loc][t] = scount[loc].get(t, 0) + 1
    # bids strictly below 100.0000, asks at/above it => the book can never cross
    rng = {'B': (995000, 999900), 'S': (1000000, 1005000)}
    def rprice(side): lo, hi = rng[side]; return lo + 100 * rnd.randint(0, (hi - lo) // 100)
    ref_ctr, match_ctr = 1000, 9000
    t1, t2 = ts_split(tick())
    out.append(b'S' + struct.pack('!HHHIc', 0, 0, t1, t2, b'O'))
    for loc, st in syms:
        out.append(build(M.StockDirectoryMessage, loc, 0, tick(), st, b'Q', b'N', 100, b'N', b'C', b'Z ', b'P', b'N', b'N', b'1', b'N', 0, b'N'))
        gcount['R'] = gcount.get('R', 0) + 1
    names = {loc: st for loc, st in syms}
    book_events = 0
    for i in range(n_events):
        loc = rnd.choice(list(names)); st = names[loc]; o = orders[loc]
        x = rnd.random()
        if x >= 0.80:   # noise the replay must skip cleanly
            t1, t2 = ts_split(tick())
            if rnd.random() < 0.5:
                out.append(b'H' + struct.pack('!HHHI8scc4s', loc, 0, t1, t2, st, b'T', b' ', b'    '))
            else:
                out.append(b'P' + struct.pack('!HHHIQcI8sIQ', loc, 0, t1, t2, 0, b'B', 100, st, 1000000, match_ctr)); match_ctr += 1
            continue
        if x < 0.40 or not o:
            side = rnd.choice('BS'); px = rprice(side); sh = rnd.randint(1, 500); ref = ref_ctr; ref_ctr += 1
            if rnd.random() < 0.12:
                out.append(build(M.AddOrderMPIDAttribution, loc, 0, tick(), ref, side.encode(), sh, st, px, b'NSDQ')); rec('F', loc)
            else:
                out.append(build(M.AddOrderNoMPIAttributionMessage, loc, 0, tick(), ref, side.encode(), sh, st, px)); rec('A', loc)
            o[ref] = [side, px, sh]
        else:
            ref = rnd.choice(sorted(o)); side, px, qty = o[ref]
            if x < 0.52:   # E
                ex = rnd.randint(1, qty)
                out.append(build(M.OrderExecutedMessage, loc, 0, tick(), ref, ex, match_ctr)); match_ctr += 1; rec('E', loc)
                o[ref][2] -= ex
                if o[ref][2] == 0: del o[ref]
            elif x < 0.56: # C
                ex = rnd.randint(1, qty)
                out.append(build(M.OrderExecutedWithPriceMessage, loc, 0, tick(), ref, ex, match_ctr, b'Y', px + 100)); match_ctr += 1; rec('C', loc)
                o[ref][2] -= ex
                if o[ref][2] == 0: del o[ref]
            elif x < 0.66 and qty > 1:  # X (partial cancel)
                c = rnd.randint(1, qty - 1)
                out.append(build(M.OrderCancelMessage, loc, 0, tick(), ref, c)); rec('X', loc)
                o[ref][2] -= c
            elif x < 0.74 or qty == 1 and x < 0.66:  # D
                out.append(build(M.OrderDeleteMessage, loc, 0, tick(), ref)); rec('D', loc)
                del o[ref]
            else:          # U
                nref = ref_ctr; ref_ctr += 1; nsh = rnd.randint(1, 500); npx = rprice(side)
                out.append(build(M.OrderReplaceMessage, loc, 0, tick(), ref, nref, nsh, npx)); rec('U', loc)
                del o[ref]; o[nref] = [side, npx, nsh]
        book_events += 1
        if book_events == 1500 and mid_ts is None: mid_ts = ts
    return out, orders, gcount, scount, names, mid_ts

def summarize(orders_for_loc):
    bids, asks = {}, {}
    for side, px, qty in orders_for_loc.values():
        d = bids if side == 'B' else asks
        d[px] = d.get(px, 0) + qty
    bb = max(bids) if bids else 0; ba = min(asks) if asks else 0
    return dict(orders=len(orders_for_loc), bid_px=bb, bid_qty=bids.get(bb, 0), ask_px=ba, ask_qty=asks.get(ba, 0),
                bid_levels=len(bids), ask_levels=len(asks))

def model_from_oracle(stream_bytes):
    """Second, independent model: replay what the ORACLE PARSER says is in the file."""
    ords, per_type = {}, {}
    for o in MessageParser(message_type=ALL_PARSE).parse_stream(stream_bytes):
        t = o.message_type.decode()
        if t == 'R': continue
        loc = o.stock_locate; d = ords.setdefault(loc, {}); per = per_type.setdefault(loc, {}); per[t] = per.get(t, 0) + 1
        if t in 'AF': d[o.order_reference_number] = [o.buy_sell_indicator.decode(), o.price, o.shares]
        elif t in 'EC':
            r = d[o.order_reference_number]; r[2] -= o.executed_shares
            if r[2] == 0: del d[o.order_reference_number]
        elif t == 'X': d[o.order_reference_number][2] -= o.cancelled_shares
        elif t == 'D': del d[o.order_reference_number]
        elif t == 'U':
            side = d[o.order_reference_number][0]; del d[o.order_reference_number]
            d[o.new_order_reference_number] = [side, o.price, o.shares]
    return ords, per_type

def main():
    # Resolved here, not at import time: gen_itch50_scale.py imports this module, and a module-level
    # os.makedirs(sys.argv[1]) once created a directory named after that script's OUTPUT FILE.
    OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests', 'data')
    os.makedirs(OUT, exist_ok=True)
    cases = gen_oracle_cases(random.Random(5150))
    with open(os.path.join(OUT, 'itch50_oracle.txt'), 'w') as f: f.write("\n".join(cases) + "\n")
    msgs, orders, gcount, scount, names, mid_ts = gen_day()
    stream = b"".join(frame(m) for m in msgs)
    with open(os.path.join(OUT, 'synth_day.itch'), 'wb') as f: f.write(stream)
    exp = {'messages_total': len(msgs), 'engine_start_ns': mid_ts, 'symbols': {}}
    ords2, per2 = model_from_oracle(stream)
    for loc, st in names.items():
        a = summarize(orders[loc]); b = summarize(ords2.get(loc, {}))
        assert a == b, f"generator model and oracle-parsed model disagree for locate {loc}: {a} vs {b}"
        assert scount[loc] == per2.get(loc, {}), f"per-type counts disagree for locate {loc}"
        exp['symbols'][st.decode().strip()] = dict(locate=loc, per_type=scount[loc], **a)
    with open(os.path.join(OUT, 'synth_day.expected.json'), 'w') as f: json.dump(exp, f, indent=1, sort_keys=True)
    print(f"oracle cases: {len(cases)} lines | synthetic day: {len(msgs)} messages, {len(stream)} bytes")
    for s, v in exp['symbols'].items(): print(f"  {s}: {v['orders']} resting orders, BBO {v['bid_px']}x{v['bid_qty']} / {v['ask_px']}x{v['ask_qty']}, types {v['per_type']}")
    print("two independent Python models (generator intent vs oracle-parsed) agree on every symbol")

if __name__ == "__main__":
    main()
