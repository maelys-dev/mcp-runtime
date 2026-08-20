CC ?= cc
AR ?= ar
PKG_CONFIG ?= pkg-config
ANALYZER ?= clang
PREFIX ?= /usr/local
VERSION := $(shell cat VERSION)
DOCKER ?= docker
DOCKER_PLATFORM ?= linux/arm64
ASAN_LINUX_IMAGE ?= maelys-mcp-runtime-asan:ubuntu24.04
FUZZ_CC ?= clang
MCP_CONFORMANCE_PACKAGE ?= @modelcontextprotocol/conformance@0.2.0-alpha.11

# Dependency flags are overridable so a release can link jansson/uriparser
# statically (pass the .a paths in MCP_DEP_LIBS) without touching the build.
MCP_DEP_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags jansson liburiparser)
MCP_DEP_LIBS ?= $(shell $(PKG_CONFIG) --libs jansson liburiparser)
CPPFLAGS += -Iinclude -I. -DMAELYS_MCP_VERSION='"$(VERSION)"' $(MCP_DEP_CFLAGS)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200809L -pthread
LDLIBS += $(MCP_DEP_LIBS) -pthread

BUILD_ROOT ?= build
BUILD_PROFILE ?= release
BUILD ?= $(BUILD_ROOT)/$(BUILD_PROFILE)
OBJ := $(BUILD)/obj
BIN := $(BUILD)/bin
LIB := $(BUILD)/lib
PC := $(LIB)/pkgconfig/maelys-mcp.pc

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
ASAN_OPTIONS_VALUE := halt_on_error=1
NO_THREAD_WRAP :=
else
ASAN_OPTIONS_VALUE := detect_leaks=1:halt_on_error=1
NO_THREAD_WRAP := -Wl,--wrap=pthread_create
endif

NO_THREAD_BUILD := $(BUILD_ROOT)/no-thread
NO_THREAD_BIN := $(NO_THREAD_BUILD)/test-channel-no-thread
NO_THREAD_CFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror \
	-D_POSIX_C_SOURCE=200809L -pthread
NO_THREAD_LDLIBS := $(MCP_DEP_LIBS) -pthread

LIB_SOURCES := \
	src/jsonrpc/core.c \
	src/core/common.c \
	src/core/channel.c \
	src/core/nested.c \
	src/core/content.c \
	src/core/outbox.c \
	src/core/uri.c \
	src/core/resources.c \
	src/core/schema.c \
	src/core/runtime.c \
	src/core/middleware.c \
	src/modules/registry.c \
	src/modules/mrtr.c \
	src/modules/tools.c \
	src/modules/resources.c \
	src/modules/subscriptions.c \
	src/provider/provider.c \
	src/provider/process_provider.c \
	src/provider/mcp_proxy.c \
	src/provider/provider_sdk.c \
	src/transport/stdio.c \
	src/transport/stdio_isolation.c
LIB_OBJECTS := $(LIB_SOURCES:%.c=$(OBJ)/%.o)
DEPENDENCY_SOURCES := $(LIB_SOURCES) host/main.c host/manifest.c providers/example/main.c \
	$(wildcard tests/*.c) tests/helpers/adversarial_provider.c
DEPENDENCY_FILES := $(DEPENDENCY_SOURCES:%.c=$(OBJ)/%.d)

-include $(DEPENDENCY_FILES)

.PHONY: all clean test test-build check check-version-header check-all check-sdks test-conformance test-provider-conformance test-sdk-nested test-mcp-conformance-official asan tsan tsan-run analyze audit install fuzz-build fuzz-smoke \
	asan-linux-image test-asan-linux test-asan-linux-fresh test-channel-no-thread-nosanitize

all: $(LIB)/libmaelys_mcp.a $(BIN)/maelys-mcp $(BIN)/example-provider $(PC)

$(PC): pkgconfig/maelys-mcp.pc.in
	@mkdir -p $(@D)
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' $< >$@

$(LIB)/libmaelys_mcp.a: $(LIB_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BIN)/maelys-mcp: $(OBJ)/host/main.o $(OBJ)/host/manifest.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/example-provider: $(OBJ)/providers/example/main.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/adversarial-provider: $(OBJ)/tests/helpers/adversarial_provider.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ -o $@

$(BIN)/bad-json-provider $(BIN)/bad-envelope-provider $(BIN)/bad-schema-provider $(BIN)/oversized-provider $(BIN)/environment-provider $(BIN)/slow-describe-provider $(BIN)/fd-check-provider $(BIN)/stubborn-provider $(BIN)/legacy-mcp-upstream $(BIN)/erroring-mcp-upstream $(BIN)/chatty-mcp-upstream $(BIN)/dying-mcp-upstream $(BIN)/exotic-schema-mcp-upstream $(BIN)/nested-provider $(BIN)/nested-double-provider $(BIN)/nested-dying-provider $(BIN)/legacy4-provider: $(BIN)/adversarial-provider
	ln -sf $(notdir $<) $@

$(BIN)/test-runtime: $(OBJ)/tests/test_runtime.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-middleware: $(OBJ)/tests/test_middleware.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-runtime-protocol: $(OBJ)/tests/test_runtime_protocol.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-process-provider: $(OBJ)/tests/test_process_provider.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-mcp-proxy: $(OBJ)/tests/test_mcp_proxy.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

# host/manifest.o is host-level, not part of libmaelys_mcp.a (see
# docs/manifest.md: the public library surface does not grow for it), so the
# test binary links the object directly - the same seam the host binary uses.
$(BIN)/test-manifest: $(OBJ)/tests/test_manifest.o $(OBJ)/host/manifest.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-provider-sdk: $(OBJ)/tests/test_provider_sdk.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-jsonrpc-core: $(OBJ)/tests/test_jsonrpc_core.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-schema: $(OBJ)/tests/test_schema.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-stdio-isolation: $(OBJ)/tests/test_stdio_isolation.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-modules-content-mrtr: $(OBJ)/tests/test_modules_content_mrtr.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-resources: $(OBJ)/tests/test_resources.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-outbox: $(OBJ)/tests/test_outbox.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-subscriptions: $(OBJ)/tests/test_subscriptions.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-nested-requests: $(OBJ)/tests/test_nested_requests.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-channels: $(OBJ)/tests/test_channels.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-channel-perf: $(OBJ)/tests/test_channel_perf.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/test-provider-perf: $(OBJ)/tests/test_provider_perf.o $(LIB)/libmaelys_mcp.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(NO_THREAD_BIN): tests/test_channel_no_thread.c $(LIB_SOURCES)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(NO_THREAD_CFLAGS) $^ $(NO_THREAD_LDLIBS) \
		$(NO_THREAD_WRAP) -o $@

test-channel-no-thread-nosanitize: $(NO_THREAD_BIN)
	$(NO_THREAD_BIN)

NESTED_FIXTURES := $(BIN)/nested-provider $(BIN)/nested-double-provider \
	$(BIN)/nested-dying-provider $(BIN)/legacy4-provider
NESTED_FIXTURE_ARGS = $(abspath $(BIN)/nested-provider) \
	$(abspath $(BIN)/nested-double-provider) \
	$(abspath $(BIN)/nested-dying-provider) \
	$(abspath $(BIN)/legacy4-provider)

TEST_ARTIFACTS := $(BIN)/test-nested-requests $(NESTED_FIXTURES) $(BIN)/test-middleware $(BIN)/test-runtime $(BIN)/test-runtime-protocol $(BIN)/test-process-provider $(BIN)/test-mcp-proxy $(BIN)/test-provider-sdk $(BIN)/test-jsonrpc-core $(BIN)/test-schema $(BIN)/test-stdio-isolation $(BIN)/test-modules-content-mrtr $(BIN)/test-resources $(BIN)/test-outbox $(BIN)/test-subscriptions $(BIN)/test-channels $(BIN)/test-channel-perf $(BIN)/test-provider-perf $(BIN)/test-manifest $(BIN)/bad-json-provider $(BIN)/bad-envelope-provider $(BIN)/bad-schema-provider $(BIN)/oversized-provider $(BIN)/environment-provider $(BIN)/slow-describe-provider $(BIN)/fd-check-provider $(BIN)/stubborn-provider $(BIN)/legacy-mcp-upstream $(BIN)/erroring-mcp-upstream $(BIN)/chatty-mcp-upstream $(BIN)/dying-mcp-upstream $(BIN)/exotic-schema-mcp-upstream

# Compile everything `test` runs, without running any of it. The split exists
# for scripts/mutate.py: a mutant that fails to compile is *stillborn*, not
# killed, and folding the two together would report a broken mutation as
# covered test behaviour. A separate build step is the only way to tell them
# apart without pattern-matching compiler output.
test-build: all $(TEST_ARTIFACTS)

test: test-build
	$(BIN)/test-jsonrpc-core
	$(BIN)/test-schema
	$(BIN)/test-stdio-isolation
	$(BIN)/test-modules-content-mrtr
	$(BIN)/test-resources
	$(BIN)/test-outbox
	$(BIN)/test-subscriptions
	$(BIN)/test-channels
	$(BIN)/test-nested-requests $(NESTED_FIXTURE_ARGS)
	$(BIN)/test-channel-perf
	$(BIN)/test-middleware
	$(BIN)/test-runtime
	$(BIN)/test-runtime-protocol
	$(BIN)/test-provider-sdk
	$(BIN)/test-provider-perf $(abspath $(BIN)/example-provider)
	$(BIN)/test-process-provider $(abspath $(BIN)/example-provider) \
		$(abspath $(BIN)/bad-json-provider) $(abspath $(BIN)/bad-envelope-provider) \
		$(abspath $(BIN)/bad-schema-provider) $(abspath $(BIN)/oversized-provider) \
		$(abspath $(BIN)/environment-provider) $(abspath $(BIN)/slow-describe-provider) \
		$(abspath $(BIN)/fd-check-provider) $(abspath $(BIN)/stubborn-provider)
	$(BIN)/test-mcp-proxy $(abspath $(BIN)/maelys-mcp) $(abspath $(BIN)/example-provider) \
		$(abspath $(BIN)/legacy-mcp-upstream) $(abspath $(BIN)/erroring-mcp-upstream) \
		$(abspath $(BIN)/chatty-mcp-upstream) $(abspath $(BIN)/dying-mcp-upstream) \
		$(abspath $(BIN)/exotic-schema-mcp-upstream)
	$(BIN)/test-manifest
	scripts/test_stdio.sh $(abspath $(BIN)/maelys-mcp) $(abspath $(BIN)/example-provider)

audit:
	scripts/audit_boundaries.sh

analyze:
	@set -e; for source in $(LIB_SOURCES) host/main.c providers/example/main.c; do \
		echo "analyze $$source"; \
		$(ANALYZER) --analyze -o /dev/null $(CPPFLAGS) $(CFLAGS) $$source; \
	done

check-version-header:
	bash scripts/generate-version-header.sh --check

check: check-version-header test audit

check-sdks:
	node --test sdk/typescript/test/*.test.js
	PYTHONPATH=sdk/python/src python3 -m unittest discover -s sdk/python/tests -v
	python3 -m unittest tests/test_provider_conformance.py -v
	python3 -m unittest tests/test_mutate.py -v

# The Python SDK's nested round trip against the real host binary, over real
# stdio. Deliberately not folded into check-sdks: that target needs no C
# toolchain, and this is the one SDK check that cannot run without a built
# runtime. Naming the binary also turns the suite's skip into a hard failure,
# so a gate that ran nothing cannot report success.
test-sdk-nested: all
	MAELYS_MCP_BINARY=$(abspath $(BIN)/maelys-mcp) PYTHONPATH=sdk/python/src \
		python3 -m unittest discover -s sdk/python/tests \
		-p test_nested_end_to_end.py -v

test-provider-conformance: all
	python3 conformance/provider_conformance.py $(abspath $(BIN)/example-provider) \
		--cases conformance/example-cases.json
	python3 conformance/provider_conformance.py \
		$(abspath sdk/typescript/test/fixture-provider.js) --cases conformance/sdk-cases.json
	PYTHONPATH=sdk/python/src python3 conformance/provider_conformance.py \
		$(abspath sdk/python/tests/fixture_provider.py) --cases conformance/sdk-cases.json

test-conformance: test-provider-conformance

test-mcp-conformance-official: all
	PYTHONPATH=sdk/python/src python3 conformance/run_official_mcp.py \
		--runtime $(abspath $(BIN)/maelys-mcp) \
		--provider $(abspath conformance/official_tools_provider.py) \
		--package '$(MCP_CONFORMANCE_PACKAGE)'

check-all: check check-sdks test-provider-conformance test-sdk-nested

install: all
	install -d "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/lib/pkgconfig" \
		"$(DESTDIR)$(PREFIX)/include"
	install -m 755 $(BIN)/maelys-mcp "$(DESTDIR)$(PREFIX)/bin/maelys-mcp"
	install -m 644 $(LIB)/libmaelys_mcp.a "$(DESTDIR)$(PREFIX)/lib/libmaelys_mcp.a"
	install -m 644 $(PC) "$(DESTDIR)$(PREFIX)/lib/pkgconfig/maelys-mcp.pc"
	cp -R include/maelys "$(DESTDIR)$(PREFIX)/include/"

asan:
	ASAN_OPTIONS=$(ASAN_OPTIONS_VALUE) UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) BUILD_PROFILE=asan check CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200809L -pthread -fsanitize=address,undefined -fno-omit-frame-pointer" LDLIBS="$(shell $(PKG_CONFIG) --libs jansson liburiparser) -pthread -fsanitize=address,undefined"

tsan:
	TSAN_OPTIONS=halt_on_error=1 \
	$(MAKE) BUILD_PROFILE=tsan tsan-run CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200809L -pthread -fsanitize=thread -fno-omit-frame-pointer" LDLIBS="$(shell $(PKG_CONFIG) --libs jansson liburiparser) -pthread -fsanitize=thread"

tsan-run: $(BIN)/test-outbox $(BIN)/test-subscriptions $(BIN)/test-channels $(BIN)/test-channel-perf $(BIN)/test-middleware $(BIN)/test-process-provider $(BIN)/test-mcp-proxy $(BIN)/test-provider-sdk \
		$(BIN)/test-nested-requests $(NESTED_FIXTURES) \
		$(BIN)/example-provider $(BIN)/bad-json-provider $(BIN)/bad-envelope-provider \
		$(BIN)/bad-schema-provider $(BIN)/oversized-provider $(BIN)/environment-provider \
		$(BIN)/slow-describe-provider $(BIN)/fd-check-provider $(BIN)/stubborn-provider \
		$(BIN)/maelys-mcp $(BIN)/legacy-mcp-upstream $(BIN)/erroring-mcp-upstream $(BIN)/chatty-mcp-upstream $(BIN)/dying-mcp-upstream \
		$(BIN)/exotic-schema-mcp-upstream
	TSAN_OPTIONS=halt_on_error=1 $(BIN)/test-outbox
	TSAN_OPTIONS=halt_on_error=1 $(BIN)/test-subscriptions
	TSAN_OPTIONS=halt_on_error=1 $(BIN)/test-channels
	# The reason this suite exists under tsan: it is the only place two
	# tools/call dispatches run at once on ONE channel, and the only place a
	# provider thread, a transport reader and a worker touch one channel's
	# nested table together.
	TSAN_OPTIONS=halt_on_error=1 $(BIN)/test-nested-requests $(NESTED_FIXTURE_ARGS)
	TSAN_OPTIONS=halt_on_error=1 $(BIN)/test-channel-perf
	# The middleware suite is here for hook 6: it decorates the delivery path
	# every request's output travels, and its concurrency case drives two
	# channels through one wrap_sink from two threads.
	TSAN_OPTIONS=halt_on_error=1 $(BIN)/test-middleware
	TSAN_OPTIONS=halt_on_error=1 $(BIN)/test-provider-sdk
	TSAN_OPTIONS=halt_on_error=1 $(BIN)/test-process-provider $(abspath $(BIN)/example-provider) \
		$(abspath $(BIN)/bad-json-provider) $(abspath $(BIN)/bad-envelope-provider) \
		$(abspath $(BIN)/bad-schema-provider) $(abspath $(BIN)/oversized-provider) \
		$(abspath $(BIN)/environment-provider) $(abspath $(BIN)/slow-describe-provider) \
		$(abspath $(BIN)/fd-check-provider) $(abspath $(BIN)/stubborn-provider)
	TSAN_OPTIONS=halt_on_error=1 $(BIN)/test-mcp-proxy $(abspath $(BIN)/maelys-mcp) $(abspath $(BIN)/example-provider) \
		$(abspath $(BIN)/legacy-mcp-upstream) $(abspath $(BIN)/erroring-mcp-upstream) \
		$(abspath $(BIN)/chatty-mcp-upstream) $(abspath $(BIN)/dying-mcp-upstream) \
		$(abspath $(BIN)/exotic-schema-mcp-upstream)

FUZZ_CFLAGS := -O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror \
	-D_POSIX_C_SOURCE=200809L -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
FUZZ_BUILD := $(BUILD_ROOT)/fuzz
FUZZ_BIN := $(FUZZ_BUILD)/bin
FUZZ_CORPUS := $(FUZZ_BUILD)/corpus
FUZZ_BINARIES := $(FUZZ_BIN)/json-lines $(FUZZ_BIN)/content-length $(FUZZ_BIN)/schema $(FUZZ_BIN)/content $(FUZZ_BIN)/uri

$(FUZZ_BIN)/json-lines: fuzz/fuzz_json_lines.c $(LIB_SOURCES)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $^ $(LDLIBS) -o $@

$(FUZZ_BIN)/content-length: fuzz/fuzz_content_length.c $(LIB_SOURCES)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $^ $(LDLIBS) -o $@

$(FUZZ_BIN)/schema: fuzz/fuzz_schema.c $(LIB_SOURCES)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $^ $(LDLIBS) -o $@

$(FUZZ_BIN)/content: fuzz/fuzz_content.c $(LIB_SOURCES)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $^ $(LDLIBS) -o $@

$(FUZZ_BIN)/uri: fuzz/fuzz_uri.c $(LIB_SOURCES)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $^ $(LDLIBS) -o $@

fuzz-build: $(FUZZ_BINARIES)

fuzz-smoke: fuzz-build
	rm -rf $(FUZZ_CORPUS)
	mkdir -p $(FUZZ_CORPUS)/json-lines $(FUZZ_CORPUS)/content-length $(FUZZ_CORPUS)/schema $(FUZZ_CORPUS)/content $(FUZZ_CORPUS)/uri
	cp fuzz/seeds/json-lines/* $(FUZZ_CORPUS)/json-lines/
	printf 'Content-Length: 2\r\n\r\n{}' >$(FUZZ_CORPUS)/content-length/frame
	cp fuzz/seeds/schema/* $(FUZZ_CORPUS)/schema/
	cp fuzz/seeds/content/* $(FUZZ_CORPUS)/content/
	cp fuzz/seeds/uri/* $(FUZZ_CORPUS)/uri/
	$(FUZZ_BIN)/json-lines -runs=2000 -max_len=8192 $(FUZZ_CORPUS)/json-lines
	$(FUZZ_BIN)/content-length -runs=2000 -max_len=8192 $(FUZZ_CORPUS)/content-length
	$(FUZZ_BIN)/schema -runs=2000 -max_len=8192 $(FUZZ_CORPUS)/schema
	$(FUZZ_BIN)/content -runs=2000 -max_len=8192 $(FUZZ_CORPUS)/content
	$(FUZZ_BIN)/uri -runs=2000 -max_len=8192 $(FUZZ_CORPUS)/uri

asan-linux-image:
	$(DOCKER) build --platform $(DOCKER_PLATFORM) \
		-t $(ASAN_LINUX_IMAGE) tools/docker/asan-linux

test-asan-linux: asan-linux-image
	$(DOCKER) run --rm --platform $(DOCKER_PLATFORM) \
		-v "$(CURDIR):/source:ro" $(ASAN_LINUX_IMAGE) \
		bash -lc 'cp -a /source /tmp/mcp-runtime && cd /tmp/mcp-runtime && make clean && make asan CC=clang && make fuzz-smoke FUZZ_CC=clang'

test-asan-linux-fresh:
	$(DOCKER) build --no-cache --pull --platform $(DOCKER_PLATFORM) \
		-t $(ASAN_LINUX_IMAGE) tools/docker/asan-linux
	$(MAKE) test-asan-linux

clean:
	rm -rf $(BUILD_ROOT)
