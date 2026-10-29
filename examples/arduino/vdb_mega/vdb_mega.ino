// vDB on an Arduino Mega 2560 (8 KB of RAM): a small table, SQL and the word chain, results on Serial.
// The Uno and the Nano (2 KB of RAM, 32 KB of flash) are too small for the SQL engine.
#include <vdb.h>

static unsigned char memory[4 * 1024];  // the PC run of these statements with AVR settings peaks at about 3.5 KB on AVR
static vdb::ArenaAllocator arena(memory, sizeof(memory));
static vdb::Database db(arena);

struct SerialOutput : vdb::Output {
    void write(const char* text, size_t length) override { Serial.write(reinterpret_cast<const uint8_t*>(text), length); }
};

static void show(vdb::Result r) {
    if (!r.ok()) {
        Serial.print(F("error: "));
        Serial.println(r.error());
        return;
    }
    SerialOutput out;
    vdb::FormatOptions csv;  // ';' separated, with a header line
    r.write(out, csv);
}

void setup() {
    Serial.begin(115200);
    db.exec("CREATE TABLE pins (pin, name, value)");
    for (int pin = 0; pin < 6; ++pin) {
        db.insert("pins").value("pin", pin).value("name", pin % 2 ? "odd" : "even").value("value", analogRead(A0 + pin)).run();
    }
    show(db.query("SELECT name, COUNT(*) n, MAX(value) top FROM pins GROUP BY name"));
    show(db.select("pin, value").from("pins").where.field("value").gt(100).order_by("value", vdb::DESC).run());
    Serial.print(F("arena peak: "));
    Serial.println(static_cast<unsigned>(arena.peak()));
}

void loop() {}
