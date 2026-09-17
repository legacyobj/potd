CC = cc
PYTHON ?= python3
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -fstack-protector-strong
LDFLAGS ?=
LDLIBS += -lexpat
BUILD ?= build

ifeq ($(shell uname -s),Linux)
CPPFLAGS += -D_FORTIFY_SOURCE=2
CFLAGS += -fPIE
LDFLAGS += -pie -Wl,-z,relro,-z,now
endif

CORE = src/parser.c src/protocol.c src/pot.c src/additions.c src/propfind.c src/buffer.c
HEADERS = $(wildcard src/*.h)
OBJECTS = $(patsubst src/%.c,$(BUILD)/%.o,$(CORE) src/server.c)

.PHONY: all test sanitize fuzz fuzz-smoke clean
all: $(BUILD)/potd

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/potd: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/unit: tests/unit.c $(CORE) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/unit.c $(CORE) $(LDLIBS) -o $@

test: $(BUILD)/potd $(BUILD)/unit
	$(BUILD)/unit
	$(PYTHON) tests/integration.py $(BUILD)/potd

sanitize:
	$(MAKE) BUILD=build/sanitize CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined' test

fuzz: | $(BUILD)
	clang $(CPPFLAGS) -O1 -g -std=c11 -fsanitize=fuzzer,address,undefined tests/fuzz.c $(CORE) $(LDLIBS) -o $(BUILD)/fuzz

$(BUILD)/fuzz-smoke: tests/fuzz.c tests/fuzz_smoke.c $(CORE) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) -O1 -g -std=c11 -Wall -Wextra -Wpedantic -fsanitize=address,undefined tests/fuzz_smoke.c tests/fuzz.c $(CORE) $(LDLIBS) -o $@

fuzz-smoke: $(BUILD)/fuzz-smoke
	$(BUILD)/fuzz-smoke

clean:
	rm -rf build

-include $(OBJECTS:.o=.d)
