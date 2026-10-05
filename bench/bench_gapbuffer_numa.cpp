// bench/bench_gapbuffer_numa.cpp
//
// Does the NUMA/hugepage-backed GapBuffer allocation (BUGS_FOUND.md #14)
// actually change anything measurable, in the pipeline's own real usage
// pattern -- not a generic microbenchmark on an arbitrary payload size?
//
// GapBuffer's slots_ array is indexed by `seq & kMask` (4096 slots, ~6MB
// total) -- under normal in-order delivery, access is sequential and
// localized, which wouldn't meaningfully exercise TLB behavior
// differently between 4KB and 2MB pages. The access pattern that
// actually stresses the full ~6MB footprint is heavy *out-of-order*
// delivery: many gaps open simultaneously across a wide span of sequence
// numbers before being reconciled, scattering writes across the entire
// structure rather than a small sliding window.
//
// This benchmark constructs exactly that: batches of packets whose
// sequence numbers are randomly permuted across a full 4096-slot span
// before being fed to GapBuffer::ingest() one at a time, then measures
// per-packet ingest latency. Compiled and run twice -- once under
// -DHFT_WITH_NUMA=ON, once without -- same source, compared honestly
// (same pattern already used for the io_uring vs default comparison in
// this repo; see BUGS_FOUND.md #12).

#include "hft/numa_support.hpp"
#include "feed/wire_format.hpp"
#include "feed/gap_buffer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace feed;
using Clock = std::chrono::steady_clock;

std::vector<uint8_t> mold_packet(uint64_t seq, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> buf(kMoldHeaderSize + 2 + body.size());
    MoldHeader hdr{};
    std::memcpy(hdr.session, "GBNUMA    ", 10);
    hdr.seq_num = seq;
    hdr.msg_count = 1;
    hdr.serialise(buf.data());
    put_be16(buf.data() + kMoldHeaderSize, uint16_t(body.size()));
    std::memcpy(buf.data() + kMoldHeaderSize + 2, body.data(), body.size());
    return buf;
}

std::vector<uint8_t> make_add(uint64_t ref, uint32_t price) {
    std::vector<uint8_t> b(36, 0);
    b[0] = 'A';
    put_be64(b.data() + 11, ref);
    b[19] = 'B';
    put_be32(b.data() + 20, 10);
    std::memcpy(b.data() + 24, "GBNM0001", 8);
    put_be32(b.data() + 32, price);
    return b;
}

int main(int argc, char** argv) {
    const uint64_t n_laps = argc > 1 ? std::stoull(argv[1]) : 200; // 200 * 4096 ~= 820K packets
    constexpr uint64_t kSpan = 4096; // exactly one GapBuffer lap

    auto gap_buf = hft::make_gap_buffer(/*node=*/0);
    if (!gap_buf) { std::fprintf(stderr, "GapBuffer allocation failed\n"); return 1; }
#ifdef HFT_HAVE_NUMA
    std::printf("config=NUMA hugepage_backed=%d\n", gap_buf.get_deleter().hugepage_backed());
#else
    std::printf("config=default(heap)\n");
#endif

    uint64_t delivered = 0;
    gap_buf->set_on_message([&](const MoldMessage&) { ++delivered; });

    std::vector<double> lap_ns;
    lap_ns.reserve(n_laps);

    std::mt19937_64 rng(42);
    uint64_t base_seq = 1;

    for (uint64_t lap = 0; lap < n_laps; ++lap) {
        // One full span of sequence numbers, randomly permuted -- forces
        // scattered writes across the whole ~6MB slots_ array before any
        // of them can be delivered in order (the first one delivered,
        // base_seq, unblocks a forward scan, but the other 4095 in this
        // lap are genuinely out of order relative to each other until
        // that scan reaches them).
        std::vector<uint64_t> seqs(kSpan);
        for (uint64_t i = 0; i < kSpan; ++i) seqs[i] = base_seq + i;
        std::shuffle(seqs.begin(), seqs.end(), rng);

        const auto t0 = Clock::now();
        for (uint64_t seq : seqs) {
            const auto body = make_add(seq, 100'0000 + static_cast<uint32_t>(seq % 100));
            const auto pkt = mold_packet(seq, body);
            gap_buf->ingest(pkt.data(), pkt.size(), /*recv_ns=*/0);
        }
        const auto t1 = Clock::now();
        lap_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(kSpan));

        base_seq += kSpan;
    }

    std::sort(lap_ns.begin(), lap_ns.end());
    auto pct = [&](double p) { return lap_ns[static_cast<std::size_t>(p * static_cast<double>(lap_ns.size() - 1))]; };

    std::printf("laps=%lu delivered=%lu  per-packet ingest: p50=%.1fns p90=%.1fns p99=%.1fns\n",
                n_laps, delivered, pct(0.50), pct(0.90), pct(0.99));
    return 0;
}
