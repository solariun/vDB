// A growable array for trivially copyable types, on a vDB allocator (no STL).
#ifndef VDB_VEC_HPP
#define VDB_VEC_HPP

#include <string.h>

#include "memory.hpp"

namespace vdb {

template <class T>
class Vec {
public:
    explicit Vec(Allocator& allocator) : allocator_(&allocator), data_(0), size_(0), capacity_(0) {}
    ~Vec() {
        if (data_) allocator_->release(data_);
    }
    Vec(const Vec&) = delete;
    Vec& operator=(const Vec&) = delete;
    Vec(Vec&& other) : allocator_(other.allocator_), data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
        other.data_ = 0;
        other.size_ = other.capacity_ = 0;
    }

    bool reserve(size_t wanted) {
        if (wanted <= capacity_) return true;
        size_t capacity = capacity_ ? capacity_ * 2 : k_first_capacity;
        if (capacity < wanted) capacity = wanted;
        if (capacity > static_cast<size_t>(-1) / sizeof(T)) return false;
        T* data = static_cast<T*>(allocator_->allocate(capacity * sizeof(T)));
        if (!data && capacity > wanted) {
            capacity = wanted;
            data = static_cast<T*>(allocator_->allocate(capacity * sizeof(T)));
        }
        if (!data) return false;
        if (size_) memcpy(static_cast<void*>(data), static_cast<const void*>(data_), size_ * sizeof(T));
        if (data_) allocator_->release(data_);
        data_ = data;
        capacity_ = capacity;
        return true;
    }

    bool push(const T& value) {
        if (size_ == capacity_ && !reserve(size_ + 1)) return false;
        data_[size_++] = value;
        return true;
    }

    // Grows with zero bytes, or shrinks.
    bool resize(size_t size) {
        if (size > size_) {
            if (!reserve(size)) return false;
            memset(static_cast<void*>(data_ + size_), 0, (size - size_) * sizeof(T));
        }
        size_ = size;
        return true;
    }

    void remove(size_t index) {
        if (index >= size_) return;
        memmove(static_cast<void*>(data_ + index), static_cast<const void*>(data_ + index + 1),
                (size_ - index - 1) * sizeof(T));
        --size_;
    }

    void clear() { size_ = 0; }
    void pop() {
        if (size_) --size_;
    }
    T& operator[](size_t index) { return data_[index]; }
    const T& operator[](size_t index) const { return data_[index]; }
    T& back() { return data_[size_ - 1]; }
    T* data() { return data_; }
    const T* data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    Allocator& allocator() const { return *allocator_; }

private:
    static const size_t k_first_capacity = 8;
    Allocator* allocator_;
    T* data_;
    size_t size_;
    size_t capacity_;
};

}  // namespace vdb

#endif
