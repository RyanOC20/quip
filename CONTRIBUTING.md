Contributing to quip
===

quip stays small and fast, and needs nothing but a C99 compiler and a POSIX system, to build and to test. Please keep it that way: no libraries, no generated code, and no test tooling in another language.

Layout
---

| Path | What it holds |
|---|---|
| `src/quip.h` | Types, constants and functions shared between modules. Every `.c` file includes it first |
| `src/vt.h` | Terminal escape sequences and the append buffer a frame is drawn into |
| `src/term.c` | Raw mode, reading keys, window size, the resize signal |
| `src/buffer.c` | The rows of the document: editing primitives, opening and saving |
| `src/syntax.c` | The highlighting lexer |
| `src/lang.c` | Loading `.lang` files, and the built-in C |
| `src/render.c` | Drawing the screen: text, gutter, status bar |
| `src/input.c` | The key dispatcher, cursor motion, selection, mouse, editing commands |
| `src/find.c` | Incremental search |
| `src/undo.c` | Undo and redo |
| `src/clipboard.c` | Copy and paste, including the terminal clipboard (OSC 52) |
| `src/filetree.c` | The file tree sidebar |
| `src/main.c` | Startup and the main loop |
| `langs/` | Language definitions, see the README |
| `tests/` | The test harness and the end-to-end tests (`tests/e2e/`) |
| `bench/` | The benchmark |

A function used by one module only is `static`. Anything another module needs is declared in `quip.h`. A new module also goes in `src/all.c`: the release build compiles every module as one unit, so the compiler can optimize across them, and the build fails if one is missing.

Workflow
---

    make                # build ./quip
    make test           # end-to-end tests
    make test-asan      # the same under AddressSanitizer and UBSan
    make fuzz           # random sessions under the sanitizers
    make format         # clang-format the sources

* **Fixing a bug:** first write an end-to-end test in `tests/e2e/` that drives quip the way a user would and fails, then fix it. `make fuzz` prints any failure it finds as a ready-made test.
* **Refactoring:** behavior must not change. `make differential REF=master` replays random editing sessions against both builds and fails on any difference in what quip draws or saves.
* **Performance:** `make bench BENCH_REF=master` compares speed, memory and binary size with master. CI fails a change that is more than 10% slower, or whose stripped binary grows more than 15%, on both its Linux and its macOS runner. Locally, treat any figure over the limit as a regression.
* **Style:** `make format` before committing; CI runs `make check-format`. Warnings are errors in CI (`make WERROR=-Werror`), on Linux and macOS, with gcc and clang.

Keep commits to one change each, with a message that says what changed for the user and why.
