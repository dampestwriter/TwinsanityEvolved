#include "games/slus_20909_crash_twinsanity/hle/game_libraries.h"

#include <utility>

#include "engine/core/verify/run_meta.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/savestate/thread_section.h"
#include "engine/ipu/hle/libmpeg_bindings.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_checkpoint.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

namespace title {

GameLibraries::GameLibraries(ps2r::ipu::GuestMemory& memory, ps2r::ipu::LibmpegBindings& mpeg,
                             Selection selection, MovieDecoder::StreamServices movie_stream,
                             std::function<void(uint16_t)> movie_volume)
    : math_(memory), collision_(memory), trig_(memory), animation_(memory), bounds_(memory),
      clipping_(memory), joint_(memory), culling_(memory),
      movie_(memory, {
          [&mpeg](CpuContext& context) { (void)mpeg.SceMpegCreate(context); },
          [&mpeg](CpuContext& context) { (void)mpeg.SceMpegReset(context); },
          [&mpeg](CpuContext& context) { mpeg.SceMpegGetPicture(context); },
          [&mpeg](CpuContext& context) { mpeg.SceMpegGetPictureRaw8(context); },
          [&mpeg] { mpeg.InitializePlayback(); },
          [&mpeg](uint32_t decoder) { mpeg.ClosePlayback(decoder); },
          [](CpuContext& context, uint32_t address) { DispatchTable::Invoke(address, context); },
          [&mpeg](uint32_t decoder) { return mpeg.HasPendingPlayback(decoder); },
          std::move(movie_volume)
      }), movie_stream_(std::move(movie_stream)),
      movie_demux_([&mpeg](uint32_t decoder, uint32_t data, int32_t bytes) {
          return mpeg.DemuxPlayback(decoder, data, bytes);
      }), selection_(selection) {}

bool GameLibraries::Install() {
    const auto capture = [](uint32_t address) { return DispatchTable::Capture(address); };
    const auto install = [](uint32_t address, DispatchTable::Body body) {
        return DispatchTable::Install(address, std::move(body));
    };
    // Verified NTSC LoadSoundBank entry; a restored interior entry still owns
    // its generated stack frame and must finish through that continuation.
    constexpr uint32_t sound_bank = 0x002ab160u;
    const auto original = capture(sound_bank);
    installed_ = math_.Install(capture, install, selection_.math_helpers) &&
                 collision_.Install(capture, install, selection_.math_helpers) &&
                 trig_.Install(capture, install, selection_.math_helpers) &&
                 animation_.Install(capture, install, selection_.math_helpers) &&
                 bounds_.Install(capture, install, selection_.math_helpers) &&
                 clipping_.Install(capture, install, selection_.math_helpers) &&
                 joint_.Install(capture, install, selection_.math_helpers) &&
                 culling_.Install(capture, install, selection_.culling) &&
                 movie_.Install(capture, install, selection_.movie_lifecycle) ==
                     (selection_.movie_lifecycle ? 7 : 3);
    if (selection_.movie_lifecycle)
        installed_ = installed_ &&
                     movie_.InstallStream(capture, install, movie_stream_) == 2 &&
                     movie_.InstallInput(capture, install, movie_demux_) == 1 &&
                     movie_.InstallAudio(capture, install) == 2 &&
                     movie_.InstallAudioPlayback(capture, install, true) == 1;
    if (selection_.sound_bank_headers)
        installed_ = installed_ && original && install(sound_bank, [original](CpuContext& context, uint32_t entry) {
            if (entry != 0 && entry != sound_bank) original(context, entry);
            else LoaderFileIo::LoadSoundBank(context);
        });
    installed_ = installed_ && StreamLibrary::Install(capture, install, selection_.stream_files);
    return installed_;
}

bool GameLibraries::CanCheckpoint(std::string* reason) const {
    return !selection_.movie_lifecycle || movie_.CanCheckpoint(reason);
}

bool GameLibraries::InstallMovieBuffers(MovieStream& stream, MovieStream::BufferServices buffers,
                                       MovieStream::Lookup lookup) {
    if (!installed_ || !selection_.movie_lifecycle || !selection_.stream_files || !lookup) return false;
    const auto capture = [](uint32_t address) { return DispatchTable::Capture(address); };
    const auto install = [](uint32_t address, DispatchTable::Body body) {
        return DispatchTable::Install(address, std::move(body));
    };
    return stream.InstallBuffers(capture, install, movie_, std::move(buffers)) == 5 &&
           stream.InstallLookup(capture, install, std::move(lookup)) == 4;
}

bool GameLibraries::RegisterCheckpointSections(ps2r::savestate::SectionRegistry& registry) {
    if (!selection_.culling) return true;
    // The legacy import contract reads the THRD chains through the codec of
    // record; CullingCheckpoint itself never decodes the thread section.
    return CullingCheckpoint::Register(registry, culling_,
        [](const std::vector<uint8_t>& payload, std::vector<CullingCheckpoint::ThreadChain>& chains,
           std::string* error) {
            std::vector<ThreadSection::Record> records;
            if (!ThreadSection::Parse(payload, records, error)) return false;
            for (const auto& record : records) {
                CullingCheckpoint::ThreadChain chain{record.tid, record.started && record.has_ctx, {}};
                for (const auto& frame : record.chain) chain.functions.push_back(frame.func_addr);
                chains.push_back(std::move(chain));
            }
            return true;
        });
}

void GameLibraries::ResetAfterRestore() {
    if (selection_.movie_lifecycle) movie_.ResetAfterRestore();
}

void GameLibraries::Record(ps2r::verify::RunMeta& meta) const {
    meta.Set("game_libraries_installed", installed_ ? "1" : "0");
    meta.Set("math_library", math_.Census());
    meta.Set("collision_math", collision_.Census());
    meta.Set("trig_math", trig_.Census());
    meta.Set("animation_math", animation_.Census());
    meta.Set("bounds_math", bounds_.Census());
    meta.Set("clipping_math", clipping_.Census());
    meta.Set("joint_library", joint_.Census());
    meta.Set("culling_library", culling_.Census());
    meta.Set("movie_decoder", movie_.Census());
    meta.Set("math_helpers_binding", selection_.math_helpers ? "on" : "off");
    meta.Set("culling_binding", selection_.culling ? "on" : "off");
    meta.Set("movie_lifecycle_binding", selection_.movie_lifecycle ? "on" : "off");
    meta.Set("movie_services_binding", selection_.movie_lifecycle ? "on" : "off");
    meta.Set("sound_bank_header_binding", selection_.sound_bank_headers ? "on" : "off");
    meta.Set("stream_files_binding", selection_.stream_files ? "on" : "off");
    const auto stream = StreamLibrary::Count();
    meta.Set("stream_library", "file_opens=" + std::to_string(stream.file_opens) +
             ",sample_opens=" + std::to_string(stream.sample_opens) +
             ",reads=" + std::to_string(stream.reads) + ",deliveries=" + std::to_string(stream.deliveries) +
             ",polls=" + std::to_string(stream.polls) + ",waits=" + std::to_string(stream.waits) +
             ",audio_reads=" + std::to_string(stream.audio_reads) +
             ",audio_registrations=" + std::to_string(stream.audio_registrations) +
             ",cancelled=" + std::to_string(stream.cancelled) +
             ",stale_deliveries=" + std::to_string(stream.stale_deliveries) +
             ",errors=" + std::to_string(stream.errors) + ",fallbacks=" + std::to_string(stream.fallbacks));
    // The replaced model's per-entry census (StreamLibrary::Fallbacks): the
    // `declined` triple is the file-jobs retirement counter; `unowned` and
    // `bank_start` are the sound-bank path's work, retired with hle.sound_samples.
    const auto fallbacks = StreamLibrary::CountFallbacks();
    meta.Set("stream_library_fallbacks",
             "start_read_declined=" + std::to_string(fallbacks.start_read_declined) +
             ",is_reading_declined=" + std::to_string(fallbacks.is_reading_declined) +
             ",wait_declined=" + std::to_string(fallbacks.wait_declined) +
             ",start_read_unowned=" + std::to_string(fallbacks.start_read_unowned) +
             ",is_reading_unowned=" + std::to_string(fallbacks.is_reading_unowned) +
             ",wait_unowned=" + std::to_string(fallbacks.wait_unowned) +
             ",bank_start=" + std::to_string(fallbacks.bank_start) +
             ",bypassed=" + std::to_string(fallbacks.bypassed));
    if (const auto* schedule = StreamLibrary::Scheduling()) {
        const auto& files = schedule->Count();
        meta.Set("file_schedule", "issued=" + std::to_string(files.issued) +
                 ",delivered=" + std::to_string(files.delivered) +
                 ",consumed=" + std::to_string(files.consumed) +
                 ",cancelled=" + std::to_string(files.cancelled) +
                 ",deferred=" + std::to_string(files.deferred) +
                 ",poll_delivered=" + std::to_string(files.poll_delivered) +
                 ",boundary_delivered=" + std::to_string(files.boundary_delivered) +
                 ",wait_delivered=" + std::to_string(files.wait_delivered) +
                 ",wait_parks=" + std::to_string(files.wait_parks) +
                 ",overruns=" + std::to_string(files.overruns) +
                 ",rejected=" + std::to_string(files.rejected));
    }
    const auto bank = LoaderFileIo::CountBankHeaders();
    meta.Set("sound_bank_headers", "calls=" + std::to_string(bank.header_calls) +
             ",loaded=" + std::to_string(bank.sound_bank_headers) + ",bytes=" + std::to_string(bank.header_bytes) +
             ",missing=" + std::to_string(bank.header_misses) + ",failures=" + std::to_string(bank.header_failures));
}

}  // namespace title
