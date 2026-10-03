#pragma once
// include/hft/numa_support.hpp
//
// Real use of this portfolio's own NUMA-aware, explicitly hugepage-backed
// allocator (cpp26-alloc's alloc/numa.hpp, vendored) for the one piece of
// this pipeline that's actually worth NUMA-aware placement: GapBuffer's
// backing storage. GapBuffer isn't a small object with a pointer to a
// separately-heap-allocated buffer -- std::array<BufferedPacket, kCapacity>
// is a *direct member*, so GapBuffer itself is the ~6MB object (confirmed
// by reading third_party/udp-multicast-receiver/include/feed/gap_buffer.hpp
// before writing this, not assumed). Placement-constructing it into
// NUMA-backed memory genuinely places that whole buffer there, not just a
// small header pointing elsewhere.
//
// This is a deliberate extension of the discipline already in
// BUGS_FOUND.md #13 (GapBuffer must be heap-allocated, not a stack local
// -- a real segfault caught while building the live-multicast tool). That
// was "heap, not stack." This is "heap, specifically NUMA-local and
// hugepage-backed, with that placement independently verified against the
// kernel" -- the same component, a further step, same verification
// standard as everything else in this repository.
//
// Opt-in via -DHFT_WITH_NUMA=ON (see CMakeLists.txt), same reasoning as
// HFT_WITH_IOURING: zero extra dependencies on any Linux box by default.

#include "feed/gap_buffer.hpp"

#include <memory>

#ifdef HFT_HAVE_NUMA
#include "alloc/numa.hpp"
#endif

namespace hft {

#ifdef HFT_HAVE_NUMA

// Owns both the GapBuffer (placement-constructed) and the NUMA region it
// lives in; destructor order matters here (object first, then the region
// that backs it), which is exactly what a custom deleter holding the
// region gets right automatically via unique_ptr's own destruction order.
class NumaGapBufferDeleter {
public:
    explicit NumaGapBufferDeleter(alloc::numa::NumaRegion&& region) noexcept
        : region_(std::move(region)) {}

    void operator()(feed::GapBuffer* p) const noexcept {
        if (p) p->~GapBuffer();
        // region_'s own destructor runs after this operator() returns, as
        // part of unique_ptr tearing down its stored deleter -- that's
        // what actually munmap()s the backing memory, in the right order.
    }

    [[nodiscard]] bool hugepage_backed() const noexcept { return region_.hugepage_backed(); }
    [[nodiscard]] int  node()            const noexcept { return region_.requested_node(); }

private:
    alloc::numa::NumaRegion region_;
};

using GapBufferPtr = std::unique_ptr<feed::GapBuffer, NumaGapBufferDeleter>;

// Returns an empty GapBufferPtr (null) if even a regular (non-huge),
// NUMA-bound allocation fails outright -- caller must check, same
// contract as any other allocating factory in this codebase.
[[nodiscard]] inline GapBufferPtr make_gap_buffer(int node = 0) {
    auto region = alloc::numa::alloc_on_node(sizeof(feed::GapBuffer), node, /*want_hugepage=*/true);
    if (!region) return GapBufferPtr(nullptr, NumaGapBufferDeleter(alloc::numa::NumaRegion{}));
    auto* raw = region->data();
    auto* p = ::new (raw) feed::GapBuffer();
    return GapBufferPtr(p, NumaGapBufferDeleter(std::move(*region)));
}

#else

using GapBufferPtr = std::unique_ptr<feed::GapBuffer>;

[[nodiscard]] inline GapBufferPtr make_gap_buffer(int /*node*/ = 0) {
    return std::make_unique<feed::GapBuffer>();
}

#endif

} // namespace hft
