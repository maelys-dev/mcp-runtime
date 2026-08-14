CC ?= cc
AR ?= ar
PKG_CONFIG ?= pkg-config
ANALYZER ?= clang
PREFIX ?= /usr/local
VERSION := 0.1.0

CPPFLAGS += -Iinclude -I. $(shell $(PKG_CONFIG) --cflags jansson)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200809L
LDLIBS += $(shell $(PKG_CONFIG) --libs jansson)

BUILD := build
OBJ := $(BUILD)/obj
BIN := $(BUILD)/bin
LIB := $(BUILD)/lib
PC := $(LIB)/pkgconfig/maelys-mcp.pc

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
ASAN_OPTIONS_VALUE := halt_on_error=1
else
ASAN_OPTIONS_VALUE := detect_leaks=1:halt_on_error=1
endif

LIB_SOURCES := \
	src/jsonrpc/core.c \
	src/core/common.c \
	src/core/schema.c \
	src/core/runtime.c \
	src/provider/provider.c \
	src/provider/process_provider.c \
	src/transport/stdio.c
LIB_OBJECTS := $(LIB_SOURCES:%.c=$(OBJ)/%.o)

.PHONY: all clean test check asan analyze audit install

all: $(LIB)/libmaelys_mcp.a $(BIN)/maelys-mcp $(BIN)/example-provider $(PC)

$(PC): pkgconfig/maelys-mcp.pc.in
	@mkdir -p $(@D)
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' $< >$@

$(LIB)/libmaelys_mcp.a: $(LIB_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BIN)/maelys-mcp: $(OBJ)/host/main.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/example-provider: $(OBJ)/providers/example/main.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-runtime: $(OBJ)/tests/test_runtime.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-process-provider: $(OBJ)/tests/test_process_provider.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-jsonrpc-core: $(OBJ)/tests/test_jsonrpc_core.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test: all $(BIN)/test-runtime $(BIN)/test-process-provider $(BIN)/test-jsonrpc-core
	$(BIN)/test-jsonrpc-core
	$(BIN)/test-runtime
	$(BIN)/test-process-provider $(abspath $(BIN)/example-provider)
	scripts/test_stdio.sh $(abspath $(BIN)/maelys-mcp) $(abspath $(BIN)/example-provider)

audit:
	scripts/audit_boundaries.sh

analyze:
	@set -e; for source in $(LIB_SOURCES) host/main.c providers/example/main.c; do \
		echo "analyze $$source"; \
		$(ANALYZER) --analyze -o /dev/null $(CPPFLAGS) $(CFLAGS) $$source; \
	done

check: test audit

install: all
	install -d "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/lib/pkgconfig" \
		"$(DESTDIR)$(PREFIX)/include"
	install -m 755 $(BIN)/maelys-mcp "$(DESTDIR)$(PREFIX)/bin/maelys-mcp"
	install -m 644 $(LIB)/libmaelys_mcp.a "$(DESTDIR)$(PREFIX)/lib/libmaelys_mcp.a"
	install -m 644 $(PC) "$(DESTDIR)$(PREFIX)/lib/pkgconfig/maelys-mcp.pc"
	cp -R include/maelys "$(DESTDIR)$(PREFIX)/include/"

asan:
	$(MAKE) clean
	ASAN_OPTIONS=$(ASAN_OPTIONS_VALUE) UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) check CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -fno-omit-frame-pointer" LDLIBS="$(shell $(PKG_CONFIG) --libs jansson) -fsanitize=address,undefined"

clean:
	rm -rf $(BUILD)
