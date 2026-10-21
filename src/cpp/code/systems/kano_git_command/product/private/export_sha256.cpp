// export_sha256.cpp -- SHA-256 streaming helper for kog export command.
//
// Translation-unit isolation: this file does NOT include <filesystem>,
// <format>, <regex>, <iostream>, <fstream> or <functional> on
// purpose, because on windows-ninja-msvc the GCC libstdc++ headers
// transitively pull in <x86intrin.h> (via
// <bits/locale_facets.h>), and <x86intrin.h> defines several short
// parameter-name macros (InValue, InWord, value, w, word, stream,
// ...) AND expands "RotateRight32" itself to the Windows intrinsic
// "_rotr", which would otherwise eat the SHA-256 helper names.
//
// This translation unit's PUBLIC identifiers (the function name and
// the parameter names visible to the header and to callers) are
// therefore deliberately renamed to "k*" Google-style names
// (kRotR32, kValue, kShift, kState, kBlock, kStateIo, kData,
// kSize) that no MinGW header uses.  The IMPLEMENTATION internals
// (loop variables, block expansion, etc.) keep the classic SHA-256
// reference names (a, b, c, d, e, f, g, h, s0, s1) and the original
// "InValue" / "InBits" parameter names inside RotateRight32's body
// (which parse cleanly here because the macros are NOT active in
// this TU).
//
// KOG-BUG-0107 r10: the public API is now kRotR32 / FormatSha256State
// / ProcessSha256Block / Sha256Stream with the k* parameter naming.

#include "export_sha256.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace kano::git::commands::export_sha256 {

auto kRotR32(std::uint32_t InValue, int InBits) -> std::uint32_t {
    return (InValue >> InBits) | (InValue << (32 - InBits));
}

auto FormatSha256State(const std::array<std::uint32_t, 8>& kState) -> std::string {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto part : kState) {
        out << std::setw(8) << part;
    }
    return out.str();
}

void ProcessSha256Block(const unsigned char* kBlock,
                        std::array<std::uint32_t, 8>& kStateIo) {
    static constexpr std::array<std::uint32_t, 64> kConstants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };

    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) {
        const std::size_t p = i * 4;
        words[i] = (static_cast<std::uint32_t>(kBlock[p]) << 24) |
                   (static_cast<std::uint32_t>(kBlock[p + 1]) << 16) |
                   (static_cast<std::uint32_t>(kBlock[p + 2]) << 8) |
                   static_cast<std::uint32_t>(kBlock[p + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = kRotR32(words[i - 15], 7) ^ kRotR32(words[i - 15], 18) ^ (words[i - 15] >> 3);
        const std::uint32_t s1 = kRotR32(words[i - 2], 17) ^ kRotR32(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    std::uint32_t a = kStateIo[0];
    std::uint32_t b = kStateIo[1];
    std::uint32_t c = kStateIo[2];
    std::uint32_t d = kStateIo[3];
    std::uint32_t e = kStateIo[4];
    std::uint32_t f = kStateIo[5];
    std::uint32_t g = kStateIo[6];
    std::uint32_t h = kStateIo[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = kRotR32(e, 6) ^ kRotR32(e, 11) ^ kRotR32(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + kConstants[i] + words[i];
        const std::uint32_t s0 = kRotR32(a, 2) ^ kRotR32(a, 13) ^ kRotR32(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    kStateIo[0] += a;
    kStateIo[1] += b;
    kStateIo[2] += c;
    kStateIo[3] += d;
    kStateIo[4] += e;
    kStateIo[5] += f;
    kStateIo[6] += g;
    kStateIo[7] += h;
}

void Sha256Stream::Update(const unsigned char* kData, std::size_t kSize) {
    totalBytes_ += static_cast<std::uint64_t>(kSize);
    std::size_t offset = 0;
    if (bufferSize_ > 0) {
        const std::size_t toCopy = std::min(kSize, buffer_.size() - bufferSize_);
        std::copy_n(kData, toCopy, buffer_.begin() + static_cast<std::ptrdiff_t>(bufferSize_));
        bufferSize_ += toCopy;
        offset += toCopy;
        if (bufferSize_ == buffer_.size()) {
            ProcessSha256Block(buffer_.data(), state_);
            bufferSize_ = 0;
        }
    }

    while (offset + buffer_.size() <= kSize) {
        ProcessSha256Block(kData + offset, state_);
        offset += buffer_.size();
    }

    if (offset < kSize) {
        bufferSize_ = kSize - offset;
        std::copy_n(kData + offset, bufferSize_, buffer_.begin());
    }
}

auto Sha256Stream::Finalize() -> std::string {
    const std::uint64_t bitLength = totalBytes_ * 8ULL;
    buffer_[bufferSize_++] = 0x80U;
    if (bufferSize_ > 56U) {
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(bufferSize_), buffer_.end(), 0U);
        ProcessSha256Block(buffer_.data(), state_);
        bufferSize_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(bufferSize_), buffer_.begin() + 56, 0U);
    for (std::size_t i = 0; i < 8; ++i) {
        buffer_[56 + i] = static_cast<unsigned char>((bitLength >> ((7 - i) * 8)) & 0xffU);
    }
    ProcessSha256Block(buffer_.data(), state_);
    return FormatSha256State(state_);
}

}  // namespace kano::git::commands::export_sha256