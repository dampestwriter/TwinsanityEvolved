// platform/nativeee (wasm side) -- retail-named functions the retail DATA points at whose PS2 bodies belonged to the
// PS2 platform arm (src/platform/ps2) and have NO meaning on NativeEE. The retail __CTOR_LIST__ (G_UnkFunTableSize,
// run by RunStaticConstructors in src/gcc2.cpp) names three static initialisers of the PS2 renderer; without a body the
// runner's patcher leaves the raw retail address and the call traps. They are no-ops here; the graphics owner owns
// whatever host-side initialisation replaces them (platform/nativeee/host/graphics.cpp).
extern "C"
{
    // src/platform/ps2/renderer/buckets.cpp RendererStaticInit
    void te_noop_FUN_001a6678() asm("FUN_001a6678");
    void te_noop_FUN_001a6678() {}
    // src/platform/ps2/renderer/vuprograms.cpp InitVuProgramsModule
    void te_noop_FUN_001dd4d0() asm("FUN_001dd4d0");
    void te_noop_FUN_001dd4d0() {}
    // src/platform/ps2/renderer/vu0programs.cpp ConstructVu0ProgramsModule
    void te_noop_FUN_002b2230() asm("FUN_002b2230");
    void te_noop_FUN_002b2230() {}
}
