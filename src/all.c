/* The release build compiles every module as one translation unit, so the
 * compiler optimizes across modules as it did when quip was one file: split
 * into separately compiled files, bracket matching ran up to 20% slower on
 * x86. The debug and sanitizer builds compile the modules one by one, which
 * keeps each module honest about what it uses from the others. The Makefile
 * fails the build when a module is missing here. */
#include "buffer.c"
#include "clipboard.c"
#include "filetree.c"
#include "find.c"
#include "input.c"
#include "lang.c"
#include "main.c"
#include "render.c"
#include "syntax.c"
#include "term.c"
#include "undo.c"
