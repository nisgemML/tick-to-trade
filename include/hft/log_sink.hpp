#pragma once
// Minimal execution-report sink used by the integrated pipeline.
// Mirrors ioq::LogEntry layout conceptually; uses blocking write by default
// so the stack builds without liburing. Swap for ioq::IOURingLogger when
// liburing is available (see CMake option HFT_WITH_IOURING).

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

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

} // namespace hft
