#pragma once
// include/ioq/classic_logger.hpp — the "traditional" logging approach,
// implemented for a fair, apples-to-apples comparison against IOURingLogger.
//
// ── Why this exists ────────────────────────────────────────────────────────
//
// README.md and BENCHMARK_RESULTS.md used to assert "write() typical:
// 500-5,000 ns" as a general claim, not a number measured anywhere in this
// repo, against this repo's own workload, on this repo's own machine. That
// is not a comparison — it's a citation of general knowledge dressed up as
// evidence. ClassicLogger closes that gap: it is the same architecture as
// IOURingLogger (a fixed-capacity SPSC ring buffer, drained by a dedicated
// thread) with exactly one variable changed — the drain thread calls a
// blocking ::write() instead of submitting to io_uring. Everything else
// (the ring buffer type, the LogEntry format, the drain-thread pattern) is
// identical, so any latency difference measured between the two is
// attributable to the I/O backend, not to incidental differences in queue
// design.
//
// ── What this is NOT ──────────────────────────────────────────────────────
//
// It is not a strawman. A dedicated logger thread with a blocking write()
// per entry is a completely reasonable, common production design — it's
// the same "matching engine thread → ring → logger thread" architecture
// this repo uses for io_uring, with a synchronous write() where the
// io_uring submission was. Comparing them isolates exactly the question
// "given that I've already decoupled the hot path with a ring buffer, is
// io_uring still worth it for the logger thread's own I/O?" — not "should
// you log synchronously from the hot path," which every version of this
// repo has already correctly ruled out.

#include "ioq/ring.hpp"

#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

namespace ioq {

class ClassicLogger {
public:
    // Per-write callback, mirroring IOURingLogger::OnComplete's shape for a
    // direct side-by-side comparison: sequence number, the actual
    // wall-clock time the blocking write() call took (this backend has no
    // separate "submission" and "completion" — the write() call itself is
    // both), and the result (bytes written, or -errno on failure).
    using OnComplete = std::function<void(uint64_t seq, int64_t write_ns, int32_t res)>;

    [[nodiscard]] bool open(const char* path) noexcept {
        fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) return false;
        open_ = true;
        return true;
    }

    void set_on_complete(OnComplete cb) noexcept { on_complete_ = std::move(cb); }

    // log() — a direct, synchronous, blocking write() of one LogEntry.
    // This IS the hot-path cost this whole repo's SPSC ring exists to move
    // off the matching engine thread — called here from a dedicated logger
    // thread, exactly where IOURingLogger::log() is called from in the
    // paired architecture, for a fair comparison.
    [[nodiscard]] bool log(uint64_t seq, const LogEntry& entry) noexcept {
        if (!open_) return false;
        const uint64_t t0 = monotonic_ns();
        const ssize_t n = ::write(fd_, &entry, sizeof(entry));
        const uint64_t t1 = monotonic_ns();
        const int32_t res = (n < 0) ? int32_t(-errno) : int32_t(n);
        if (on_complete_) on_complete_(seq, int64_t(t1 - t0), res);
        if (n < 0) { ++errors_; return false; }
        if (uint64_t(n) != sizeof(entry)) { ++errors_; return false; }
        ++written_;
        return true;
    }

    // flush() — fsync, the closest analogue to IOURingLogger::flush()'s
    // "genuinely durable" guarantee. write() itself is synchronous (the
    // call doesn't return until the kernel has accepted the bytes), but
    // that's not the same as durable on disk — fsync forces that.
    void flush() noexcept {
        if (open_) ::fsync(fd_);
    }

    void close() noexcept {
        if (!open_) return;
        flush();
        ::close(fd_);
        open_ = false;
    }

    ~ClassicLogger() { close(); }

    [[nodiscard]] uint64_t written_count() const noexcept { return written_; }
    [[nodiscard]] uint64_t error_count()   const noexcept { return errors_; }

private:
    static uint64_t monotonic_ns() noexcept {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
    }

    int      fd_      = -1;
    bool     open_    = false;
    uint64_t written_ = 0;
    uint64_t errors_  = 0;
    OnComplete on_complete_;
};

} // namespace ioq
