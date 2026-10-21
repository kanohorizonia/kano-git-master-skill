// export_sha256.hpp -- SHA-256 streaming helper for kog export command.
//
// KOG-BUG-0107: the previous r6..r9 attempts (separate translation
// unit, push_macro / #undef / pop_macro) all hit a wall on
// windows-ninja-msvc: the MinGW <x86intrin.h> header transitively
// included by <filesystem> defines a number of short parameter-name
// macros AND expands the function name "RotateRight32" itself to
// the Windows intrinsic "_rotr".  Neither parameter renaming nor
// #undef of the name tokens could reliably win against a MinGW
// header that turns entire identifiers into macros, because the
// #pragma arguments themselves get preprocessed before the pragma
// fires.
//
// The fix that actually works: rename everything to identifiers that
// no MinGW header uses.  We use a `k_` Google-style prefix on every
// identifier that was previously colliding (kRotR32, kValue, kShift,
// kState, kBlock, kStateIo, kData, kSize).  The original InValue /
// InBits / etc. names are kept ONLY inside the implementation file
// (export_sha256.cpp) which does not transitively include
// <x86intrin.h>, so they parse cleanly there.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kano::git::commands::export_sha256 {

// Rotates a 32-bit word right by kShift positions.
auto kRotR32(std::uint32_t kValue, int kShift) -> std::uint32_t;

// Formats the eight 32-bit state words of a SHA-256 midstate as a
// 64-character lowercase hex string.
auto FormatSha256State(const std::array<std::uint32_t, 8>& kState) -> std::string;

// Processes one 64-byte SHA-256 block, mutating kStateIo in place.
void ProcessSha256Block(const unsigned char* kBlock,
                        std::array<std::uint32_t, 8>& kStateIo);

// Streaming SHA-256 hasher.  The finalizer returns the lowercase hex
// digest as a 64-character string.
class Sha256Stream {
public:
    void Update(const unsigned char* kData, std::size_t kSize);
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