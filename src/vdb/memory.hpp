// Memory: the replaceable allocator, the arena for MCUs, the budget cap and the bump pool (spec A5, 4.3).
#ifndef VDB_MEMORY_HPP
#define VDB_MEMORY_HPP

#include "config.hpp"

namespace vdb {
struct PlaceTag {};
}  // namespace vdb

// Placement construction without <new>.
inline void* operator new(size_t, void* where, vdb::PlaceTag) noexcept { return where; }
inline void operator delete(void*, void*, vdb::PlaceTag) noexcept {}

namespace vdb {

const size_t k_align = alignof(int64_t) > alignof(double)
                           ? (alignof(int64_t) > alignof(void*) ? alignof(int64_t) : alignof(void*))
                           : (alignof(double) > alignof(void*) ? alignof(double) : alignof(void*));

inline size_t align_up(size_t n) { return (n + k_align - 1) & ~(k_align - 1); }

// Every byte vDB uses comes through one of these. allocate() returns 0 when it cannot serve.
class Allocator {
public:
    virtual void* allocate(size_t size);
    virtual void release(void* block);

protected:
    ~Allocator() {}
};

#ifndef VDB_NO_HEAP
class HeapAllocator final : public Allocator {
public:
    void* allocate(size_t size) override;
    void release(void* block) override;
};

Allocator& heap_allocator();
#endif

// Serves a caller-owned buffer: first fit, neighbouring free blocks merged. For MCUs and hard caps.
class ArenaAllocator final : public Allocator {
public:
    ArenaAllocator(void* buffer, size_t size);
    void* allocate(size_t size) override;
    void release(void* block) override;
    size_t capacity() const { return size_; }
    size_t used() const { return used_; }
    size_t peak() const { return peak_; }

private:
    unsigned char* base_;
    size_t size_;
    size_t used_;
    size_t peak_;
};

// Passes requests to a parent until `limit` bytes are in use; counts use and peak.
class BudgetAllocator final : public Allocator {
public:
    BudgetAllocator(Allocator& parent, size_t limit);
    void* allocate(size_t size) override;
    void release(void* block) override;
    size_t used() const { return used_; }
    size_t peak() const { return peak_; }
    size_t limit() const { return limit_; }
    void set_limit(size_t limit) { limit_ = limit; }

private:
    Allocator* parent_;
    size_t limit_;
    size_t used_;
    size_t peak_;
};

// Bump allocator in chunks; mark()/release() give stack discipline for per-row scratch.
class Pool {
public:
    struct Mark {
        void* chunk;
        size_t used;
    };

    explicit Pool(Allocator& allocator, size_t chunk_size = VDB_POOL_CHUNK);
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    void* allocate(size_t size);
    char* copy(const char* text, size_t length);  // NUL added
    Mark mark() const;
    void release(const Mark& mark);
    void clear();
    Allocator& allocator() const { return *allocator_; }

private:
    struct Chunk {
        Chunk* next;
        size_t size;
        size_t used;
    };
    static unsigned char* data(Chunk* chunk);
    void drop(Chunk* chunk);

    Allocator* allocator_;
    size_t chunk_size_;
    Chunk* head_;
    Chunk* spare_;
};

template <class T>
T* make(Pool& pool) {
    void* memory = pool.allocate(sizeof(T));
    return memory ? new (memory, PlaceTag()) T() : 0;
}

template <class T>
T* make(Allocator& allocator) {
    void* memory = allocator.allocate(sizeof(T));
    return memory ? new (memory, PlaceTag()) T() : 0;
}

template <class T>
void destroy(Allocator& allocator, T* object) {
    if (!object) return;
    object->~T();
    allocator.release(object);
}

}  // namespace vdb

#endif
