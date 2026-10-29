// Shared by sel and vdb: files, stdout, options, and the resolver that opens CSV files named in FROM (spec 7, A10).
#ifndef VDB_TOOLS_CLI_HPP
#define VDB_TOOLS_CLI_HPP

#include <stdio.h>

#include "vdb/vdb.hpp"

namespace cli {

class FileSource final : public vdb::ByteSource {
public:
    explicit FileSource(FILE* file) : file_(file) {}
    size_t read(char* buffer, size_t size) override { return fread(buffer, 1, size, file_); }

private:
    FILE* file_;
};

class FileOutput final : public vdb::Output {
public:
    explicit FileOutput(FILE* file) : file_(file) {}
    void write(const char* text, size_t length) override { fwrite(text, 1, length, file_); }

private:
    FILE* file_;
};

// Everything the CSV options of both programs set.
struct Settings {
    vdb::CsvOptions csv;
    vdb::FormatOptions format;
    bool delimiter_given;
    bool output_delimiter_given;
    bool decimal_comma;
    bool stats;
    bool streaming;
    size_t memory_limit;  // 0: none
    Settings() : delimiter_given(false), output_delimiter_given(false), decimal_comma(false), stats(false), streaming(true), memory_limit(0) {}
};

// Parses one CSV/output option at argv[i] (advancing i past its value). Returns 1 when used, 0 when not an option
// of this group, -1 on a bad value (message printed).
int parse_common_option(const char* program, int argc, char** argv, int& i, Settings& settings);

// "tab", "comma", ";", "\t", ... to a character. False when the name is unknown.
bool parse_delimiter(const char* text, char& out, bool& is_auto);
// "512M", "2G", "100000" to bytes.
bool parse_size(const char* text, size_t& out);

// Opens CSV files named in FROM and JOIN; "-", "stdin" and "_" read stdin when it is not a terminal.
class Files {
public:
    Files(vdb::Database& db, Settings& settings, const char* program);
    ~Files();
    // Registers NAME=FILE or a plain FILE (named after the file) as a lazily read table.
    bool add_table(const char* spec);
    void install();  // becomes the database's table resolver
    void report_notes();  // irregular rows, delimiter hints (stderr)
    size_t rows_read() const;
    vdb::Table* open(const char* name, const char* path, vdb::Status& status);

private:
    struct Entry {
        FILE* file;
        FileSource* source;
        vdb::CsvReader* reader;
        char* name;
        char* path;
    };
    vdb::Database& db_;
    Settings& settings_;
    const char* program_;
    vdb::Vec<Entry> entries_;
};

// A heap copy of a C string (free() it); 0 when memory ran out.
char* copy_text(const char* text);

bool stdin_is_terminal();
bool stdout_is_terminal();
double now_seconds();

}  // namespace cli

#endif
