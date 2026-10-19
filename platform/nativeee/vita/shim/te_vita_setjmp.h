/* platform/nativeee/vita/shim/te_vita_setjmp.h -- force-included (-include) into the wasm2c runtime's TUs on the Vita
 * lane: wasm-rt.h spells its trap unwinding as sigsetjmp(buf, 1) / siglongjmp on every non-Windows host, and the Vita's
 * newlib defines those only for Cygwin / RTEMS. There are no signals on the device, so the plain setjmp / longjmp pair
 * IS the POSIX pair here (the mask save that sigsetjmp adds has nothing to save). */
#ifndef TE_VITA_SHIM_SETJMP_H
#define TE_VITA_SHIM_SETJMP_H
#include <setjmp.h>
#ifndef sigsetjmp
#define sigsetjmp(env, savemask) setjmp(env)
#endif
#ifndef siglongjmp
#define siglongjmp(env, val) longjmp(env, val)
#endif
#endif
