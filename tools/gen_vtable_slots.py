#!/usr/bin/env python3
"""gen_vtable_slots.py -- the declared signature of every GCC 2.9x vtable slot the decomp calls, from the decomp itself.

wasm type-checks every indirect call: a `call_indirect` traps unless the table entry's signature equals the caller's.
The decomp calls the retail vtables through `CallVirtual<Result, Args...>(object, vtable, slot, args...)` (include/gcc2.h),
so the CALLER's signature of a slot is the template instantiation; the body the retail vtable holds may be declared
with fewer parameters (`void SoundTableNothing3()` in a slot called as `void(ResourceTable*, u32)`) -- harmless on MIPS,
a trap on wasm. This tool recovers, per retail vtable symbol, the wasm signature each slot is
called with:

  1. every TU is preprocessed and rewritten exactly as tools/te_cxx.py compiles it (zero edits to game code) and parsed
     with libclang (the pinned wheel in tools/toolchain/libclang, same clang major as the wasi-sdk);
  2. `CallVirtual<R, A...>(obj, vt, SLOT, ...)` sites give (class of obj, slot, signature);
  3. `X->vtable = g_V` (any member of type `const GccVTableEntry*`), constructor initialisers and aggregate initialisers
     give (class of X, vtable symbol); a vtable that only flows through a `const GccVTableEntry*` PARAMETER is followed
     through the calls (`MakeTable(g_ObjectTableBaseVTable, ...)` -> `ConstructTable(table, base)` -> `table->vtable = base`);
  4. a class's slots are its own CallVirtual sites plus its bases' (a derived class's site wins a conflict);
  5. join: vtable symbol -> classes -> slots -> signature. Slots called with a float argument go through
     Abi::CallEabi (the host dispatcher adapts to the body's real signature) and are recorded as `eabi`: no thunk.

Output: a TSV (tools/vtable_slots.tsv is the committed snapshot; tools/ci/build_all.sh regenerates and compares):
    vtable_symbol  slot  wasm_sig  classes  cpp_sig  sites (the TUs holding the call sites, up to three)
followed by `#conflict` rows (one slot, two signatures: a decomp inconsistency -- the most frequent signature is used,
the others listed) and `#vtable` rows (EVERY retail vtable symbol the decomp declares, with no slot information, so the
patcher knows where each vtable ends). The wasm signature scalarises like the seam: pointers/references/bool/enum/<=32-bit
integers -> i32, 64-bit integers -> i64, f32 -> f32, f64 -> f64, void -> nil.

    gen_vtable_slots.py --wasi-sdk DIR --libclang DIR --game DIR --shim DIR -I ... --out vtable_slots.tsv [--json x.json] src...
"""
import argparse
import collections
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import rspfile  # noqa: E402
import te_cxx  # noqa: E402


def load_libclang(libclang_dir):
    d = Path(libclang_dir)
    cands = sorted(d.glob("**/platlib")) + [d]
    for c in cands:
        if (c / "clang" / "cindex.py").exists():
            sys.path.insert(0, str(c))
            import clang.cindex as ci  # noqa: E402
            lib = c / "clang" / "native"
            for name in ("libclang.dylib", "libclang.so", "libclang.dll"):
                if (lib / name).exists():
                    ci.Config.set_library_file(str(lib / name))
                    break
            return ci
    raise SystemExit(f"gen_vtable_slots: no libclang python package under {libclang_dir} (tools/fetch_toolchain.sh)")


class Extractor:
    def __init__(self, ci):
        self.ci = ci
        self.K = ci.CursorKind
        self.T = ci.TypeKind
        # facts
        self.calls = []            # (class, slot|None, sig, cpp_sig, loc, slot_tokens)
        self.assigns = []          # (class, vtable_symbol, loc)
        self.param_class = collections.defaultdict(set)   # (func usr, param index) -> {class}
        self.param_flow = collections.defaultdict(set)    # (func usr, param index) -> {(callee usr, arg index)}
        self.arg_vtable = collections.defaultdict(set)    # (callee usr, arg index) -> {vtable symbol}
        self.vtables = {}          # vtable symbol (retail label) -> C++ name
        self.bases = collections.defaultdict(set)         # class -> {base class}
        self.unresolved_slots = []
        self.record_by_value = []

    # ----- type helpers -------------------------------------------------------------------------------------------
    def strip(self, t):
        t = t.get_canonical()
        while t.kind in (self.T.POINTER, self.T.LVALUEREFERENCE, self.T.RVALUEREFERENCE):
            t = t.get_pointee().get_canonical()
        return t

    def class_name(self, t):
        t = self.strip(t)
        d = t.get_declaration()
        if d.kind == self.K.NO_DECL_FOUND:
            return None
        return self.qualified(d)

    def qualified(self, d):
        parts = []
        c = d
        while c is not None and c.kind != self.K.TRANSLATION_UNIT:
            if c.kind in (self.K.NAMESPACE,) and not c.spelling:
                pass  # anonymous namespace
            elif c.spelling:
                parts.append(c.spelling)
            c = c.semantic_parent
        return "::".join(reversed(parts))

    def wasm_scalar(self, t, loc):
        c = t.get_canonical()
        k = c.kind
        if k == self.T.VOID:
            return "nil"
        if k in (self.T.POINTER, self.T.LVALUEREFERENCE, self.T.RVALUEREFERENCE, self.T.CONSTANTARRAY,
                 self.T.INCOMPLETEARRAY, self.T.FUNCTIONPROTO, self.T.MEMBERPOINTER, self.T.NULLPTR):
            return "i32"
        if k == self.T.FLOAT:
            return "f32"
        if k == self.T.DOUBLE or k == self.T.LONGDOUBLE:
            return "f64"
        if k == self.T.ENUM:
            return "i32" if c.get_size() <= 4 else "i64"
        if k in (self.T.BOOL, self.T.CHAR_U, self.T.UCHAR, self.T.CHAR16, self.T.CHAR32, self.T.USHORT, self.T.UINT,
                 self.T.ULONG, self.T.ULONGLONG, self.T.CHAR_S, self.T.SCHAR, self.T.WCHAR, self.T.SHORT, self.T.INT,
                 self.T.LONG, self.T.LONGLONG, self.T.INT128, self.T.UINT128):
            return "i32" if c.get_size() <= 4 else "i64"
        if k == self.T.RECORD:
            self.record_by_value.append((c.spelling, loc))
            return "i32"  # clang's wasm32 ABI passes a non-trivial aggregate indirectly; a single-scalar one as the scalar
        return "i32"

    # ----- expression helpers -------------------------------------------------------------------------------------
    def inner(self, e):
        """Skip implicit casts / parens (UNEXPOSED_EXPR, PAREN_EXPR); keep explicit casts (their type is the intent)."""
        while e.kind in (self.K.UNEXPOSED_EXPR, self.K.PAREN_EXPR):
            ch = list(e.get_children())
            if len(ch) != 1:
                break
            e = ch[0]
        return e

    def int_value(self, e):
        """A constant integer: literal, enum constant, constexpr variable, unary minus, parenthesised."""
        e = self.inner(e)
        K = self.K
        if e.kind == K.INTEGER_LITERAL:
            toks = [t.spelling for t in e.get_tokens()]
            if toks:
                return int(toks[0].rstrip("uUlL"), 0)
            return None
        if e.kind == K.DECL_REF_EXPR:
            r = e.referenced
            if r is None:
                return None
            if r.kind == K.ENUM_CONSTANT_DECL:
                return r.enum_value
            if r.kind == K.VAR_DECL:
                for ch in r.get_children():
                    if ch.kind not in (K.TYPE_REF, K.TEMPLATE_REF, K.NAMESPACE_REF, K.ASM_LABEL_ATTR):
                        v = self.int_value(ch)
                        if v is not None:
                            return v
            return None
        if e.kind == K.UNARY_OPERATOR:
            toks = [t.spelling for t in e.get_tokens()]
            ch = list(e.get_children())
            if toks and toks[0] == "-" and ch:
                v = self.int_value(ch[0])
                return -v if v is not None else None
            if ch:
                return self.int_value(ch[0])
        if e.kind == K.BINARY_OPERATOR:
            ch = list(e.get_children())
            if len(ch) == 2:
                a, b = self.int_value(ch[0]), self.int_value(ch[1])
                toks = [t.spelling for t in e.get_tokens()]
                # the operator token sits between the operand token runs
                la = len(list(ch[0].get_tokens()))
                op = toks[la] if la < len(toks) else None
                if a is not None and b is not None and op in ("+", "-", "*"):
                    return {"+": a + b, "-": a - b, "*": a * b}[op]
        if e.kind in (K.CXX_STATIC_CAST_EXPR, K.CXX_FUNCTIONAL_CAST_EXPR, K.CSTYLE_CAST_EXPR):
            ch = list(e.get_children())
            if ch:
                return self.int_value(ch[-1])
        return None

    def vtable_symbol_of(self, e):
        """The retail label of a `const GccVTableEntry[]` variable this expression names, else None."""
        e = self.inner(e)
        if e.kind == self.K.UNARY_OPERATOR:  # &g_X[0] forms
            ch = list(e.get_children())
            if ch:
                return self.vtable_symbol_of(ch[0])
        if e.kind == self.K.ARRAY_SUBSCRIPT_EXPR:
            ch = list(e.get_children())
            if ch:
                return self.vtable_symbol_of(ch[0])
        if e.kind == self.K.DECL_REF_EXPR and e.referenced is not None and e.referenced.kind == self.K.VAR_DECL:
            return self.label_of(e.referenced)
        return None

    def label_of(self, var):
        if "GccVTableEntry" not in var.type.spelling:
            return None
        for ch in var.get_children():
            if ch.kind == self.K.ASM_LABEL_ATTR:
                return ch.spelling
        return None

    def param_index_of(self, e, func):
        e = self.inner(e)
        if e.kind == self.K.DECL_REF_EXPR and e.referenced is not None and e.referenced.kind == self.K.PARM_DECL:
            for i, p in enumerate(func.get_arguments()):
                if p == e.referenced or (p.spelling == e.referenced.spelling and p.location == e.referenced.location):
                    return i
        return None

    def is_vtable_member(self, member_type):
        c = member_type.get_canonical()
        if c.kind != self.T.POINTER:
            return False
        return "GccVTableEntry" in c.get_pointee().get_canonical().spelling

    # ----- the walk ------------------------------------------------------------------------------------------------
    def visit_tu(self, tu, src_name):
        self.src = src_name
        self._walk(tu.cursor, None)

    def _walk(self, c, func):
        K = self.K
        if c.kind in (K.FUNCTION_DECL, K.CXX_METHOD, K.CONSTRUCTOR, K.DESTRUCTOR, K.FUNCTION_TEMPLATE):
            if c.is_definition():
                func = c
        if c.kind in (K.STRUCT_DECL, K.CLASS_DECL) and c.is_definition():
            name = self.qualified(c)
            for ch in c.get_children():
                if ch.kind == K.CXX_BASE_SPECIFIER:
                    b = self.class_name(ch.type)
                    if b:
                        self.bases[name].add(b)
        if c.kind == K.VAR_DECL:
            lab = self.label_of(c)
            if lab:
                self.vtables[lab] = c.spelling
        if c.kind == K.CALL_EXPR:
            self._call(c, func)
        elif c.kind == K.BINARY_OPERATOR:
            self._assign(c, func)
        elif c.kind == K.CONSTRUCTOR and c.is_definition():
            self._ctor_inits(c)
        elif c.kind == K.INIT_LIST_EXPR:
            self._init_list(c, func)
        for ch in c.get_children():
            self._walk(ch, func)

    def _loc(self, c):
        # the TU only: line numbers would be those of the preprocessed text, which change with every header edit
        return self.src

    def _call(self, c, func):
        K = self.K
        ref = c.referenced
        args = list(c.get_arguments())
        if c.spelling == "CallVirtual" and ref is not None and len(args) >= 3:
            ft = ref.type
            res = ft.get_result()
            ats = list(ft.argument_types())
            loc = self._loc(c)
            params = [self.wasm_scalar(t, loc) for t in ats[3:]]
            sig_params = ["i32"] + params  # `this`
            r = self.wasm_scalar(res, loc)
            if any(p in ("f32", "f64") for p in params):
                sig = "eabi"  # Abi::IsMixed -> CallEabi -> the host dispatcher adapts
            else:
                sig = f"({', '.join(sig_params)}) -> {r}"
            cpp = f"{res.spelling}({', '.join(t.spelling for t in ats[3:])})"
            obj = self.inner(args[0])
            cls = self.class_name(obj.type)
            vt = self.inner(args[1])
            if vt.kind == K.MEMBER_REF_EXPR:
                base = list(vt.get_children())
                if base:
                    vcls = self.class_name(self.inner(base[0]).type)
                    if vcls:
                        cls = vcls  # the object whose vtable member is read is the one whose class matters
            slot = self.int_value(args[2])
            if slot is None:
                self.unresolved_slots.append((f"{loc}:{c.location.line}(ii)", " ".join(t.spelling for t in args[2].get_tokens())))
            self.calls.append((cls, slot, sig, cpp, loc))
            # a vtable variable named directly at the call site binds that symbol to the class too
            sym = self.vtable_symbol_of(args[1])
            if sym and cls:
                self.assigns.append((cls, sym, loc))
            return
        if ref is None or ref.kind not in (K.FUNCTION_DECL, K.CXX_METHOD, K.CONSTRUCTOR):
            return
        callee = ref.get_usr()
        # arguments that are vtable symbols or vtable-typed parameters of the enclosing function
        for i, a in enumerate(args):
            sym = self.vtable_symbol_of(a)
            if sym:
                self.arg_vtable[(callee, i)].add(sym)
                continue
            if func is not None:
                pi = self.param_index_of(a, func)
                if pi is not None and self.is_vtable_member(self.inner(a).type):
                    self.param_flow[(func.get_usr(), pi)].add((callee, i))

    def _assign(self, c, func):
        ch = list(c.get_children())
        if len(ch) != 2:
            return
        toks = [t.spelling for t in c.get_tokens()]
        lhs = self.inner(ch[0])
        if lhs.kind != self.K.MEMBER_REF_EXPR or not self.is_vtable_member(lhs.type):
            return
        # the assignment operator: the first token after the LHS token run
        nl = len(list(ch[0].get_tokens()))
        if nl < len(toks) and toks[nl] != "=":
            return
        base = list(lhs.get_children())
        cls = self.class_name(self.inner(base[0]).type) if base else None
        if cls is None:
            return
        self._bind(cls, ch[1], func, self._loc(c))

    def _bind(self, cls, rhs, func, loc):
        sym = self.vtable_symbol_of(rhs)
        if sym:
            self.assigns.append((cls, sym, loc))
            return
        if func is not None:
            pi = self.param_index_of(rhs, func)
            if pi is not None:
                self.param_class[(func.get_usr(), pi)].add(cls)

    def _ctor_inits(self, c):
        # a constructor's member initialisers appear as MEMBER_REF followed by the initialiser expression
        ch = list(c.get_children())
        cls = self.qualified(c.semantic_parent) if c.semantic_parent is not None else None
        for i, x in enumerate(ch):
            if x.kind == self.K.MEMBER_REF and self.is_vtable_member(x.type) and i + 1 < len(ch) and cls:
                self._bind(cls, ch[i + 1], c, self._loc(x))

    def _init_list(self, c, func):
        t = c.type.get_canonical()
        if t.kind != self.T.RECORD:
            return
        decl = t.get_declaration()
        fields = [f for f in decl.get_children() if f.kind == self.K.FIELD_DECL]
        cls = self.qualified(decl)
        for f, e in zip(fields, c.get_children()):
            if self.is_vtable_member(f.type):
                self._bind(cls, e, func, self._loc(c))

    # ----- the join ------------------------------------------------------------------------------------------------
    def join(self):
        # 1. vtable symbol -> classes (direct + through parameters, to a fixpoint)
        vt_classes = collections.defaultdict(set)
        for cls, sym, loc in self.assigns:
            vt_classes[sym].add(cls)
        # a vtable symbol arriving at (callee, i) flows to param_class[(callee, i)] and on through param_flow
        seen = set()
        work = [(k, sym) for k, syms in self.arg_vtable.items() for sym in syms]
        while work:
            key, sym = work.pop()
            if (key, sym) in seen:
                continue
            seen.add((key, sym))
            for cls in self.param_class.get(key, ()):
                vt_classes[sym].add(cls)
            for nxt in self.param_flow.get(key, ()):
                work.append((nxt, sym))
        # 2. class -> {slot -> Counter(sig)}, with bases inherited (own sites first)
        own = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
        cpp_of = {}
        sites = collections.defaultdict(list)
        for cls, slot, sig, cpp, loc in self.calls:
            if cls is None or slot is None:
                continue
            own[cls][slot][sig] += 1
            cpp_of[(cls, slot, sig)] = cpp
            sites[(cls, slot, sig)].append(loc)

        def all_bases(cls, acc=None):
            acc = acc if acc is not None else []
            for b in sorted(self.bases.get(cls, ())):
                if b not in acc:
                    acc.append(b)
                    all_bases(b, acc)
            return acc

        def slots_of(cls):
            out = {}
            for k, v in own.get(cls, {}).items():
                out[k] = (v, cls)
            for b in all_bases(cls):
                for k, v in own.get(b, {}).items():
                    out.setdefault(k, (v, b))
            return out

        rows = []
        conflicts = []
        for sym in sorted(vt_classes):
            per_slot = collections.defaultdict(collections.Counter)
            who = collections.defaultdict(set)
            for cls in sorted(vt_classes[sym]):
                for slot, (ctr, src_cls) in slots_of(cls).items():
                    for sig, n in ctr.items():
                        per_slot[slot][sig] += n
                        who[(slot, sig)].add(src_cls)
            for slot in sorted(per_slot):
                ctr = per_slot[slot]
                best, n = ctr.most_common(1)[0]
                if len(ctr) > 1:
                    conflicts.append((sym, slot, [(s, k, sorted(who[(slot, s)])) for s, k in ctr.most_common()]))
                classes = sorted(who[(slot, best)])
                cpp = cpp_of.get((classes[0], slot, best), "")
                locs = []
                for c in classes:
                    locs += sites.get((c, slot, best), [])
                rows.append((sym, slot, best, ",".join(classes), cpp, ";".join(sorted(set(locs))[:3])))
        return rows, conflicts, vt_classes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wasi-sdk", required=True)
    ap.add_argument("--libclang", required=True, help="dir holding the libclang python wheel contents")
    ap.add_argument("--game", required=True)
    ap.add_argument("--shim", required=True)
    ap.add_argument("-I", dest="incs", action="append", default=[])
    ap.add_argument("--std", default="gnu++26")
    ap.add_argument("--out", required=True)
    ap.add_argument("--json", default=None)
    ap.add_argument("--keep-ii", default=None, help="directory for the preprocessed texts (debugging)")
    ap.add_argument("sources", nargs="+")
    a = ap.parse_args(rspfile.expand_argv(sys.argv[1:]))

    ci = load_libclang(a.libclang)
    sdk = Path(a.wasi_sdk)
    clang = str(sdk / "bin" / "clang++")
    flags = ["--target=wasm32-wasi", f"--sysroot={sdk / 'share' / 'wasi-sysroot'}", *te_cxx.CXXFLAGS_BASE, "-O2"]
    flags = [f if not f.startswith("-std=") else f"-std={a.std}" for f in flags]
    inc = [f"-I{Path(a.game) / 'include'}", f"-I{a.shim}", *[f"-I{i}" for i in a.incs]]
    resource_dirs = sorted((sdk / "lib" / "clang").glob("*"))
    parse_args = [f for f in flags if not f.startswith("-O")] + inc
    if resource_dirs:
        parse_args += ["-resource-dir", str(resource_dirs[0])]

    ex = Extractor(ci)
    index = ci.Index.create()
    stats = {"tus": 0, "preprocess_failed": [], "parse_errors": {}}
    for src in a.sources:
        pp, err = te_cxx.preprocess(clang, [*flags, *inc], src)
        if pp is None:
            stats["preprocess_failed"].append(src)
            continue
        text, _log = te_cxx.rewrite(pp)
        name = Path(src).name + ".ii"
        if a.keep_ii:
            Path(a.keep_ii).mkdir(parents=True, exist_ok=True)
            Path(a.keep_ii, name).write_text(text)
        tu = index.parse(name, args=parse_args, unsaved_files=[(name, text)])
        errs = [str(d) for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
        if errs:
            stats["parse_errors"][src] = errs[:5]
        rel = os.path.relpath(src, a.game)
        ex.visit_tu(tu, rel)
        stats["tus"] += 1

    rows, conflicts, vt_classes = ex.join()
    thunkable = [r for r in rows if r[2] != "eabi"]
    L = ["# GENERATED by tools/gen_vtable_slots.py from the decomp (libclang over the te_cxx-preprocessed TUs) -- DO NOT EDIT.",
         "# The wasm signature every GCC 2.9x vtable slot is CALLED with (CallVirtual<R, A...> instantiations joined to the",
         "# vtable symbols their classes carry). `eabi` = called through Abi::CallEabi (float argument): the dispatcher adapts.",
         f"# vtables with slot data: {len(vt_classes)}; slot rows: {len(rows)} ({len(thunkable)} typed, {len(rows) - len(thunkable)} eabi); "
         f"conflicts: {len(conflicts)}; declared vtable symbols: {len(ex.vtables)}; TUs: {stats['tus']}",
         "#vtable_symbol\tslot\twasm_sig\tclasses\tcpp_sig\tsites"]
    for sym, slot, sig, classes, cpp, locs in rows:
        L.append(f"{sym}\t{slot}\t{sig}\t{classes}\t{cpp}\t{locs}")
    for sym, slot, lst in conflicts:
        L.append("#conflict\t" + sym + f"\t{slot}\t" + " | ".join(f"{s} x{k} [{','.join(w)}]" for s, k, w in lst))
    for sym in sorted(ex.vtables):
        L.append(f"#vtable\t{sym}\t{ex.vtables[sym]}")
    Path(a.out).write_text("\n".join(L) + "\n")
    if a.json:
        Path(a.json).write_text(json.dumps({
            "stats": stats, "rows": rows, "conflicts": conflicts,
            "vtable_classes": {k: sorted(v) for k, v in vt_classes.items()},
            "unresolved_slots": ex.unresolved_slots, "record_by_value": ex.record_by_value,
            "calls_total": len(ex.calls), "calls_no_class": sum(1 for c in ex.calls if c[0] is None),
            "vtables_declared": len(ex.vtables)}, indent=1))
    print(f"gen_vtable_slots: {stats['tus']} TUs, {len(ex.calls)} CallVirtual sites ({sum(1 for c in ex.calls if c[0] is None)} "
          f"without a class, {len(ex.unresolved_slots)} unresolved slots), {len(ex.vtables)} vtable symbols declared, "
          f"{len(vt_classes)} with slot data -> {len(rows)} slot rows ({len(thunkable)} typed), {len(conflicts)} conflicts, "
          f"{len(stats['parse_errors'])} TUs with parse errors -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
