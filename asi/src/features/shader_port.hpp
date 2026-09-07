#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Porting a DirectX 11 pixel-shader replacement to the game's DirectX 12 shader build.
//
// FusionFix (and any mod made the same way with 3Dmigoto) ships pixel-shader replacements
// as DXBC blobs named by the DirectX 11 original's container checksum, and swaps them in
// through a hook on the DirectX 11 device. The game's DirectX 12 backend uses a separately
// compiled set of the same shaders (measured 2026-09-07): the code is the same to the byte
// except that every constant buffer is declared larger and every immediate constant index
// is shifted by that size difference (the same constants sit after a common block), and the
// vertex shaders feed narrower input masks. So a DirectX 11 replacement becomes a working
// DirectX 12 one by: shifting each constant-buffer index by the per-slot size difference,
// declaring the DirectX 12 sizes, taking the DirectX 12 original's input-signature chunk,
// and re-signing the container (D3D12 checks the checksum; it is MD5 with Microsoft's own
// final block). Which DirectX 11 checksum corresponds to which DirectX 12 one comes from a
// map file shipped beside the replacements folder, since the replacement's own size cannot
// tell same-signature candidates apart. Every port is checked: the shifted indices must be
// what the DirectX 12 original reads.
namespace mgs4e::shaderport
{
    using Checksum = std::array<uint8_t, 16>;

    // "8-8-8-8" hex dwords, little-endian, as FusionFix names its files -> the 16 header bytes.
    bool ParseChecksumName(const std::string& name, Checksum& out);
    std::string ChecksumName(const Checksum& sum);

    // The DXBC container checksum of a blob (the 16 bytes at +4).
    Checksum ComputeChecksum(const uint8_t* blob, size_t length);

    // Ports `replacement` (a DirectX 11 replacement) against `original` (the DirectX 12 shader
    // a pipeline is being built with). Returns the ported blob, or empty with `why` set.
    std::vector<uint8_t> Port(const std::vector<uint8_t>& replacement, const uint8_t* original, size_t originalLength, std::string& why);

    // Reads "<dx11 checksum> <dx12 checksum>" lines. Returns dx12 -> dx11.
    std::map<Checksum, Checksum> LoadMap(const std::filesystem::path& file);
}
