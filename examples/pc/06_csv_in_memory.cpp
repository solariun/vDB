// 06: CSV in the library: load text (any delimiter or a regex), query it, write it in every format.
#include <stdio.h>

#include <vdb.h>

struct Stdout : vdb::Output {
    void write(const char* text, size_t length) override { fwrite(text, 1, length, stdout); }
};

int main() {
    vdb::Database db;
    vdb::CsvOptions semicolons;  // ';' is the default delimiter
    db.load_csv_text("radios", "model;band;watts\nIC-705;HF/VHF/UHF;10\nIC-7300;HF/6m;100\nX6200;HF/6m;8\n", semicolons);

    vdb::CsvOptions spaces;
    spaces.delimiter_regex = "\\s+";  // columns split by any run of spaces
    db.load_csv_text("ps", "PID   USER     CPU\n101   root     0.5\n2047  gustavo  12.0\n", spaces);

    Stdout out;
    vdb::Format formats[] = {vdb::k_format_csv, vdb::k_format_table, vdb::k_format_json, vdb::k_format_markdown};
    const char* names[] = {"csv", "table", "json", "markdown"};
    for (int i = 0; i < 4; ++i) {
        printf("-- %s\n", names[i]);
        vdb::FormatOptions options;
        options.format = formats[i];
        vdb::Result r = db.query("SELECT model, watts FROM radios WHERE watts >= 10 ORDER BY watts DESC");
        r.write(out, options);
    }
    printf("-- regex-split table\n");
    vdb::FormatOptions table;
    table.format = vdb::k_format_table;
    db.query("SELECT USER, CPU FROM ps WHERE CPU > 1").write(out, table);
    return 0;
}
