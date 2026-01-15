#pragma once
#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace Contest {

namespace ReqSlab3 {

static inline bool enabled() {
    // Default: enabled to satisfy assignment, but easy to disable.
    // Set REQ_SLAB3=0 to bypass and use malloc/new in the partitioned build.
    static const bool on = [] {
        const char* v = std::getenv("REQ_SLAB3");
        if (!v) return true;
        return *v && *v != '0';
    }();
    return on;
}

static inline std::size_t align_up(std::size_t x, std::size_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

// Level 1: Global allocator handing out large blocks.
class GlobalArena {
public:
    explicit GlobalArena(std::size_t default_block_bytes = (8ull << 20))
        : default_block_bytes_(default_block_bytes) {}

    GlobalArena(const GlobalArena&) = delete;
    GlobalArena& operator=(const GlobalArena&) = delete;

    void* alloc_block(std::size_t min_bytes) {
        const std::size_t bytes = std::max(min_bytes, default_block_bytes_);
        void* p = ::operator new(bytes);
        {
            std::lock_guard<std::mutex> g(mu_);
            blocks_.push_back(p);
            block_sizes_.push_back(bytes);
        }
        return p;
    }

    std::size_t default_block_bytes() const { return default_block_bytes_; }

    ~GlobalArena() {
        for (void* p : blocks_) ::operator delete(p);
    }

private:
    std::mutex mu_;
    std::vector<void*> blocks_;
    std::vector<std::size_t> block_sizes_;
    std::size_t default_block_bytes_;
};

// Level 2: Thread-local arena (per worker thread during build).
class ThreadArena {
public:
    explicit ThreadArena(GlobalArena* global,
                         std::size_t default_block_bytes = 0)
        : global_(global)
        , default_block_bytes_(default_block_bytes ? default_block_bytes : global->default_block_bytes()) {}

    ThreadArena(const ThreadArena&) = delete;
    ThreadArena& operator=(const ThreadArena&) = delete;

    ThreadArena(ThreadArena&&) = default;
    ThreadArena& operator=(ThreadArena&&) = default;

    void reset() {
        cur_ = nullptr;
        cur_bytes_ = 0;
        cur_off_ = 0;
        // Keep owned blocks for reuse across builds by just rewinding offsets.
        for (auto& b : owned_) b.off = 0;
        owned_idx_ = 0;
    }

    void* alloc(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
        bytes = align_up(bytes, alignment);
        if (!cur_ || (cur_off_ + bytes > cur_bytes_)) {
            acquire_block(std::max(bytes, default_block_bytes_));
        }

        std::uintptr_t base = reinterpret_cast<std::uintptr_t>(cur_) + cur_off_;
        std::uintptr_t aligned = align_up(static_cast<std::size_t>(base), alignment);
        std::size_t pad = static_cast<std::size_t>(aligned - base);

        if (cur_off_ + pad + bytes > cur_bytes_) {
            acquire_block(std::max(bytes, default_block_bytes_));
            base = reinterpret_cast<std::uintptr_t>(cur_) + cur_off_;
            aligned = align_up(static_cast<std::size_t>(base), alignment);
            pad = static_cast<std::size_t>(aligned - base);
        }

        cur_off_ += pad;
        void* p = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(cur_) + cur_off_);
        cur_off_ += bytes;
        return p;
    }

private:
    struct Block {
        void* ptr{nullptr};
        std::size_t bytes{0};
        std::size_t off{0};
    };

    void acquire_block(std::size_t bytes) {
        // Reuse an existing owned block if available.
        if (owned_idx_ < owned_.size() && owned_[owned_idx_].bytes >= bytes) {
            Block& b = owned_[owned_idx_++];
            cur_ = b.ptr;
            cur_bytes_ = b.bytes;
            cur_off_ = b.off; // normally 0 after reset
            return;
        }

        void* p = global_->alloc_block(bytes);
        owned_.push_back(Block{p, bytes, 0});
        owned_idx_ = owned_.size();
        cur_ = p;
        cur_bytes_ = bytes;
        cur_off_ = 0;
    }

    GlobalArena* global_;
    std::size_t default_block_bytes_;

    std::vector<Block> owned_;
    std::size_t owned_idx_{0};
    void* cur_{nullptr};
    std::size_t cur_bytes_{0};
    std::size_t cur_off_{0};
};

// Level 3: Partition-local allocator. Each (thread, partition) has a cursor.
struct PartitionCursor {
    uint8_t* ptr{nullptr};
    std::size_t remaining{0};
};

static inline void* alloc_from_partition(ThreadArena& ta,
                                        PartitionCursor& pc,
                                        std::size_t bytes,
                                        std::size_t alignment = alignof(std::max_align_t),
                                        std::size_t refill_bytes = (64ull << 10)) {
    bytes = align_up(bytes, alignment);
    if (pc.ptr == nullptr || pc.remaining < bytes) {
        const std::size_t take = std::max(bytes, refill_bytes);
        pc.ptr = static_cast<uint8_t*>(ta.alloc(take, alignment));
        pc.remaining = take;
    }

    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(pc.ptr);
    std::uintptr_t aligned = align_up(static_cast<std::size_t>(base), alignment);
    std::size_t pad = static_cast<std::size_t>(aligned - base);
    if (pad + bytes > pc.remaining) {
        const std::size_t take = std::max(bytes, refill_bytes);
        pc.ptr = static_cast<uint8_t*>(ta.alloc(take, alignment));
        pc.remaining = take;
        base = reinterpret_cast<std::uintptr_t>(pc.ptr);
        aligned = align_up(static_cast<std::size_t>(base), alignment);
        pad = static_cast<std::size_t>(aligned - base);
    }

    pc.ptr += pad;
    pc.remaining -= pad;
    void* out = pc.ptr;
    pc.ptr += bytes;
    pc.remaining -= bytes;
    return out;
}

} // namespace ReqSlab3

// Simple toggleable slab allocator. When disabled, falls back to operator new/delete.
class SlabBackend {
public:
    static SlabBackend& instance() {
        static SlabBackend inst;
        return inst;
    }

    void* alloc(std::size_t bytes) {
        if (!enabled_) return ::operator new(bytes);
        // Thread-local bump allocator on a reusable buffer
        thread_local Buffer buf;
        if (bytes > buf.remaining()) {
            const std::size_t chunk = bytes > default_chunk_ ? bytes : default_chunk_;
            buf.storage.resize(chunk);
            buf.offset = 0;
        }
        void* p = buf.storage.data() + buf.offset;
        buf.offset += bytes;
        return p;
    }

    void dealloc(void* p, std::size_t bytes) {
        if (!enabled_) {
            ::operator delete(p);
            return;
        }
        // No-op when slab is enabled; memory is reclaimed at process end.
        (void)p; (void)bytes;
    }

    bool enabled() const { return enabled_; }

private:
    struct Buffer {
        std::vector<char> storage;
        std::size_t offset{0};
        std::size_t remaining() const { return storage.size() - offset; }
    };

    SlabBackend() {
        const char* v = std::getenv("EXP_SLAB_ALLOC");
        enabled_ = v && *v && *v != '0';
    }

    bool enabled_{false};
    const std::size_t default_chunk_ = 1ull << 20; // 1 MiB per thread
};

// STL allocator wrapper that uses SlabBackend when enabled.
template <class T>
class SlabAllocator {
public:
    using value_type = T;

    SlabAllocator() noexcept {}
    template <class U>
    SlabAllocator(const SlabAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
        return static_cast<T*>(SlabBackend::instance().alloc(bytes));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        SlabBackend::instance().dealloc(p, n * sizeof(T));
    }

    bool operator==(const SlabAllocator&) const noexcept { return true; }
    bool operator!=(const SlabAllocator&) const noexcept { return false; }
};

} // namespace Contest
