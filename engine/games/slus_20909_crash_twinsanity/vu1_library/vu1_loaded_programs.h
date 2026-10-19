#pragma once
// The title's loadable VU1 microprograms: the shader-type programs the
// renderer uploads into VU1 micro memory past the resident set (variant base
// 0x1720) when a material needs them, back to back in material order (TR
// renderer/vuprograms.cpp: Register / LoadProgramForMaterial). Each row is the
// code hash and upload size of one program (title data; no code bytes). The
// engine's analyzer identifies an image's loaded programs by these rows -- the
// layout key that survives the stale bytes earlier uploads leave past the last
// loaded program -- and the native programs identify their sub bodies by the
// program at a dispatch pc instead of by whole-image pins.
#include <cstddef>
#include <cstdint>

#include "engine/vu/hle/vu1_hle_contract.h"
#include "engine/vu/hle/vu1_library_entry.h"

class Vu1LoadedPrograms {
public:
    // Catalog order (the table's row index; the engine reports it in
    // Vu1LoadedAt::program). Named by TR's shader type.
    enum Kind : uint16_t {
        kType01 = 0, kType02, kType03, kType04, kType0A, kType0B, kType0C, kType0D, kType0F, kType10, kType11,
        kType12, kType13, kType15, kType16, kType17, kType18, kType19, kType1A, kType1C, kType1E, kType1F, kType20,
        kCount,
        kNone = 0xFFFF
    };

    static const Vu1LoadedProgram* Table();
    static size_t Count();

    // The program whose first bundle is at `pc` in the analysed image; kNone
    // when the input carries no analysis or no catalog program starts there.
    static Kind At(const Vu1HleInput& in, uint32_t pc);
    // The resident / loaded program's byte size (0 for kNone).
    static uint32_t SizeBytes(Kind k);
};
