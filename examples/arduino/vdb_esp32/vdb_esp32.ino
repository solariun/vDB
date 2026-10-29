// vDB on an ESP32: sensor readings in a database inside a static arena, queried with SQL every 5 seconds.
// Board: any ESP32. Open the serial monitor at 115200 baud.
#include <vdb.h>

static unsigned char memory[48 * 1024];  // everything the database may use
static vdb::ArenaAllocator arena(memory, sizeof(memory));
static vdb::Database db(arena);

// Sends what vDB writes to the serial port.
struct SerialOutput : vdb::Output {
    void write(const char* text, size_t length) override { Serial.write(reinterpret_cast<const uint8_t*>(text), length); }
};

static const unsigned long k_period_ms = 500;
static const int k_report_every = 10;
static const int k_keep_seconds = 60;

void setup() {
    Serial.begin(115200);
    db.exec("CREATE TABLE readings (ms, sensor, celsius)");
    Serial.println("vDB on ESP32: readings every 0.5 s, a report every 5 s");
}

void loop() {
    static int count = 0;
    unsigned long now = millis();
    double celsius = 20.0 + static_cast<double>(random(0, 100)) / 10.0;  // stands in for a real sensor
    vdb::Result added = db.insert("readings")
                            .value("ms", static_cast<unsigned long>(now))
                            .value("sensor", count % 2 ? "outdoor" : "indoor")
                            .value("celsius", celsius)
                            .run();
    if (!added.ok()) Serial.printf("insert: %s\n", added.error());

    if (++count % k_report_every == 0) {
        vdb::Result r = db.query("SELECT sensor, COUNT(*) AS n, ROUND(AVG(celsius), 1) AS avg, MAX(celsius) AS max "
                                 "FROM readings GROUP BY sensor ORDER BY sensor");
        SerialOutput out;
        vdb::FormatOptions table;
        table.format = vdb::k_format_table;
        r.write(out, table);
        // Keep one minute of readings: the arena never fills.
        vdb::Params old(arena);
        old.add(static_cast<long long>(now) - k_keep_seconds * 1000LL);
        db.exec("DELETE FROM readings WHERE ms < ?", old);
        Serial.printf("arena: %u of %u bytes in use (peak %u)\n", static_cast<unsigned>(arena.used()),
                      static_cast<unsigned>(arena.capacity()), static_cast<unsigned>(arena.peak()));
    }
    delay(k_period_ms);
}
