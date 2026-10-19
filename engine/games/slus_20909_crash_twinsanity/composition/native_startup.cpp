#include "games/slus_20909_crash_twinsanity/composition/native_startup.h"

#include <algorithm>
#include <cctype>

#include "engine/core/verify/run_meta.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_effects.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_transport.h"

namespace ps2r::app {
namespace {
// The file stem of a disc path ("cdrom0:\CRASH6\SYS\STREAM.IRX" -> "STREAM.IRX"), upper case.
std::string Stem(const std::string& path) {
    const size_t cut = path.find_last_of("\\/");
    std::string stem = cut == std::string::npos ? path : path.substr(cut + 1);
    for (char& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return stem;
}
}  // namespace

NativeStartup::NativeStartup(bool selected, Owners owners, EeRead read, EeWrite write)
    : selected_(selected), owners_(owners), server_(*this, std::move(read), std::move(write)) {}

const char* NativeStartup::OwnerOf(const std::string& module) const {
    if (module == "SIO2MAN.IRX" || module == "PADMAN.IRX") return owners_.input ? "InputBindings" : nullptr;
    if (module == "MCMAN.IRX" || module == "MCSERV.IRX") return owners_.saves ? "SaveStorage" : nullptr;
    if (module == "LIBSD.IRX") return owners_.audio ? "Audio+SoundEffects" : nullptr;
    if (module == "SDRDRV.IRX") return owners_.audio && owners_.banks ? "SdrRemote+SoundBanks" : nullptr;
    if (module == "STREAM.IRX") return owners_.transport ? "StreamTransport" : nullptr;
    if (module == "CDVDSTM.IRX") return owners_.files && owners_.movie_cursor ? "FileServices+MovieStream" : nullptr;
    return nullptr;
}

int32_t NativeStartup::LoadModule(const std::string& path) {
    const std::string module = Stem(path);
    const char* owner = OwnerOf(module);
    if (!owner) {
        ++census_.refused_loads;
        census_.modules.push_back(module + ":refused");
        return -1;
    }
    ++census_.loads;
    census_.modules.push_back(module + ":" + owner);
    return static_cast<int32_t>(census_.loads);   // the owner's handle, in the title's request order
}

uint32_t NativeStartup::AllocIopHeap(uint32_t) {
    ++census_.heap_allocs_refused;
    return 0;
}
void NativeStartup::FreeIopHeap(uint32_t) { ++census_.heap_frees; }

void NativeStartup::OnBoundary() { frame_base_batches_ = title::StreamTransport::Count().batches; }
uint64_t NativeStartup::RoundTrips() const { return title::StreamTransport::Count().batches; }
bool NativeStartup::TimeLeft(uint64_t service_us, uint64_t frame_us) const {
    const uint64_t in_frame = title::StreamTransport::Count().batches - frame_base_batches_;
    return service_us == 0 || in_frame * service_us < frame_us;
}

void NativeStartup::TransportServer::Call(const RpcRouter::CallRequest& req) {
    ++owner_.census_.server_calls;
    std::vector<uint8_t> request(req.send_size), reply(std::max<uint32_t>(req.recv_size, 4));
    for (uint32_t i = 0; i < req.send_size; ++i) request[i] = read_(req.send + i);
    if (!title::StreamTransport::Serve(request.data(), request.size(), reply.data(), reply.size())) {
        ++owner_.census_.server_malformed;
        return;   // nothing written: the client sees its previous reply (its busy word still clears)
    }
    for (uint32_t i = 0; i < req.recv_size; ++i) write_(req.recv + i, reply[i]);
}

void NativeStartup::Record(verify::RunMeta& meta) const {
    meta.Set("startup", selected_ ? "native" : "retained");
    std::string modules;
    for (const auto& m : census_.modules) modules += (modules.empty() ? "" : ",") + m;
    meta.Set("startup_modules", modules.empty() ? "none" : modules);
    meta.Set("startup_loads", std::to_string(census_.loads) + "/" + std::to_string(census_.refused_loads));
    meta.Set("stream_transport", title::StreamTransport::Census() + ",server_calls=" + std::to_string(census_.server_calls) +
                                     ",server_malformed=" + std::to_string(census_.server_malformed));
    meta.Set("stream_transport_unowned", title::StreamTransport::UnownedCensus());
    meta.Set("sound_effects", title::SoundEffects::Census());
    meta.Set("iop_heap_native", "refused=" + std::to_string(census_.heap_allocs_refused) + ",frees=" + std::to_string(census_.heap_frees));
}

}  // namespace ps2r::app
