#pragma once
// MenuComposition -- the composition root's declaration of the title's added
// option pages: which widgets sit on the 'Display' and
// 'Controls' pages and how each reads / applies the session's live setting
// (SessionRuntime state, SessionControls actions; the persisted keys are
// written by SessionRuntime through SettingsFile). Also the guest face the
// extension works through: EE memory and DispatchTable::Invoke.
//
// Adding an option = one widget here (a MenuStrings id, a settings key, the
// read and the action), nothing in the extension.
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_extension.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_guest.h"

namespace ps2r::app {

class SessionRuntime;

class MenuComposition {
public:
    // Owns the extension. `session` null = not selected (a headless / tick-synced
    // leg, or ui.menu_pages=off): nothing is declared or installed. Otherwise the
    // pages are declared over the session and the extension is installed at the
    // title's Options page construction; Refused() reports an install failure.
    explicit MenuComposition(SessionRuntime* session);
    // The extension when installed (the boundary sync / census / restore hooks), else null.
    title::MenuExtension* Extension() { return installed_ ? &extension_ : nullptr; }
    bool Refused() const { return refused_; }

    // The real guest: memory::read/write and DispatchTable::Invoke with a0-a3/t0-t3.
    static title::MenuGuest Guest();
    // Declares the pages (before MenuExtension::Install):
    //   Display:  fps overlay (ui.perf_overlay), display mode (video.fullscreen),
    //             internal scale (video.scale), vsync (video.vsync), aspect ratio
    //             (video.aspect), render rate (video.render_rate)
    //   Controls: keyboard and mouse (input.kbm_pad)
    static void Declare(title::MenuExtension& extension, SessionRuntime& session);

private:
    title::MenuExtension extension_;
    bool installed_ = false, refused_ = false;
};

}  // namespace ps2r::app
