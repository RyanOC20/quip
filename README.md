Quip
===

[![CI](https://github.com/RyanOC20/quip/actions/workflows/ci.yml/badge.svg)](https://github.com/RyanOC20/quip/actions/workflows/ci.yml)

Quip is a lightweight fork of the Kilo editor that maintains no reliance on libraries, but adds additional modern features.

Build & Install
---

Build (needs only a C99 compiler and a POSIX system):

    make

The usual variables apply, as in `make CC=gcc CFLAGS=-O2`. `make debug` builds `quip-debug` for a debugger, and `make asan` builds `quip-asan` with AddressSanitizer and UndefinedBehaviorSanitizer. `make install` strips the installed binary; `make install INSTALL_STRIP=` keeps its symbols.

Install for the current user (to `~/.local/bin`):

    make install

Install system-wide (all users):

    sudo make install PREFIX=/usr/local

Uninstall (use the same PREFIX you installed with):

    make uninstall

Usage: `quip <filename>`

Shortcuts:

| Shortcut | Action |
|---|---|
| <kbd>Ctrl</kbd>+<kbd>S</kbd> | Save |
| <kbd>Ctrl</kbd>+<kbd>Q</kbd> | Quit |
| <kbd>Ctrl</kbd>+<kbd>E</kbd> | Toggle the file tree sidebar (arrows/click to navigate, <kbd>Enter</kbd> to expand a folder or open a file, <kbd>Esc</kbd> to return to the editor) |
| <kbd>Ctrl</kbd>+<kbd>F</kbd> | Find string in file (<kbd>Esc</kbd> to exit search, arrows to navigate) |
| <kbd>Ctrl</kbd>+<kbd>C</kbd> | Copy to clipboard |
| <kbd>Ctrl</kbd>+<kbd>X</kbd> | Cut to clipboard |
| <kbd>Ctrl</kbd>+<kbd>V</kbd> | Paste from clipboard |
| <kbd>Ctrl</kbd>+<kbd>Z</kbd> | Undo |
| <kbd>Ctrl</kbd>+<kbd>Y</kbd> | Redo |
| <kbd>Ctrl</kbd>+<kbd>/</kbd> | Toggle inline comment |
| <kbd>Shift</kbd>+<kbd>Tab</kbd> | Remove one level of indentation from the current line |
| <kbd>Ctrl</kbd>+<kbd>↑</kbd> | Start of file |
| <kbd>Ctrl</kbd>+<kbd>↓</kbd> | End of file |
| <kbd>Ctrl</kbd>+<kbd>←</kbd> | Start of line |
| <kbd>Ctrl</kbd>+<kbd>→</kbd> | End of line |
| <kbd>Alt</kbd>/<kbd>Option</kbd>+<kbd>←</kbd> | Start of previous word |
| <kbd>Alt</kbd>/<kbd>Option</kbd>+<kbd>→</kbd> | End of next word |
| Double-click | Select word (drag to extend by whole words) |
| Triple-click | Select line (drag to extend by whole lines) |

Quip does not depend on any library (not even curses).

Languages
---

Syntax highlighting and <kbd>Ctrl</kbd>+<kbd>/</kbd> come from language files, one plain text `.lang` file per language. These ship in `langs/`:

| File | Language | Files |
|---|---|---|
| `c.lang` | C | `.c` |
| `cpp.lang` | C++, and C/C++ headers | `.cpp` `.cc` `.cxx` `.c++` `.h` `.hpp` `.hh` `.hxx` `.h++` `.ipp` `.tpp` |
| `java.lang` | Java | `.java` |
| `markdown.lang` | Markdown (comments only) | `.md` `.markdown` `.mdown` `.mkd` `.mkdn` |
| `ocaml.lang` | OCaml | `.ml` `.mli` |
| `python.lang` | Python | `.py` `.pyw` `.pyi` |
| `rust.lang` | Rust | `.rs` |
| `javascript.lang` | JavaScript, TypeScript | `.js` `.mjs` `.cjs` `.jsx` `.ts` `.mts` `.cts` `.tsx` |
| `go.lang` | Go | `.go` |
| `sh.lang` | Shell | `.sh` `.bash` `.zsh`, plus `.bashrc` `.bash_profile` `.zshrc` `.zprofile` `.profile` |

`make install` copies them next to the binary, to `$PREFIX/share/quip/langs`. C and C++ are also built into quip, so a lone `quip` binary still highlights them. When a language has no line comments (OCaml, Markdown), <kbd>Ctrl</kbd>+<kbd>/</kbd> wraps the line in its block comment, such as `(* ... *)` or `<!-- ... -->`, and unwraps it when pressed again.

To add a language or change a shipped one, put a `.lang` file in `~/.config/quip/langs/`. No rebuild is needed. A file there replaces a shipped file with the same name, so your own `python.lang` replaces the shipped one and your own `c.lang` replaces the built-in C and C++. Quip looks in these folders, in order:

1. `~/.config/quip/langs/` (or `$XDG_CONFIG_HOME/quip/langs/`)
2. `$QUIP_LANGDIR`, if set
3. `langs/` next to the quip binary, so `./quip` works straight from a checkout
4. `../share/quip/langs/` next to the quip binary, where `make install` puts them
5. `~/.local/share/quip/langs/`, then `quip/langs/` in each `$XDG_DATA_DIRS` folder (`/usr/local/share`, `/usr/share`)

A language file looks like this:

    quip-lang 1
    # Lines starting with # are comments.
    extensions    .py .pyw
    filenames     SConstruct
    line_comment  #
    block_comment /* */
    features      strings numbers
    keywords      def class if else return
                  while for in
    types         int float str

* `quip-lang 1` must come first.
* `extensions` or `filenames` is required. `filenames` matches the whole file name, like `Makefile`.
* `line_comment` and `block_comment` are optional, and can be any length.
* `types` are colored differently from `keywords`.
* An indented line continues the line above.
* `features` switches on parts of quip's highlighter:
  * `strings`: `"..."` and `'...'`
  * `numbers`: literals like `0x1F`, `3.14` and `1e-5`
  * `nested_comments`: block comments nest
  * `multiline_strings`: strings may span lines
  * `quoted_strings`: OCaml `{id|...|id}` strings
  * `prime_quotes`: OCaml and Rust `'`, read as a char literal when one fits, else a type variable or lifetime (`'a`)
  * `triple_quotes`: `"""..."""` and `'''...'''` strings that may span lines, such as Python docstrings and Java text blocks
  * `word_start_comments`: `line_comment` starts a comment only at the start of a word, as `#` does in shell (`${#arr}` and `x#y` are not comments). Without it, a line comment starts anywhere outside a string.

If a file has a mistake, quip skips it and names the file and line on the status bar at startup.

Testing
---

The tests are C and POSIX only, like quip. They drive the real binary through a pseudo terminal and check what it draws, cell by cell, using a small terminal model (`tests/screen.c`) that fails on any escape sequence it does not know.

| Command | What it does |
|---|---|
| `make test` | End-to-end tests (`tests/e2e/`). Pick tests with `T`, as in `make test T=undo` |
| `make test-asan` | The same tests against a build with AddressSanitizer and UndefinedBehaviorSanitizer |
| `make fuzz` | Random editing sessions against the sanitizer build. Each failure is shrunk to the fewest keys and printed as a test to paste into `tests/e2e/`. Options go in `FUZZ`, as in `make fuzz FUZZ="-n 2000 -s 5000"` |
| `make differential` | Replays random sessions against quip built from git ref `REF` (default `HEAD`) and the working tree, and fails on any difference in output or saved files. Refactoring must pass it |
| `make bench` | Times loading, scrolling, typing and highlighting on large files, plus memory and binary size. `make bench BENCH_REF=master` compares against a git ref and fails on a slowdown |
| `make format` | Formats the C sources with clang-format (`.clang-format`). `make check-format` only checks |

CI runs the tests, the sanitizer tests and a short fuzz run on Linux and macOS with gcc and clang, with warnings as errors. It also benchmarks each change against its base and against quip before the 2026 rework, and checks the formatting.

Kilo was written by Salvatore Sanfilippo aka antirez and is released
under the BSD 2 clause license.
