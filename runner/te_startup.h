// runner/te_startup.h -- starting without a terminal: the disc image named in a file beside the executable, a message
// box for a start that cannot go on, and (Windows) the parent terminal's console for the windowed executable.
#pragma once
#include <string>

namespace te::startup {

// The file beside the executable that names the disc image (one line, the image's path). The launcher writes it into
// the folder it installs the game to, so the game starts from a shortcut with no arguments.
inline constexpr const char* kDiscImageFile = "disc-image.txt";

// The disc image named by kDiscImageFile beside the running executable, or empty.
std::string DiscImageBesideExecutable();

// The first start of a downloaded game: the player picks their disc image in the system's file dialog (Windows' Open
// dialog, macOS' chooser, zenity / kdialog on Linux). Empty when cancelled or no dialog exists.
std::string ChooseDiscImage();
// Writes `path` into kDiscImageFile beside the executable, so later starts need no dialog. False when the folder is
// read-only (the game then asks again next time).
bool RememberDiscImage(const std::string& path);

// Where the memory cards and ps2r.ini live: `userdata` beside the executable when it exists, or when the folder is
// writable and there is no earlier <home>/.twinsanityevolved (a downloaded game keeps everything in its own folder,
// on whatever drive it was unzipped to); else <home>/.twinsanityevolved. Empty when neither can be named.
std::string DataDirectory();

// Windows: the executable is a windowed (GUI-subsystem) program; started from a terminal it writes to that terminal.
// Output redirected to a file or a pipe is left alone. Elsewhere a no-op.
void AttachParentConsole();

// The reason the game cannot start: on stderr, and (Windows, no console) in a message box.
void ShowError(const char* title, const std::string& text);

}  // namespace te::startup
