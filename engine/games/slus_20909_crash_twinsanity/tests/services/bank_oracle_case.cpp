#include "games/slus_20909_crash_twinsanity/tests/services/bank_oracle_case.h"
#include "games/slus_20909_crash_twinsanity/tests/services/sample_control_oracle_case.h"
#include "games/slus_20909_crash_twinsanity/tests/services/stream_oracle_helpers.h"

#include <array>
#include <cstdio>
#include <cstring>
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_banks.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_state.h"

using namespace stream_oracle;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

int RunBankOracle(StreamOracleInputs& fixture) {
    int g_fail = 0;
    auto& ctx = fixture.ctx;
    auto& plugin = fixture.plugin;
    auto& spu = fixture.spu;
    auto& iop_mem = fixture.iop_mem;
    auto& face = fixture.face;
    auto& model = fixture.model;
    auto& meter = fixture.meter;
    const auto& deps = fixture.deps;
    const auto& layout = fixture.layout;
    auto& tick = fixture.tick;
    auto& pump_calls = fixture.pump_calls;
    const auto mgr = fixture.mgr;
    const auto capture = [](uint32_t address) { return DispatchTable::Capture(address); };
    const auto install = [](uint32_t address, StreamLibrary::Body body) {
        return DispatchTable::Install(address, std::move(body));
    };
    // OLD SOUND_P4/P6: the real unaligned CRASH.BD ranges, including the
    // high-ID title jingle. The model arm executes StartSoundBankRead through
    // STREAM's CD worker and sample upload. Compare its actual SPU bytes with
    // the owned asset, rather than using a known-ISO read as the only oracle.
    struct Bank { uint32_t id, offset, length; };
    const std::array<Bank, 2> banks{{{0x8000, 0x3b2ed2, 5168}, {9, 0x98d658, 0x620}}};
    title::Audio audio;
    // Game/readers.cpp FileReadersBlock is 0x10000. The small 0x800 ring
    // above belongs to the .mh header loader, not the sample-bank reader.
    Prep(ctx); SetArg(ctx, 4, mgr); SetArg(ctx, 5, 0x10000); SetArg(ctx, 6, 0); SetArg(ctx, 7, 0);
    DispatchTable::Invoke(kFnStreamOpen, ctx);
    const uint32_t bank_fs = ctx.cpuRegs.GPR.r[2].UL[0];
    const uint8_t bank_slot = memory::read<uint8_t>(bank_fs);
    CHECK(bank_fs >= kRigHeap && bank_fs < kRigHeapEnd, "bank_reader_uses_source_capacity");
    Prep(ctx); DispatchTable::Invoke(0x1e6e60, ctx); // source ResetSound initializes sample transfer ownership
    Prep(ctx); SetArg(ctx, 4, 0); DispatchTable::Invoke(0x1e2958, ctx);
    // Distinct live IDs receive disjoint sample allocations, as the title's
    // sound allocator supplies. The owned arm treats this solely as fixture
    // input; it allocates no corresponding hardware address.
    uint32_t sample_address = 0x80000;
    const char* archive_path = "crash6\\crash.bd";
    for (uint32_t i = 0; i <= std::strlen(archive_path); ++i)
        memory::write<uint8_t>(0x00500000 + i, static_cast<uint8_t>(archive_path[i]));
    uint64_t bank_bytes = 0;
    for (const auto& bank : banks) {
        std::vector<uint8_t> model_bytes;
        for (const bool owned : {false, true}) {
            CHECK(title::SoundBanks::Install(capture, install, nullptr), "bank_detaches_empty_owner");
            CHECK(StreamLibrary::Install(capture, install, owned), "bank_selects_file_arm");
            LoaderFileIo::Bind(deps, layout);
            CHECK(title::SoundBanks::Install(capture, install, owned ? &audio : nullptr), "bank_selects_transfer_arm");
            const auto rpc_before = face.Calls(), cd_before = plugin.Cdvdman().ReadsIssued();
            const auto pumps_before = pump_calls;
            const auto transfers_before = plugin.Libsd().VoiceTransBytes();
            const auto enqueues_before = model.Enqueued(), jobs_before = meter.LastScheduledId();
            Prep(ctx); SetArg(ctx, 4, 256); DispatchTable::Invoke(0x1e7540, ctx);
            memory::write<int32_t>(kHandleCounter, 3);
            Prep(ctx); SetArg(ctx, 4, 0x00500000); LoaderFileIo::Open(ctx);
            CHECK(ctx.cpuRegs.GPR.r[2].SL[0] == 3, "bank_archive_open");
            memory::write<int32_t>(bank_fs + LoaderFileIo::kFsHandle, 3);
            Prep(ctx); SetArg(ctx, 4, bank_fs); LoaderFileIo::FileSize(ctx);
            const uint32_t archive_size = ctx.cpuRegs.GPR.r[2].UL[0];
            CHECK(archive_size > bank.offset + bank.length, "bank_range_in_archive");
            memory::write<uint32_t>(bank_fs + LoaderFileIo::kFsFileSize, archive_size);
            memory::write<uint32_t>(bank_fs, bank_slot | kFlagRingOpen | LoaderFileIo::kFlagReadPending);
            memory::write<uint32_t>(bank_fs + LoaderFileIo::kFsDest, 0);
            memory::write<uint32_t>(0x309ad8, sample_address); // comparison arm's allocator result
            std::memset(spu.SpuRam() + sample_address, 0x5a, bank.length + 64);
            Prep(ctx); SetArg(ctx, 4, bank_fs); SetArg(ctx, 5, bank.id);
            SetArg(ctx, 6, bank.offset); SetArg(ctx, 7, bank.length);
            DispatchTable::Invoke(0x2ada68, ctx);
            CHECK(TitlePoll(ctx, bank_fs) != 0, "bank_pending_before_delivery");
            if (owned) {
                CHECK(!audio.HasSound(bank.id), "bank_asset_not_published_early");
                CHECK(!StreamLibrary::BindSounds({}, {}) &&
                          title::SoundBanks::Install(capture, install, &audio),
                      "bank_delivery_binding_cannot_be_removed_while_pending");
                std::vector<uint8_t> snapshot{42};
                std::string error;
                CHECK(!StreamState::Capture(snapshot, &error) && snapshot == std::vector<uint8_t>{42} &&
                          error == "sound requests require composed Audio state", "bank_snapshot_declines_missing_audio_composition");
            }
            Prep(ctx); SetArg(ctx, 4, bank_fs); LoaderFileIo::WaitReadComplete(ctx);
            CHECK(TitlePoll(ctx, bank_fs) == 0, "bank_wait_delivers_before_done");
            if (!owned) {
                // File completion can precede the final SPU transfer interrupt.
                // The title publishes its separate loaded intent afterwards;
                // advance the model's owner to the same asset-ready boundary.
                Prep(ctx); SetArg(ctx, 4, bank.id); DispatchTable::Invoke(0x1e8278, ctx);
                Prep(ctx); SetArg(ctx, 4, 0); DispatchTable::Invoke(0x1e2958, ctx);
                for (unsigned lap = 0; lap < 64 &&
                     plugin.Libsd().VoiceTransBytes() - transfers_before < bank.length; ++lap) {
                    deps.pump_module(ctx);
                }
                CHECK(plugin.Libsd().VoiceTransBytes() - transfers_before >= bank.length,
                      "bank_model_finishes_every_upload_chunk");
            }
            if (!owned) {
                spu.MixInto(nullptr, 0); // comparison-only queued commands; no output frames
                model_bytes.assign(spu.SpuRam() + sample_address, spu.SpuRam() + sample_address + bank.length);
                const auto entries = iop_mem.Read<uint32_t>(0x12e5c);
                const auto table_address = iop_mem.Read<uint32_t>(0x133e0);
                bool labelled = false;
                for (uint32_t index = 0; index < entries && index < 256; ++index)
                    if (iop_mem.Read<uint32_t>(table_address + index * 16) == bank.id)
                        labelled = iop_mem.Read<uint32_t>(table_address + index * 16 + 4) == sample_address;
                CHECK(labelled, "bank_model_publishes_the_requested_label_and_allocation");
                CHECK(face.Calls() > rpc_before && plugin.Cdvdman().ReadsIssued() > cd_before &&
                          pump_calls > pumps_before, "bank_model_really_executes_stream_and_cd_worker");
            } else {
                CHECK(face.Calls() == rpc_before && plugin.Cdvdman().ReadsIssued() == cd_before &&
                          pump_calls == pumps_before && model.Enqueued() == enqueues_before &&
                          meter.LastScheduledId() == jobs_before &&
                          plugin.Libsd().VoiceTransBytes() == transfers_before,
                      "bank_owned_arm_zero_stream_cd_worker_and_model_queue");
                CHECK(audio.HasSound(bank.id), "bank_owns_completed_asset");
                if (audio.HasSound(bank.id)) {
                    if (*audio.Inspect().samples.at(bank.id).bytes != model_bytes) {
                        const auto& actual = *audio.Inspect().samples.at(bank.id).bytes;
                        size_t first = 0, different = 0;
                        while (first < actual.size() && actual[first] == model_bytes[first]) ++first;
                        for (size_t i = 0; i < actual.size(); ++i) different += actual[i] != model_bytes[i];
                        std::printf("BANK_DIFF id=%u first=%zu different=%zu model=", bank.id, first, different);
                        for (size_t i = 0; i < 32; ++i) std::printf("%02x", model_bytes[i]);
                        std::printf(" owned=");
                        for (size_t i = 0; i < 32; ++i) std::printf("%02x", actual[i]);
                        std::printf(" libsd=%s bytes=%llu\n", plugin.Libsd().CallsCensus().c_str(),
                                    static_cast<unsigned long long>(plugin.Libsd().VoiceTransBytes()));
                        for (uint32_t at = 0; at < ps2r::sound::kSpuRamSize - actual.size(); ++at)
                            if (std::memcmp(spu.SpuRam() + at, actual.data(), 32) == 0) {
                                std::printf("BANK_MODEL_MATCH id=%u at=%x complete=%d\n", bank.id, at,
                                            std::memcmp(spu.SpuRam() + at, actual.data(), actual.size()) == 0);
                                break;
                            }
                    }
                    CHECK(*audio.Inspect().samples.at(bank.id).bytes == model_bytes, "bank_owned_bytes_equal_model_upload");
                    CHECK(!audio.Inspect().samples.at(bank.id).ready, "bank_wait_does_not_invent_loaded_ack");
                    Prep(ctx); SetArg(ctx, 4, bank.id); DispatchTable::Invoke(0x1e8278, ctx);
                    CHECK(audio.Inspect().samples.at(bank.id).ready, "bank_title_ack_publishes_readiness");
                    CHECK(audio.PlaySound(bank.id, 0, 12000, 10000, 0x1000, 0, 0, 0) == 0,
                          "bank_asset_reaches_owned_voice");
                    const auto pcm = audio.MixTick();
                    bool audible = false;
                    for (float value : pcm) if (value != 0) audible = true;
                    CHECK(audible, "bank_owned_voice_produces_pcm");
                    audio.HandOffVoice(0);
                    if (bank.id == 0x8000) {
                        // The preceding owned-transfer pin deliberately
                        // poisoned model RAM to prove it was untouched.
                        // Restore the MODEL arm's captured upload here, so
                        // playback receives the same input asset in both arms.
                        std::copy(model_bytes.begin(), model_bytes.end(), spu.SpuRam() + sample_address);
                        // Actual command-boundary oracle. Both arms receive
                        // the same already-loaded bank and EE service calls.
                        // The comparison arm runs the original emitters,
                        // STREAM command bodies, libsd queue and common mixer.
                        g_fail += RunSampleControlOracle(ctx, spu, audio, bank.id);
                    }
                    bank_bytes += bank.length;
                }
            }
            Prep(ctx); SetArg(ctx, 4, 3); LoaderFileIo::Close(ctx);
            if (owned) {
                Prep(ctx); SetArg(ctx, 4, bank.id); DispatchTable::Invoke(0x1e75d8, ctx);
                CHECK(!audio.HasSound(bank.id), "bank_free_removes_asset");
            }
        }
        sample_address += bank.length;
    }
    std::printf("SoundBankSelfOracle: requests=%zu bytes=%llu failures=%d\n", banks.size(),
                static_cast<unsigned long long>(bank_bytes), g_fail);

    // OLD SOUND_P6: a whole batch of bank jobs must not collapse to one last
    // descriptor. Every read uses a distinct game-owned FileStream object.
    const auto bank_deliveries = title::SoundBanks::Count().delivered;
    const auto rpc_before_banks = face.Calls(), cd_before_banks = plugin.Cdvdman().ReadsIssued();
    memory::write<int32_t>(kHandleCounter, 3);
    Prep(ctx); SetArg(ctx, 4, 0x00500000); LoaderFileIo::Open(ctx);
    for (uint32_t index = 0; index < 120; ++index) {
        const uint32_t object = 0x00530000 + index * 0x80;
        memory::write<uint32_t>(object, 0);
        memory::write<int32_t>(object + LoaderFileIo::kFsHandle, 3);
        Prep(ctx); SetArg(ctx, 4, object); SetArg(ctx, 5, 0x9000 + index);
        SetArg(ctx, 6, banks[1].offset); SetArg(ctx, 7, banks[1].length);
        DispatchTable::Invoke(0x2ada68, ctx);
        CHECK(ctx.cpuRegs.GPR.r[2].SL[0] == 0, "bank_batch_request_accepted");
    }
    CHECK(audio.Inspect().samples.empty(), "bank_batch_not_published_before_delivery");
    Prep(ctx); SetArg(ctx, 4, 0x00530000 + 119 * 0x80); LoaderFileIo::WaitReadComplete(ctx);
    CHECK(audio.Inspect().samples.size() == 120 &&
              title::SoundBanks::Count().delivered - bank_deliveries == 120,
          "bank_batch_keeps_all_120_requests");
    for (uint32_t index = 0; index < 120; ++index) {
        Prep(ctx); SetArg(ctx, 4, 0x9000 + index); DispatchTable::Invoke(0x1e8278, ctx);
        CHECK(audio.Inspect().samples.at(0x9000 + index).ready, "bank_batch_ack_after_its_bytes");
        Prep(ctx); SetArg(ctx, 4, 0x9000 + index); DispatchTable::Invoke(0x1e75d8, ctx);
    }
    // Free while pending invalidates the asset ticket but cannot detach its
    // delivery owner. Close then cancels the typed file job before reuse.
    const uint32_t pending_object = 0x00530000;
    Prep(ctx); SetArg(ctx, 4, pending_object); SetArg(ctx, 5, 0x8000);
    SetArg(ctx, 6, banks[0].offset); SetArg(ctx, 7, banks[0].length);
    DispatchTable::Invoke(0x2ada68, ctx);
    Prep(ctx); SetArg(ctx, 4, 0x8000); DispatchTable::Invoke(0x1e75d8, ctx);
    CHECK(!title::SoundBanks::Install(capture, install, nullptr), "bank_pending_delivery_keeps_owner_attached");
    const auto cancellations = title::SoundBanks::Count().cancelled;
    const uint32_t second_pending = pending_object + 0x80;
    Prep(ctx); SetArg(ctx, 4, second_pending); SetArg(ctx, 5, 0x8001);
    SetArg(ctx, 6, banks[1].offset); SetArg(ctx, 7, banks[1].length);
    DispatchTable::Invoke(0x2ada68, ctx);
    title::Audio::State audio_state;
    CHECK(!audio.Capture(audio_state), "bank_live_ticket_requires_file_owner");
    Prep(ctx); SetArg(ctx, 4, 3); LoaderFileIo::Close(ctx);
    StreamLibrary::AdvanceBoundary(tick + 1000);
    CHECK(!audio.HasSound(0x8000) && !StreamLibrary::HasSoundReads(), "bank_closed_read_cannot_publish_late");
    CHECK(audio.Capture(audio_state) && title::SoundBanks::Count().cancelled == cancellations + 1,
          "bank_close_cancels_unfreed_ticket_before_late_delivery");
    CHECK(face.Calls() == rpc_before_banks && plugin.Cdvdman().ReadsIssued() == cd_before_banks,
          "bank_batch_and_cancel_need_no_stream_or_iop_cd_work");
    CHECK(title::SoundBanks::Install(capture, install, nullptr), "bank_owner_detached_before_destruction");
    LoaderFileIo::ResetState();

    return g_fail;
}
