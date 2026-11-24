PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
LANGDIR ?= $(PREFIX)/share/quip/langs

all: quip

quip: quip.c
	$(CC) -Os -o quip quip.c -Wall -W -pedantic -std=c99
	strip quip

# quip finds its language files relative to its own binary, as
# $(BINDIR)/../share/quip/langs, so LANGDIR must stay beside BINDIR.
install: quip
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 quip $(DESTDIR)$(BINDIR)/quip
	install -d $(DESTDIR)$(LANGDIR)
	install -m 644 langs/*.lang $(DESTDIR)$(LANGDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/quip
	rm -f $(addprefix $(DESTDIR)$(LANGDIR)/,$(notdir $(wildcard langs/*.lang)))
	-rmdir $(DESTDIR)$(LANGDIR) $(DESTDIR)$(PREFIX)/share/quip 2>/dev/null

# ------------------------------------------------------------------ Tests
# Everything below is C and POSIX only, like quip itself.

JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
TEST_CFLAGS = -std=c99 -Wall -Wextra -pedantic -g -O1 \
	-D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer
TEST_LIB = tests/test.c tests/pty.c tests/screen.c
TEST_HDR = tests/pty.h tests/screen.h tests/test.h tests/keys.h
E2E = $(wildcard tests/e2e/*.c)

# quip built with AddressSanitizer and UndefinedBehaviorSanitizer.
quip-asan: quip.c
	$(CC) -std=c99 -Wall -W -pedantic -O1 -g $(SAN_FLAGS) -o $@ quip.c

tests/run: tests/runner.c $(TEST_LIB) $(E2E) $(TEST_HDR)
	$(CC) $(TEST_CFLAGS) -o $@ tests/runner.c $(TEST_LIB) $(E2E)

tests/fuzz: tests/fuzz.c tests/keygen.c tests/keygen.h $(TEST_LIB) $(TEST_HDR)
	$(CC) $(TEST_CFLAGS) -o $@ tests/fuzz.c tests/keygen.c $(TEST_LIB)

# Random keystroke sessions against the sanitizer build, e.g.
# make fuzz FUZZ="-n 2000 -s 1000".
FUZZ ?= -n 300
fuzz: quip-asan tests/fuzz
	QUIP_BIN=./quip-asan QUIP_FRAME_TIMEOUT_MS=30000 tests/fuzz -j $(JOBS) $(FUZZ)

tests/differential: tests/differential.c tests/keygen.c tests/keygen.h $(TEST_LIB) $(TEST_HDR)
	$(CC) $(TEST_CFLAGS) -o $@ tests/differential.c tests/keygen.c $(TEST_LIB)

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

bench/bench: bench/bench.c $(TEST_LIB) $(TEST_HDR)
	$(CC) $(TEST_CFLAGS) -O2 -o $@ bench/bench.c $(TEST_LIB)

# Measure the working tree build, or compare it with git ref BENCH_REF:
# make bench BENCH_REF=master. Fails on a slowdown over 10% or a binary
# over 15% bigger.
bench: quip bench/bench
	@if [ -n "$(BENCH_REF)" ]; then \
	  git worktree remove --force .build/bench-ref 2>/dev/null; \
	  git worktree add --detach --quiet .build/bench-ref $(BENCH_REF) && \
	  $(MAKE) -s -C .build/bench-ref quip && \
	  bench/bench .build/bench-ref/quip ./quip $(BENCH); \
	  status=$$?; git worktree remove --force .build/bench-ref; exit $$status; \
	else bench/bench ./quip $(BENCH); fi

test: quip tests/run
	QUIP_BIN=./quip tests/run -j$(JOBS) $(T)

test-asan: quip-asan tests/run
	QUIP_BIN=./quip-asan QUIP_FRAME_TIMEOUT_MS=30000 tests/run -j$(JOBS) $(T)

clean:
	rm -f quip quip-asan tests/run tests/fuzz tests/differential bench/bench
	rm -rf *.dSYM tests/*.dSYM

.PHONY: all install uninstall clean test test-asan fuzz differential bench
