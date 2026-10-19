// test_harness.cpp -- te_harness.h: the perceptual descriptor is a pure function of the image, equal between the C++
// (what the port's leg writes) and tools/oracle/pdesc.py (what diff.py computes for the oracle's PPMs); RGBA and RGB
// inputs describe alike; the GameController state word decodes as --trace-state does.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "te_harness.h"
#include "te_test.h"

#if defined(_WIN32)
#define TE_POPEN _popen   // the UCRT spells the POSIX pair with an underscore
#define TE_PCLOSE _pclose
#define TE_PYTHON "python"   // the Windows name (python.org / winget); python3 is not one there
#else
#define TE_POPEN popen
#define TE_PCLOSE pclose
#define TE_PYTHON "python3"
#endif

namespace {

// A synthetic frame: a horizon (bright warm sky over a dark blue sea), a gradient, a few flat rectangles -- distinct bands
void Paint(std::vector<uint8_t>& rgba, int w, int h) {
    rgba.assign(size_t(w) * h * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            if (y < h / 2) { p[0] = uint8_t(200 - y / 4); p[1] = uint8_t(160 + (x * 60) / w); p[2] = 90; }
            else { p[0] = 20; p[1] = uint8_t(40 + (y - h / 2) / 8); p[2] = uint8_t(120 + x / 16); }
            if (x > w / 3 && x < w / 2 && y > h / 4 && y < h / 3) { p[0] = 255; p[1] = 255; p[2] = 255; }
            if (x > 3 * w / 4 && y > 2 * h / 3) { p[0] = 0; p[1] = 0; p[2] = 0; }
            p[3] = 0x80;
        }
}

std::string Run(const std::string& cmd) {
    std::string out;
    FILE* p = TE_POPEN(cmd.c_str(), "r");
    if (!p) return out;
    char buf[512];
    while (fgets(buf, sizeof buf, p)) out += buf;
    TE_PCLOSE(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

}  // namespace

int main() {
    using namespace te_test;
    const int w = 128, h = 112;   // the NTSC shape at a quarter scale: 25 % / 30 % ... bounds fall on fractions like 512x448
    std::vector<uint8_t> rgba;
    Paint(rgba, w, h);
    const std::string row = te::harness::Describe(rgba.data(), w, h, 4);
    std::printf("describe: %s\n", row.c_str());
    CHECK(!row.empty());
    // RGB input (a PPM's) describes identically to RGBA
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; ++i) std::memcpy(&rgb[i * 3], &rgba[i * 4], 3);
    CHECK(te::harness::Describe(rgb.data(), w, h, 3) == row);
    // Fields: w,h then three band luminances, means, clear share, 16 hex digits
    {
        int fw = 0, fh = 0;
        double sky = 0, mid = 0, ground = 0, r = 0, g = 0, b = 0, share = 0;
        char hex[32] = {};
        CHECK_EQ(std::sscanf(row.c_str(), "%d,%d,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%31s", &fw, &fh, &sky, &mid, &ground, &r, &g, &b, &share, hex), 10);
        (void)mid; (void)r; (void)g; (void)b;
        CHECK_EQ(fw, w);
        CHECK_EQ(fh, h);
        CHECK(sky > ground);              // the bright sky band over the dark sea band
        CHECK(share > 0.0 && share < 0.5);
        CHECK_EQ(std::strlen(hex), 16u);
    }
    // A black frame: every band 0, one colour = share 1, dhash 0
    {
        std::vector<uint8_t> black(size_t(w) * h * 4, 0);
        char expect[128];
        std::snprintf(expect, sizeof expect, "%d,%d,0.00,0.00,0.00,0.00,0.00,0.00,1.0000,0000000000000000", w, h);
        CHECK(te::harness::Describe(black.data(), w, h, 4) == std::string(expect));
    }
    // The Python mirror (tools/oracle/pdesc.py) over the same image as a PPM gives the same row
    {
        // ctest sets TE_SOURCE_DIR; run by hand, the tree is two directories above this file
        std::string src = std::getenv("TE_SOURCE_DIR") ? std::getenv("TE_SOURCE_DIR") : std::string(__FILE__).substr(0, std::string(__FILE__).find("/platform/nativeee/tests/"));
        const std::string dir = (std::filesystem::temp_directory_path() / "te_test_harness").string();
        std::filesystem::create_directories(dir);
        const std::string ppm = dir + "/f123.ppm";
        {
            std::ofstream f(ppm, std::ios::binary);
            f << "P6\n" << w << " " << h << "\n255\n";
            f.write(reinterpret_cast<const char*>(rgb.data()), long(rgb.size()));
        }
        // double quotes: both sh and cmd.exe read them
        const std::string py = Run(std::string(TE_PYTHON) + " \"" + src + "/tools/oracle/pdesc.py\" \"" + ppm + "\"");
        std::printf("pdesc.py: %s\n", py.c_str());
        CHECK(py == "123," + row);
        std::remove(ppm.c_str());
    }
    // The state word: --trace-state's decode (bits 44..49 state, 38..43 last, 50..55 next)
    CHECK(std::string(te::harness::StateName(7)) == "Title");
    CHECK(std::string(te::harness::StateName(12)) == "Playing");
    CHECK(std::string(te::harness::StateName(60)) == "?");
    Reset(DefaultSettings());
    unsigned s = 1, l = 1, n = 1;
    CHECK(!te::harness::ReadGameState(&s, &l, &n));   // no controller in the test memory (G_GameController is 0)
    // A controller at 0x400000 whose word says state Title (7), lastState LoadingTitle (6), nextState MainMenu (8)
    WriteValue<uint32_t>(0x00309888, 0x400000);
    WriteValue<uint64_t>(0x400008, (uint64_t(6) << 38) | (uint64_t(7) << 44) | (uint64_t(8) << 50));
    CHECK(te::harness::ReadGameState(&s, &l, &n));
    CHECK_EQ(s, 7u);
    CHECK_EQ(l, 6u);
    CHECK_EQ(n, 8u);
    return Finish("test_harness");
}
