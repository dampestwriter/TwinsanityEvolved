#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace title {

// The transport owner: the title's MultiStream client
// batches its commands once a frame and ships them to the module's RPC server
// (server 0x12345, function 0) with a 0x800-byte reply the client's reply
// processor reads back into its statics. Under service-only startup no module
// exists: this owner is the server. It consumes the batch words directly (they
// are the title's own command list, TR multistream.h: a version/count header,
// then per command its opcode, its word count and its words), answers each
// command by the owner that already holds its intent, and publishes the reply
// words the processor expects. The library owners (StreamLibrary, SoundBanks,
// SoundControls, MusicLibrary, MusicBuffers) republish their own state into
// the title's statics after every reply (their 0x1e1f48 hooks), so the reply
// carries only what this owner holds: the status counter (a new reply), no
// in-service records (every stream the title still plays is owned), no load
// request, no disc error, the registry it recorded. No SIF, no IOP thread, no
// round-trip charge; nothing is modelled of the module's job engine.
//
// A command whose producer has no owner in the composed selection is counted
// per opcode as `unowned` (a reply is still produced so the client's busy word
// clears) -- the census row the startup pin requires at zero.
class StreamTransport {
public:
    // The composed owners the root names (each true = the producer of that
    // command family is selected and never reaches the batch; when it does
    // reach the batch anyway the command is unowned here).
    struct Owners {
        bool files = false;        // StreamLibrary + FileServices: 0x1F 0x20 0x29 0x31 0x32 0x1 (loads)
        bool banks = false;        // SoundBanks: 0x1C 0x25 0x28 0xE 0x103
        bool controls = false;     // SoundControls: 0x2 0x3 0x4 0x39 0x5C 0x60, and 0x7 0xA 0xB applied natively
        bool music = false;        // MusicLibrary: 0x5 0x2B 0x2C 0x2E 0x2F 0x63
        bool buffers = false;      // MusicBuffers: 0xD 0xF 0x2D 0x52
        bool movie_pcm = false;    // the Movie PCM sink (SoundOutput::BindMovie): 0x3B 0x3C
    };
    // EE RAM word write (the wait-update word of 0x49; injected so the library
    // holds no EE memory dependency).
    using WriteWord = std::function<void(uint32_t addr, uint32_t value)>;

    static bool Install(bool selected, Owners owners, WriteWord write_word = {});
    static bool Enabled();
    static void Reset();

    // The wire sizes the title uses (TR multistream.h TransferSize).
    static constexpr size_t kTransferBytes = 0x800;
    static constexpr uint16_t kVersion = 72;   // SOUND_MULTISTREAM_VERSION_ID 7.2
    static constexpr uint32_t kServerId = 0x12345;

    // Serves one batch: decodes `request` (request_size bytes of u16 words),
    // answers every command, writes the reply into `reply` (reply_size bytes,
    // zero-filled beyond what is published). False = the header is malformed
    // or the sizes are not the title's (nothing served, `malformed` counted).
    static bool Serve(const uint8_t* request, size_t request_size, uint8_t* reply, size_t reply_size);

    struct Registry {
        std::array<uint16_t, 8> disc_types{};   // 0x2A in request order (DiscDvd, DiscSpinStream, ...)
        uint8_t disc_type_count = 0;
        uint16_t load_type = 0, max_files = 0, max_sounds = 0;   // 0xC
        bool stream_data_initialised = false;
        uint32_t stream_count = 0;                 // 0x10
        bool fast_load_bound = false;              // 0x51 FastLoadBindRpc
        uint16_t fast_load_mode = 0;               // 0x51 FastLoadOnOff value
        uint32_t wait_update_word = 0;             // 0x49 (0 = closed)
        uint32_t wait_updates = 0;                 // written to that word, once per batch served
        uint16_t last_status_stream = 0xFFFF;      // 0x9 (NoStreamAllowed when none may load)
    };
    static const Registry& Inspect();

    struct Counters {
        uint64_t batches = 0, commands = 0, status = 0, registry = 0, global = 0, effects = 0,
                 compactions = 0, movie = 0, recovery = 0, unowned = 0, malformed = 0;
        uint32_t reply_counter = 0;   // the last reply's status counter
    };
    static Counters Count();
    // `installed=1,batches=..,...,unowned=..,malformed=..` and the per-opcode
    // unowned tally `0x1f:3,0x29:3` (or `none`).
    static std::string Census();
    static std::string UnownedCensus();

    // The reply layout (u32 word indices into the 0x800-byte reply), derived from
    // TR multistream.h: the record count word, ReplyStatus at word 1 (its
    // CHECK_OFFSETs: channelStates +5, maxIopMemory +49, discErrors +57,
    // iopBufferCount +66), ReplyChannels right after the (empty) IOP buffer
    // list, ReplyEnd right after the (empty) DSP transfer list.
    static constexpr size_t kWordRecordCount = 0;
    static constexpr size_t kWordCounter = 1;
    static constexpr size_t kWordLoadRequest = 2;
    static constexpr size_t kWordDiscBusy = 3;
    static constexpr size_t kWordSpuWriteAddress = 4;
    static constexpr size_t kWordStereoNotStarted = 5;
    static constexpr size_t kWordChannelStates = 6;    // 3 words
    static constexpr size_t kWordMaxIopMemory = 50;
    static constexpr size_t kWordDiscErrors = 58;
    static constexpr size_t kWordIopBufferCount = 67;
    static constexpr size_t kWordChannels = 68;        // ReplyChannels (no IOP buffers)
    static constexpr size_t kWordDspTransferCount = 130;
    static constexpr size_t kWordEnd = 131;            // ReplyEnd (no DSP transfers)
    static constexpr size_t kReplyWords = 135;
};

} // namespace title
