# Fixture project for scripts/mutate.py's self-tests. Nothing in the runtime
# build refers to it; it exists so the mutation runner can be exercised
# end-to-end against a real compiler on a project small enough that every
# verdict is predictable by hand.
CC ?= cc
CFLAGS ?= -O0 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -Iinclude
BUILD ?= build

SOURCES := src/range.c src/skips.c src/guard.c

.PHONY: build weak strong clean

$(BUILD)/weak: $(SOURCES) tests/weak_test.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/strong: $(SOURCES) tests/strong_test.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

build: $(BUILD)/weak $(BUILD)/strong

weak: $(BUILD)/weak
	$(BUILD)/weak

strong: $(BUILD)/strong
	$(BUILD)/strong

clean:
	rm -rf $(BUILD)
