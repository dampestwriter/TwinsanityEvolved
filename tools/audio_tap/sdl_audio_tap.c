// sdl_audio_tap.c -- the device-side audio tap: records what an SDL2 program's audio device actually plays.
//
// Interposes SDL_OpenAudioDevice (macOS: DYLD_INSERT_LIBRARIES + __interpose; Linux: LD_PRELOAD + dlsym(RTLD_NEXT)) and
// wraps the program's callback: every buffer the device pulls is written to a WAV file as it is handed to the device.
// The program is untouched -- the tap works on any build (the frozen play binaries, the recomp oracle), paced by the
// real device clock, so what lands in the file is exactly what the speakers get: pitch, pads, gaps and skips included.
//
//   cc -O2 -shared -fPIC -o libsdl_audio_tap.dylib sdl_audio_tap.c -L<sdl lib dir> -lSDL2   (tools/audio_tap/build.sh)
//   SDL_AUDIO_TAP=/abs/out.wav DYLD_INSERT_LIBRARIES=/abs/libsdl_audio_tap.dylib <program> ...
//
// The WAV is written in the device's own format (float32 / s16 / s32, the channel count and rate SDL gave the
// program); the RIFF sizes are patched when the device closes and again at exit. SDL_AUDIO_TAP unset: pass-through.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#define TAP_INTERPOSE 1
#else
#define TAP_INTERPOSE 0
#include <dlfcn.h>
#endif

/* The SDL2 ABI pieces we need, spelled out so the tap builds without SDL headers. */
typedef uint32_t SDL_AudioDeviceID;
typedef uint16_t SDL_AudioFormat;
typedef void (*SDL_AudioCallback)(void* userdata, uint8_t* stream, int len);
typedef struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    SDL_AudioCallback callback;
    void* userdata;
} SDL_AudioSpec;

extern SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                             SDL_AudioSpec* obtained, int allowed_changes);
extern void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);

static FILE* g_file = NULL;
static SDL_AudioCallback g_inner = NULL;
static void* g_inner_user = NULL;
static SDL_AudioSpec g_spec;
static uint64_t g_bytes = 0;

static int BitsOf(SDL_AudioFormat f) { return f & 0xff; }
static int IsFloat(SDL_AudioFormat f) { return (f & 0x100) != 0; }

static void WriteHeader(void) {
    if (!g_file) return;
    const int bits = BitsOf(g_spec.format), channels = g_spec.channels;
    const uint16_t fmt = IsFloat(g_spec.format) ? 3 : 1;   /* WAVE_FORMAT_IEEE_FLOAT / PCM */
    const uint32_t data = (uint32_t)(g_bytes > 0xffffffffu - 44u ? 0xffffffffu - 44u : g_bytes);
    const uint32_t byte_rate = (uint32_t)g_spec.freq * (uint32_t)channels * (uint32_t)(bits / 8);
    const uint16_t block = (uint16_t)(channels * bits / 8), bps = (uint16_t)bits, ch = (uint16_t)channels;
    const uint32_t riff = 36u + data, rate = (uint32_t)g_spec.freq, fmt_size = 16;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmt_size, 4);
    memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4); memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block, 2); memcpy(h + 34, &bps, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(g_file, 0, SEEK_SET);
    fwrite(h, 1, 44, g_file);
    fseek(g_file, 0, SEEK_END);
    fflush(g_file);
}

static void TapCallback(void* userdata, uint8_t* stream, int len) {
    (void)userdata;
    if (g_inner) g_inner(g_inner_user, stream, len);
    else memset(stream, 0, (size_t)len);
    if (g_file && len > 0) {
        fwrite(stream, 1, (size_t)len, g_file);
        g_bytes += (uint64_t)len;
    }
}

static void Finish(void) {
    if (!g_file) return;
    WriteHeader();
    fclose(g_file);
    g_file = NULL;
    fprintf(stderr, "[sdl_audio_tap] wrote %llu bytes (%d Hz, %d ch, %s%d)\n", (unsigned long long)g_bytes, g_spec.freq,
            g_spec.channels, IsFloat(g_spec.format) ? "f" : "s", BitsOf(g_spec.format));
}

static SDL_AudioDeviceID TapOpen(SDL_AudioDeviceID (*real)(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int),
                                 const char* device, int iscapture, const SDL_AudioSpec* desired, SDL_AudioSpec* obtained,
                                 int allowed_changes) {
    const char* path = getenv("SDL_AUDIO_TAP");
    if (!path || !*path || iscapture || !desired || !desired->callback || g_file) {
        return real(device, iscapture, desired, obtained, allowed_changes);
    }
    SDL_AudioSpec want = *desired;
    g_inner = desired->callback;
    g_inner_user = desired->userdata;
    want.callback = TapCallback;
    want.userdata = NULL;
    SDL_AudioSpec got;
    memset(&got, 0, sizeof got);
    const SDL_AudioDeviceID dev = real(device, iscapture, &want, &got, allowed_changes);
    if (dev == 0) { g_inner = NULL; return 0; }
    /* allowed_changes == 0: SDL converts to `want`, the callback sees `want`; otherwise the device's `got`. */
    g_spec = allowed_changes ? got : want;
    if (g_spec.freq == 0) g_spec = got;
    g_file = fopen(path, "wb");
    if (g_file) {
        g_bytes = 0;
        WriteHeader();
        atexit(Finish);
        fprintf(stderr, "[sdl_audio_tap] recording the device to %s (%d Hz, %d ch, %s%d)\n", path, g_spec.freq, g_spec.channels,
                IsFloat(g_spec.format) ? "f" : "s", BitsOf(g_spec.format));
    } else {
        fprintf(stderr, "[sdl_audio_tap] cannot open %s\n", path);
    }
    if (obtained) *obtained = got;
    return dev;
}

#if TAP_INTERPOSE
static SDL_AudioDeviceID tap_SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                                 SDL_AudioSpec* obtained, int allowed_changes) {
    return TapOpen(SDL_OpenAudioDevice, device, iscapture, desired, obtained, allowed_changes);
}
static void tap_SDL_CloseAudioDevice(SDL_AudioDeviceID dev) {
    SDL_CloseAudioDevice(dev);
    Finish();
}
__attribute__((used)) static const struct { const void* replacement; const void* replacee; }
interposers[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void*)tap_SDL_OpenAudioDevice, (const void*)SDL_OpenAudioDevice},
    {(const void*)tap_SDL_CloseAudioDevice, (const void*)SDL_CloseAudioDevice},
};
#else
SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                      SDL_AudioSpec* obtained, int allowed_changes) {
    SDL_AudioDeviceID (*real)(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int) =
        (SDL_AudioDeviceID (*)(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int))dlsym(RTLD_NEXT, "SDL_OpenAudioDevice");
    return TapOpen(real, device, iscapture, desired, obtained, allowed_changes);
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) {
    void (*real)(SDL_AudioDeviceID) = (void (*)(SDL_AudioDeviceID))dlsym(RTLD_NEXT, "SDL_CloseAudioDevice");
    real(dev);
    Finish();
}
#endif
