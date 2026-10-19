// te_language.h -- the game's LANGUAGE as a real setting (ONE choice drives text AND speech).
//
// The retail game has ONE language index, g_CurrentLanguage (0..4 on the PAL executable: English, French, German, Spanish,
// Italian; 0 `American` on the US one), picked from the console's OSD language (Platform::System::Language() -> GameLanguageOf).
// EVERYTHING follows that index: the two text files Language\Code|AgentLab\<slot>.txt, the streamed speech bank
// Crash6\<slot>.mb/.mh (SetVoiceBank; every long dialogue line), the FMV's sound track (GameController::StateMovie:
// channel = g_CurrentLanguage), a level's voice table (CRASH.BD subsection VoicesSubsection + index), the title-card pictures
// (Language\Titles\<slot>\*.psm), the credits text. On a retail disc every component of slot N is in language N.
//
// The user's PAL image (SLES-52568) is a Brazilian-Portuguese fan patch of CRASH.BH/BD ONLY (2013 dates; every .mb, .mh and
// .pss is the 2004 retail file): it put Portuguese Code/AgentLab text into the `English` slot and the original English text into
// the `Spanish` slot, and touched nothing else -- so slot 0 = Portuguese text + English speech/FMV/pictures, slot 3 = English text
// + Spanish speech/FMV/pictures. No single retail index gives English text with English speech on that disc. The first language setting picked the text
// bank by content and let the whole index follow it (English text + Spanish voice; Portuguese text + English voice): the text/voice disagreement.
//
// a LANGUAGE is a tuple derived per disc -- (text slot, voice slot):
//   * text slot  = the slot whose Code text IS that language (recognised from its menu lines, as the first language setting did);
//   * voice slot = the slot NAMED for that language whose Crash6\<name>.mb speech bank the disc carries (retail names its speech
//                  banks truthfully: nothing in the patch touched them), else English's (American's) slot, else the text slot.
//   The voice slot is the RETAIL INDEX the game runs on (Language() answers its console code; speech bank, FMV track, level
//   voices, pictures, credits all follow it as retail did); the text slot goes to the game through LanguageTextBank() and only the
//   two text files read it. A language is offered when a text bank in it exists OR a speech bank in it exists: a speech-only
//   language (this disc's Spanish: Spanish.mb, Spanish title cards and credits, but the `Spanish` slot's text is English) takes
//   English's text slot (else its own slot's text) and says so in its row. The Options row is labelled by what the banks CONTAIN
//   ("english"; "portuguese (english voice)" when the speech is another language; "spanish (english text)" when the text is).
//   Default english. On a retail disc every tuple is (N, N) and the game behaves exactly as retail.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace te::language {

// A slot of the game's language table (g_LanguageNames order) as the disc fills it
struct Bank {
    uint32_t slot = 0;             // the game's language index
    std::string name;              // the slot's name as the executable names it ("English", "American")
    std::string content;           // the language its Code text is written in, lower case ("portuguese"); the slot name
                                   // lower-cased when not recognised
    bool recognised = false;       // the content was recognised from the text (false: the slot's name stands)
    std::string probe;             // the line the recognition read (the bank's "new game" text; the handoff's evidence)
    int32_t code = 1;              // the Platform::System::ConsoleLanguage the game's GameLanguageOf maps to this slot
    bool speech = false;           // Crash6\<name>.mb is on the disc: the retail speech bank this index streams
    std::string speech_language;   // the speech bank's language = the slot's name, lower case ("american" -> "english")
};

// A language the disc carries: what the player chooses
struct Language {
    std::string name;              // the language ("english"): the setting's value, the Options row's word -- the text's
                                   // language, or the speech's for a language the disc has speech but no text in
    int32_t text_slot = 0;         // the slot whose Code/AgentLab text files are in `name` (LanguageTextBank())
    int32_t voice_slot = 0;        // the retail index: speech bank, FMV sound track, level voices, pictures, credits (Language())
    std::string voice_language;    // the voice slot's speech language ("english")
    std::string text_language;     // the text slot's content ("english"): `name` for a text language, another for a speech-only one
    std::string label;             // the Options row: `name`, " (<voice> voice)" appended when the speech is another language,
                                   // " (<text> text)" when the text is
    bool raw = false;              // a raw slot (--language-bank N / --language <code>): text = voice = slot, as retail ran
};

struct Table {
    std::vector<Bank> banks;             // every slot the executable names that the disc carries a Code bank for, in slot order
    std::vector<Language> languages;     // the languages offered: english first, then text-slot order; a raw entry appended
                                         // when one was asked for
    std::string requested;               // the name asked for (settings), lower case
    int32_t selected = -1;               // index into `languages` (-1: none -- no disc / no banks)
    std::string how;                     // how the selection was made ("name", "default english", "raw slot N", "console code N")
    std::string error;                   // why enumeration found nothing (empty when it did)
};

// The disc's banks, the languages and the selection, enumerated once (lazily: the retail image and the disc must be in place
// -- after the runner loaded the image, before or after the services started). Re-resolved by Select / Reset.
const Table& Resolve();
// Forget the enumeration (a test, or a changed disc).
void Reset();
// Platform::System::Language(): the console code of the selected language's VOICE slot (1 = English when nothing is selected).
int32_t ConsoleLanguage();
// Platform::System::LanguageTextBank(): the selected language's TEXT slot; -1 when nothing is selected (the game then reads the
// slot Language() names, as retail did).
int32_t TextBank();
// Platform::System::LanguageCount / LanguageSelected / LanguageLabel: the Options row.
uint32_t Count();
int32_t Selected();
std::string Label(uint32_t language);
// Platform::System::SetLanguage: the player's choice from the Options menu -- language `index` is selected now and its name
// persisted as ps2r.ini `game.language` (when the settings name a file). Returns false (logged) when the index is unknown.
bool Select(uint32_t index);
// One line for the log / the handoff / the judge: the slots found, their contents and speech banks, the languages, the selection.
std::string Describe();

// The content recognition alone (public for the tests): the Code text's lines (the first few hundred bytes) -> the language
// name, "" when none matches; `probe` receives the line the decision rests on.
std::string RecognizeContent(const std::string& text, std::string* probe);
// The disc image's banks without the retail image (public for the tests / tools): slot order = the given names (or the
// PAL order when empty), filtered to the Code banks the BH names; `speech` set for the slots whose Crash6\<name>.mb exists.
bool EnumerateFromDisc(const std::string& iso_path, const std::vector<std::string>& slot_names, std::vector<Bank>& out,
                       std::string* error);
// The languages a set of banks offers (public for the tests): the tuple rule above, english first, then the text languages in
// slot order, then the speech-only languages in slot order.
std::vector<Language> LanguagesOf(const std::vector<Bank>& banks);

}  // namespace te::language
