#include "fft_plugin/fft_waveset_file_service.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace fftplugin {

namespace {

constexpr int32_t kEntryOffset = 0x20;
constexpr int32_t kEntrySize = 16;
constexpr int32_t kBlockSize = 16;
constexpr int32_t kSamplesPerBlock = 28;
constexpr uint8_t kFlagLoopEnd = 0x01;
constexpr uint8_t kFlagLoopRepeat = 0x02;
constexpr uint8_t kFlagLoopStart = 0x04;

uint32_t read_u32_le(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
        (static_cast<uint32_t>(data[offset + 1]) << 8) |
        (static_cast<uint32_t>(data[offset + 2]) << 16) |
        (static_cast<uint32_t>(data[offset + 3]) << 24);
}

uint16_t read_u16_le(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint16_t>(data[offset]) |
        (static_cast<uint16_t>(data[offset + 1]) << 8);
}

int16_t read_s16_le(const std::vector<uint8_t>& data, size_t offset) {
    const uint16_t raw = read_u16_le(data, offset);
    return static_cast<int16_t>(raw);
}

bool is_zero_block(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + kBlockSize > data.size()) {
        return false;
    }
    return std::all_of(
        data.begin() + static_cast<std::ptrdiff_t>(offset),
        data.begin() + static_cast<std::ptrdiff_t>(offset + kBlockSize),
        [](uint8_t value) { return value == 0; }
    );
}

void fill_loop_metadata(
    FFTInstrumentInfo& instrument_info,
    FFTSpuInstrumentData& spu_instrument,
    const std::vector<uint8_t>& adpcm_bank
) {
    if (instrument_info.is_null || instrument_info.sample_size <= 0) {
        return;
    }

    const size_t adpcm_start = static_cast<size_t>(instrument_info.sample_offset);
    const size_t adpcm_end = std::min(
        adpcm_bank.size(),
        adpcm_start + static_cast<size_t>(instrument_info.sample_size)
    );
    if (adpcm_start >= adpcm_bank.size() || adpcm_start >= adpcm_end) {
        return;
    }

    if (is_zero_block(adpcm_bank, adpcm_start)) {
        instrument_info.start_offset_bytes = kBlockSize;
        instrument_info.start_sample_skip = kSamplesPerBlock;
        spu_instrument.start_offset_bytes = kBlockSize;
        spu_instrument.start_sample_skip = kSamplesPerBlock;
    }

    int32_t decoded_samples = 0;
    for (size_t offset = adpcm_start; offset + kBlockSize <= adpcm_end; offset += kBlockSize) {
        const uint8_t flags = adpcm_bank[offset + 1];
        const int32_t local_offset = static_cast<int32_t>(offset - adpcm_start);

        if ((flags & kFlagLoopStart) != 0U) {
            instrument_info.loop_start = decoded_samples;
            instrument_info.loop_offset_bytes = local_offset;
            instrument_info.has_explicit_loop_start = true;
            spu_instrument.loop_start = decoded_samples;
            spu_instrument.loop_offset_bytes = local_offset;
            spu_instrument.has_explicit_loop_start = true;
        }

        if ((flags & kFlagLoopRepeat) != 0U) {
            instrument_info.has_loop_repeat = true;
            spu_instrument.has_loop_repeat = true;
        }

        decoded_samples += kSamplesPerBlock;

        if ((flags & kFlagLoopEnd) != 0U) {
            if ((flags & kFlagLoopRepeat) != 0U && instrument_info.loop_start < 0) {
                instrument_info.loop_start = 0;
                spu_instrument.loop_start = 0;
            }
            break;
        }
    }
}

}  // namespace

FFTWavesetLoadResult FFTWavesetFileService::load_from_path(const std::string& waveset_path) {
    clear();

    std::ifstream input(waveset_path, std::ios::binary);
    if (!input) {
        return FFTWavesetLoadResult {
            .ok = false,
            .message = "Could not open WAVESET file",
            .instrument_count = 0,
        };
    }

    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()
    );
    if (data.size() < static_cast<size_t>(kEntryOffset)) {
        return FFTWavesetLoadResult {
            .ok = false,
            .message = "WAVESET file too small",
            .instrument_count = 0,
        };
    }

    if (data[0] != 'd' || data[1] != 'w' || data[2] != 'd' || data[3] != 's') {
        return FFTWavesetLoadResult {
            .ok = false,
            .message = "Invalid WAVESET signature",
            .instrument_count = 0,
        };
    }

    const uint32_t data_offset = read_u32_le(data, 0x10);
    if (data_offset < static_cast<uint32_t>(kEntryOffset) || data_offset > data.size()) {
        return FFTWavesetLoadResult {
            .ok = false,
            .message = "Invalid WAVESET data offset",
            .instrument_count = 0,
        };
    }
    if (((data_offset - kEntryOffset) % kEntrySize) != 0U) {
        return FFTWavesetLoadResult {
            .ok = false,
            .message = "Invalid WAVESET entry table size",
            .instrument_count = 0,
        };
    }

    const int32_t entry_count = static_cast<int32_t>((data_offset - kEntryOffset) / kEntrySize);
    instruments_.reserve(static_cast<size_t>(entry_count));
    spu_instruments_.reserve(static_cast<size_t>(entry_count));
    adpcm_bank_.assign(data.begin() + static_cast<std::ptrdiff_t>(data_offset), data.end());

    for (int32_t instrument_id = 0; instrument_id < entry_count; ++instrument_id) {
        const size_t entry_offset = static_cast<size_t>(kEntryOffset + instrument_id * kEntrySize);
        const int32_t sample_offset = static_cast<int32_t>(read_u32_le(data, entry_offset));
        const int32_t sample_size = static_cast<int32_t>(read_u16_le(data, entry_offset + 4));
        const int32_t fine_tune = static_cast<int32_t>(read_s16_le(data, entry_offset + 6));

        const uint8_t ar = data[entry_offset + 8] & 0x7F;
        const uint8_t dr = data[entry_offset + 9] & 0x0F;
        const uint8_t sr = data[entry_offset + 10] & 0x7F;
        const uint8_t rr = data[entry_offset + 11] & 0x1F;
        const uint8_t sl = data[entry_offset + 12] & 0x0F;
        const uint8_t sm = (data[entry_offset + 14] >> 2) & 0x01;
        const uint8_t rm = (data[entry_offset + 15] >> 2) & 0x01;

        FFTInstrumentInfo instrument_info;
        instrument_info.instrument_id = instrument_id;
        instrument_info.name = "Instrument " + std::to_string(instrument_id);
        instrument_info.is_null = (sample_offset == 0 && sample_size == 0);
        instrument_info.fine_tune = fine_tune;
        instrument_info.adsr1 = (static_cast<int32_t>(ar) << 8) |
            (static_cast<int32_t>(dr) << 4) |
            static_cast<int32_t>(sl);
        instrument_info.adsr2 = (static_cast<int32_t>(sm) << 15) |
            (1 << 14) |
            (static_cast<int32_t>(sr) << 6) |
            (static_cast<int32_t>(rm) << 5) |
            static_cast<int32_t>(rr);
        instrument_info.sample_offset = sample_offset;
        instrument_info.sample_size = sample_size;

        FFTSpuInstrumentData spu_instrument;
        spu_instrument.is_null = instrument_info.is_null;
        spu_instrument.fine_tune = instrument_info.fine_tune;
        spu_instrument.adsr1 = instrument_info.adsr1;
        spu_instrument.adsr2 = instrument_info.adsr2;
        spu_instrument.sample_offset = instrument_info.sample_offset;
        spu_instrument.sample_size = instrument_info.sample_size;

        fill_loop_metadata(instrument_info, spu_instrument, adpcm_bank_);

        instruments_.push_back(instrument_info);
        spu_instruments_.push_back(spu_instrument);
    }

    loaded_ = true;
    loaded_path_ = waveset_path;
    return FFTWavesetLoadResult {
        .ok = true,
        .message = "Loaded WAVESET metadata",
        .instrument_count = entry_count,
    };
}

bool FFTWavesetFileService::is_loaded() const {
    return loaded_;
}

std::string FFTWavesetFileService::loaded_path() const {
    return loaded_path_;
}

std::string FFTWavesetFileService::bank_name() const {
    return bank_name_;
}

int32_t FFTWavesetFileService::instrument_count() const {
    return static_cast<int32_t>(instruments_.size());
}

std::optional<FFTInstrumentInfo> FFTWavesetFileService::instrument_info(int32_t instrument_id) const {
    if (!has_instrument(instrument_id)) {
        return std::nullopt;
    }
    return instruments_[static_cast<size_t>(instrument_id)];
}

bool FFTWavesetFileService::has_instrument(int32_t instrument_id) const {
    return instrument_id >= 0 && instrument_id < instrument_count();
}

std::string FFTWavesetFileService::describe_instrument(int32_t instrument_id) const {
    const auto info = instrument_info(instrument_id);
    return info.has_value() ? info->name : "Missing Instrument";
}

const std::vector<FFTSpuInstrumentData>& FFTWavesetFileService::spu_instruments() const {
    return spu_instruments_;
}

const std::vector<uint8_t>& FFTWavesetFileService::adpcm_bank() const {
    return adpcm_bank_;
}

void FFTWavesetFileService::clear() {
    loaded_ = false;
    loaded_path_.clear();
    instruments_.clear();
    spu_instruments_.clear();
    adpcm_bank_.clear();
}

}  // namespace fftplugin
