// savestate.h -- the port's save-state registry over the engine's .ps2st envelope (engine/core/savestate: FileHeader,
// SectionRegistry, PayloadReader) and the ONE checkpoint the game thread passes: the end of te::Clock::Boundary().
//
// Model: a save state is the game at a frame boundary = the 32 MB linear memory (RAM0, the
// engine's MainMemory), the module's globals and runtime state (WASM), the game fiber's native stack and register file
// (STCK; runner/te_fiber.h: the stack is at a fixed address so the bytes go back where they were), and one section per
// host service (clock, files, pads, saves, IOP heap, stream, audio, graphics textures ...). Each owner registers a
// SectionSpec here when it is constructed; the runner (runner/te_savestate.cpp) registers the module's own sections,
// captures everything when the game fiber yields at a checkpoint, and restores everything before the first switch into
// a loaded fiber. Section payloads are packed little-endian PODs (PutPod / PayloadReader), never struct dumps.
//
// Tags: the engine's RAM0 for the memory; port-owned payloads carry port tags ('TE' + two letters) so a recomp
// .ps2st reader never mistakes them for its own payloads.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/savestate/payload_reader.h"
#include "engine/core/savestate/savestate_format.h"
#include "engine/core/savestate/section_registry.h"

namespace te::savestate {

using ps2r::savestate::Blob;
using ps2r::savestate::PayloadReader;
using ps2r::savestate::SectionSpec;
using ps2r::savestate::Tag;

constexpr uint32_t kTagHead = Tag('T', 'E', 'H', 'D');      // runner: build id, boundary, bases
constexpr uint32_t kTagModule = Tag('T', 'E', 'W', 'A');    // runner: module globals, table size, runtime TLS
constexpr uint32_t kTagStack = Tag('T', 'E', 'S', 'K');     // runner: fiber register file + stack bytes
constexpr uint32_t kTagClock = Tag('T', 'E', 'C', 'K');     // te::Clock
constexpr uint32_t kTagFiles = Tag('T', 'E', 'F', 'L');     // te::Files (title::Files::Encode)
constexpr uint32_t kTagPads = Tag('T', 'E', 'P', 'D');      // te::Pads (title::InputStateCodec + buffers)
constexpr uint32_t kTagSaves = Tag('T', 'E', 'S', 'V');     // te::Saves (title::SaveStateCodec)
constexpr uint32_t kTagIoHeap = Tag('T', 'E', 'I', 'O');    // te::IoHeap
constexpr uint32_t kTagStream = Tag('T', 'E', 'S', 'T');    // te::stream::StreamService
constexpr uint32_t kTagAudio = Tag('T', 'E', 'A', 'U');     // te::audio::AudioService (banks + music windows)
constexpr uint32_t kTagGraphics = Tag('T', 'E', 'G', 'X');  // graphics host: the game textures (header + packet copies)
constexpr uint32_t kTagSystem = Tag('T', 'E', 'S', 'Y');    // te::Services: which services were constructed/started
constexpr uint32_t kTagMovie = Tag('T', 'E', 'M', 'V');     // te::movie::MoviePlayer: the players the game constructed (by guest address)

// The port's registry (one per process). Owners call Registry().Register(spec) once; a second registration of a tag
// is refused by the engine (returns false), which is the idempotence a lazily constructed service needs.
ps2r::savestate::SectionRegistry& Registry();

// Convenience: register {tag, owner, serialize, restore}; optional sections are skipped when absent from a file.
void Register(uint32_t tag, const char* owner, bool required,
              std::function<void(std::vector<uint8_t>&)> serialize,
              std::function<bool(const Blob&, std::string*)> restore);

// Little-endian put helpers over std::vector<uint8_t> (the engine's PutPod/PutBytes re-exported, plus strings).
template <typename T> inline void Put(std::vector<uint8_t>& v, const T& x) { ps2r::savestate::PutPod(v, x); }
inline void PutBytes(std::vector<uint8_t>& v, const void* p, size_t n) { ps2r::savestate::PutBytes(v, p, n); }
void PutString(std::vector<uint8_t>& v, const std::string& s);
bool GetString(PayloadReader& in, std::string& s, size_t max = 4096);
bool GetVector(PayloadReader& in, std::vector<uint8_t>& out, size_t max);

// Touch every owner that registers lazily (stream, audio, graphics) so a restore finds an owner for each section.
void PrepareOwners();

// ---- the checkpoint ----------------------------------------------------------------------------------------------------
// Called by te::Clock::Boundary() as its LAST statement with nothing live in its frame (no heap pointer is used after
// it returns): the only place the game fiber may be suspended for a save, and the place a loaded fiber resumes.
void Checkpoint(uint64_t boundary);
using Handler = void (*)(uint64_t boundary);
void SetCheckpointHandler(Handler handler);

// ---- requests the services may make (the runner polls them at the checkpoint) ----------------------------------------
// A save requested from inside the game thread (a hotkey, a script): taken at the next checkpoint.
void RequestSave(const std::string& path);
bool TakeSaveRequest(std::string& path);
// A LOAD requested from inside the game thread (the speedrun mod's practice ring): taken at the next checkpoint, where the
// runner restores the file in place (the game fiber resumes at the saved boundary).
void RequestLoad(const std::string& path);
bool TakeLoadRequest(std::string& path);

}  // namespace te::savestate
