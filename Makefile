# quip needs only a C99 compiler and a POSIX system, and so do its tests.
# The usual variables work: make CC=gcc CFLAGS=-O2, make install PREFIX=/usr.

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
LANGDIR ?= $(PREFIX)/share/quip/langs

CFLAGS ?= -Os
# Warnings are part of the build, not of CFLAGS, so overriding CFLAGS keeps
# them. CI adds WERROR=-Werror.
WARN = -std=c99 -Wall -Wextra -pedantic $(WERROR)
# quip is a program, not a library: nothing outside it needs its symbols,
# and hidden ones are addressed directly instead of through the GOT.
CODEGEN = -fvisibility=hidden
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

# The version is "git describe" in a checkout (v0.1.0, v0.1.0-3-gabc1234,
# ...-dirty), else the fallback in src/quip.h. .build/version changes only when
# the version does, so a new commit rebuilds quip but a plain "make" does not.
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null)
VERSION_DEF = $(if $(VERSION),-DQUIP_VERSION='"$(VERSION)"')

# The modules; src/all.c includes them all for the release build.
SRC = $(filter-out src/all.c,$(wildcard src/*.c))
HDR = $(wildcard src/*.h)

all: quip

.build/version: FORCE
	@mkdir -p .build
	@if [ "$$(cat $@ 2>/dev/null)" != "$(VERSION)" ]; then echo "$(VERSION)" > $@; fi

# One translation unit, see src/all.c.
quip: src/all.c $(SRC) $(HDR) .build/version
	@for m in $(notdir $(SRC)); do \
	  grep -q "^#include \"$$m\"" src/all.c || { echo "src/all.c does not include $$m" >&2; exit 1; }; \
	done
	$(CC) $(WARN) $(CODEGEN) $(CPPFLAGS) $(VERSION_DEF) $(CFLAGS) $(LDFLAGS) -o $@ src/all.c $(LDLIBS)

# For a debugger: no optimization, full debug info. The modules are compiled
# separately here and in quip-asan.
quip-debug: $(SRC) $(HDR) .build/version
	$(CC) $(WARN) $(CODEGEN) $(CPPFLAGS) $(VERSION_DEF) -O0 -g $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

# AddressSanitizer and UndefinedBehaviorSanitizer.
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer
quip-asan: $(SRC) $(HDR) .build/version
	$(CC) $(WARN) $(CODEGEN) $(CPPFLAGS) $(VERSION_DEF) -O1 -g $(SAN_FLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

debug: quip-debug
asan: quip-asan

# quip finds its language files relative to its own binary, as
# $(BINDIR)/../share/quip/langs, so LANGDIR must stay beside BINDIR.
# The installed binary is stripped; INSTALL_STRIP= keeps its symbols.
INSTALL_STRIP ?= -s
install: quip
	install -d $(DESTDIR)$(BINDIR)
	install $(INSTALL_STRIP) -m 755 quip $(DESTDIR)$(BINDIR)/quip
	install -d $(DESTDIR)$(LANGDIR)
	install -m 644 langs/*.lang $(DESTDIR)$(LANGDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/quip
	rm -f $(addprefix $(DESTDIR)$(LANGDIR)/,$(notdir $(wildcard langs/*.lang)))
	-rmdir $(DESTDIR)$(LANGDIR) $(DESTDIR)$(PREFIX)/share/quip 2>/dev/null

# ------------------------------------------------------------------ Tests

TEST_CFLAGS = $(WARN) -g -O1 -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE
TEST_LIB = tests/test.c tests/pty.c tests/screen.c
TEST_HDR = tests/pty.h tests/screen.h tests/test.h tests/keys.h
KEYGEN = tests/keygen.c tests/keygen.h
E2E = $(wildcard tests/e2e/*.c)

tests/run: tests/runner.c $(KEYGEN) $(TEST_LIB) $(E2E) $(TEST_HDR)
	$(CC) $(TEST_CFLAGS) -o $@ tests/runner.c tests/keygen.c $(TEST_LIB) $(E2E)

tests/fuzz: tests/fuzz.c $(KEYGEN) $(TEST_LIB) $(TEST_HDR)
	$(CC) $(TEST_CFLAGS) -o $@ tests/fuzz.c tests/keygen.c $(TEST_LIB)

tests/differential: tests/differential.c $(KEYGEN) $(TEST_LIB) $(TEST_HDR)
	$(CC) $(TEST_CFLAGS) -o $@ tests/differential.c tests/keygen.c $(TEST_LIB)

bench/bench: bench/bench.c $(TEST_LIB) $(TEST_HDR)
	$(CC) $(TEST_CFLAGS) -O2 -o $@ bench/bench.c $(TEST_LIB)

# End-to-end tests; T picks tests by name: make test T=undo.
test: quip tests/run
	QUIP_BIN=./quip tests/run -j$(JOBS) $(T)

test-asan: quip-asan tests/run
	QUIP_BIN=./quip-asan QUIP_FRAME_TIMEOUT_MS=30000 tests/run -j$(JOBS) $(T)

# Random keystroke sessions against the sanitizer build, e.g.
# make fuzz FUZZ="-n 2000 -s 1000".
FUZZ ?= -n 300
fuzz: quip-asan tests/fuzz
	QUIP_BIN=./quip-asan QUIP_FRAME_TIMEOUT_MS=30000 tests/fuzz -j $(JOBS) $(FUZZ)

# Replay random sessions against quip built from git ref REF (in a git
# worktree at .build/ref) and the working tree; any difference in terminal
# output or saved files fails. Refactoring must pass this, for example
# make differential REF=master DIFF="-n 500".
REF ?= HEAD
DIFF ?= -n 100
differential: quip tests/differential
	git worktree remove --force .build/ref 2>/dev/null || true
	git worktree add --detach --quiet .build/ref $(REF)
	$(MAKE) -s -C .build/ref quip
	tests/differential .build/ref/quip ./quip $(DIFF); \
	status=$$?; git worktree remove --force .build/ref; exit $$status

# Measure the working tree build, or compare it with git ref BENCH_REF:
# make bench BENCH_REF=master. Fails on a slowdown over 10% or a stripped
# binary over 15% bigger.
bench: quip bench/bench
	@if [ -n "$(BENCH_REF)" ]; then \
	  git worktree remove --force .build/bench-ref 2>/dev/null; \
	  git worktree add --detach --quiet .build/bench-ref $(BENCH_REF) && \
	  $(MAKE) -s -C .build/bench-ref quip && \
	  bench/bench .build/bench-ref/quip ./quip $(BENCH); \
	  status=$$?; git worktree remove --force .build/bench-ref; exit $$status; \
	else bench/bench ./quip $(BENCH); fi

# ------------------------------------------------------------------ Style

CLANG_FORMAT ?= clang-format
FORMAT_SRC = $(SRC) src/all.c $(HDR) $(wildcard tests/*.c tests/*.h tests/e2e/*.c bench/*.c tools/*.c)

format:
	$(CLANG_FORMAT) -i $(FORMAT_SRC)

check-format:
	$(CLANG_FORMAT) --dry-run -Werror $(FORMAT_SRC)

clean:
	rm -f quip quip-debug quip-asan tests/run tests/fuzz tests/differential bench/bench
	rm -f .build/version
	rm -rf *.dSYM tests/*.dSYM bench/*.dSYM

.PHONY: all debug asan install uninstall test test-asan fuzz differential bench \
	format check-format clean FORCE
