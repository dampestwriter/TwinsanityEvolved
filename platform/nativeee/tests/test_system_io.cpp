// test_system_io.cpp -- system.h (Initialise / StartServices construction order and idempotence, Language, LocalTime,
// kputs) and io.h (no-op restart, positive driver ids, the opaque IOP heap budget) and the stream boot façade over a
// generated ISO (synchronous reads into guest memory, FileSize, the buffer budget).
#include <cstdio>

#include "ps2e_seam.h"
#include "te_language.h"
#include "te_test.h"
#include "te_test_iso.h"

namespace {
constexpr uint32_t kPath = 0x1000, kBuf = 0x100000;
}

int main() {
    using namespace te_test;
    const std::vector<uint8_t> bh = Pattern(70000, 11);
    const std::string iso = WriteTestIso({{"CRASH6", "CRASH.BH", bh}, {"", "SLES_525.68", Pattern(100, 12)}});
    CHECK(!iso.empty());

    // --- system: the boot order
    te::Settings s = DefaultSettings();
    s.iso_path = iso;
    s.language = 4;   // German
    s.stream_instant = true;   // this test's stream rows assert the instant model (the drive model: test_stream)
    Reset(s);
    te::Services& services = te::Services::Get();
    CHECK(!services.Initialised());
    ps2e_system_initialise();
    CHECK(services.Initialised());
    CHECK(!services.Started());
    CHECK(services.disc().IsOpen());
    CHECK_EQ(ps2e_disc_wait_ready(), 2);
    ps2e_system_start_services();
    CHECK(services.Started());
    ps2e_system_initialise();      // idempotent
    ps2e_system_start_services();
    CHECK_EQ(ps2e_system_language(), 4);
    // The pads are up after StartServices: the setup frames then find a DualShock 2
    CHECK_EQ(ps2e_pads_open(0, 0, 0x5000), 1);
    ps2e_graphics_wait_v_sync();   // the boot façade's boundary (one field)
    CHECK_EQ(ps2e_pads_get_state(0, 0), 6);
    CHECK_EQ(services.clock().BoundaryCount(), 1u);
    ps2e_graphics_present(0);
    ps2e_graphics_present_from_interrupt(0);
    CHECK_EQ(services.clock().BoundaryCount(), 3u);
    ps2e_time_initialise();
    CHECK_EQ(ps2e_time_ticks(), 0u);
    ps2e_graphics_present(0);
    CHECK_EQ(ps2e_time_ticks(), 9600u);
    // Language out of range reads English
    {
        te::Settings bad = s; bad.language = 42;
        te::SetSettings(bad);
        CHECK_EQ(ps2e_system_language(), 1);
        te::SetSettings(s);
    }
    // kputs: a guest string to stderr (no crash on an unmapped address)
    WriteString(kPath, "[game] hello from kputs\n");
    ps2e_system_kputs(kPath);
    ps2e_system_kputs(0x7FFFFFF0u);

    // --- io
    ps2e_io_initialise();
    WriteString(kPath, "cdrom0:\\CRASH6\\SYS\\IOPRP300.IMG");
    ps2e_io_restart(kPath);
    WriteString(kPath, "cdrom0:\\CRASH6\\SYS\\STREAM.IRX");
    const int32_t id1 = ps2e_io_load_driver(kPath);
    const int32_t id2 = ps2e_io_load_driver(kPath);
    CHECK(id1 > 0 && id2 > id1);
    ps2e_io_initialise_heap();
    const uint32_t a = ps2e_io_allocate_heap(1000);
    const uint32_t b = ps2e_io_allocate_heap(0x10000);
    CHECK(a != 0 && b != 0 && b > a);
    CHECK_EQ(a & 15u, 0u);
    CHECK(a < 0x02000000u || a >= 0x00100000u);   // an IOP address, not a guest pointer the game could dereference
    CHECK_EQ(ps2e_io_allocate_heap(0), 0u);
    CHECK_EQ(ps2e_io_allocate_heap(0x7FFFFFFF), 0u);
    CHECK_EQ(ps2e_io_free_heap(b), 0);
    CHECK_EQ(ps2e_io_free_heap(b), -1);
    const uint32_t c = ps2e_io_allocate_heap(16);
    CHECK_EQ(c, b);   // the top block's bytes came back
    CHECK_EQ(ps2e_io_free_heap(a), 0);

    // --- stream boot façade
    CHECK_EQ(ps2e_stream_initialise(9, 0x8000), 0);
    CHECK_EQ(ps2e_stream_get_state(), 1);
    CHECK_EQ(ps2e_stream_update(), 0);
    const int32_t free0 = ps2e_stream_free_buffer_memory();
    ps2e_stream_attach_buffer(0, 0x20000, 0, 0);
    CHECK_EQ(ps2e_stream_free_buffer_memory(), free0 - 0x20000);
    ps2e_stream_detach_buffer(0);
    CHECK_EQ(ps2e_stream_free_buffer_memory(), free0);
    WriteString(kPath, "Crash6/crash.bh");
    const int32_t f = ps2e_stream_open_file(kPath);
    CHECK(f >= 0);
    CHECK_EQ(ps2e_stream_file_size(), 70000);
    WriteString(kPath, "Crash6/none.bh");
    const int32_t g = ps2e_stream_open_file(kPath);
    CHECK(g == f + 1);
    CHECK_EQ(ps2e_stream_file_size(), 0);
    // Reads complete before they return: the bytes are there, nothing is reading, Wait is 0
    ps2e_stream_read(2, f, 2048, 4096, kBuf);
    CHECK_EQ(ps2e_stream_is_reading(2), 0);
    CHECK_EQ(ps2e_stream_wait(2), 0);
    CHECK(std::memcmp(te::GuestPointer(kBuf, 4096), bh.data() + 2048, 4096) == 0);
    ps2e_stream_read(2, f, 69000, 4096, kBuf);   // clipped at the end
    CHECK(std::memcmp(te::GuestPointer(kBuf, 1000), bh.data() + 69000, 1000) == 0);
    ps2e_stream_read(2, g, 0, 16, kBuf);          // no such file: nothing
    ps2e_stream_read(2, f, 0, 16, 0x7FFFFFF0u);   // bad destination: nothing, no crash
    ps2e_stream_read_sound_bank(3, 1, f, 0, 100); // dropped
    ps2e_stream_close_file(f);
    ps2e_stream_read(2, f, 0, 16, kBuf);          // closed: nothing

    // --- stream without a disc: Initialise fails
    {
        te::Settings none = DefaultSettings(); none.iso_path = "";
        Reset(none);
        CHECK_EQ(ps2e_stream_initialise(9, 0x8000), -1);
        CHECK_EQ(ps2e_stream_get_state(), 0);
    }
    te::Services::ResetForTest();
    std::remove(iso.c_str());
    // --- the language setting -- content recognition and the disc's banks
    {
        std::string probe;
        CHECK(te::language::RecognizeContent("off\r\non\r\nno\r\nyes\r\nback\r\ncontinue\r\nmono\r\nstereo\r\ndolby pro logic ii\r\n\r\nnew game\r\nload game\r\noptions\r\nlanguage\r\ngraphic options\r\n", &probe) == "english");
        CHECK(probe == "new game");
        CHECK(te::language::RecognizeContent("desativar\nativar\nn\xe3o\nsim\nvoltar\ncontinuar\nmono\nst\xe9" "reo\ndolby pro logic ii\n\nnovo jogo\ncarregar jogo\nop\xe7\xf5" "es\nidioma\n", &probe) == "portuguese");
        CHECK(probe == "novo jogo");
        CHECK(te::language::RecognizeContent("aus\nein\nnein\nja\nzur\xfc\x63k\nweiter\nmono\nstereo\ndolby pro logic ii\n\nneues spiel\nspiel laden\noptionen\nsprache\n", nullptr) == "german");
        CHECK(te::language::RecognizeContent("off\non\nno\nyes\n", nullptr) == "");   // too little to tell (English's and Spanish's first lines coincide)
        // A disc whose `English` slot carries Portuguese and whose `Spanish` slot carries the English text (the user's image):
        // the speech banks Crash6\<slot>.mb are the retail ones (English.mb English, Spanish.mb Spanish; none for French here)
        auto bank = [](const std::string& name, const std::string& text, std::vector<uint8_t>& bh, std::vector<uint8_t>& bd) {
            const std::string path = "Language\\Code\\" + name + ".txt";
            auto put32 = [&bh](uint32_t v) { for (int i = 0; i < 4; ++i) bh.push_back(uint8_t(v >> (8 * i))); };
            put32(uint32_t(path.size()));
            bh.insert(bh.end(), path.begin(), path.end());
            put32(uint32_t(bd.size()));
            put32(uint32_t(text.size()));
            bd.insert(bd.end(), text.begin(), text.end());
            bd.resize((bd.size() + 2047) & ~size_t(2047));
        };
        std::vector<uint8_t> bh = {4, 0, 0, 0}, bd;
        bank("English", "desativar\nativar\nn\xe3o\nsim\nvoltar\ncontinuar\nmono\nst\xe9" "reo\ndolby\n\nnovo jogo\ncarregar jogo\nop\xe7\xf5" "es\nidioma\n", bh, bd);
        bank("French", "off\non\nnon\noui\nretour\ncontinuer\nmono\nst\xe9" "r\xe9" "o\ndolby\n\nnouvelle partie\ncharger\noptions\nlangue\n", bh, bd);
        bank("Spanish", "off\non\nno\nyes\nback\ncontinue\nmono\nstereo\ndolby\n\nnew game\nload game\noptions\nlanguage\ngraphic options\n", bh, bd);
        bank("Startup\\Fonts\\x", "not a bank", bh, bd);
        const std::string disc = WriteTestIso({{"CRASH6", "CRASH.BH", bh}, {"CRASH6", "CRASH.BD", bd}, {"", "SLES_525.68", Pattern(100, 12)},
                                               {"CRASH6", "ENGLISH.MB", Pattern(4096, 3)}, {"CRASH6", "SPANISH.MB", Pattern(4096, 4)}});
        std::vector<te::language::Bank> banks;
        std::string error;
        CHECK(te::language::EnumerateFromDisc(disc, {}, banks, &error));
        CHECK(banks.size() == 3);   // the PAL slots the disc has Code banks for: English 0, French 1, Spanish 3
        if (banks.size() == 3) {
            CHECK(banks[0].slot == 0 && banks[0].name == "English" && banks[0].content == "portuguese" && banks[0].recognised && banks[0].code == 1);
            CHECK(banks[0].speech && banks[0].speech_language == "english");
            CHECK(banks[1].slot == 1 && banks[1].content == "french" && banks[1].code == 2 && !banks[1].speech);
            CHECK(banks[2].slot == 3 && banks[2].name == "Spanish" && banks[2].content == "english" && banks[2].code == 3 && banks[2].speech);
        }
        // the languages -- one per TEXT language, english first; the voice slot is the slot NAMED for it with a speech
        // bank, else English's (French's speech bank is missing here -> English's); then one per SPEECH bank whose language has
        // no text (Spanish.mb with the English text in the `Spanish` slot): its own slot for the voice, English's text slot
        const std::vector<te::language::Language> langs = te::language::LanguagesOf(banks);
        CHECK(langs.size() == 4);
        if (langs.size() == 4) {
            CHECK(langs[0].name == "english" && langs[0].text_slot == 3 && langs[0].voice_slot == 0 && langs[0].label == "english");
            CHECK(langs[1].name == "portuguese" && langs[1].text_slot == 0 && langs[1].voice_slot == 0 && langs[1].label == "portuguese (english voice)");
            CHECK(langs[2].name == "french" && langs[2].text_slot == 1 && langs[2].voice_slot == 0 && langs[2].label == "french (english voice)");
            CHECK(langs[3].name == "spanish" && langs[3].text_slot == 3 && langs[3].voice_slot == 3 && langs[3].text_language == "english" &&
                  langs[3].voice_language == "spanish" && langs[3].label == "spanish (english text)");
        }
        // A speech-only language on a disc with no English text: its own slot's text
        {
            std::vector<te::language::Bank> two;
            te::language::Bank fr;
            fr.slot = 1; fr.name = "French"; fr.content = "french"; fr.recognised = true; fr.code = 2; fr.speech = true; fr.speech_language = "french";
            te::language::Bank es;
            es.slot = 3; es.name = "Spanish"; es.content = "french"; es.recognised = true; es.code = 3; es.speech = true; es.speech_language = "spanish";
            two.push_back(fr);
            two.push_back(es);
            const std::vector<te::language::Language> l2 = te::language::LanguagesOf(two);
            CHECK(l2.size() == 2);
            if (l2.size() == 2) {
                CHECK(l2[0].name == "french" && l2[0].text_slot == 1 && l2[0].voice_slot == 1 && l2[0].label == "french");
                CHECK(l2[1].name == "spanish" && l2[1].text_slot == 3 && l2[1].voice_slot == 3 && l2[1].text_language == "french" && l2[1].label == "spanish (french text)");
            }
        }
        // The resolution through the settings: english -> text slot 3 (the Spanish slot's English text), Language() 1 (the
        // English slot: its speech bank, FMV track, voices, pictures); a raw slot / code stands; the Options menu's choice
        te::Settings lang = s;
        lang.iso_path = disc;
        lang.language = -1;
        lang.language_name = "english";
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 1);
        CHECK_EQ(ps2e_system_language_text_bank(), 3);
        CHECK_EQ(ps2e_system_language_count(), 4);
        CHECK_EQ(ps2e_system_language_selected(), 0);
        CHECK(te::language::Label(0) == "english");
        CHECK(te::language::Label(1) == "portuguese (english voice)");
        CHECK(te::language::Label(3) == "spanish (english text)");
        lang.language_name = "portuguese";
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 1);
        CHECK_EQ(ps2e_system_language_text_bank(), 0);
        lang.language_name = "italian";   // no such language on this disc: english
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 1);
        CHECK_EQ(ps2e_system_language_text_bank(), 3);
        lang.language_name = "spanish";   // no Spanish TEXT on this disc, but Spanish.mb: the Spanish slot's voice, English's text
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 3);
        CHECK_EQ(ps2e_system_language_text_bank(), 3);
        CHECK_EQ(ps2e_system_language_selected(), 3);
        lang.language_name.clear();
        lang.language = 3;   // a raw console code: the Spanish slot for everything, as a console set to Spanish ran retail
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 3);
        CHECK_EQ(ps2e_system_language_text_bank(), 3);
        CHECK_EQ(ps2e_system_language_count(), 4);   // the raw entry is not a row
        lang.language = -1;
        lang.language_bank = 0;   // a raw slot (the pad logs' pin): slot 0 for everything
        te::SetSettings(lang);
        te::language::Reset();
        CHECK_EQ(ps2e_system_language(), 1);
        CHECK_EQ(ps2e_system_language_text_bank(), 0);
        ps2e_system_set_language(2);   // the Options menu picked the third row (french): Language() + the text bank follow, the setting is its name
        CHECK_EQ(ps2e_system_language(), 1);   // French's speech bank is missing on this disc: English's slot
        CHECK_EQ(ps2e_system_language_text_bank(), 1);
        CHECK(te::GetSettings().language_name == "french");
        te::language::Reset();
        te::SetSettings(s);
    }

    return Finish("test_system_io");
}
