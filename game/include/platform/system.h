#pragma once

#include "common.h"

// The platform layer: what the game needs of the machine it runs on. The game's C++ calls these interfaces and a platform
// implements them (src/platform/<platform>/, picked by configure.py). The PS2 one is on PS2SDK; the retail asm that's left calls
// Sony's SDK by name, which src/sce/ puts on the same interfaces. The platform's entry sets things up and calls Main.
namespace Platform::System
{
// Makes the machine ready for the game, first thing: on the PS2 the graphics hardware reset and the I/O processor restarted with
// the disc's modules
void Initialise();
// Starts what the pads, the memory cards and the sound go through
void StartServices();
[[noreturn]] void Exit(s32 status);

// The console's languages (the PS2's system settings')
enum ConsoleLanguage : s32
{
    LanguageJapanese = 0,
    LanguageEnglish = 1,
    LanguageFrench = 2,
    LanguageSpanish = 3,
    LanguageGerman = 4,
    LanguageItalian = 5,
    LanguageDutch = 6,
    LanguagePortuguese = 7,
};

ConsoleLanguage Language();

// TwinsanityEvolved (D49, D56): the languages the inserted disc carries, enumerated by the platform at run time. The game has ONE
// language index (g_CurrentLanguage, the slot of g_LanguageNames) and retail drove everything from it: the two text files
// Language\\Code|AgentLab\\<slot>.txt, the speech bank Crash6\\<slot>.mb, the FMV's sound track, a level's voice table, the
// title-card pictures, the credits. A disc whose TEXT banks were moved between slots (a translation patch) has no single slot
// where text and speech agree, so a LANGUAGE is a pair of slots: Language() names the slot of everything that is NOT the two text
// files (the retail index, through GameLanguageOf), LanguageTextBank() the slot whose text files are in that language (the same
// slot on a retail disc). The Options row: how many languages, the one in force, a row's label -- the language of the text,
// lower case like the game's own menu texts, " (<x> voice)" appended when its speech is another language's -- copied into `out`
// (`size` bytes, NUL-terminated). A platform that cannot enumerate (the PS2) answers the game's own slots and persists nothing.
u32 LanguageCount();
u32 LanguageSelected();
void LanguageLabel(u32 language, char* out, u32 size);
// The slot the two text files are read from; NoLanguageBank = the slot Language() names (retail)
constexpr u32 NoLanguageBank = 0xFFFFFFFF;
u32 LanguageTextBank();
// The language the player picked in the Options menu: the platform persists it (its settings file); Language() and
// LanguageTextBank() answer for it from now on. The game re-reads its texts and swaps its speech bank itself
void SetLanguage(u32 language);

// TwinsanityEvolved: the platform's built-in MODS, shown on the Options menu's MODS page -- how many (the PS2: none), a mod's
// name (lower case like the game's menu texts) copied into `out` (`size` bytes, NUL-terminated), whether the player has it on
// (all off by default; the platform persists the switch), and the player's new choice
u32 ModCount();
void ModLabel(u32 mod, char* out, u32 size);
u32 ModEnabled(u32 mod);
void SetModEnabled(u32 mod, u32 enabled);
// Whether the mod has an off / on switch on the Mods page (1: a toggle row, its own page linked while it is on). A mod without
// one (0) is always available: its row on the Mods page IS the link to its page, and opening that page is what turns the mod on
// (SetModEnabled(mod, 1) as the row is chosen) -- one click, no setup step
u32 ModHasSwitch(u32 mod);
// A mod's own page (Mods > <name>): its rows (0 = no page), its title (the Mods page's link to it reads the same), a row's label,
// and what the game does when the player chooses one
u32 ModPageRows(u32 mod);
void ModPageLabel(u32 mod, char* out, u32 size);
void ModPageRowLabel(u32 mod, u32 row, char* out, u32 size);
enum ModAction : u32
{
    ModActionNone = 0,
    // Start the chunk ModWarpTarget names, through the game's own level-entry path (a saved level's)
    ModActionWarp = 1,
    // Back to the title; the credits
    ModActionTitle = 2,
    ModActionCredits = 3,
};
u32 ModPageRowChosen(u32 mod, u32 row);
// After ModActionWarp: the chunk's path (as a save names it, "levels\\earth\\cavern\\cavent") and the character played there
// (GameProgress's; NoCharacter leaves it to the chunk's start script)
void ModWarpTarget(char* chunk, u32 size, u32* character);

struct DateTime
{
    u16 year;
    // From 1
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
};

// The local date and time
DateTime LocalTime();
}
