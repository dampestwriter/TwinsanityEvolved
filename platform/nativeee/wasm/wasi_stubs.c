// platform/nativeee (wasm side) -- NO WASI. wasi-libc's vsnprintf/sprintf (the string functions the game uses) drag
// the stdio FILE machinery in, which references three WASI imports the game never calls (its stdio is the retail
// libc.cpp's kputs path). Defining the import symbols here resolves them inside the module, so the module's ONLY imports
// are `env.memory` and the seam (tools/seam_imports.txt). Each returns __WASI_ERRNO_NOSYS.
typedef unsigned int u32; typedef unsigned long long u64;
#define WASI_ENOSYS 52u
u32 __imported_wasi_snapshot_preview1_fd_close(u32 fd) { (void)fd; return WASI_ENOSYS; }
u32 __imported_wasi_snapshot_preview1_fd_seek(u32 fd, u64 offset, u32 whence, u32 out) { (void)fd; (void)offset; (void)whence; (void)out; return WASI_ENOSYS; }
u32 __imported_wasi_snapshot_preview1_fd_write(u32 fd, u32 iovs, u32 iovs_len, u32 out) { (void)fd; (void)iovs; (void)iovs_len; (void)out; return WASI_ENOSYS; }
u32 __imported_wasi_snapshot_preview1_fd_read(u32 fd, u32 iovs, u32 iovs_len, u32 out) { (void)fd; (void)iovs; (void)iovs_len; (void)out; return WASI_ENOSYS; }
u32 __imported_wasi_snapshot_preview1_fd_fdstat_get(u32 fd, u32 out) { (void)fd; (void)out; return WASI_ENOSYS; }
u32 __imported_wasi_snapshot_preview1_environ_sizes_get(u32 a, u32 b) { (void)a; (void)b; return WASI_ENOSYS; }
u32 __imported_wasi_snapshot_preview1_environ_get(u32 a, u32 b) { (void)a; (void)b; return WASI_ENOSYS; }
void __imported_wasi_snapshot_preview1_proc_exit(u32 code) { (void)code; __builtin_trap(); }
