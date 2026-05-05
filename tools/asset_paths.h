// Resolve test-asset paths without baking in any user-specific path.
//
// Resolution order:
//   1. EXMATERIA_ASSETS_DIR env var (points at an extracted disc tree).
//   2. Standard exmateria assets dir (populated by `exmateria-extract`):
//        Linux/BSD:  $XDG_DATA_HOME/exmateria/assets/
//                    (fallback ~/.local/share/exmateria/assets/)
//        macOS:      ~/Library/Application Support/exmateria/assets/
//        Windows:    %APPDATA%\exmateria\assets
//   3. Walk up from this header's directory to project-assets/fft-extract/
//      (monorepo development layout).
//
// Returns "" anywhere else. This file is published unchanged because it
// contains no machine-specific paths.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fftplugin::asset_paths {

inline std::filesystem::path standard_assets_dir() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata != nullptr && *appdata != '\0') {
        return std::filesystem::path(appdata) / "exmateria" / "assets";
    }
    if (const char* home = std::getenv("USERPROFILE"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / "AppData" / "Roaming" / "exmateria" / "assets";
    }
    return {};
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / "Library" / "Application Support" / "exmateria" / "assets";
    }
    return {};
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "exmateria" / "assets";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local" / "share" / "exmateria" / "assets";
    }
    return {};
#endif
}

inline std::filesystem::path assets_root() {
    if (const char* env = std::getenv("EXMATERIA_ASSETS_DIR"); env != nullptr && *env != '\0') {
        return std::filesystem::path(env);
    }
    if (const auto std_dir = standard_assets_dir();
        !std_dir.empty() && std::filesystem::is_directory(std_dir / "SOUND")) {
        return std_dir;
    }
    // __FILE__ → fft-plugin/tools/asset_paths.h. Walk up looking for
    // project-assets/fft-extract/ for monorepo development.
    std::filesystem::path here(__FILE__);
    here = here.parent_path();
    for (int i = 0; i < 8; ++i) {
        const auto candidate = here / "project-assets" / "fft-extract";
        if (std::filesystem::is_directory(candidate)) {
            return candidate;
        }
        if (!here.has_parent_path() || here.parent_path() == here) {
            break;
        }
        here = here.parent_path();
    }
    return {};
}

inline std::string default_waveset() {
    const auto root = assets_root();
    if (root.empty()) return {};
    return (root / "SOUND" / "WAVESET.WD").string();
}

inline std::string default_smd() {
    const auto root = assets_root();
    if (root.empty()) return {};
    return (root / "SOUND" / "MUSIC_31.SMD").string();
}

}  // namespace fftplugin::asset_paths
