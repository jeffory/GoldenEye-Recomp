#include "ge_asset_check.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system.h>  // rex::ShowSimpleMessageBox

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "ge_asset_manifest.inc"  // kGeRequiredAssets (generated)

// Escape hatch for non-canonical dumps: when false the check still logs what
// is missing but the guest launches anyway (and fails however it fails).
REXCVAR_DEFINE_BOOL(ge_fatal_on_missing_file, true, "Game",
                    "Refuse to launch (with an error listing the files) when required "
                    "game files are missing from the game data root.");

namespace ge {
namespace {

// How many individual files to name in ge.log / the dialog. The full list
// always goes to ge_missing_files.txt.
constexpr size_t kMaxListedInLog = 50;
constexpr size_t kMaxListedInMessage = 8;

std::string ToLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Runtime/system subtrees that live inside the game dir but are not part of
// the dump (skipping "goldeneye 007 xbla" also avoids walking a stray copy of
// the raw package, ~1800 extra files).
bool SkipTopLevelDir(const std::string& lower_name) {
  return lower_name == "user" || lower_name == "cache" ||
         lower_name == "goldeneye 007 xbla";
}

std::vector<std::string> FindMissingAssets(const std::filesystem::path& game_root) {
  namespace fs = std::filesystem;
  // One enumeration pass instead of ~1800 individual stats: much cheaper on
  // Android's FUSE-backed /sdcard, and it gives case-insensitivity for free.
  std::unordered_set<std::string> present;
  present.reserve(4096);
  std::error_code ec;
  fs::recursive_directory_iterator it(
      game_root, fs::directory_options::skip_permission_denied, ec);
  if (!ec) {
    for (fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
      if (ec) break;
      if (it->is_directory(ec)) {
        if (it.depth() == 0 &&
            SkipTopLevelDir(ToLowerAscii(it->path().filename().string()))) {
          it.disable_recursion_pending();
        }
        continue;
      }
      if (!it->is_regular_file(ec)) continue;
      present.insert(
          ToLowerAscii(it->path().lexically_relative(game_root).generic_string()));
    }
  }

  std::vector<std::string> missing;
  for (const char* required : kGeRequiredAssets) {
    if (!present.count(required)) missing.emplace_back(required);
  }
  return missing;
}

// Full list for bug reports; best-effort (the check must not fail on an
// unwritable user dir).
std::filesystem::path WriteReportFile(const std::filesystem::path& user_root,
                                      const std::vector<std::string>& missing) {
  std::error_code ec;
  std::filesystem::create_directories(user_root, ec);
  auto report = user_root / "ge_missing_files.txt";
  std::ofstream f(report, std::ios::trunc);
  if (!f) return {};
  f << missing.size() << " required game file(s) missing:\n";
  for (const auto& m : missing) f << m << '\n';
  return report;
}

}  // namespace

bool RunStartupAssetCheck(const std::filesystem::path& game_root,
                          const std::filesystem::path& user_root,
                          rex::ui::WindowedAppContext& app_context) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto missing = FindMissingAssets(game_root);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  if (missing.empty()) {
    REXLOG_INFO("Asset check OK: all {} required files present ({} ms)",
                std::size(kGeRequiredAssets), ms);
    return true;
  }

  // Summary first, then the files -- the Android loader greps these markers
  // out of ge.log to build its error screen.
  REXLOG_ERROR("GEMISSING total={} root='{}' ({} ms)", missing.size(),
               game_root.string(), ms);
  for (size_t i = 0; i < missing.size() && i < kMaxListedInLog; ++i) {
    REXLOG_ERROR("GEMISSING file={}", missing[i]);
  }
  if (missing.size() > kMaxListedInLog) {
    REXLOG_ERROR("GEMISSING (and {} more)", missing.size() - kMaxListedInLog);
  }
  const auto report = WriteReportFile(user_root, missing);
  if (!report.empty()) {
    REXLOG_ERROR("GEMISSING full list written to '{}'", report.string());
  }

  if (!REXCVAR_GET(ge_fatal_on_missing_file)) {
    REXLOG_WARN(
        "ge_fatal_on_missing_file=false: launching anyway with {} missing file(s)",
        missing.size());
    return true;
  }

#if defined(__ANDROID__)
  // Veto the launch but keep the process alive: GoldenEyeActivity's loader
  // overlay sees the GEMISSING markers in ge.log and becomes the error screen
  // (quitting here would just bounce the user back to the launcher with no
  // explanation -- there is no native message box on Android).
  return false;
#else
  std::string message =
      fmt::format("Missing {} required game file(s), e.g.:\n", missing.size());
  for (size_t i = 0; i < missing.size() && i < kMaxListedInMessage; ++i) {
    message += "  " + missing[i] + "\n";
  }
  if (missing.size() > kMaxListedInMessage) {
    message += fmt::format("  ... and {} more\n", missing.size() - kMaxListedInMessage);
  }
  message += fmt::format(
      "\nGame folder: {}\nCopy the complete GoldenEye 007 game dump into that "
      "folder and relaunch.",
      game_root.string());
  if (!report.empty()) {
    message += fmt::format("\nFull list: {}", report.string());
  }
  // Modal on Windows; prints to stderr on Linux (the message is also in
  // ge.log either way).
  rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, message);
  app_context.QuitFromUIThread();
  return false;
#endif
}

}  // namespace ge
