#include "table.hpp"

#include <string.h>

namespace vdb {

int RowStream::next(const Value*&, size_t&, Status&) { return 0; }

// ── records ──────────────────────────────────────────────────────────────────

uint8_t* encode_record(Allocator& allocator, const Value* values, size_t count) {
    size_t size = varint_size(static_cast<uint32_t>(count));
    for (size_t i = 0; i < count; ++i) {
        if (values[i].type == k_null) {
            size += 1;
        } else {
            size += varint_size(static_cast<uint32_t>(values[i].n + 1)) + values[i].n + 1;
        }
    }
    uint8_t* record = static_cast<uint8_t*>(allocator.allocate(size));
    if (!record) return 0;
    uint8_t* p = varint_write(record, static_cast<uint32_t>(count));
    for (size_t i = 0; i < count; ++i) {
        if (values[i].type == k_null) {
            *p++ = 0;
            continue;
        }
        p = varint_write(p, static_cast<uint32_t>(values[i].n + 1));
        if (values[i].n) memcpy(p, values[i].s, values[i].n);
        p += values[i].n;
        *p++ = 0;
    }
    return record;
}

size_t record_field_count(const uint8_t* record) {
    uint32_t count;
    varint_read(record, count);
    return count;
}

void decode_record(const uint8_t* record, Value* out, size_t columns) {
    uint32_t count = 0;
    const uint8_t* p = record ? varint_read(record, count) : 0;
    for (size_t i = 0; i < columns; ++i) {
        if (i >= count) {
            out[i] = Value::null();
            continue;
        }
        uint32_t length;
        p = varint_read(p, length);
        if (!length) {
            out[i] = Value::null();
            continue;
        }
        out[i] = Value::text(reinterpret_cast<const char*>(p), length - 1);
        p += length;
    }
}

bool record_field(const uint8_t* record, size_t index, Value& out) {
    uint32_t count;
    const uint8_t* p = varint_read(record, count);
    out = Value::null();
    if (index >= count) return false;
    for (size_t i = 0;; ++i) {
        uint32_t length;
        p = varint_read(p, length);
        if (i == index) {
            if (length) out = Value::text(reinterpret_cast<const char*>(p), length - 1);
            return true;
        }
        if (length) p += length;
    }
}

// ── Table ────────────────────────────────────────────────────────────────────

Table::Table(Allocator& allocator, const char* name, size_t name_length)
    : allocator_(&allocator), name_(0), columns_(allocator), records_(allocator), stream_(0), stream_done_(false), streamed_(false) {
    name_ = static_cast<char*>(allocator.allocate(name_length + 1));
    if (name_) {
        memcpy(name_, name, name_length);
        name_[name_length] = 0;
    }
}

Table::~Table() {
    clear_rows();
    for (size_t i = 0; i < columns_.size(); ++i) allocator_->release(columns_[i]);
    if (name_) allocator_->release(name_);
}

int Table::find_column(const char* name, size_t length) const {
    for (size_t i = 0; i < columns_.size(); ++i) {
        if (names_equal(name, length, columns_[i])) return static_cast<int>(i);
    }
    return -1;
}

int Table::find_column(const char* name) const { return find_column(name, strlen(name)); }

int Table::add_column(const char* name, size_t length) {
    char* copy = static_cast<char*>(allocator_->allocate(length + 1));
    if (!copy) return -1;
    memcpy(copy, name, length);
    copy[length] = 0;
    if (!columns_.push(copy)) {
        allocator_->release(copy);
        return -1;
    }
    return static_cast<int>(columns_.size() - 1);
}

int Table::ensure_column(const char* name, size_t length) {
    int index = find_column(name, length);
    return index >= 0 ? index : add_column(name, length);
}

bool Table::append(uint8_t* record) {
    if (!record) return false;
    if (!records_.push(record)) {
        allocator_->release(record);
        return false;
    }
    return true;
}

bool Table::append_values(const Value* values, size_t count) { return append(encode_record(*allocator_, values, count)); }

void Table::replace(size_t index, uint8_t* record) {
    allocator_->release(records_[index]);
    records_[index] = record;
}

void Table::remove_rows(const uint8_t* marks) {
    size_t kept = 0;
    for (size_t i = 0; i < records_.size(); ++i) {
        if (marks[i]) {
            allocator_->release(records_[i]);
        } else {
            records_[kept++] = records_[i];
        }
    }
    records_.resize(kept);
}

void Table::clear_rows() {
    for (size_t i = 0; i < records_.size(); ++i) allocator_->release(records_[i]);
    records_.clear();
}

bool Table::load_stream(Status& status) {
    while (stream_pending()) {
        const Value* values;
        size_t count;
        int got = stream_->next(values, count, status);
        if (got < 0) return false;
        if (got == 0) {
            stream_done_ = true;
            break;
        }
        while (count > columns_.size()) {
            // More fields than the header: new columns cN, as load_csv does (spec 5).
            Text name(*allocator_);
            name.append_char('c');
            name.append_int(static_cast<int64_t>(columns_.size() + 1));
            if (!name.ok() || ensure_column(name.c_str(), name.size()) < 0) {
                status.fail_memory();
                return false;
            }
        }
        if (!append_values(values, count)) {
            status.fail_memory();
            return false;
        }
    }
    return true;
}

// ── HashIndex ────────────────────────────────────────────────────────────────

HashIndex::HashIndex(Allocator& allocator) : slots_(allocator), offsets_(allocator), keys_(allocator) {}

void HashIndex::clear() {
    slots_.clear();
    offsets_.clear();
    keys_.clear();
}

bool HashIndex::grow() {
    const size_t k_first_slots = 16;
    size_t capacity = slots_.size() ? slots_.size() * 2 : k_first_slots;
    Vec<Slot> grown(slots_.allocator());
    if (!grown.resize(capacity)) return false;
    for (size_t i = 0; i < slots_.size(); ++i) {
        const Slot& s = slots_[i];
        if (!s.id_plus_one) continue;
        size_t at = s.hash & (capacity - 1);
        while (grown[at].id_plus_one) at = (at + 1) & (capacity - 1);
        grown[at] = s;
    }
    slots_.clear();
    // Swap storage by moving the grown table in.
    if (!slots_.resize(capacity)) return false;
    memcpy(static_cast<void*>(slots_.data()), static_cast<const void*>(grown.data()), capacity * sizeof(Slot));
    return true;
}

long HashIndex::find(const char* key, size_t length) const {
    if (!slots_.size()) return -1;
    uint32_t h = hash_bytes(key, length);
    size_t mask = slots_.size() - 1;
    for (size_t at = h & mask;; at = (at + 1) & mask) {
        const Slot& s = slots_[at];
        if (!s.id_plus_one) return -1;
        if (s.hash != h) continue;
        size_t id = s.id_plus_one - 1;
        size_t start = offsets_[id], end = offsets_[id + 1];
        if (end - start == length && (length == 0 || memcmp(keys_.c_str() + start, key, length) == 0)) {
            return static_cast<long>(id);
        }
    }
}

long HashIndex::find_or_add(const char* key, size_t length, bool& added) {
    added = false;
    long found = find(key, length);
    if (found >= 0) return found;
    const size_t k_load_num = 7, k_load_den = 10;
    if ((size() + 1) * k_load_den > slots_.size() * k_load_num && !grow()) return -1;
    if (offsets_.empty() && !offsets_.push(0)) return -1;
    size_t id = size();
    if (!keys_.append(key, length) || !offsets_.push(keys_.size())) {
        keys_.truncate(offsets_[id]);
        return -1;
    }
    uint32_t h = hash_bytes(key, length);
    size_t mask = slots_.size() - 1;
    size_t at = h & mask;
    while (slots_[at].id_plus_one) at = (at + 1) & mask;
    slots_[at].hash = h;
    slots_[at].id_plus_one = static_cast<uint32_t>(id + 1);
    added = true;
    return static_cast<long>(id);
}

// ── merge sort ───────────────────────────────────────────────────────────────

bool merge_sort(uint32_t* items, size_t count, CompareIndexes compare, void* context, Allocator& allocator) {
    if (count < 2) return true;
    uint32_t* buffer = static_cast<uint32_t*>(allocator.allocate(count * sizeof(uint32_t)));
    if (!buffer) return false;
    const size_t k_run = 8;
    for (size_t start = 0; start < count; start += k_run) {
        size_t end = start + k_run < count ? start + k_run : count;
        for (size_t i = start + 1; i < end; ++i) {
            uint32_t item = items[i];
            size_t j = i;
            while (j > start && compare(items[j - 1], item, context) > 0) {
                items[j] = items[j - 1];
                --j;
            }
            items[j] = item;
        }
    }
    uint32_t* from = items;
    uint32_t* to = buffer;
    for (size_t width = k_run; width < count; width *= 2) {
        for (size_t left = 0; left < count; left += 2 * width) {
            size_t mid = left + width < count ? left + width : count;
            size_t right = left + 2 * width < count ? left + 2 * width : count;
            size_t i = left, j = mid, k = left;
            while (i < mid && j < right) to[k++] = compare(from[j], from[i], context) < 0 ? from[j++] : from[i++];
            while (i < mid) to[k++] = from[i++];
            while (j < right) to[k++] = from[j++];
        }
        uint32_t* swap = from;
        from = to;
        to = swap;
    }
    if (from != items) memcpy(items, from, count * sizeof(uint32_t));
    allocator.release(buffer);
    return true;
}

}  // namespace vdb
