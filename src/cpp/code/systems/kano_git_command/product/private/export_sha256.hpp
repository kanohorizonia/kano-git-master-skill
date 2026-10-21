// export_sha256.hpp -- SHA-256 streaming helper for kog export command.
//
// KOG-BUG-0107: this header and export_sha256.cpp live in a separate
// translation unit (no <filesystem>, no <format>, no <regex>, etc.)
// so the original InValue / InBits parameter names parse cleanly.
// The header itself is included by export_cmd.cpp which on
// windows-ninja-msvc transitively includes <filesystem> ->
// <bits/locale_facets.h> -> <x86intrin.h>, which defines macros for
// many short identifiers (InValue, InWord, value, w, word, stream,
// AND RotateRight32 itself -- expanded to the Windows intrinsic
// _rotr).  We push/undef/pop the entire known collision set around
// the declarations so the original InValue / InBits / RotateRight32
// names parse in the header's namespace.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// KOG-BUG-0107: MinGW <x86intrin.h> parameter-name macros.
// push / undef / pop them around the namespace below.
#pragma push_macro("RotateRight32")
#pragma push_macro("FormatSha256State")
#pragma push_macro("ProcessSha256Block")
#pragma push_macro("Sha256Stream")
#pragma push_macro("InValue")
#pragma push_macro("InBits")
#pragma push_macro("InState")
#pragma push_macro("InBlock")
#pragma push_macro("InOutState")
#pragma push_macro("InData")
#pragma push_macro("InSize")
#pragma push_macro("InWord")
#pragma push_macro("value")
#pragma push_macro("word")
#pragma push_macro("w")
#pragma push_macro("stream")
#undef RotateRight32
#undef FormatSha256State
#undef ProcessSha256Block
#undef Sha256Stream
#undef InValue
#undef InBits
#undef InState
#undef InBlock
#undef InOutState
#undef InData
#undef InSize
#undef InWord
#undef value
#undef word
#undef w
#undef stream

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

// KOG-BUG-0107: pop the macros we pushed above so the rest of the
// including translation unit (export_cmd.cpp on windows-ninja-msvc
// still has <x86intrin.h> macros active for its own code) sees them
// again.  This is intentionally local to the header.
#pragma pop_macro("stream")
#pragma pop_macro("w")
#pragma pop_macro("word")
#pragma pop_macro("value")
#pragma pop_macro("InWord")
#pragma pop_macro("InSize")
#pragma pop_macro("InData")
#pragma pop_macro("InOutState")
#pragma pop_macro("InBlock")
#pragma pop_macro("InState")
#pragma pop_macro("InBits")
#pragma pop_macro("InValue")
#pragma pop_macro("Sha256Stream")
#pragma pop_macro("ProcessSha256Block")
#pragma pop_macro("FormatSha256State")
#pragma pop_macro("RotateRight32")