#include "csv.hpp"

#include <string.h>

namespace vdb {

size_t ByteSource::read(char*, size_t) { return 0; }

TextSource::TextSource(const char* text) : text_(text), length_(text ? strlen(text) : 0), at_(0) {}

size_t TextSource::read(char* buffer, size_t size) {
    size_t n = length_ - at_ < size ? length_ - at_ : size;
    if (n) memcpy(buffer, text_ + at_, n);
    at_ += n;
    return n;
}

CsvReader::CsvReader(Allocator& allocator, ByteSource& source, const CsvOptions& options)
    : allocator_(allocator),
      source_(source),
      options_(options),
      delimiter_(options.delimiter),
      pool_(allocator, VDB_POOL_CHUNK),
      regex_(0),
      work_(allocator),
      buffer_(allocator),
      buffer_at_(0),
      buffer_end_(0),
      eof_(false),
      first_line_(true),
      record_(allocator),
      line_text_(allocator),
      out_(allocator),
      fields_(allocator),
      values_(allocator),
      names_(allocator),
      line_(0),
      record_line_(0),
      records_(0),
      short_rows_(0),
      long_rows_(0),
      pending_(false) {}

bool CsvReader::fill() {
    if (eof_) return false;
    if (buffer_.empty()) {
        // Little memory: a smaller buffer reads the same file, only in more calls.
        const size_t k_smallest = 64;
        size_t size = VDB_CSV_CHUNK;
        while (!buffer_.resize(size) && size > k_smallest) size /= 2;
        if (buffer_.empty()) return false;
    }
    buffer_end_ = source_.read(buffer_.data(), buffer_.size());
    buffer_at_ = 0;
    if (!buffer_end_) eof_ = true;
    return buffer_end_ != 0;
}

int CsvReader::read_line(Text& line) {
    bool got = false;
    for (;;) {
        if (buffer_at_ >= buffer_end_ && !fill()) {
            if (buffer_.empty() && !eof_) return -1;
            break;
        }
        got = true;
        const char* start = buffer_.data() + buffer_at_;
        size_t available = buffer_end_ - buffer_at_;
        const char* nl = static_cast<const char*>(memchr(start, '\n', available));
        size_t take = nl ? static_cast<size_t>(nl - start) : available;
        if (!line.append(start, take)) return -1;
        buffer_at_ += take + (nl ? 1 : 0);
        if (nl) break;
    }
    if (!got) return 0;
    ++line_;
    if (line.size() && line.data()[line.size() - 1] == '\r') line.truncate(line.size() - 1);
    if (first_line_) {
        first_line_ = false;
        const char k_bom[] = "\xEF\xBB\xBF";
        if (line.size() >= 3 && memcmp(line.c_str(), k_bom, 3) == 0) {
            Text rest(allocator_);
            if (!rest.append(line.c_str() + 3, line.size() - 3)) return -1;
            line.clear();
            if (!line.append(rest.c_str(), rest.size())) return -1;
        }
    }
    return 1;
}

bool CsvReader::find_delimiter(size_t from, size_t& start, size_t& end, Status& status) {
    const char* s = record_.c_str();
    size_t n = record_.size();
    if (!regex_) {
        const char* hit = from < n ? static_cast<const char*>(memchr(s + from, delimiter_, n - from)) : 0;
        if (!hit) return false;
        start = static_cast<size_t>(hit - s);
        end = start + 1;
        return true;
    }
    size_t caps[2];
    for (size_t at = from; at <= n;) {
        int r = regex_->search(s, n, at, caps, 1, work_);
        if (r < 0) {
            status.fail_memory();
            return false;
        }
        if (r == 0) return false;
        if (caps[1] > caps[0]) {
            start = caps[0];
            end = caps[1];
            return true;
        }
        if (caps[0] >= n) return false;
        uint32_t cp;
        at = caps[0] + utf8_decode(s + caps[0], n - caps[0], cp);
    }
    return false;
}

// Reads one record into fields_ (text in out_). 1 a record, 0 the end, -1 an error.
int CsvReader::read_record(Status& status) {
    record_.clear();
    out_.clear();
    fields_.clear();
    for (;;) {
        int got = read_line(record_);
        if (got < 0) {
            status.fail_memory();
            return -1;
        }
        if (got == 0) return 0;
        bool blank = true;
        for (size_t i = 0; i < record_.size() && blank; ++i) blank = is_space(record_.c_str()[i]);
        if (!blank) break;
        record_.clear();
    }
    record_line_ = line_;
    char quote = options_.quote;
    size_t pos = 0;
    for (;;) {
        Field f;
        f.offset = out_.size();
        bool quoted = false;
        if (options_.trim) {
            while (pos < record_.size() && (record_.c_str()[pos] == ' ' || record_.c_str()[pos] == '\t') &&
                   !(regex_ == 0 && record_.c_str()[pos] == delimiter_))
                ++pos;
        }
        size_t dstart = 0, dend = 0;
        bool more = false;
        if (quote && pos < record_.size() && record_.c_str()[pos] == quote) {
            quoted = true;
            ++pos;
            for (;;) {
                if (pos >= record_.size()) {
                    line_text_.clear();
                    int got = read_line(line_text_);
                    if (got < 0) {
                        status.fail_memory();
                        return -1;
                    }
                    if (got == 0) {
                        if (options_.strict) {
                            status.fail(k_error_csv, VDB_F("csv line %ld: quote not closed"), static_cast<long>(record_line_));
                            return -1;
                        }
                        break;
                    }
                    if (!record_.append_char('\n') || !record_.append(line_text_.c_str(), line_text_.size())) {
                        status.fail_memory();
                        return -1;
                    }
                    continue;
                }
                char c = record_.c_str()[pos];
                if (c == quote) {
                    if (pos + 1 < record_.size() && record_.c_str()[pos + 1] == quote) {
                        out_.append_char(quote);
                        pos += 2;
                        continue;
                    }
                    ++pos;
                    break;
                }
                out_.append_char(c);
                ++pos;
            }
            more = find_delimiter(pos, dstart, dend, status);
            if (!status.ok()) return -1;
            size_t junk_end = more ? dstart : record_.size();
            size_t junk_start = pos;
            if (options_.trim) {
                while (junk_start < junk_end && (record_.c_str()[junk_start] == ' ' || record_.c_str()[junk_start] == '\t')) ++junk_start;
            }
            if (junk_start < junk_end) {
                if (options_.strict) {
                    status.fail(k_error_csv, VDB_F("csv line %ld: text after a closing quote"), static_cast<long>(record_line_));
                    return -1;
                }
                out_.append(record_.c_str() + junk_start, junk_end - junk_start);
            }
        } else {
            more = find_delimiter(pos, dstart, dend, status);
            if (!status.ok()) return -1;
            size_t end = more ? dstart : record_.size();
            if (options_.trim) {
                while (end > pos && (record_.c_str()[end - 1] == ' ' || record_.c_str()[end - 1] == '\t')) --end;
            }
            out_.append(record_.c_str() + pos, end - pos);
        }
        f.length = out_.size() - f.offset;
        f.null = !quoted && f.length == 0 && !options_.empty_text;
        out_.append_char(0);
        if (!out_.ok() || !fields_.push(f)) {
            status.fail_memory();
            return -1;
        }
        if (!more) break;
        pos = dend;
    }
    return 1;
}

bool CsvReader::make_names(Status& status) {
    for (size_t i = 0; i < fields_.size(); ++i) {
        const char* s = options_.header ? out_.c_str() + fields_[i].offset : "";
        size_t n = options_.header ? fields_[i].length : 0;
        while (n && is_space(*s)) {
            ++s;
            --n;
        }
        while (n && is_space(s[n - 1])) --n;
        Text name(allocator_);
        if (n) name.append(s, n);
        else {
            name.append_char('c');
            name.append_int(static_cast<int64_t>(i + 1));
        }
        // Repeated names get _2, _3, ... (spec 5).
        size_t base = name.size();
        for (int copy = 2;; ++copy) {
            bool taken = false;
            for (size_t j = 0; j < names_.size() && !taken; ++j) taken = names_equal(names_[j], name.c_str());
            if (!taken) break;
            name.truncate(base);
            name.append_char('_');
            name.append_int(copy);
        }
        char* copy = pool_.copy(name.c_str(), name.size());
        if (!name.ok() || !copy || !names_.push(copy)) {
            status.fail_memory();
            return false;
        }
    }
    return true;
}

Status CsvReader::open() {
    Status status;
    if (options_.delimiter_regex) {
        regex_ = Regex::compile(pool_, options_.delimiter_regex, strlen(options_.delimiter_regex), 0, status);
        if (!regex_) {
            Status wrapped;
            return wrapped.fail(k_error_csv, VDB_F("delimiter %s"), status.message());
        }
    }
    if (options_.auto_delimiter && !regex_) {
        // Peek the first line to sniff the delimiter, then parse it as a record.
        if (!fill() && !eof_) return status.fail_memory();
        const char k_candidates[] = ";,\t|:";
        size_t best = 0;
        char chosen = options_.delimiter;
        size_t end = buffer_at_;
        while (end < buffer_end_ && buffer_[end] != '\n') ++end;
        for (const char* c = k_candidates; *c; ++c) {
            size_t count = 0;
            bool in_quote = false;
            for (size_t i = buffer_at_; i < end; ++i) {
                if (buffer_[i] == options_.quote) in_quote = !in_quote;
                else if (!in_quote && buffer_[i] == *c) ++count;
            }
            if (count > best) {
                best = count;
                chosen = *c;
            }
        }
        delimiter_ = chosen;
    }
    int got = read_record(status);
    if (got < 0) return status;
    if (got == 0) return status;  // an empty file: no columns, no rows
    if (!make_names(status)) return status;
    if (!options_.header) pending_ = true;
    return status;
}

int CsvReader::next(const Value*& values, size_t& count, Status& status) {
    if (!pending_) {
        int got = read_record(status);
        if (got <= 0) return got;
    }
    pending_ = false;
    ++records_;
    size_t fields = fields_.size();
    if (fields < names_.size()) {
        ++short_rows_;
        if (options_.strict) {
            status.fail(k_error_csv, VDB_F("csv line %ld: %d fields, the header has %d"), static_cast<long>(record_line_),
                        static_cast<int>(fields), static_cast<int>(names_.size()));
            return -1;
        }
    } else if (fields > names_.size()) {
        ++long_rows_;
        if (options_.strict) {
            status.fail(k_error_csv, VDB_F("csv line %ld: %d fields, the header has %d"), static_cast<long>(record_line_),
                        static_cast<int>(fields), static_cast<int>(names_.size()));
            return -1;
        }
    }
    if (!values_.resize(fields + 1)) {
        status.fail_memory();
        return -1;
    }
    for (size_t i = 0; i < fields; ++i) {
        const Field& f = fields_[i];
        values_[i] = f.null ? Value::null() : Value::text(out_.c_str() + f.offset, f.length);
    }
    values = values_.data();
    count = fields;
    return 1;
}

// ── Database CSV helpers ─────────────────────────────────────────────────────

namespace {
bool extra_columns(Table* t, size_t count) {
    while (t->column_count() < count) {
        Text name(t->allocator());
        name.append_char('c');
        name.append_int(static_cast<int64_t>(t->column_count() + 1));
        if (!name.ok() || t->ensure_column(name.c_str(), name.size()) < 0) return false;
    }
    return true;
}
}  // namespace

Status Database::load_csv(const char* name, ByteSource& source, const CsvOptions& options) {
    Status status;
    if (table(name)) return status.fail(k_error_misuse, VDB_F("table %s already exists (DROP TABLE it first)"), name);
    CsvReader reader(*allocator_, source, options);
    status = reader.open();
    if (!status.ok()) return status;
    Table* t = add_table(name, strlen(name), status);
    if (!t) return status;
    for (size_t i = 0; i < reader.column_count(); ++i) {
        if (t->add_column(reader.column_name(i), strlen(reader.column_name(i))) < 0) {
            drop_table(name);
            return status.fail_memory();
        }
    }
    for (;;) {
        const Value* values;
        size_t count;
        int got = reader.next(values, count, status);
        if (got < 0) {
            drop_table(name);
            return status;
        }
        if (got == 0) break;
        if ((count > t->column_count() && !extra_columns(t, count)) || !t->append_values(values, count)) {
            drop_table(name);
            return status.fail_memory();
        }
    }
    return status;
}

Status Database::load_csv_text(const char* name, const char* text, const CsvOptions& options) {
    TextSource source(text);
    return load_csv(name, source, options);
}

Status Database::attach_csv(const char* name, CsvReader& reader) {
    Status status;
    if (table(name)) return status.fail(k_error_misuse, VDB_F("table %s already exists"), name);
    Table* t = add_table(name, strlen(name), status);
    if (!t) return status;
    for (size_t i = 0; i < reader.column_count(); ++i) {
        if (t->add_column(reader.column_name(i), strlen(reader.column_name(i))) < 0) {
            drop_table(name);
            return status.fail_memory();
        }
    }
    t->set_stream(&reader);
    return status;
}

}  // namespace vdb
