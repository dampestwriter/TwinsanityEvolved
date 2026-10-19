"""rspfile.py -- response files for the build's long argument lists (the object and source lists).

The top-level CMake writes the wasm object list (~200 paths) and the game source list to files and passes `@<file>`
to the generators, link_wasm.py and gen_vtable_slots.py instead of the paths themselves: on the Windows lane Ninja
runs every custom command through cmd.exe, whose command line is capped at 8191 characters, and CreateProcess caps
the line a Python subprocess builds at 32767 -- a path list of this size does not fit either. The same files are used
on every host (one path through the build; the content of what is generated does not depend on how the list arrived).

  expand_argv(argv)        every `@path` argument -> the file's non-blank lines (one argument per line), in place
  tool_args(tool, args)    the argument list for an LLVM tool (llvm-nm, wasm-ld, wasm-objdump): when the joined line
                           is long, the arguments are written to `<tool-name>.<pid>.rsp` beside the first output-ish
                           path given (or the cwd) and `@rsp` is passed instead -- every LLVM tool expands @files
"""
import os
import sys


def expand_argv(argv):
    out = []
    for a in argv:
        if len(a) > 1 and a.startswith("@") and os.path.isfile(a[1:]):
            with open(a[1:], encoding="utf-8") as f:
                out.extend(line.strip() for line in f if line.strip())
        else:
            out.append(a)
    return out


def tool_args(args, rsp_path, limit=7000):
    """`args` (without the tool itself) -> the list to pass; a response file when the joined line exceeds `limit`.
    One double-quoted argument per line: LLVM's response-file tokenizer honours double quotes on every host; the
    paths carry neither quotes nor (on POSIX) backslashes, and the Windows-style tokenizer keeps a backslash literal."""
    if sum(len(a) + 1 for a in args) <= limit:
        return list(args)
    with open(rsp_path, "w", encoding="utf-8") as f:
        for a in args:
            f.write('"' + a + '"\n')
    return ["@" + rsp_path]
