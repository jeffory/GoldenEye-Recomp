#pragma once

#include <filesystem>

#include <rex/ui/windowed_app_context.h>

namespace ge {

// Startup manifest check: verifies every file in ge_asset_manifest.inc exists
// under game_root (one recursive enumeration, case-insensitive to match the
// VFS's case-insensitive host lookup).
//
// Returns true if the guest launch may proceed. On failure it logs GEMISSING
// lines (parsed by the Android loader overlay), writes the full list to
// user_root/ge_missing_files.txt, and on desktop shows a message box and posts
// quit; on Android it leaves the app alive so the Java loader can present the
// error. Runs on the UI thread (called from GeApp::OnPreLaunchModule).
bool RunStartupAssetCheck(const std::filesystem::path& game_root,
                          const std::filesystem::path& user_root,
                          rex::ui::WindowedAppContext& app_context);

}  // namespace ge
