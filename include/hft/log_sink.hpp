#pragma once
// Minimal execution-report sink used by the integrated pipeline.
//
// Two implementations, selected at configure time via the CMake option
// HFT_WITH_IOURING (see CMakeLists.txt):
//
//   - FileLogSink (default): blocking write()+fsync(). No extra
//     dependencies, builds everywhere. This is what LIMITATIONS.md has
//     always honestly described as the active default.
//   - IOURingLogSink (opt-in, -DHFT_WITH_IOURING=ON, requires liburing):
//     wraps third_party/io-uring-queue's real, bug-fixed
//     ioq::IOURingLogger. hft::LogEntry and ioq::LogEntry are
//     independently-defined but byte-identical 40-byte layouts (both
//     static_assert'd); convert_entry() below does an explicit
//     field-by-field copy rather than reinterpret_cast, so a future
//     layout change in either struct fails to compile instead of
//     silently aliasing.
//
// Until this file's HFT_WITH_IOURING branch existed, third_party/
// io-uring-queue was vendored but never actually linked into the
// pipeline -- the README claimed an io_uring-backed logger while the
// only code path that ran was the blocking one. This is the fix for
// that gap, not just a documentation correction.

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#ifdef HFT_HAVE_IOURING
#include "ioq/ring.hpp"
#endif

namespace hft {

struct LogEntry {
    uint64_t timestamp_ns{0};
    uint64_t order_id{0};
    int64_t  price{0};
    uint32_t qty{0};
    char     side{' '};
    char     event_type{' '};
    char     symbol[6]{};
    char     _pad[4]{};
};
static_assert(sizeof(LogEntry) == 40, "LogEntry size");

class FileLogSink {
public:
    bool open(const char* path) {
        fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        return fd_ >= 0;
    }

    bool log(const LogEntry& e) {
        if (fd_ < 0) return false;
        const ssize_t n = ::write(fd_, &e, sizeof(e));
        return n == static_cast<ssize_t>(sizeof(e));
    }

    void flush() {
        if (fd_ >= 0) ::fsync(fd_);
    }

    void close() {
        if (fd_ >= 0) {
            flush();
            ::close(fd_);
            fd_ = -1;
        }
    }

    ~FileLogSink() { close(); }

private:
    int fd_{-1};
};

#ifdef HFT_HAVE_IOURING

inline ioq::LogEntry convert_entry(const LogEntry& e) {
    ioq::LogEntry out{};
    out.timestamp_ns = e.timestamp_ns;
    out.order_id     = e.order_id;
    out.price        = e.price;
    out.qty          = e.qty;
    out.side         = e.side;
    out.event_type   = e.event_type;
    std::memcpy(out.symbol, e.symbol, sizeof(out.symbol));
    return out;
}

// Same four-method surface as FileLogSink, so Pipeline's sink_ member
// can be either one with no other code change (see the LogSink alias
// below). open() intentionally takes just a path, matching
// FileLogSink's signature -- ioq::IOURingLoggerConfig's queue-depth/
// SQPOLL knobs stay at their defaults here, which is the right choice
// given io-uring-queue's own BENCHMARK_RESULTS.md: SQPOLL specifically
// needs a dedicated pinned core to help rather than hurt, which this
// pipeline does not currently arrange for.
class IOURingLogSink {
public:
    bool open(const char* path) { return ring_.open(path); }
    bool log(const LogEntry& e) { return ring_.log(convert_entry(e)); }
    void flush() { ring_.flush(); }
    void close() { ring_.close(); }
    ~IOURingLogSink() { close(); }

private:
    ioq::IOURingLogger ring_;
};

using LogSink = IOURingLogSink;

#else

using LogSink = FileLogSink;

#endif

} // namespace hft

