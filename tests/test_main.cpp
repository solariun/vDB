// Runs every registered test, or those whose name contains the first argument.
#include "test_harness.hpp"

namespace test {

Case*& first_case() {
    static Case* first = 0;
    return first;
}

int& failures() {
    static int count = 0;
    return count;
}

}  // namespace test

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : 0;
    int run = 0, failed = 0;
    for (test::Case* c = test::first_case(); c; c = c->next) {
        if (filter && !strstr(c->name, filter)) continue;
        int before = test::failures();
        c->function();
        ++run;
        if (test::failures() != before) {
            ++failed;
            printf("FAIL %s (%s:%d)\n", c->name, c->file, c->line);
        }
    }
    printf("%d tests, %d passed, %d failed\n", run, run - failed, failed);
    return failed ? 1 : 0;
}
