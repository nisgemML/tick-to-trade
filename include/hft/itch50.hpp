#pragma once
// include/hft/itch50.hpp
//
// Spec-accurate NASDAQ TotalView-ITCH 5.0 decoding, a reference order book, and a
// translator onto the matching engine's message model.
//
// Why this exists alongside the vendored feed::ItchAddOrder (udp-multicast-receiver):
// that parser reads a SIMPLIFIED layout -- timestamp at +1, order_ref at +7, price at
// +28 -- while the real NASDAQ spec (TOTALVIEW-ITCH 5.0, v5.0 03/06/2015, section 4)
// puts a 2-byte stock locate at +1 and a 2-byte tracking number at +3, so timestamp is
// at +5, order_ref at +11, side +19, shares +20, stock +24, price +32. Fed a real
// NASDAQ file, the vendored parser silently reads garbage. See BUGS_FOUND.md #15.
// This header implements the real layout; tests/test_itch50_oracle.cpp checks it against
// an independent third-party implementation (the `itchfeed` package), not just itself.
//
// Book semantics follow spec section 4.4: Executed ('E','C') and Cancel ('X') messages
// are cumulative reductions of display shares; an order is dead at zero; Delete ('D')
// removes it; Replace ('U') kills the original and creates a new order under a new
// reference, inheriting the original's side and symbol.

#include "core/types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::itch50 {

// Total message lengths, spec section 4.
constexpr std::size_t kLenStockDirectory = 39;
constexpr std::size_t kLenAdd            = 36;  // 'A'
constexpr std::size_t kLenAddMpid        = 40;  // 'F'
constexpr std::size_t kLenExec           = 31;  // 'E'
constexpr std::size_t kLenExecPrice      = 36;  // 'C'
constexpr std::size_t kLenCancel         = 23;  // 'X'
constexpr std::size_t kLenDelete         = 19;  // 'D'
constexpr std::size_t kLenReplace        = 35;  // 'U'

[[nodiscard]] inline uint16_t be16(const uint8_t* p) noexcept { return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]); }
[[nodiscard]] inline uint32_t be32(const uint8_t* p) noexcept { return (uint32_t(be16(p)) << 16) | be16(p + 2); }
[[nodiscard]] inline uint64_t be48(const uint8_t* p) noexcept { return (uint64_t(be16(p)) << 32) | be32(p + 2); }
[[nodiscard]] inline uint64_t be64(const uint8_t* p) noexcept { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

// Every message starts: type@0 locate@1(2) tracking@3(2) timestamp@5(6).
struct Header {
    uint8_t  type{0};
    uint16_t locate{0};
    uint16_t tracking{0};
    uint64_t timestamp_ns{0};
};

[[nodiscard]] inline bool parse_header(const uint8_t* b, std::size_t len, Header& h) noexcept {
    if (len < 11) return false;
    h.type = b[0]; h.locate = be16(b + 1); h.tracking = be16(b + 3); h.timestamp_ns = be48(b + 5);
    return true;
}

[[nodiscard]] inline std::string trim_symbol(const char* s8) {
    std::size_t n = 8;
    while (n > 0 && (s8[n - 1] == ' ' || s8[n - 1] == '\0')) --n;
    return std::string(s8, n);
}

struct StockDirectory {
    Header h; char stock[9]{};
    [[nodiscard]] static bool parse(const uint8_t* b, std::size_t len, StockDirectory& o) noexcept {
        if (len < kLenStockDirectory || b[0] != 'R' || !parse_header(b, len, o.h)) return false;
        std::memcpy(o.stock, b + 11, 8); o.stock[8] = '\0'; return true;
    }
};

struct AddOrder {
    Header h; uint64_t order_ref{0}; char side{0}; uint32_t shares{0}; char stock[9]{}; uint32_t price{0}; bool has_mpid{false};
    [[nodiscard]] static bool parse(const uint8_t* b, std::size_t len, AddOrder& o) noexcept {
        if (len < 1 || (b[0] != 'A' && b[0] != 'F')) return false;
        if (len < (b[0] == 'A' ? kLenAdd : kLenAddMpid) || !parse_header(b, len, o.h)) return false;
        o.order_ref = be64(b + 11); o.side = char(b[19]); o.shares = be32(b + 20);
        std::memcpy(o.stock, b + 24, 8); o.stock[8] = '\0'; o.price = be32(b + 32); o.has_mpid = (b[0] == 'F');
        return true;
    }
};

// 'E' (order executed) and 'C' (executed with price) share the first 31 bytes.
struct OrderExecuted {
    Header h; uint64_t order_ref{0}; uint32_t shares{0}; uint64_t match{0}; bool has_price{false}; char printable{0}; uint32_t price{0};
    [[nodiscard]] static bool parse(const uint8_t* b, std::size_t len, OrderExecuted& o) noexcept {
        if (len < 1 || (b[0] != 'E' && b[0] != 'C')) return false;
        if (len < (b[0] == 'E' ? kLenExec : kLenExecPrice) || !parse_header(b, len, o.h)) return false;
        o.order_ref = be64(b + 11); o.shares = be32(b + 19); o.match = be64(b + 23);
        o.has_price = (b[0] == 'C');
        if (o.has_price) { o.printable = char(b[31]); o.price = be32(b + 32); }
        return true;
    }
};

struct OrderCancel {
    Header h; uint64_t order_ref{0}; uint32_t canceled_shares{0};
    [[nodiscard]] static bool parse(const uint8_t* b, std::size_t len, OrderCancel& o) noexcept {
        if (len < kLenCancel || b[0] != 'X' || !parse_header(b, len, o.h)) return false;
        o.order_ref = be64(b + 11); o.canceled_shares = be32(b + 19); return true;
    }
};

struct OrderDelete {
    Header h; uint64_t order_ref{0};
    [[nodiscard]] static bool parse(const uint8_t* b, std::size_t len, OrderDelete& o) noexcept {
        if (len < kLenDelete || b[0] != 'D' || !parse_header(b, len, o.h)) return false;
        o.order_ref = be64(b + 11); return true;
    }
};

struct OrderReplace {
    Header h; uint64_t orig_ref{0}; uint64_t new_ref{0}; uint32_t shares{0}; uint32_t price{0};
    [[nodiscard]] static bool parse(const uint8_t* b, std::size_t len, OrderReplace& o) noexcept {
        if (len < kLenReplace || b[0] != 'U' || !parse_header(b, len, o.h)) return false;
        o.orig_ref = be64(b + 11); o.new_ref = be64(b + 19); o.shares = be32(b + 27); o.price = be32(b + 31); return true;
    }
};

// ── Independent reference book ──────────────────────────────────────────────────
// Deliberately separate from the translator below and from the engine: it is the oracle
// the engine's book is compared against, and it is itself checked against a Python model.
struct RefOrder { char side; uint32_t price; uint32_t qty; };

struct Bbo {
    uint32_t bid_px{0}, ask_px{0};
    uint64_t bid_qty{0}, ask_qty{0};
    std::size_t bid_levels{0}, ask_levels{0}, orders{0};
};

struct DepthLevel { uint32_t price; uint64_t qty; uint32_t orders; };

class RefBook {
public:
    struct Anomalies { uint64_t unknown_ref{0}, over_reduce{0}, duplicate_ref{0}; };

    void add(uint64_t ref, char side, uint32_t price, uint32_t qty) {
        if (!orders_.emplace(ref, RefOrder{side, price, qty}).second) { ++an_.duplicate_ref; return; }
        Level& l = level(side)[price]; l.qty += qty; ++l.orders;
    }
    void reduce(uint64_t ref, uint32_t by) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++an_.unknown_ref; return; }
        uint32_t d = by;
        if (by > it->second.qty) { ++an_.over_reduce; d = it->second.qty; }
        it->second.qty -= d;
        sub_level(it->second.side, it->second.price, d, it->second.qty == 0);
        if (it->second.qty == 0) orders_.erase(it);
    }
    void remove(uint64_t ref) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++an_.unknown_ref; return; }
        sub_level(it->second.side, it->second.price, it->second.qty, true);
        orders_.erase(it);
    }
    void replace(uint64_t orig, uint64_t nref, uint32_t shares, uint32_t price) {
        auto it = orders_.find(orig);
        if (it == orders_.end()) { ++an_.unknown_ref; return; }
        const char side = it->second.side;
        remove(orig);
        add(nref, side, price, shares);
    }
    [[nodiscard]] Bbo bbo() const {
        Bbo b; b.orders = orders_.size(); b.bid_levels = bids_.size(); b.ask_levels = asks_.size();
        if (!bids_.empty()) { b.bid_px = bids_.rbegin()->first; b.bid_qty = bids_.rbegin()->second.qty; }
        if (!asks_.empty()) { b.ask_px = asks_.begin()->first;  b.ask_qty = asks_.begin()->second.qty; }
        return b;
    }
    // Every price level, best first (bids descending, asks ascending), with total quantity and
    // number of resting orders -- the same three things the engine's depth API reports.
    [[nodiscard]] std::vector<DepthLevel> depth(char side) const {
        std::vector<DepthLevel> v;
        if (side == 'B') for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) v.push_back({it->first, it->second.qty, it->second.orders});
        else             for (auto it = asks_.begin(); it != asks_.end(); ++it)   v.push_back({it->first, it->second.qty, it->second.orders});
        return v;
    }
    [[nodiscard]] bool crossed() const {
        return !bids_.empty() && !asks_.empty() && bids_.rbegin()->first >= asks_.begin()->first;
    }
    [[nodiscard]] const Anomalies& anomalies() const { return an_; }
private:
    struct Level { uint64_t qty{0}; uint32_t orders{0}; };
    std::map<uint32_t, Level>& level(char s) { return s == 'B' ? bids_ : asks_; }
    void sub_level(char s, uint32_t px, uint32_t q, bool order_gone) {
        auto& m = level(s); auto it = m.find(px);
        if (it == m.end()) return;
        it->second.qty -= std::min<uint64_t>(it->second.qty, q);
        if (order_gone && it->second.orders > 0) --it->second.orders;
        if (it->second.qty == 0) m.erase(it);
    }
    std::unordered_map<uint64_t, RefOrder> orders_;
    std::map<uint32_t, Level> bids_, asks_;
    Anomalies an_;
};

// ── Translator: ITCH book events -> engine::MarketDataMsg ───────────────────────────
// ITCH price (4 implied decimals) -> engine price (6 implied decimals) is an exact x100.
// E/C/X (reductions) become ModifyOrder(remaining) -- the engine's modify_order keeps
// queue priority on a reduction -- or CancelOrder when the order reaches zero. The
// pre-existing hft::ItchAdapter mapped 'X' to a FULL cancel and ignored 'E' and 'U'
// (BUGS_FOUND.md #16); this does not.
class Translator {
public:
    struct Out { engine::MarketDataMsg msgs[2]; int n{0}; };
    struct Counters { uint64_t adds{0}, execs{0}, partial_cancels{0}, deletes{0}, replaces{0}, unknown_ref{0}; };

    explicit Translator(engine::SymbolId sym) : sym_(sym) {}

    Out on_add(uint64_t ref, char side, uint32_t price, uint32_t shares) {
        Out o; orders_[ref] = RefOrder{side, price, shares}; ++c_.adds;
        o.msgs[o.n++] = make(engine::MarketDataMsg::Type::NewOrder, ref, price, shares, side);
        return o;
    }
    Out on_reduce(uint64_t ref, uint32_t by, bool is_exec) {
        Out o; auto it = orders_.find(ref);
        if (it == orders_.end()) { ++c_.unknown_ref; return o; }
        const uint32_t d = std::min(by, it->second.qty);
        it->second.qty -= d;
        ++(is_exec ? c_.execs : c_.partial_cancels);
        if (it->second.qty == 0) {
            o.msgs[o.n++] = make(engine::MarketDataMsg::Type::CancelOrder, ref, it->second.price, 0, it->second.side);
            orders_.erase(it);
        } else {
            o.msgs[o.n++] = make(engine::MarketDataMsg::Type::ModifyOrder, ref, it->second.price, it->second.qty, it->second.side);
        }
        return o;
    }
    Out on_delete(uint64_t ref) {
        Out o; auto it = orders_.find(ref);
        if (it == orders_.end()) { ++c_.unknown_ref; return o; }
        ++c_.deletes;
        o.msgs[o.n++] = make(engine::MarketDataMsg::Type::CancelOrder, ref, it->second.price, 0, it->second.side);
        orders_.erase(it);
        return o;
    }
    Out on_replace(uint64_t orig, uint64_t nref, uint32_t shares, uint32_t price) {
        Out o; auto it = orders_.find(orig);
        if (it == orders_.end()) { ++c_.unknown_ref; return o; }
        ++c_.replaces;
        const char side = it->second.side;
        o.msgs[o.n++] = make(engine::MarketDataMsg::Type::CancelOrder, orig, it->second.price, 0, side);
        orders_.erase(it);
        orders_[nref] = RefOrder{side, price, shares};
        o.msgs[o.n++] = make(engine::MarketDataMsg::Type::NewOrder, nref, price, shares, side);
        return o;
    }
    // Resting orders as NewOrders in ascending reference order (references are day-unique
    // and increase with arrival, so this preserves time priority) -- used to start the
    // engine mid-stream from a snapshot, as a real consumer would from a book snapshot.
    [[nodiscard]] std::vector<engine::MarketDataMsg> snapshot() {
        std::vector<uint64_t> refs; refs.reserve(orders_.size());
        for (const auto& kv : orders_) refs.push_back(kv.first);
        std::sort(refs.begin(), refs.end());
        std::vector<engine::MarketDataMsg> out; out.reserve(refs.size());
        for (uint64_t r : refs) { const auto& o = orders_[r]; out.push_back(make(engine::MarketDataMsg::Type::NewOrder, r, o.price, o.qty, o.side)); }
        return out;
    }
    [[nodiscard]] const Counters& counters() const { return c_; }
    [[nodiscard]] std::size_t resting() const { return orders_.size(); }
private:
    engine::MarketDataMsg make(engine::MarketDataMsg::Type t, uint64_t id, uint32_t px, uint32_t qty, char side) {
        engine::MarketDataMsg m{};
        m.seq = ++seq_; m.order_id = id; m.price = static_cast<engine::Price>(px) * 100; m.qty = qty;
        m.symbol = sym_; m.side = (side == 'B') ? engine::Side::Buy : engine::Side::Sell;
        m.msg_type = t; m.order_type = engine::OrderType::Limit;
        return m;
    }
    engine::SymbolId sym_;
    uint64_t seq_{0};
    std::unordered_map<uint64_t, RefOrder> orders_;
    Counters c_;
};

} // namespace hft::itch50
