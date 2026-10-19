#pragma once
// The title's VU1 program library: the ONE home of the family signature and
// the registration table (every entry in REGISTRATION ORDER -- the registry's
// first-match rule makes the order observable), handed to the engine's
// Vu1Registry / Vu1HleRuntime as data. Every row names its native program;
// a program that meets an unproven data shape DECLINES (the fallback tier's
// cue) rather than guessing.
#include "engine/vu/hle/vu1_hle_contract.h"
#include "engine/vu/hle/vu1_library_entry.h"

class Vu1Library {
public:
    static const Vu1FamilySignature& Signature();
    static const Vu1ProgramLibrary& Get();
};
