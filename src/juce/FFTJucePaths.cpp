#include "FFTJucePaths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace fftplugin {
namespace jucewrap {

namespace {

// Standard exmateria assets directory — populated by `exmateria-extract`.
// Mirror of fft-plugin/tools/asset_paths.h::standard_assets_dir() so the
// runtime plugin and the test drivers resolve to the same place.
std::filesystem::path standard_assets_dir() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata != nullptr && *appdata != '\0') {
        return std::filesystem::path(appdata) / "exmateria" / "assets";
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

// Returns a populated extracted-disc directory or empty.
//   1. EXMATERIA_ASSETS_DIR env var
//   2. Standard exmateria dir (presence-checked via SOUND/ subdir)
std::filesystem::path resolve_assets_root() {
    if (const char* env = std::getenv("EXMATERIA_ASSETS_DIR"); env != nullptr && *env != '\0') {
        return std::filesystem::path(env);
    }
    if (const auto std_dir = standard_assets_dir();
        !std_dir.empty() && std::filesystem::is_directory(std_dir / "SOUND")) {
        return std_dir;
    }
    return {};
}

// Config file location: ~/.config/fft-plugin/paths.conf (Linux/Mac)
//                       %APPDATA%\fft-plugin\paths.conf  (Windows)
// Format: one key=value per line, # comments, blank lines ignored.
//   waveset_path=/path/to/WAVESET.WD
//   smd_path=/path/to/MUSIC_31.SMD
//   fluidsynth_path=...    (SF2 fallback backend only)
//   soundfont_path=...     (SF2 fallback backend only)
//
// paths.conf is the *legacy* override mechanism. New installs should
// prefer `exmateria-extract`, which populates the standard assets dir
// that resolve_assets_root() picks up automatically.
std::unordered_map<std::string, std::string> load_config() {
    std::unordered_map<std::string, std::string> cfg;

#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    if (!base) return cfg;
    std::filesystem::path config_path = std::filesystem::path(base) / "fft-plugin" / "paths.conf";
#else
    const char* home = std::getenv("HOME");
    if (!home) return cfg;
    std::filesystem::path config_path = std::filesystem::path(home) / ".config" / "fft-plugin" / "paths.conf";
#endif

    std::ifstream f(config_path);
    if (!f.is_open()) return cfg;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // trim trailing whitespace
        while (!key.empty() && (key.back() == ' ' || key.back() == '\r')) key.pop_back();
        while (!val.empty() && (val.back() == ' ' || val.back() == '\r')) val.pop_back();
        cfg[key] = val;
    }
    return cfg;
}

const std::unordered_map<std::string, std::string>& config() {
    static auto cfg = load_config();
    return cfg;
}

std::string config_or(const std::string& key, const std::string& fallback) {
    const auto& cfg = config();
    const auto it = cfg.find(key);
    return it != cfg.end() && !it->second.empty() ? it->second : fallback;
}

}  // namespace

std::string default_waveset_path() {
    // Resolution order:
    //   1. EXMATERIA_ASSETS_DIR / standard exmateria dir → SOUND/WAVESET.WD
    //   2. paths.conf "waveset_path" (legacy override)
    //   3. Empty — user picks via the file picker.
    if (const auto root = resolve_assets_root(); !root.empty()) {
        const auto candidate = root / "SOUND" / "WAVESET.WD";
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate.string();
        }
    }
    return config_or("waveset_path", "");
}

std::string default_smd_path() {
    // Same order as default_waveset_path(); SMD is also picked per-session
    // via the file picker, so this is just a useful starting point.
    if (const auto root = resolve_assets_root(); !root.empty()) {
        const auto candidate = root / "SOUND" / "MUSIC_31.SMD";
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate.string();
        }
    }
    return config_or("smd_path", "");
}

std::string default_fluidsynth_path() {
    // Empty string = use PATH lookup. Config can override with an explicit path.
    return config_or("fluidsynth_path", "");
}

std::string default_soundfont_path() {
#if defined(_WIN32)
    constexpr const char* fallback = "";
#else
    // Common Linux locations in priority order; first existing one wins.
    for (const char* p : {
            "/usr/share/soundfonts/FluidR3_GM.sf2",
            "/usr/share/sounds/sf2/FluidR3_GM.sf2",
            "/usr/share/soundfonts/FluidR3Mono_GM.sf3",
            "/usr/share/soundfonts/default.sf2",
        }) {
        if (std::filesystem::exists(p)) {
            return config_or("soundfont_path", p);
        }
    }
    constexpr const char* fallback = "";
#endif
    return config_or("soundfont_path", fallback);
}

}  // namespace jucewrap
}  // namespace fftplugin
