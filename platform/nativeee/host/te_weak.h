// te_weak.h -- the two weak-symbol idioms the host bodies use, spelled for both object formats.
//
// Mach-O / ELF (clang, gcc): `__attribute__((weak))` -- a weak DEFINITION is replaced by any strong one, a weak
// DECLARATION that nothing defines is null. COFF (clang-cl, cl) has neither: the MSVC linker's `/alternatename:A=B`
// directive, carried by the object through `#pragma comment(linker, ...)`, resolves an otherwise-undefined A to B.
//
//   TE_WEAK_DEFAULT(ret, name, params, body)   a default body for `name` that a strong definition anywhere else replaces
//                                              (COFF: `name_te_default` + the directive; the strong definition, when one
//                                              is linked, leaves `name` defined and the directive unused)
//   TE_WEAK_REF_VOID(name, params)             declare `void name params` that may be ABSENT from the link; TE_WEAK_PRESENT
//                                              (name) says whether to call it (COFF: an absent `name` resolves to an empty
//                                              body `name_te_absent`, so the call is always safe and PRESENT is simply true)
//   Both are complete declarations: no `;` after the macro at the use site.
//
// extern "C" names only (the directive names the undecorated symbol: x64 adds no prefix). The game's own symbols never
// pass through here -- the module is wasm2c output; these are host-side hooks between the runner, the tests and the bodies.
#pragma once

#if defined(_MSC_VER)
#define TE_WEAK_DEFAULT(ret, name, params, body) \
    extern "C" ret name##_te_default params body  \
    __pragma(comment(linker, "/alternatename:" #name "=" #name "_te_default"))
#define TE_WEAK_REF_VOID(name, params)         \
    extern "C" void name params;               \
    extern "C" void name##_te_absent params {} \
    __pragma(comment(linker, "/alternatename:" #name "=" #name "_te_absent"))
#define TE_WEAK_PRESENT(name) true
#else
#define TE_WEAK_DEFAULT(ret, name, params, body) extern "C" __attribute__((weak)) ret name params body
#define TE_WEAK_REF_VOID(name, params) extern "C" __attribute__((weak)) void name params;
#define TE_WEAK_PRESENT(name) ((name) != nullptr)
#endif
