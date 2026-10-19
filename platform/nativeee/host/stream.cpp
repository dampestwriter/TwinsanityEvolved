// platform/nativeee (host side) -- stream.h: the ps2e_stream_* imports over te::stream::StreamService
// (stream_service.h). The path argument is a guest address (te::GuestString); the read destination stays a
// guest address (the service writes through te::GuestPointer on delivery).
#include <cstdint>

#include "ps2e_seam.h"
#include "stream_service.h"
#include "te_host.h"

namespace {
inline te::stream::StreamService& S() { return te::stream::Service(); }
inline uint32_t U32(ps2e_s32 v) { return static_cast<uint32_t>(v); }
}  // namespace

extern "C" {

ps2e_s32 ps2e_stream_initialise(ps2e_s32 channels, ps2e_u32 workMemory) { return S().Initialise(channels, workMemory); }
ps2e_s32 ps2e_stream_update(void) { return S().Update(); }
ps2e_s32 ps2e_stream_get_state(void) { return static_cast<ps2e_s32>(S().GetState()); }
void ps2e_stream_attach_buffer(ps2e_s32 channel, ps2e_s32 size, ps2e_s32 soundAddress, ps2e_s32 soundSize) {
    S().AttachBuffer(channel, U32(size), U32(soundAddress), U32(soundSize));
}
void ps2e_stream_detach_buffer(ps2e_s32 channel) { S().DetachBuffer(channel); }
ps2e_s32 ps2e_stream_free_buffer_memory(void) { return static_cast<ps2e_s32>(S().FreeBufferMemory()); }
ps2e_s32 ps2e_stream_open_file(ps2e_u32 path) { return S().OpenFile(te::GuestString(path, 0x100)); }
ps2e_s32 ps2e_stream_file_size(void) { return static_cast<ps2e_s32>(S().FileSize()); }
void ps2e_stream_close_file(ps2e_s32 file) { S().CloseFile(file); }
void ps2e_stream_read(ps2e_s32 channel, ps2e_s32 file, ps2e_s32 offset, ps2e_s32 size, ps2e_u32 destination) {
    S().Read(channel, file, U32(offset), U32(size), destination);
}
void ps2e_stream_read_sound_bank(ps2e_s32 channel, ps2e_s32 bank, ps2e_s32 file, ps2e_s32 offset, ps2e_s32 size) {
    S().ReadSoundBank(channel, U32(bank), file, U32(offset), U32(size));
}
ps2e_s32 ps2e_stream_is_reading(ps2e_s32 channel) { return S().IsReading(channel) ? 1 : 0; }
ps2e_s32 ps2e_stream_wait(ps2e_s32 channel) { return S().Wait(channel); }

}  // extern "C"
