// tools/run_live_pipeline.cpp
//
// The live-multicast entry point the README's own roadmap has listed as
// open since Milestone 1: `feed::MulticastReceiver` (real UDP multicast
// socket, SO_TIMESTAMPING, vendored from udp-multicast-receiver) wired
// through `hft::ItchAdapter` into the real `hft::Pipeline` -- not a
// synthetic stream fed via direct function calls, but actual bytes
// received off an actual UDP socket.
//
// This is still a *local* multicast group (default 239.1.1.1 on the "lo"
// interface), not a real exchange feed -- that distinction matters and
// isn't being hidden: see README/LIMITATIONS.md for exactly what this
// does and doesn't prove. What it does prove: the receive path, kernel
// timestamping, gap buffer, ITCH adapter, and matching pipeline compose
// correctly end-to-end over a real socket, not just in a unit test that
// calls ingest() directly.
//
// Usage: run_live_pipeline [group] [port] [iface] [duration_s] [rerequest_host:port]
//   defaults: 239.1.1.1 15001 lo 10 (no re-request server)
//
// With a re-request server given, sequence gaps are recovered over UDP
// (feed/retransmit_client.hpp) instead of only being logged; see
// send_live_itch's drop_pct/rerequest_port arguments for a local venue.

#include "hft/pipeline.hpp"
#include "hft/itch_adapter.hpp"
#include "hft/numa_support.hpp"
#include "feed/receiver.hpp"
#include "feed/gap_buffer.hpp"
#include "feed/retransmit_client.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {
std::atomic<bool> g_interrupted{false};
void on_sigint(int) { g_interrupted.store(true, std::memory_order_relaxed); }
}

int main(int argc, char** argv) {
    std::signal(SIGINT, on_sigint);

    const std::string group = argc > 1 ? argv[1] : "239.1.1.1";
    const uint16_t    port  = argc > 2 ? static_cast<uint16_t>(std::stoi(argv[2])) : 15001;
    const std::string iface = argc > 3 ? argv[3] : "lo";
    const int    duration_s = argc > 4 ? std::stoi(argv[4]) : 10;
    const std::string rr_addr = argc > 5 ? argv[5] : "";

    hft::PipelineConfig pcfg;
    pcfg.log_path = "live_fills.log";
    hft::Pipeline pipeline(pcfg);
    if (!pipeline.start()) {
        std::fprintf(stderr, "pipeline start failed\n");
        return 1;
    }

    // GapBuffer is ~6MB -- must be heap-allocated, not a stack local.
    // This exact mistake is already documented in BUGS_FOUND.md's roadmap
    // (caught a second time, writing this file, before it ever ran: the
    // first draft of this tool had it on the stack and segfaulted
    // immediately, zero output, before the first printf -- see the
    // commit history for this file).
    // NUMA-aware, explicitly hugepage-backed when built with
    // -DHFT_WITH_NUMA=ON (falls back to plain heap allocation otherwise --
    // see include/hft/numa_support.hpp for both paths).
    // With a re-request server we can recover from the start of the
    // session, so expect seq 1 (a lost first packet is then a gap, not a
    // silent late join). Without one, sync to whatever arrives first.
    feed::GapBuffer::Config gcfg;
    if (!rr_addr.empty()) gcfg.expected_first_seq = 1;
    auto gap_buf = hft::make_gap_buffer(/*node=*/0, gcfg);
    if (!gap_buf) {
        std::fprintf(stderr, "GapBuffer allocation failed\n");
        pipeline.stop();
        return 1;
    }
#ifdef HFT_HAVE_NUMA
    std::printf("GapBuffer: NUMA node %d requested, hugepage_backed=%d\n",
                gap_buf.get_deleter().node(), gap_buf.get_deleter().hugepage_backed());
#endif

    hft::ItchAdapterConfig icfg;
    hft::ItchAdapter adapter(icfg, [&](const engine::MarketDataMsg& m, uint64_t recv_ns) {
        pipeline.submit_with_ts(m, recv_ns);
    });
    adapter.attach(*gap_buf);

    feed::MulticastReceiver::Config rcfg;
    rcfg.multicast_group = group;
    rcfg.port = port;
    rcfg.interface_name = iface;
    feed::MulticastReceiver receiver(rcfg, *gap_buf);
    if (!receiver.open()) {
        std::fprintf(stderr, "receiver open failed -- is this host's '%s' interface up?\n", iface.c_str());
        pipeline.stop();
        return 1;
    }

    std::unique_ptr<feed::RetransmitClient> retx;
    if (!rr_addr.empty()) {
        const auto colon = rr_addr.rfind(':');
        if (colon == std::string::npos) {
            std::fprintf(stderr, "rerequest address must be host:port\n");
            pipeline.stop();
            return 1;
        }
        feed::RetransmitClient::Config ccfg;
        ccfg.server_ip   = rr_addr.substr(0, colon);
        ccfg.server_port = static_cast<uint16_t>(std::stoi(rr_addr.substr(colon + 1)));
        retx = std::make_unique<feed::RetransmitClient>(ccfg);
        if (!retx->open()) { pipeline.stop(); return 1; }
        receiver.attach_retransmit_client(*retx);
        std::printf("run_live_pipeline: gap recovery via re-request server %s\n", rr_addr.c_str());
    }

    std::printf("run_live_pipeline: listening on %s:%u (%s) for %ds (Ctrl-C to stop early)\n",
                group.c_str(), port, iface.c_str(), duration_s);

    std::atomic<bool> recv_running{true};
    std::thread recv_thread([&] { receiver.run(recv_running); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);
    while (std::chrono::steady_clock::now() < deadline && !g_interrupted.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    recv_running.store(false, std::memory_order_relaxed);
    recv_thread.join();
    receiver.close();
    pipeline.stop();

    const auto& r = pipeline.result();
    std::printf(
        "recv_calls=%lu delivered=%lu parse_errors=%lu | "
        "submitted=%lu accepted=%lu fills=%lu queue_full_rejects=%lu\n",
        receiver.stat_recv_calls(), receiver.stat_messages_delivered(), receiver.stat_parse_errors(),
        r.events_submitted, r.events_accepted, r.fills, r.queue_full_rejects);
    std::printf(
        "gaps=%lu retx_requests=%lu next_expected=%lu end_of_session=%d session_complete=%d",
        gap_buf->stat_gaps(), gap_buf->stat_retransmit_requests(), gap_buf->next_expected(),
        int(gap_buf->end_of_session()), int(gap_buf->session_complete()));
    if (retx)
        std::printf(" | retx_sent=%lu replies=%lu recovered=%lu",
                    retx->stat_requests_sent(), retx->stat_replies(), retx->stat_messages_recovered());
    std::printf("\n");

    return 0;
}
