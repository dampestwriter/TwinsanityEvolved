// games/slus_20909_crash_twinsanity/iop/stream_irx/feed_contract.h --
// FeedContract: the SPU2 feed contract between STREAM.IRX's libsd calls and the
// sound core (Phase 4E consumes it). Each field names one observable the module
// relies on; the LibsdModule records them, the sound core honours them. Numbers
// are libsd ordinals (libsd_stub_N in the generated TU) and SPU2 register facts.
#pragma once

#include <cstdint>

namespace ps2r::title::stream_irx {

struct FeedContract {
    // 1. SdInit (ordinal 0) is called once at module init; the core is idle before it.
    static constexpr int kInitOrdinal = 0;
    // 2. SdSetParam (1) / 3. SdGetParam (2): voice/core register store; GetParam reads back what
    //    SetParam wrote (95 SetParam call sites: the module programs every voice through it).
    static constexpr int kSetParamOrdinal = 1;
    static constexpr int kGetParamOrdinal = 2;
    // 4. ENVX (param register 5 of a voice) reads 0 while the voice is not keyed on; the CD-worker
    //    watchdog aborts a primed stream record when ENVX is 0 -- the core must raise it on key-on.
    static constexpr uint32_t kEnvxParam = 5;
    // 5. SdSetSwitch (3) / 6. SdGetSwitch (4): KON/KOFF/PMON/NON/VMIXL... bitmasks; GetSwitch of
    //    ENDX reflects voices that reached their loop end (the streamer's block flip signal).
    static constexpr int kSetSwitchOrdinal = 3;
    static constexpr int kGetSwitchOrdinal = 4;
    // 7. SdSetAddr (5) / 8. SdGetAddr (6): SSA/LSAX/NAX byte addresses; GetAddr(NAX) advances
    //    through the ring while playing -- the streamer computes its next block from it.
    static constexpr int kSetAddrOrdinal = 5;
    static constexpr int kGetAddrOrdinal = 6;
    // 9. SdSetCoreAttr (7): core attributes (effect enable, IRQ enable, mute...). Idle result 0.
    static constexpr int kSetCoreAttrOrdinal = 7;
    // 10. sceSdProcBatch (8): a batch of set/get param records; idle result 0 (none processed).
    static constexpr int kProcBatchOrdinal = 8;
    // 11. SdVoiceTrans (9): IOP RAM -> SPU RAM DMA of `size` bytes (5th arg on the stack, +0x10);
    //     returns the byte count, and 12. the transfer completes asynchronously: SdVoiceTransStatus
    //     (11) returns 1 when idle/complete, 0 while in flight.
    static constexpr int kVoiceTransOrdinal = 9;
    static constexpr int kVoiceTransStatusOrdinal = 11;
    static constexpr uint32_t kVoiceTransSizeStackOffset = 0x10;
    // 13. SdBlockTrans (10) / SdBlockTransStatus (12): the block (PCM) path; idle result 0.
    static constexpr int kBlockTransOrdinal = 10;
    static constexpr int kBlockTransStatusOrdinal = 12;
    // 14. SdSetEffectAttr (13) / SdClearEffectWorkArea (14): reverb; idle result 0.
    static constexpr int kSetEffectAttrOrdinal = 13;
    static constexpr int kClearEffectWorkAreaOrdinal = 14;
    // 15. SdSetTransIntrHandler (15): (chan, fn, arg) -- the module's completion handler at module
    //     offset 0x13a8 clears the caller's busy flag and signals the worker semaphore; the core
    //     fires it on a fresh IopContext ($a0 = chan, $a1 = arg, $gp = module gp) after each
    //     VoiceTrans completes, never under a sound-core lock and never overlapping thread code.
    static constexpr int kSetTransIntrHandlerOrdinal = 15;
    static constexpr uint32_t kTransIntrHandlerOffset = 0x13a8;
    // 16. The pending voice-transfer sequence numbers (one per core) are transient: a savestate
    //     restore resets them to 0 (LibsdModule::ResetPendingSequence via the IOPM after_restore).
    static constexpr int kCores = 2;
};

}  // namespace ps2r::title::stream_irx
