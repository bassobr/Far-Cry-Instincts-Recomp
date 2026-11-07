// The main menu's "Map Editor" (GitHub #12).
//
// On the Xbox the map editor is a program of its own on the disc, ige.xbe ("MasterInGameEditor", a second build of
// the engine with the editor, 6.6 MB, its own code at the same addresses as default.xbe). The menu's handler
// (0x0014F155, button B_MapEditor) calls 0x0014F880: a fresh state block ('FCI1', 0x00012190), the game shuts down
// (sound, renderer: 0x00200EE0, 0x002019F0, 0x001FBB00, 0x00507790), then XLaunchNewImage("D:\ige.xbe", block),
// which ends in HalReturnToFirmware(2). This PC version has only default.xbe recompiled, so the launch could only
// end the process (kernel::Relaunch starts default.xbe alone): the game vanished to the desktop.
//
// Replaced here: nothing shuts down, a notice says the editor is not supported, and the menu handler goes on as
// after any other button (it returns 0), so the game stays in the main menu. FCI_MAP_EDITOR_LAUNCH=1 runs the
// original (tests: the launch as before).
#include <windows.h>

#include <thread>

#include "app/menu.h"
#include "xdk/hle.h"

namespace {

bool HleOff() { return GetEnvironmentVariableA("FCI_MAP_EDITOR_LAUNCH", nullptr, 0) > 0; }

constexpr const char* kTitle = "MAP EDITOR";
constexpr const char* kText =
    "The Map Editor is not supported yet.\n\n"
    "On the Xbox it is a separate program on the disc (ige.xbe) that this PC version does not include. "
    "The game stays in the main menu.";

}  // namespace

// 0x0014F880: no arguments, plain ret
HLE_CDECL(Game_LaunchMapEditor) {
    rt::Log("map editor: the main menu asked for the map editor (ige.xbe), which is not supported; staying in the menu");
    if (!app::ShowNotice(kTitle, kText)) {
        // no notice in the menu's panel (LLE graphics or no picture yet): a window of its own, the game goes on
        std::thread([] {
            MessageBoxA(nullptr, kText, "Far Cry Instincts", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        }).detach();
    }
    return 0;
}
