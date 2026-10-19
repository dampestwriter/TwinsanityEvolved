#pragma once

#include "common.h"
#include "platform/system.h"

// The game's two text files of each language: Language\Code\<language>.txt (the game's texts) and Language\AgentLab\<language>.txt
// (the scripts'), a text a line
enum TextFile : u32
{
    CodeTexts = 0,
    AgentLabTexts = 1,
    TextFiles = 2,
};

extern "C"
{
    // The languages: how many, their names (the folders and files of the Language files) and the current one
    extern u32 g_LanguageCount RETAIL(GlobalLanguagesAmount);
    extern const char** g_LanguageNames RETAIL(G_unkLanguagesStruct);
    extern u32 g_CurrentLanguage RETAIL(CurrentLanguageIndex);
    // TwinsanityEvolved: the slot the two text files are read from. Retail had one index for everything; a disc whose
    // text banks were moved between slots (a translation patch) needs the text's slot apart from the speech's, so the texts
    // follow this one and g_CurrentLanguage (the speech bank, the FMV sound track, a level's voices, the pictures, the credits)
    // stays the retail index. Equal on a retail disc. Not a retail global
    extern u32 g_CurrentTextLanguage;
    // The names the start-up gives (English, French, German, Spanish, Italian)
    extern const char* g_GameLanguageNames[] RETAIL(GlobalLanguagesArray);
    // Each text file's lines: how many line breaks the last one read had, every language's lines (none until read) and the
    // current language's
    extern u32 g_TextLineCounts[TextFiles] RETAIL(D_00309B00);
    extern const char*** g_LanguageTexts[TextFiles] RETAIL(D_00309B08);
    extern const char** g_Texts[TextFiles] RETAIL(D_00309B10);

    // The languages there are, every language's texts none yet
    void InitLanguages(u32 count, const char** names) RETAIL(InitAGlobalRelatedToLanguages);
    // The language played in: its texts the current ones (once there are languages), the game told
    void SetGameLanguage(u32 language) RETAIL(SetGameLanguage);
    // TwinsanityEvolved: the language played in AND the slot its texts come from (the platform's LanguageTextBank, the
    // same slot when the platform names none)
    void SetGameLanguages(u32 language, u32 textLanguage);
    // A text file's text (in memory that stays) split into its lines, the line breaks made ends; blank lines aren't lines, so
    // a text's number counts the lines that aren't blank. The lines are the language's and the current ones
    void ReadTextFile(u32 file, u32 language, char* text) RETAIL(FUN_0017e088);
    // The current text language's texts the current ones, the game told
    void UseLanguageTexts() RETAIL(FUN_00181aa8);
}

// The game's language index of a console language (game/gamecontext.cpp: French 1, German 2, Spanish 3, Italian 4, English 0 the
// rest) -- TwinsanityEvolved: the Options page maps the platform's Language() with it
s32 GameLanguageOf(Platform::System::ConsoleLanguage consoleLanguage);

// A game text by its number (the code's text file's)
inline const char* GameText(u32 text)
{
    return g_Texts[CodeTexts][text];
}
