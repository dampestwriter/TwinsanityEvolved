/* platform/nativeee/vita/shim/sys/mman.h -- the Vita lane's <sys/mman.h>: newlib has none, and the wasm2c runtime
 * (wasm-rt-impl.c) includes the header unconditionally on every non-Windows host even when it never maps memory
 * (WASM_RT_USE_MMAP=0, the setting every lane of this tree builds with). Declarations only; a call would be a link
 * error, which is the intent -- nothing on the Vita maps memory. Put first on tegame's include path by CMakeLists.txt
 * under TE_VITA. */
#ifndef TE_VITA_SHIM_SYS_MMAN_H
#define TE_VITA_SHIM_SYS_MMAN_H
#include <stddef.h>
#include <sys/types.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FIXED 0x10
#define MAP_FAILED ((void*)-1)
#ifdef __cplusplus
extern "C" {
#endif
void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void* addr, size_t length);
int mprotect(void* addr, size_t length, int prot);
#ifdef __cplusplus
}
#endif
#endif
