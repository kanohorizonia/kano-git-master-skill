// export_sha256.hpp -- SHA-256 streaming helper for kog export command.
//
// IMPORTANT: This header and its implementation in export_sha256.cpp
// are deliberately kept free of any include that transitively pulls in
// MinGW's <x86intrin.h> (which happens via <filesystem> -> <locale> ->
// <bits/locale_facets.h> on GCC's libstdc++ on windows-ninja-msvc).
// <x86intrin.h> defines several short parameter-name macros
// (InValue, InWord, value, w, word, stream, ...) that collide with
// common identifier names used in this SHA-256 implementation.
// KOG-BUG-0107 unblocked P2 only after the RotateRight32 helper was
// moved out of export_cmd.cpp into a translation unit that does NOT
// include <filesystem> or anything that drags in <x86intrin.h>.
//
// The implementation in export_sha256.cpp uses only the minimal safe
// includes <array>, <algorithm>, <cstdint>, <cstring>, <sstream>,
// <string>.  Do NOT add <filesystem>, <format>, <regex>, <iostream>,
// <fstream> or <functional> here -- they transitively include
// <x86intrin.h> on windows-ninja-msvc and reintroduce the macro
// collision.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kano::git::commands::export_sha256 {

// Rotates a 32-bit word right by InBits positions.
auto RotateRight32(std::uint32_t InValue, int InBits) -> std::uint32_t;

// Formats the eight 32-bit state words of a SHA-256 midstate as a
// 64-character lowercase hex string.
auto FormatSha256State(const std::array<std::uint32_t, 8>& InState) -> std::string;

// Processes one 64-byte SHA-256 block, mutating InOutState in place.
void ProcessSha256Block(const unsigned char* InBlock,
                        std::array<std::uint32_t, 8>& InOutState);

// Streaming SHA-256 hasher.  The finalizer returns the lowercase hex
// digest as a 64-character string.
class Sha256Stream {
public:
    void Update(const unsigned char* InData, std::size_t InSize);
    auto Finalize() -> std::string;

private:
    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U,
                                          0x3c6ef372U, 0xa54ff53aU,
                                          0x510e527fU, 0x9b05688cU,
                                          0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> buffer_{};
    std::size_t bufferSize_ = 0;
    std::uint64_t totalBytes_ = 0;
};

}  // namespace kano::git::commands::export_sha256
