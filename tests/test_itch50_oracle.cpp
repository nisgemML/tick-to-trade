// tests/test_itch50_oracle.cpp
//
// Differential test of include/hft/itch50.hpp against an INDEPENDENT oracle: every line of
// tests/data/itch50_oracle.txt was produced by scripts/gen_itch50_oracle.py using the
// third-party `itchfeed` package -- bytes packed with its own struct format strings, expected
// field values read back out of its own parser. Nothing here shares code, or one person's
// reading of the NASDAQ spec, with the parser under test. Includes all-zero and all-max
// boundary vectors for every field width.
//
// Also pins a finding (BUGS_FOUND.md #15): the vendored feed::ItchAddOrder does NOT read the
// real ITCH 5.0 layout. The final section asserts that, on spec-accurate bytes, the legacy
// parser does not recover the true order_ref / price -- so if someone "fixes" the vendored
// parser later, this test flips and forces the docs to be updated with it.

#include "hft/itch50.hpp"
#include "feed/wire_format.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef ITCH50_ORACLE_FILE
#error "ITCH50_ORACLE_FILE must be defined by the build"
#endif

namespace {
using namespace hft::itch50;
int g_failures = 0, g_checks = 0;
#define CHECK_EQ(a, b, what) do { ++g_checks; if (!((a) == (b))) { ++g_failures; if (g_failures <= 10) std::fprintf(stderr, "FAIL line %zu [%c] %s: got %llu expected %llu\n", lineno, type, what, (unsigned long long)(a), (unsigned long long)(b)); } } while (0)

std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> out; out.reserve(s.size() / 2);
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}
} // namespace

int main() {
    std::ifstream in(ITCH50_ORACLE_FILE);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", ITCH50_ORACLE_FILE); return 2; }
    std::string line; std::size_t lineno = 0, n_msgs = 0;
    std::vector<uint8_t> first_add_body; std::map<char, std::size_t> per_type;

    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string t, hex; ss >> t >> hex;
        const char type = t[0];
        std::map<std::string, std::string> kv; std::string tok;
        while (ss >> tok) { const auto eq = tok.find('='); kv[tok.substr(0, eq)] = tok.substr(eq + 1); }
        auto num = [&](const char* k) -> uint64_t { return std::stoull(kv.at(k)); };
        const auto b = unhex(hex);
        ++n_msgs; ++per_type[type];

        Header h;
        ++g_checks; if (!parse_header(b.data(), b.size(), h)) { ++g_failures; std::fprintf(stderr, "header parse failed line %zu\n", lineno); continue; }
        CHECK_EQ(h.locate, num("locate"), "locate"); CHECK_EQ(h.tracking, num("tracking"), "tracking"); CHECK_EQ(h.timestamp_ns, num("timestamp"), "timestamp");

        switch (type) {
        case 'R': { StockDirectory o; ++g_checks; if (!StockDirectory::parse(b.data(), b.size(), o)) { ++g_failures; break; }
            std::string hx; for (int i = 0; i < 8; ++i) { char c[3]; std::snprintf(c, 3, "%02x", (uint8_t)o.stock[i]); hx += c; }
            ++g_checks; if (hx != kv.at("stock")) { ++g_failures; std::fprintf(stderr, "FAIL line %zu stock\n", lineno); } break; }
        case 'A': case 'F': { AddOrder o; ++g_checks; if (!AddOrder::parse(b.data(), b.size(), o)) { ++g_failures; break; }
            CHECK_EQ(o.order_ref, num("order_ref"), "order_ref"); CHECK_EQ((uint64_t)(uint8_t)o.side, (uint64_t)(uint8_t)kv.at("side")[0], "side");
            CHECK_EQ(o.shares, num("shares"), "shares"); CHECK_EQ(o.price, num("price"), "price"); CHECK_EQ(o.has_mpid, type == 'F', "has_mpid");
            std::string hx; for (int i = 0; i < 8; ++i) { char c[3]; std::snprintf(c, 3, "%02x", (uint8_t)o.stock[i]); hx += c; }
            ++g_checks; if (hx != kv.at("stock")) { ++g_failures; std::fprintf(stderr, "FAIL line %zu stock\n", lineno); }
            if (type == 'A' && first_add_body.empty()) first_add_body = b; break; }
        case 'E': case 'C': { OrderExecuted o; ++g_checks; if (!OrderExecuted::parse(b.data(), b.size(), o)) { ++g_failures; break; }
            CHECK_EQ(o.order_ref, num("order_ref"), "order_ref"); CHECK_EQ(o.shares, num("shares"), "shares"); CHECK_EQ(o.match, num("match"), "match");
            if (type == 'C') { CHECK_EQ(o.price, num("price"), "price"); CHECK_EQ((uint64_t)(uint8_t)o.printable, (uint64_t)(uint8_t)kv.at("printable")[0], "printable"); } break; }
        case 'X': { OrderCancel o; ++g_checks; if (!OrderCancel::parse(b.data(), b.size(), o)) { ++g_failures; break; }
            CHECK_EQ(o.order_ref, num("order_ref"), "order_ref"); CHECK_EQ(o.canceled_shares, num("shares"), "shares"); break; }
        case 'D': { OrderDelete o; ++g_checks; if (!OrderDelete::parse(b.data(), b.size(), o)) { ++g_failures; break; }
            CHECK_EQ(o.order_ref, num("order_ref"), "order_ref"); break; }
        case 'U': { OrderReplace o; ++g_checks; if (!OrderReplace::parse(b.data(), b.size(), o)) { ++g_failures; break; }
            CHECK_EQ(o.orig_ref, num("order_ref"), "orig_ref"); CHECK_EQ(o.new_ref, num("new_ref"), "new_ref");
            CHECK_EQ(o.shares, num("shares"), "shares"); CHECK_EQ(o.price, num("price"), "price"); break; }
        default: ++g_checks; ++g_failures; std::fprintf(stderr, "unknown type line %zu\n", lineno);
        }
    }

    // Truncated input must be rejected, never read past the buffer.
    {
        std::vector<uint8_t> short_add(35, 0); short_add[0] = 'A';
        AddOrder o; ++g_checks; if (AddOrder::parse(short_add.data(), short_add.size(), o)) { ++g_failures; std::fprintf(stderr, "35-byte Add Order accepted\n"); }
    }

    // Pin the finding: the vendored legacy parser misreads real ITCH 5.0 bytes.
    {
        AddOrder good; const std::size_t lineno = 0; const char type = '?'; (void)lineno; (void)type;
        ++g_checks;
        if (first_add_body.empty() || !AddOrder::parse(first_add_body.data(), first_add_body.size(), good)) { ++g_failures; }
        else {
            feed::ItchAddOrder legacy{};
            const bool ok = feed::ItchAddOrder::parse(first_add_body.data(), first_add_body.size(), legacy);
            ++g_checks;
            if (ok && legacy.order_ref == good.order_ref && legacy.price == good.price) {
                ++g_failures; std::fprintf(stderr, "legacy vendored parser now agrees with the spec -- update BUGS_FOUND.md #15 and this test\n");
            }
        }
    }

    std::printf("%zu oracle messages (R=%zu A=%zu F=%zu E=%zu C=%zu X=%zu D=%zu U=%zu), %d field checks, %d failures\n",
                n_msgs, per_type['R'], per_type['A'], per_type['F'], per_type['E'], per_type['C'], per_type['X'], per_type['D'], per_type['U'], g_checks, g_failures);
    if (g_failures == 0) { std::printf("PASS: itch50.hpp matches the independent oracle on every field of every message\n"); return 0; }
    return 1;
}
