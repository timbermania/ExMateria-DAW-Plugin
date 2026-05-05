// Byte-identity gate. Pins SMD codec output across refactors.
//
// For each fixture present:
//   - tiny.fftauth (always): load .fftauth -> compile -> SHA-256 of SMD bytes
//     must equal the value in expected_hashes.json["tiny.fftauth.compiled_smd"].
//   - MUSIC_31.SMD (optional): load .smd -> parse -> re-serialize must be
//     byte-identical to the input file. Also SHA-256 of input must equal
//     expected_hashes.json["MUSIC_31.SMD"] (pins the fixture itself).
//   - aerith.5.fftauth (optional): load -> compile_for_game(target=2048)
//     -> SHA-256 of compiled SMD must equal
//     expected_hashes.json["aerith.5.fftauth.target_2048.compiled_smd"].
//
// Modes:
//   default  : verify each present fixture against expected_hashes.json
//   --update : compute current hashes and overwrite expected_hashes.json
//
// Run --update once after intentionally changing fixture or codec; commit
// the new JSON. Subsequent CI/ctest invocations run in verify mode.

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "fft_plugin/fft_smd_authoring_codec.h"
#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/fft_smd_file.h"

namespace {

// --- minimal SHA-256 (public-domain pseudo-code transcription) ---
// Self-contained so the test driver has no dependencies beyond the plugin core.
struct Sha256 {
    static constexpr std::array<uint32_t, 64> K = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
    };

    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    static std::string hex(const std::vector<uint8_t>& bytes) {
        std::array<uint32_t, 8> H = {
            0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
            0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u,
        };

        const uint64_t bit_len = static_cast<uint64_t>(bytes.size()) * 8u;
        std::vector<uint8_t> padded = bytes;
        padded.push_back(0x80u);
        while (padded.size() % 64u != 56u) {
            padded.push_back(0u);
        }
        for (int i = 7; i >= 0; --i) {
            padded.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFu));
        }

        for (size_t chunk_start = 0; chunk_start < padded.size(); chunk_start += 64u) {
            std::array<uint32_t, 64> W{};
            for (size_t i = 0; i < 16; ++i) {
                W[i] = (static_cast<uint32_t>(padded[chunk_start + i * 4 + 0]) << 24) |
                       (static_cast<uint32_t>(padded[chunk_start + i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(padded[chunk_start + i * 4 + 2]) << 8) |
                       (static_cast<uint32_t>(padded[chunk_start + i * 4 + 3]));
            }
            for (size_t i = 16; i < 64; ++i) {
                const uint32_t s0 = rotr(W[i - 15], 7) ^ rotr(W[i - 15], 18) ^ (W[i - 15] >> 3);
                const uint32_t s1 = rotr(W[i - 2], 17) ^ rotr(W[i - 2], 19) ^ (W[i - 2] >> 10);
                W[i] = W[i - 16] + s0 + W[i - 7] + s1;
            }

            uint32_t a = H[0], b = H[1], c = H[2], d = H[3];
            uint32_t e = H[4], f = H[5], g = H[6], h = H[7];
            for (size_t i = 0; i < 64; ++i) {
                const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                const uint32_t ch = (e & f) ^ ((~e) & g);
                const uint32_t t1 = h + S1 + ch + K[i] + W[i];
                const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
                const uint32_t t2 = S0 + mj;
                h = g; g = f; f = e;
                e = d + t1;
                d = c; c = b; b = a;
                a = t1 + t2;
            }
            H[0] += a; H[1] += b; H[2] += c; H[3] += d;
            H[4] += e; H[5] += f; H[6] += g; H[7] += h;
        }

        std::ostringstream oss;
        for (uint32_t word : H) {
            for (int i = 7; i >= 0; --i) {
                const uint32_t nibble = (word >> (i * 4)) & 0xFu;
                oss << static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
            }
        }
        return oss.str();
    }
};

// --- minimal flat JSON read/write for {string: string} maps ---
// Format: pretty-printed object, two-space indent, sorted by insertion.
// Only supports values without escapes — sufficient for hex hashes.

std::map<std::string, std::string> read_hash_file(const std::string& path) {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    if (!in.is_open()) return out;
    std::string line;
    while (std::getline(in, line)) {
        const auto first_quote = line.find('"');
        if (first_quote == std::string::npos) continue;
        const auto second_quote = line.find('"', first_quote + 1);
        if (second_quote == std::string::npos) continue;
        const auto third_quote = line.find('"', second_quote + 1);
        if (third_quote == std::string::npos) continue;
        const auto fourth_quote = line.find('"', third_quote + 1);
        if (fourth_quote == std::string::npos) continue;
        const std::string key = line.substr(first_quote + 1, second_quote - first_quote - 1);
        const std::string value = line.substr(third_quote + 1, fourth_quote - third_quote - 1);
        out[key] = value;
    }
    return out;
}

void write_hash_file(const std::string& path, const std::map<std::string, std::string>& hashes) {
    std::ofstream out(path, std::ios::trunc);
    out << "{\n";
    size_t i = 0;
    for (const auto& [k, v] : hashes) {
        out << "  \"" << k << "\": \"" << v << "\"";
        if (++i < hashes.size()) out << ",";
        out << "\n";
    }
    out << "}\n";
}

std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};
    return std::vector<uint8_t>(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool file_exists(const std::string& path) {
    std::ifstream in(path);
    return in.good();
}

struct CheckOutcome {
    bool checked = false;
    bool passed = false;
    std::string detail;
};

// Verifies (or in update mode, captures) a single hash entry.
// Returns true if the entry passed (or update was applied).
bool reconcile(
    const std::string& key,
    const std::string& computed,
    bool update_mode,
    std::map<std::string, std::string>& expected
) {
    if (update_mode) {
        expected[key] = computed;
        std::cout << "  " << key << " <- " << computed << "\n";
        return true;
    }
    const auto it = expected.find(key);
    if (it == expected.end()) {
        std::cerr << "  MISSING expected hash for " << key
                  << " (got " << computed << "). Re-run with --update to capture.\n";
        return false;
    }
    if (it->second != computed) {
        std::cerr << "  MISMATCH " << key << "\n"
                  << "    expected: " << it->second << "\n"
                  << "    actual:   " << computed << "\n";
        return false;
    }
    std::cout << "  OK " << key << " " << computed << "\n";
    return true;
}

CheckOutcome check_synthetic_fftauth(
    const std::string& path,
    bool update_mode,
    std::map<std::string, std::string>& expected
) {
    CheckOutcome out;
    out.checked = true;
    if (!file_exists(path)) {
        out.detail = "missing fixture: " + path;
        return out;
    }
    std::string error;
    const auto authored = fftplugin::load_smd_authoring_document(path, &error);
    if (!authored.has_value()) {
        out.detail = "load failed: " + error;
        return out;
    }
    const auto compiled = fftplugin::compile_smd_authoring_document(*authored);
    const auto smd_bytes = fftplugin::serialize_smd_file(compiled.smd, &error);
    if (smd_bytes.empty()) {
        out.detail = "serialize failed: " + error;
        return out;
    }
    // Parse-and-reserialize round-trip on the compiled bytes.
    const auto reparsed = fftplugin::parse_smd_bytes(smd_bytes, &error);
    if (!reparsed.has_value()) {
        out.detail = "parse failed: " + error;
        return out;
    }
    const auto re_smd_bytes = fftplugin::serialize_smd_file(*reparsed, &error);
    if (re_smd_bytes != smd_bytes) {
        out.detail = "SMD codec round-trip not byte-identical for " + path;
        return out;
    }
    const std::string hash = Sha256::hex(smd_bytes);
    out.passed = reconcile("tiny.fftauth.compiled_smd", hash, update_mode, expected);
    return out;
}

CheckOutcome check_smd_file_roundtrip(
    const std::string& key,
    const std::string& path,
    bool update_mode,
    std::map<std::string, std::string>& expected
) {
    CheckOutcome out;
    if (!file_exists(path)) {
        out.detail = "skipped (fixture absent): " + path;
        return out;
    }
    out.checked = true;
    std::string error;
    const auto raw_bytes = read_file_bytes(path);

    // File-integrity gate: the fixture file itself must hash to the value
    // we recorded (or be captured into the file in --update mode). This
    // catches both fixture drift and accidental fixture replacement.
    const std::string hash = Sha256::hex(raw_bytes);
    out.passed = reconcile(key, hash, update_mode, expected);

    // Round-trip enforced check: parse + reserialize and compare. Real-disc
    // SMDs must round-trip byte-identically — that's the user-stated goal
    // "an export with 0 bytes should compile down to an identical SMD."
    const auto parsed = fftplugin::parse_smd_bytes(raw_bytes, &error);
    if (!parsed.has_value()) {
        std::cerr << "  FAIL parse failed: " << error << "\n";
        out.passed = false;
        return out;
    }
    const auto reserialized = fftplugin::serialize_smd_file(*parsed, &error);
    if (reserialized.empty()) {
        std::cerr << "  FAIL reserialize failed: " << error << "\n";
        out.passed = false;
        return out;
    }
    if (reserialized != raw_bytes) {
        size_t first_diff = 0;
        const size_t common = std::min(reserialized.size(), raw_bytes.size());
        while (first_diff < common && reserialized[first_diff] == raw_bytes[first_diff]) {
            ++first_diff;
        }
        std::cerr << "  FAIL parse->reserialize differs"
                  << "; src_size=" << raw_bytes.size()
                  << " out_size=" << reserialized.size()
                  << " first_diff_offset=" << first_diff;
        if (first_diff < common) {
            std::cerr << " src_byte=0x" << std::hex << static_cast<int>(raw_bytes[first_diff])
                      << " out_byte=0x" << static_cast<int>(reserialized[first_diff]) << std::dec;
        }
        std::cerr << "\n";
        out.passed = false;
        return out;
    }
    std::cout << "  OK round-trip byte-identical for " << path << "\n";
    return out;
}

CheckOutcome check_aerith_compile(
    const std::string& path,
    bool update_mode,
    std::map<std::string, std::string>& expected
) {
    CheckOutcome out;
    if (!file_exists(path)) {
        out.detail = "skipped (fixture absent): " + path;
        return out;
    }
    out.checked = true;
    std::string error;
    const auto authored = fftplugin::load_smd_authoring_document(path, &error);
    if (!authored.has_value()) {
        out.detail = "load failed: " + error;
        return out;
    }
    fftplugin::FFTSmdGameCompileBudget budget;
    budget.target_bytes = 2048;
    const auto result = fftplugin::compile_smd_authoring_document_for_game(*authored, budget);
    if (!result.ok) {
        out.detail = "game compile failed: " + result.report.error;
        return out;
    }
    const auto smd_bytes = fftplugin::serialize_smd_file(result.compiled.smd, &error);
    if (smd_bytes.empty()) {
        out.detail = "serialize failed: " + error;
        return out;
    }
    const std::string hash = Sha256::hex(smd_bytes);
    out.passed = reconcile("aerith.5.fftauth.target_2048.compiled_smd", hash, update_mode, expected);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    bool update_mode = false;
    std::string hashes_path;
    std::string synthetic_path;
    std::string music31_path;
    std::string aerith_path;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--update") {
            update_mode = true;
        } else if (arg == "--hashes" && i + 1 < argc) {
            hashes_path = argv[++i];
        } else if (arg == "--synthetic" && i + 1 < argc) {
            synthetic_path = argv[++i];
        } else if (arg == "--music31" && i + 1 < argc) {
            music31_path = argv[++i];
        } else if (arg == "--aerith" && i + 1 < argc) {
            aerith_path = argv[++i];
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 1;
        }
    }
    if (hashes_path.empty()) {
        std::cerr << "missing required --hashes <path-to-expected_hashes.json>\n";
        return 1;
    }
    if (synthetic_path.empty()) {
        std::cerr << "missing required --synthetic <path-to-tiny.fftauth>\n";
        return 1;
    }

    auto expected = read_hash_file(hashes_path);
    bool all_ok = true;
    bool any_checked = false;

    std::cout << (update_mode ? "[update]" : "[verify]") << " " << hashes_path << "\n";

    {
        std::cout << "synthetic .fftauth: " << synthetic_path << "\n";
        const auto r = check_synthetic_fftauth(synthetic_path, update_mode, expected);
        any_checked |= r.checked;
        if (r.checked && !r.passed) all_ok = false;
        if (!r.checked && !r.detail.empty()) {
            std::cerr << "  ERROR " << r.detail << "\n";
            all_ok = false;
        }
        if (!r.detail.empty() && r.checked && !r.passed) {
            std::cerr << "  detail: " << r.detail << "\n";
        }
    }

    if (!music31_path.empty()) {
        std::cout << "MUSIC_31.SMD: " << music31_path << "\n";
        const auto r = check_smd_file_roundtrip("MUSIC_31.SMD", music31_path, update_mode, expected);
        any_checked |= r.checked;
        if (r.checked && !r.passed) all_ok = false;
        if (!r.detail.empty()) std::cout << "  " << r.detail << "\n";
    }

    if (!aerith_path.empty()) {
        std::cout << "aerith.5.fftauth: " << aerith_path << "\n";
        const auto r = check_aerith_compile(aerith_path, update_mode, expected);
        any_checked |= r.checked;
        if (r.checked && !r.passed) all_ok = false;
        if (!r.detail.empty()) std::cout << "  " << r.detail << "\n";
    }

    if (update_mode) {
        write_hash_file(hashes_path, expected);
        std::cout << "wrote " << hashes_path << " (" << expected.size() << " entries)\n";
        return 0;
    }
    if (!any_checked) {
        std::cerr << "no fixtures checked\n";
        return 1;
    }
    return all_ok ? 0 : 2;
}
