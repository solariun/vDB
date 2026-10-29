// 08: a microcontroller-style database: a fixed static buffer, nothing else allocated, failures are errors.
#include <stdio.h>

#include <vdb.h>

static unsigned char memory[64 * 1024];  // all the database may use (an ESP32 has 320 KB of RAM)

int main() {
    vdb::ArenaAllocator arena(memory, sizeof(memory));
    vdb::Database db(arena);

    db.exec("CREATE TABLE readings (sensor, celsius)");
    for (int i = 0; i < 40; ++i) {
        db.insert("readings").value("sensor", i % 2 ? "out" : "in").value("celsius", 18.0 + i * 0.25).run();
    }
    vdb::Result r = db.query("SELECT sensor, COUNT(*) n, ROUND(AVG(celsius), 2) avg, MAX(celsius) max FROM readings GROUP BY sensor");
    for (size_t i = 0; i < r.size(); ++i) {
        printf("%s: %s readings, avg %s C, max %s C\n", r[i]["sensor"].text(), r[i]["n"].text(), r[i]["avg"].text(), r[i]["max"].text());
    }
    printf("arena: %zu of %zu bytes in use, peak %zu\n", arena.used(), arena.capacity(), arena.peak());

    // Fill the arena on purpose: the database says so and stays usable.
    int rows = 0;
    for (;; ++rows) {
        vdb::Result full = db.insert("readings").value("sensor", "spare").value("celsius", 0.0).run();
        if (!full.ok()) {
            printf("after %d more rows: %s\n", rows, full.error());
            break;
        }
    }
    db.exec("DELETE FROM readings WHERE sensor = 'spare'");
    printf("still works: %s readings\n", db.query("SELECT COUNT(*) FROM readings")[0][0].text());
    return 0;
}
