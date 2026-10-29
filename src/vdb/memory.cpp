#include "memory.hpp"

#include <string.h>
#ifndef VDB_NO_HEAP
#include <stdlib.h>
#endif

namespace vdb {

void* Allocator::allocate(size_t) { return 0; }
void Allocator::release(void*) {}

#ifndef VDB_NO_HEAP
void* HeapAllocator::allocate(size_t size) { return malloc(size ? size : 1); }
void HeapAllocator::release(void* block) { free(block); }

// Constructed on first use without a guard variable (no __cxa_guard on MCUs); it has no state.
Allocator& heap_allocator() {
    alignas(HeapAllocator) static unsigned char storage[sizeof(HeapAllocator)];
    static bool ready = false;
    if (!ready) {
        new (storage, PlaceTag()) HeapAllocator();
        ready = true;
    }
    return *reinterpret_cast<HeapAllocator*>(storage);
}
#endif

// ── ArenaAllocator ─────────────────────────────────────────────────────────
// Blocks lie back to back: [header: size | used bit][payload]. Sizes are multiples of k_align.

namespace {
const size_t k_used_bit = 1;
const size_t k_header = align_up(sizeof(size_t));
const size_t k_min_split = k_header + k_align;

size_t& header_at(unsigned char* p) { return *reinterpret_cast<size_t*>(p); }
}  // namespace

ArenaAllocator::ArenaAllocator(void* buffer, size_t size) : base_(0), size_(0), used_(0), peak_(0) {
    uintptr_t start = reinterpret_cast<uintptr_t>(buffer);
    uintptr_t aligned = (start + k_align - 1) & ~(static_cast<uintptr_t>(k_align) - 1);
    size_t lost = static_cast<size_t>(aligned - start);
    if (!buffer || size < lost + k_min_split) return;
    base_ = reinterpret_cast<unsigned char*>(aligned);
    size_ = (size - lost) & ~(k_align - 1);
    header_at(base_) = size_;
}

void* ArenaAllocator::allocate(size_t size) {
    if (!base_ || size > size_) return 0;
    size_t need = align_up(size ? size : 1) + k_header;
    unsigned char* end = base_ + size_;
    for (unsigned char* p = base_; p < end;) {
        size_t header = header_at(p);
        size_t block = header & ~k_used_bit;
        if (!(header & k_used_bit)) {
            // Merge the free blocks that follow.
            unsigned char* next = p + block;
            while (next < end && !(header_at(next) & k_used_bit)) {
                block += header_at(next);
                next = p + block;
            }
            header_at(p) = block;
            if (block >= need) {
                if (block - need >= k_min_split) {
                    header_at(p + need) = block - need;
                    block = need;
                }
                header_at(p) = block | k_used_bit;
                used_ += block;
                if (used_ > peak_) peak_ = used_;
                return p + k_header;
            }
        }
        p += block;
    }
    return 0;
}

void ArenaAllocator::release(void* block) {
    if (!block) return;
    unsigned char* p = static_cast<unsigned char*>(block) - k_header;
    size_t size = header_at(p) & ~k_used_bit;
    used_ -= size;
    unsigned char* next = p + size;
    if (next < base_ + size_ && !(header_at(next) & k_used_bit)) size += header_at(next);
    header_at(p) = size;
}

// ── BudgetAllocator ────────────────────────────────────────────────────────

BudgetAllocator::BudgetAllocator(Allocator& parent, size_t limit) : parent_(&parent), limit_(limit), used_(0), peak_(0) {}

void* BudgetAllocator::allocate(size_t size) {
    size_t total = size + k_header;
    if (total < size || used_ + total > limit_ || used_ + total < used_) return 0;
    unsigned char* p = static_cast<unsigned char*>(parent_->allocate(total));
    if (!p) return 0;
    header_at(p) = total;
    used_ += total;
    if (used_ > peak_) peak_ = used_;
    return p + k_header;
}

void BudgetAllocator::release(void* block) {
    if (!block) return;
    unsigned char* p = static_cast<unsigned char*>(block) - k_header;
    used_ -= header_at(p);
    parent_->release(p);
}

// ── Pool ───────────────────────────────────────────────────────────────────

Pool::Pool(Allocator& allocator, size_t chunk_size)
    : allocator_(&allocator), chunk_size_(chunk_size), head_(0), spare_(0) {}

Pool::~Pool() {
    clear();
    while (spare_) {
        Chunk* next = spare_->next;
        allocator_->release(spare_);
        spare_ = next;
    }
}

unsigned char* Pool::data(Chunk* chunk) { return reinterpret_cast<unsigned char*>(chunk) + align_up(sizeof(Chunk)); }

void Pool::drop(Chunk* chunk) {
    if (chunk->size == chunk_size_) {
        chunk->next = spare_;
        spare_ = chunk;
    } else {
        allocator_->release(chunk);
    }
}

void* Pool::allocate(size_t size) {
    size_t need = align_up(size ? size : 1);
    if (need < size) return 0;
    if (head_ && head_->size - head_->used >= need) {
        void* p = data(head_) + head_->used;
        head_->used += need;
        return p;
    }
    Chunk* chunk = 0;
    if (need <= chunk_size_ && spare_) {
        chunk = spare_;
        spare_ = spare_->next;
    } else {
        size_t capacity = need > chunk_size_ ? need : chunk_size_;
        size_t total = align_up(sizeof(Chunk)) + capacity;
        if (total < capacity) return 0;
        chunk = static_cast<Chunk*>(allocator_->allocate(total));
        if (!chunk && capacity > need) {
            // Little memory left: a chunk just big enough (a full database can still run DELETE).
            capacity = need;
            chunk = static_cast<Chunk*>(allocator_->allocate(align_up(sizeof(Chunk)) + capacity));
        }
        if (!chunk) return 0;
        chunk->size = capacity;
    }
    chunk->used = need;
    chunk->next = head_;
    head_ = chunk;
    return data(chunk);
}

char* Pool::copy(const char* text, size_t length) {
    char* p = static_cast<char*>(allocate(length + 1));
    if (!p) return 0;
    if (length) memcpy(p, text, length);
    p[length] = 0;
    return p;
}

Pool::Mark Pool::mark() const {
    Mark m;
    m.chunk = head_;
    m.used = head_ ? head_->used : 0;
    return m;
}

void Pool::release(const Mark& mark) {
    while (head_ && head_ != mark.chunk) {
        Chunk* next = head_->next;
        drop(head_);
        head_ = next;
    }
    if (head_) head_->used = mark.used;
}

void Pool::clear() {
    while (head_) {
        Chunk* next = head_->next;
        drop(head_);
        head_ = next;
    }
}

}  // namespace vdb
