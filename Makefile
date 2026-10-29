# =============================================================================
# Makefile — vDB (spec 1.2, 10)
# =============================================================================
CXX      ?= c++
WARN      = -Wall -Wextra -Wpedantic -Werror
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++11 $(WARN) -MMD -MP
INCLUDES  = -Isrc
PREFIX   ?= /usr/local

BUILDDIR  = build
BINDIR    = bin

CORE_SRC  = $(wildcard src/vdb/*.cpp)
CORE_OBJ  = $(CORE_SRC:%.cpp=$(BUILDDIR)/%.o)
LIB       = $(BUILDDIR)/libvdb.a

TOOL_COMMON = $(BUILDDIR)/tools/cli.o
SEL_BIN   = $(BINDIR)/sel
SHELL_BIN = $(BINDIR)/vdb

TEST_SRC  = $(wildcard tests/test_*.cpp)
TEST_OBJ  = $(TEST_SRC:%.cpp=$(BUILDDIR)/%.o)
TEST_BIN  = $(BINDIR)/vdb_tests

EXAMPLE_SRC = $(wildcard examples/pc/*.cpp)
EXAMPLE_BIN = $(EXAMPLE_SRC:examples/pc/%.cpp=$(BINDIR)/examples/%)

# ── Embedded check (spec 8): no C++ library, no exceptions, no RTTI ─────────
EMBED_FLAGS = -std=c++11 -Os $(WARN) -fno-exceptions -fno-rtti -nostdinc++ -Isrc
FORBIDDEN_SYMBOLS = '__cxa_throw|__gxx_personality|_Znwm|_Znam|_Znwj|_Znaj|_ZdlPv|_ZdaPv|_ZTI|_ZTS|_ZNSt|__cxa_guard'
ARDUINO15 := $(firstword $(wildcard $(HOME)/Library/Arduino15) $(wildcard $(HOME)/.arduino15))
AVR_GXX   := $(firstword $(wildcard $(ARDUINO15)/packages/arduino/tools/avr-gcc/*/bin/avr-g++) $(shell command -v avr-g++ 2>/dev/null))
XTENSA_GXX := $(firstword $(wildcard $(ARDUINO15)/packages/esp32/tools/esp-x32/*/bin/xtensa-esp32-elf-g++) $(shell command -v xtensa-esp32-elf-g++ 2>/dev/null))
ARM_GXX   := $(shell command -v arm-none-eabi-g++ 2>/dev/null)
FQBN_ESP32 = esp32:esp32:esp32
FQBN_MEGA  = arduino:avr:mega
ARDUINO_BUILD = $(BUILDDIR)/arduino

.PHONY: all lib tools test test_small test_tools sanitize check_embedded arduino_check examples examples_run install uninstall clean help

all: lib tools examples

help:
	@echo "make [all|lib|tools|test|test_small|test_tools|sanitize|check_embedded|arduino_check|examples|examples_run|install|clean]"

lib: $(LIB)

tools: $(SEL_BIN) $(SHELL_BIN)

$(LIB): $(CORE_OBJ)
	@mkdir -p $(dir $@)
	rm -f $@
	ar rcs $@ $^

$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Itools -Itests -c $< -o $@

$(SEL_BIN): $(BUILDDIR)/tools/sel.o $(TOOL_COMMON) $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(SHELL_BIN): $(BUILDDIR)/tools/vdb_shell.o $(TOOL_COMMON) $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(TEST_BIN): $(TEST_OBJ) $(TOOL_COMMON) $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(TEST_BIN)
	./$(TEST_BIN) $(FILTER)

# The unit tests with the microcontroller sizes (spec 8): small pool, scratch and CSV chunks, AVR nesting bound.
SMALL_FLAGS = -DVDB_POOL_CHUNK=256 -DVDB_SCRATCH_CHUNK=128 -DVDB_CSV_CHUNK=64 -DVDB_MAX_DEPTH=12 -DVDB_REGEX_MAX_PROGRAM=256
test_small:
	@mkdir -p $(BUILDDIR)/small
	$(CXX) -std=c++11 -O1 $(WARN) $(SMALL_FLAGS) -Isrc -Itools -Itests $(TEST_SRC) tools/cli.cpp $(CORE_SRC) \
		-o $(BUILDDIR)/small/vdb_tests
	./$(BUILDDIR)/small/vdb_tests

test_tools: tools
	sh tests/tools/run.sh $(BINDIR)

# The unit tests again under AddressSanitizer and UndefinedBehaviorSanitizer.
sanitize:
	@mkdir -p $(BUILDDIR)/sanitize
	$(CXX) -std=c++11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(WARN) \
		-Isrc -Itools -Itests $(TEST_SRC) tools/cli.cpp $(CORE_SRC) -o $(BUILDDIR)/sanitize/vdb_tests
	./$(BUILDDIR)/sanitize/vdb_tests

examples: $(EXAMPLE_BIN)

$(BINDIR)/examples/%: $(BUILDDIR)/examples/pc/%.o $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

examples_run: examples
	@for e in $(EXAMPLE_BIN); do echo "== $$e"; (cd examples/data && ../../$$e) || exit 1; done

check_embedded:
	@mkdir -p $(BUILDDIR)/embedded
	@for f in $(CORE_SRC); do \
		$(CXX) $(EMBED_FLAGS) -DVDB_NO_HEAP -c $$f -o $(BUILDDIR)/embedded/$$(basename $$f .cpp).o || exit 1; \
	done
	@if nm -u $(BUILDDIR)/embedded/*.o | grep -E $(FORBIDDEN_SYMBOLS); then \
		echo "check_embedded: FAILED (forbidden symbol above)"; exit 1; \
	else echo "check_embedded: core free of the C++ library, exceptions, RTTI and heap (-nostdinc++, VDB_NO_HEAP)"; fi
	@if nm -u $(BUILDDIR)/embedded/*.o | grep -E '^ *U _?(malloc|free|calloc|realloc)$$'; then \
		echo "check_embedded: FAILED (heap call with VDB_NO_HEAP)"; exit 1; fi
	@if [ -n "$(AVR_GXX)" ]; then \
		for f in $(CORE_SRC); do "$(AVR_GXX)" $(EMBED_FLAGS) -mmcu=atmega2560 -c $$f -o /dev/null || exit 1; done; \
		echo "check_embedded: avr (atmega2560) OK"; \
	else echo "check_embedded: avr-g++ not found, skipped"; fi
	@if [ -n "$(XTENSA_GXX)" ]; then \
		for f in $(CORE_SRC); do "$(XTENSA_GXX)" $(EMBED_FLAGS) -mlongcalls -c $$f -o /dev/null || exit 1; done; \
		echo "check_embedded: xtensa (esp32) OK"; \
	else echo "check_embedded: xtensa-esp32-elf-g++ not found, skipped"; fi
	@if [ -n "$(ARM_GXX)" ]; then \
		for f in $(CORE_SRC); do $(ARM_GXX) $(EMBED_FLAGS) -mcpu=cortex-m4 -mthumb -c $$f -o /dev/null || exit 1; done; \
		echo "check_embedded: arm cortex-m4 OK"; \
	else echo "check_embedded: arm-none-eabi-g++ not found, skipped"; fi

# Compiles the Arduino sketches against this library; flash and RAM use in build/arduino/*.log.
arduino_check:
	@command -v arduino-cli >/dev/null || { echo "arduino-cli not found"; exit 1; }
	@mkdir -p $(ARDUINO_BUILD)
	@for job in $(FQBN_ESP32)@examples/arduino/vdb_esp32 $(FQBN_MEGA)@examples/arduino/vdb_mega; do \
		fqbn=$${job%@*}; sketch=$${job#*@}; name=$$(basename $$sketch); log=$(ARDUINO_BUILD)/$$name.log; \
		echo "  arduino $$fqbn $$sketch"; \
		arduino-cli compile --fqbn $$fqbn --warnings all --library $(CURDIR) --build-path $(ARDUINO_BUILD)/$$name \
			$$sketch > $$log 2>&1 || { cat $$log; exit 1; }; \
		if grep -E "(src/vdb|$$name\.ino).*warning:" $$log; then echo "arduino_check: warnings above"; exit 1; fi; \
		grep -E "^(Sketch uses|Global variables)" $$log | sed 's/^/    /'; \
	done

install: tools
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(SEL_BIN) $(SHELL_BIN) $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/sel $(DESTDIR)$(PREFIX)/bin/vdb

clean:
	rm -rf $(BUILDDIR) $(BINDIR)

-include $(CORE_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(BUILDDIR)/tools/*.d $(BUILDDIR)/examples/pc/*.d
