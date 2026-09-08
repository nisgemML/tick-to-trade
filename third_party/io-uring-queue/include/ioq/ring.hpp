#pragma once
// include/ioq/ring.hpp — io_uring-backed async I/O ring for trading systems.
//
// This header implements two components that solve distinct problems
// in low-latency trading system design:
//
// 1. SPSCRingBuffer<T, N> — a lock-free single-producer single-consumer
//    ring buffer using C++20 atomics (acquire/release, no seq_cst).
//    Used for inter-thread message passing on the hot path.
//
// 2. IOURingLogger — async trade event logging via io_uring, writing
//    execution reports to disk without blocking the matching engine thread.
//
// ── Why io_uring for trading systems? ────────────────────────────────────────
//
// Traditional async I/O options for logging execution reports:
//
//   write() — synchronous, blocks the matching engine thread (100–10,000 ns)
//   aio_write() — POSIX AIO, kernel thread pool, high overhead per syscall
//   pwrite() + thread pool — adds queue depth + context switch overhead
//
// io_uring (Linux 5.1+):
//   - Submission queue (SQ) and completion queue (CQ) in shared memory
//   - The matching engine submits write requests by writing to the SQ ring
//     (a plain memory store — no syscall on the hot path)
//   - The kernel drains the SQ asynchronously via io_uring_enter()
//   - With IORING_SETUP_SQPOLL: kernel polls the SQ continuously on a
//     dedicated core — zero syscall overhead even for submission
//
// Hot path cost breakdown:
//   Synchronous write():  100–10,000 ns (syscall + kernel scheduling)
//   io_uring submit:       10–30 ns  (SQ store + optional io_uring_enter)
//   io_uring SQPOLL:        5–15 ns  (SQ store only — no syscall)
//
// This is the same pattern used in production HFT systems for:
//   - Execution report logging (post-trade compliance)
//   - Market data recording (PCAP-style tick capture)
//   - Order audit trails
//
// ── SPSCRingBuffer memory ordering ───────────────────────────────────────────
//
// The ring buffer uses the same acquire/release pattern as the MPSC queue
// (see mpsc-queue/proof/memory_model.md), but simplified for SPSC:
//
//   Producer: data write →_sb head_.store(release)
//   Consumer: tail_.load(acquire) →_sw head_.store(release)
//
// The acquire on tail_.load ensures the consumer sees all data written
// before the producer's head_.store. No seq_cst needed — the directed
// happens-before edge is sufficient.
//
// ── References ───────────────────────────────────────────────────────────────
// Axboe, J. (2019). Efficient I/O with io_uring. Kernel.dk.
// Lord, J. (2022). io_uring and networking in 2022. Kernel.dk.

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <optional>
#include <span>
#include <new>      // std::hardware_destructive_interference_size
#include <fcntl.h>  // open, O_WRONLY, O_CREAT, O_TRUNC
#include <sys/stat.h>
#include <liburing.h>

namespace ioq {

// ── 1. SPSC Ring Buffer ───────────────────────────────────────────────────────

// Cache-line size — avoid false sharing between head_ and tail_.
// C++17 std::hardware_destructive_interference_size is implementation-defined;
// 64 bytes is correct for all modern x86-64 processors.
static constexpr std::size_t kCacheLine = 64;

template<typename T, std::size_t N>
requires (N > 0 && (N & (N-1)) == 0)  // N must be power of 2
class SPSCRingBuffer {
    // Producer cache line: head_ (write index)
    // Consumer cache line: tail_ (read index)
    // Separate cache lines eliminate false sharing between producer and consumer.
    alignas(kCacheLine) std::atomic<uint64_t> head_{0};
    alignas(kCacheLine) std::atomic<uint64_t> tail_{0};

    // Data array — sized to N (power of 2 for bitmask indexing)
    alignas(kCacheLine) T data_[N];

    static constexpr uint64_t kMask = N - 1;

public:
    // try_push: non-blocking enqueue.
    // Returns false if the ring is full.
    // Memory ordering: release on head_ store — ensures data_[slot] write
    // is visible to the consumer before the updated head_ is visible.
    [[nodiscard]] bool try_push(const T& val) noexcept {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= N) return false;           // full
        data_[h & kMask] = val;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // try_pop: non-blocking dequeue.
    // Returns std::nullopt if the ring is empty.
    // Memory ordering: acquire on head_ load — synchronises-with the
    // producer's release store, making data_[slot] visible.
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        if (h == t) return std::nullopt;        // empty
        T val = data_[t & kMask];
        tail_.store(t + 1, std::memory_order_release);
        return val;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return head_.load(std::memory_order_acquire) -
               tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() noexcept { return N; }
};

// ── 2. io_uring async logger ──────────────────────────────────────────────────

struct LogEntry {
    uint64_t timestamp_ns;   // RDTSC timestamp (nanoseconds)    [8]
    uint64_t order_id;       //                                   [8]
    int64_t  price;          // fixed-point                       [8]
    uint32_t qty;            //                                   [4]
    char     side;           // 'B' or 'S'                        [1]
    char     event_type;     // 'F' fill, 'A' add, 'C' cancel    [1]
    char     symbol[6];      // first 6 chars of ticker           [6]
    // Total: 8+8+8+4+1+1+6 = 36 — pad to 40 bytes (5 cache-line friendly)
    char     _pad[4];        //                                   [4]
};                           // Total: 40 bytes
static_assert(sizeof(LogEntry) == 40, "LogEntry must be 40 bytes");

// Namespace-scope (not nested in IOURingLogger) deliberately: a Config type
// used as a default argument on a member function of the very class it's
// nested inside has hit real compiler portability issues elsewhere in this
// author's other repos (a GCC-13/Clang-18 divergence over exactly when the
// enclosing class is considered "complete" for that purpose) — keeping
// Config independent sidesteps the question entirely rather than relying
// on a specific compiler's leniency.
struct IOURingLoggerConfig {
    // io_uring_submit() is called every `batch_size` log() calls, not on
    // every call — see docs/failure-modes-and-batching.md for the
    // measured latency/throughput tradeoff this represents. batch_size=1
    // (the default) reproduces this class's original one-syscall-per-entry
    // behavior exactly.
    unsigned batch_size = 1;

    // IORING_SETUP_SQPOLL: the kernel polls the SQ ring on a dedicated
    // core instead of requiring an io_uring_enter() syscall per submit.
    // Requires CAP_SYS_NICE (or root). See docs/failure-modes-and-batching.md
    // for what happens when the caller doesn't have it (open() fails
    // cleanly — see the SQPOLL section there for exactly which errno).
    bool     sqpoll         = false;
    unsigned sqpoll_idle_ms = 2000; // how long the SQ poll thread spins before sleeping
    int      sqpoll_cpu     = -1;   // pin the SQ poll thread to this CPU; -1 = no pin
};

class IOURingLogger {
public:
    static constexpr unsigned kQueueDepth = 256;

    // Per-completion callback: fires once per completed write, with the
    // sequence number log() returned it as (see log()'s doc), the
    // submit-to-complete latency in nanoseconds, and the raw CQE result
    // (sizeof(LogEntry) on success; a negative -errno, or a positive short
    // count, on failure — see record_completion()). This is how
    // bench/bench_latency_breakdown.cpp measures completion latency for
    // real, separately from submission latency: submission latency is
    // "how long log() itself took to return," which was already
    // measurable before; completion latency — how long the actual write
    // took once handed to the kernel — was not observable at all before
    // this callback existed.
    using OnComplete = std::function<void(uint64_t seq, int64_t latency_ns, int32_t res)>;

    // open() — initialise the io_uring and open the log file.
    // Must be called once before log().
    // Returns false on failure (io_uring_queue_init or file open error).
    [[nodiscard]] bool open(const char* path, IOURingLoggerConfig cfg = IOURingLoggerConfig{}) noexcept {
        cfg_ = cfg;
        if (cfg_.batch_size == 0) cfg_.batch_size = 1;

        fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) return false;

        int ret;
        if (cfg_.sqpoll) {
            struct io_uring_params params{};
            params.flags = IORING_SETUP_SQPOLL;
            params.sq_thread_idle = cfg_.sqpoll_idle_ms;
            if (cfg_.sqpoll_cpu >= 0) {
                params.flags |= IORING_SETUP_SQ_AFF;
                params.sq_thread_cpu = unsigned(cfg_.sqpoll_cpu);
            }
            ret = io_uring_queue_init_params(kQueueDepth, &ring_, &params);
        } else {
            ret = io_uring_queue_init(kQueueDepth, &ring_, 0);
        }
        if (ret < 0) {
            sqpoll_setup_errno_ = -ret;
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        open_ = true;
        return true;
    }

    void set_on_complete(OnComplete cb) noexcept { on_complete_ = std::move(cb); }

    // log() — submit an async write request for one LogEntry.
    // Hot path cost: one SQ slot write, plus an io_uring_submit() call
    // every cfg.batch_size calls (every call, if batch_size == 1 — the
    // default). With IORING_SETUP_SQPOLL: even that submit becomes a
    // plain memory store, no syscall.
    //
    // The LogEntry is copied to an internal buffer — the caller does not
    // need to keep the entry alive after log() returns, BUT that buffer has
    // exactly kQueueDepth slots, reused round-robin. Before writing into a
    // slot, this checks that the PREVIOUS write which used that slot has
    // actually completed — not just that the SQ ring has room for a new
    // submission. Those are two different things: io_uring_get_sqe()
    // returning non-null only means there is space in the submission ring
    // for a new entry, which the kernel can free up as soon as it reads the
    // SQE off the ring — well before the write it describes has actually
    // finished and stopped touching the buffer memory that SQE pointed at.
    // Gating only on SQ availability (the previous version of this
    // function) lets `log()` overwrite a buffer slot the kernel is still
    // asynchronously reading for an earlier write, corrupting that earlier
    // entry's on-disk bytes — silently, with no error anywhere. See
    // docs/failure-modes-and-batching.md for the full explanation and
    // tests/test_ring.cpp's wraparound stress test that reproduces it.
    //
    // If the slot isn't free yet, this blocks waiting for at least one
    // completion — i.e. sustained logging faster than the kernel completes
    // writes creates backpressure on the caller, not corruption. This is a
    // deliberate choice: this logger decouples the MATCHING ENGINE thread
    // from disk I/O (via the SPSC ring in front of it), not the LOGGER
    // thread — the logger thread blocking under sustained overload is the
    // correct failure mode, unlike the matching engine thread blocking.
    [[nodiscard]] bool log(const LogEntry& entry) noexcept {
        if (!open_) return false;

        while (submitted_ - completed_ >= kQueueDepth) {
            if (!wait_for_one_completion()) return false; // ring/error unrecoverable
        }

        const uint32_t slot = uint32_t(submitted_ & kMask);
        buf_[slot] = entry;

        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            drain_completed_nonblocking();
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe) return false;
        }

        const off_t offset = static_cast<off_t>(file_offset_);
        file_offset_ += sizeof(LogEntry);

        io_uring_prep_write(sqe, fd_, &buf_[slot], sizeof(LogEntry), offset);
        // Written directly to the sqe's user_data field rather than via
        // io_uring_sqe_set_data64() — that helper was added to liburing
        // more recently than the io_uring_sqe_set_data()/user_data field
        // itself, and isn't present in the liburing-dev version Ubuntu
        // 22.04's apt repository ships (this repo's own CI target). Direct
        // field assignment is unconditionally portable: user_data has been
        // part of io_uring's stable ABI since its first version.
        sqe->user_data = submitted_;
        submit_ts_ns_[slot] = monotonic_ns();

        ++submitted_;
        ++pending_since_submit_;

        if (pending_since_submit_ >= cfg_.batch_size) {
            const int ret = submit_with_eintr_retry();
            pending_since_submit_ = 0;
            if (ret < 0) return false;
        }
        return true;
    }

    // flush() — block until every write submitted so far has genuinely
    // completed (not just been handed to the kernel). Call before shutdown.
    // Always submits any batch-pending entries first, regardless of
    // cfg.batch_size, so a partially-filled batch at shutdown is never
    // silently dropped.
    void flush() noexcept {
        if (!open_) return;
        if (pending_since_submit_ > 0) {
            submit_with_eintr_retry();
            pending_since_submit_ = 0;
        }
        while (completed_ < submitted_) {
            if (!wait_for_one_completion()) break; // unrecoverable — stop trying
        }
    }

    void close() noexcept {
        if (!open_) return;
        flush();
        io_uring_queue_exit(&ring_);
        ::close(fd_);
        open_ = false;
    }

    ~IOURingLogger() { close(); }

    // ── Observability ──────────────────────────────────────────────────────
    //
    // A write's CQE result (`cqe->res`) can be negative (a -errno, e.g.
    // -ENOSPC on a full disk, -EIO) or a short write (positive but less
    // than sizeof(LogEntry)) — the previous version of this class checked
    // neither; both were silently indistinguishable from a normal
    // successful write. Both now count as errors here — a "compliance"
    // logger that doesn't tell you when it failed to write isn't one.
    [[nodiscard]] uint64_t submitted_count() const noexcept { return submitted_; }
    [[nodiscard]] uint64_t completed_count() const noexcept { return completed_; }
    [[nodiscard]] uint64_t error_count()     const noexcept { return errors_; }
    [[nodiscard]] int64_t  last_error()      const noexcept { return last_error_; }

    // errno from io_uring_queue_init[_params]() if open() failed at that
    // step (0 if it failed at the file-open step instead, or hasn't been
    // called). Specifically useful for diagnosing an SQPOLL open() failure
    // — see docs/failure-modes-and-batching.md.
    [[nodiscard]] int sqpoll_setup_errno() const noexcept { return sqpoll_setup_errno_; }

private:
    // io_uring_submit() can also return -EINTR under the same real-world
    // conditions as io_uring_wait_cqe() (see wait_for_one_completion()) —
    // retried here for the same reason.
    int submit_with_eintr_retry() noexcept {
        int ret;
        do {
            ret = io_uring_submit(&ring_);
        } while (ret == -EINTR);
        return ret;
    }

    static uint64_t monotonic_ns() noexcept {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
    }

    // Drains whatever completions are ALREADY available, without blocking.
    // Used to free SQ ring space; does not by itself guarantee any
    // particular slot has completed (see log()'s blocking wait for that).
    void drain_completed_nonblocking() noexcept {
        struct io_uring_cqe* cqe;
        while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
            record_completion(cqe);
            io_uring_cqe_seen(&ring_, cqe);
        }
    }

    // Blocks until at least one completion is reaped. Returns false only
    // if waiting on the ring itself failed for a genuinely unrecoverable
    // reason — a write error (bad cqe->res) is still recorded and still
    // returns true, since the RING is fine even if that one write wasn't.
    //
    // io_uring_wait_cqe() can return -EINTR if the blocking wait is
    // interrupted by an unrelated signal — a normal, expected occurrence
    // on any real multi-core Linux system under load, not a sign of
    // anything wrong with the ring. Treating -EINTR as fatal (the
    // previous version of this function did) causes log() to spuriously
    // fail and silently drop entries under exactly the conditions a real
    // production system runs under — this was caught by CI on real
    // hardware (SQPOLL actually delivering its intended low latency,
    // running fast enough for a stray -EINTR to land mid-run) after
    // going unnoticed on a single-core sandbox where SQPOLL was slow
    // enough that this race window rarely mattered. -EINTR is the one
    // return value every blocking syscall wrapper must retry on, per
    // standard POSIX practice — anything else negative is treated as a
    // real, unrecoverable ring failure.
    [[nodiscard]] bool wait_for_one_completion() noexcept {
        struct io_uring_cqe* cqe;
        int ret;
        do {
            ret = io_uring_wait_cqe(&ring_, &cqe);
        } while (ret == -EINTR);
        if (ret < 0) return false;
        record_completion(cqe);
        io_uring_cqe_seen(&ring_, cqe);
        return true;
    }

    void record_completion(struct io_uring_cqe* cqe) noexcept {
        const uint64_t seq  = cqe->user_data;
        const uint32_t slot = uint32_t(seq & kMask);
        const int64_t  now  = int64_t(monotonic_ns());
        const int64_t  lat  = now - int64_t(submit_ts_ns_[slot]);

        ++completed_;
        const int64_t res = cqe->res;
        if (res < 0 || uint64_t(res) != sizeof(LogEntry)) {
            ++errors_;
            last_error_ = res;
        }
        if (on_complete_) on_complete_(seq, lat, int32_t(res));
    }

    static constexpr uint64_t kMask = kQueueDepth - 1;

    struct io_uring ring_{};
    int      fd_                   = -1;
    bool     open_                 = false;
    uint64_t submitted_            = 0;
    uint64_t completed_            = 0;
    uint64_t errors_               = 0;
    int64_t  last_error_           = 0;
    uint64_t file_offset_          = 0;
    unsigned pending_since_submit_ = 0;
    int      sqpoll_setup_errno_   = 0;
    IOURingLoggerConfig cfg_{};
    OnComplete on_complete_;

    // Write buffer and per-slot submit timestamps — one slot per queue
    // depth entry.
    alignas(kCacheLine) LogEntry buf_[kQueueDepth];
    alignas(kCacheLine) uint64_t submit_ts_ns_[kQueueDepth]{};
};

} // namespace ioq
