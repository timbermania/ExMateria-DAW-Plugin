# FFT Plugin

Play and edit *Final Fantasy Tactics* (PSX) music inside your DAW.

The plugin loads `WAVESET.WD` + `.SMD` files from the FFT disc and renders
them through a faithful PSX SPU emulation. Drop it on a track, pick a song,
hit play.

> **Status:** early. VST3 in REAPER on Windows is the validated host. Other
> hosts and formats may work but aren't tested yet.

## Install

### From a release build *(when available)*

Download the `.vst3` bundle from the Releases page, unzip into your VST3
folder (`%PROGRAMFILES%\Common Files\VST3\` or per-user
`%LOCALAPPDATA%\Programs\Common\VST3\`), and rescan plugins in your DAW.

### From source

#### Prerequisites

- **CMake ≥ 3.21**
- **A C++20 compiler.** On Windows that's MSVC (Visual Studio 2022 or
  the Build Tools for Visual Studio). On Linux/macOS, recent
  GCC/Clang.
- **Git** + a working network connection on first configure — CMake
  fetches [JUCE](https://github.com/juce-framework/JUCE) automatically
  via `FetchContent` (pinned to JUCE 8.0.4). Subsequent builds are offline.

##### Windows install via [Chocolatey](https://chocolatey.org/) (recommended)

From an elevated PowerShell:

```powershell
choco install cmake -y
choco install visualstudio2022buildtools -y --package-parameters `
  "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

`visualstudio2022community` works too if you'd rather have the full
IDE. The key piece either way is the **C++ build tools** workload
(MSVC + Windows SDK).

##### Linux (Debian/Ubuntu)

```bash
sudo apt install cmake build-essential
```

##### macOS

```bash
brew install cmake
xcode-select --install   # if you don't already have command-line tools
```

#### Build

From the repo root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

On Windows, run those from the **"x64 Native Tools Command Prompt for
VS 2022"** (or any shell where `cl.exe` is on PATH), or just open a
plain shell after the Build Tools are installed and CMake will find
MSVC automatically.

#### Install

The build produces a `.vst3` bundle under `build/`. Copy it into your
host's VST3 folder:

| OS | VST3 folder |
|----|-------------|
| Windows (per-user) | `%LOCALAPPDATA%\Programs\Common\VST3\` |
| Windows (system) | `%PROGRAMFILES%\Common Files\VST3\` |
| macOS | `~/Library/Audio/Plug-Ins/VST3/` |
| Linux | `~/.vst3/` |

Then restart your host and rescan plugins. A first-class install step
(`cmake --install` target, or per-OS scripts) is on the roadmap.

## First-run setup

The plugin needs access to the music data on your FFT disc. Extract it once
with the [exmateria iso-patcher][isop] and the plugin auto-discovers it:

```
fft-iso-patcher extract path/to/Final\ Fantasy\ Tactics.bin
```

That writes the disc's contents to a standard location:

| OS | Path |
|----|------|
| Linux / BSD | `~/.local/share/exmateria/assets/` |
| macOS | `~/Library/Application Support/exmateria/assets/` |
| Windows | `%APPDATA%\exmateria\assets\` |

The plugin picks up `SOUND/WAVESET.WD` automatically on first load. Done.

[isop]: https://github.com/timbermania/ExMateria-ISO-Patcher

## Using the plugin

1. Drop the plugin on a track in your DAW.
2. The waveset loads automatically if you ran `fft-iso-patcher extract`. If not,
   click the waveset button and pick `WAVESET.WD` manually.
3. Click the SMD button and pick a song (e.g. `MUSIC_31.SMD` for "Trisection").
4. Press play.

The plugin saves its waveset/SMD selection in the DAW project, so reopening
a session resumes where you left off.

## Overriding the asset location

If you have your FFT files somewhere non-standard, set:

```
EXMATERIA_ASSETS_DIR=/path/to/your/extracted/disc
```

For per-file overrides (or the SF2 fallback backend), see
[`paths.conf` (advanced)](#pathsconf-advanced) below.

## Limitations / known issues

- The processor runs at 44.1 kHz stereo. Your DAW's resampler bridges any
  other session rate. (REAPER's built-in resampler is fine.)
- Not every SMD opcode is implemented yet — most music tracks play, but
  some sound effects and edge cases are still in progress.
- Editing UI is minimal at this stage; this release is playback-first.

## `paths.conf` (advanced)

If `fft-iso-patcher extract` doesn't fit your setup, the plugin still reads a
manual config file:

| OS | Path |
|----|------|
| Linux / macOS | `~/.config/fft-plugin/paths.conf` |
| Windows | `%APPDATA%\fft-plugin\paths.conf` |

```
# Per-file overrides for the FFT extract:
waveset_path=/path/to/SOUND/WAVESET.WD
smd_path=/path/to/SOUND/MUSIC_31.SMD

# SF2 fallback backend (unrelated to FFT assets):
fluidsynth_path=
soundfont_path=/usr/share/soundfonts/FluidR3_GM.sf2
```

Resolution order, highest priority first:

1. `EXMATERIA_ASSETS_DIR` env var
2. Standard exmateria assets dir (populated by `fft-iso-patcher extract`)
3. `paths.conf`
4. Empty → manual file picker

---

## Development

Build/test workflow for contributors. Skip if you just want to use the plugin.

### Layout

- `include/fft_plugin/` — host-agnostic core headers (model, state, controller, backend, waveset, SPU)
- `src/` — shared core implementation + JUCE wrapper + raw VST3 shell
- `tools/` — smoke + parity drivers

### Build & test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The byte-identity gate (`ctest -R byte_identity`) is always-on; it checks
a synthetic fixture committed to the repo and any ROM-derived fixtures
that are present locally. Drivers needing ROM fixtures auto-skip when
those files are absent — CI works without them.

First-time fixture bootstrap (run once after a deliberate codec change):

```bash
cmake --build build --target fft_bootstrap_byte_identity
git add tests/fixtures/synthetic/tiny.fftauth tests/fixtures/expected_hashes.json
```

### Architecture

- `IFFTSpuCore` — low-level SPU operations (ADPCM decode, gauss interpolation, ADSR, reverb, pitch LFO).
- `IFFTWavesetService` — `WAVESET.WD` loading and instrument metadata.
- `FFTSpuPreviewCore` / `FFTSpuPreviewAdapter` — host-neutral preview path bridging note requests into the SPU core.
- `FFTPluginController` — talks to the abstractions above; doesn't know about JUCE or Godot.
- `tools/fft_spu_parity_driver` renders a single note through the plugin-side core. Comparison fixtures (e.g. against the Godot-side renderer) live with that side of the work; cross-repo parity tests aren't wired up here.

The third-party VST3 SDK is vendored under `third_party/vst3`.
