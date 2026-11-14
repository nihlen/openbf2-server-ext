# QuickJS-NG (vendored)

The JavaScript engine of the admin channel's scripts (`src/Script.cpp`, `docs/admin-channel-draft.md` "Scripts").

| | |
|---|---|
| Version | v0.18.0 (released 2026-11-01) |
| Source | <https://github.com/quickjs-ng/quickjs>, tag `v0.18.0`: `https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.18.0.tar.gz` |
| Tarball sha256 | `28bcd89aa3683fd2a946537e5ce3f11cc46b5cbf3383c7561aaef3df5ff9f1dd` |
| License | MIT (`LICENSE`) |

The files are the tarball's, byte for byte (`.gitattributes` here turns line-ending conversion off), with one local
change: in `quickjs.c` the line `#define JS_INTERRUPT_COUNTER_INIT 10000` is wrapped in `#ifndef`/`#endif`, so the
build can set how often a script's time budget is checked (`CMakeLists.txt` sets 100; with 10000 a loop around a slow
built-in runs far past the budget, `docs/findings/scripts.md` "Limits of the limits"). Only what the library itself
needs is here:

- sources: `quickjs.c`, `dtoa.c`, `libregexp.c`, `libunicode.c`
- headers: `quickjs.h`, `quickjs-atom.h`, `quickjs-opcode.h`, `quickjs-c-atomics.h`, `cutils.h`, `dtoa.h`,
  `libregexp.h`, `libregexp-opcode.h`, `libunicode.h`, `libunicode-table.h`, `list.h`,
  `builtin-array-fromasync.h`, `builtin-iterator-zip.h`, `builtin-iterator-zip-keyed.h`
- `LICENSE`

Not here: `quickjs-libc.c` (the `std` and `os` modules; scripts get no file, process or network access), the `qjs`
and `qjsc` programs, the tests and the build files.

The module's `CMakeLists.txt` builds these four sources as the static library `serverext_qjs` with the definitions
and warning flags of QuickJS-NG's own `CMakeLists.txt` (C11 with GNU extensions, `_GNU_SOURCE`, `-funsigned-char`),
plus `NDEBUG` in every build type and hidden visibility.

## Updating

1. Download the new tag's tarball and check it against the release.
2. Replace the files above with the tarball's (check `quickjs.c`'s `#include "..."` lines for new headers), make the
   one local change again, and update this table.
3. Run the unit tests (`serverext_script_tests` covers what the script host relies on: the limits, the interrupt
   handler, what the language has and has not) and `tools/adminclient/adminclient.py --scenario script` against a
   test server.

Do not edit the files beyond that. A change the module needs belongs in `src/Script.cpp` or upstream.
