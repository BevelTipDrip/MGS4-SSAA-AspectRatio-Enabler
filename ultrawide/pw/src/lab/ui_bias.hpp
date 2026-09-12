#pragma once

// Live editing of Peace Walker UI elements (private module, Lab only). The public draw census
// (pw\src\features\draw_census.cpp) calls these from its Unmap hooks and its live command
// reader; without this module the public stub header turns them into no-ops.
//
// The method, the identity and the register are described in pw/docs/ui-elements.md.
namespace UiBias
{
    // Parses and applies a live command line; returns true when the line was one of ours
    // (bias ..., tbias ...), whether or not it was well formed.
    bool Command(const std::string& line);

    // True while any bias is registered; the census keeps the bound texture current when so.
    bool Active();

    // The unmapped buffer is a constant buffer of `size` bytes, still writable.
    void OnConstantUnmap(void* data, size_t size);

    // The unmapped buffer is a vertex buffer of `size` bytes, still writable; `texture` is
    // the census's description of the texture bound to pixel-shader slot 0 ("WxH fmtN 0x...").
    void OnVertexUnmap(const std::string& texture, void* data, size_t size);

    // How many uploads each kind of bias has rewritten so far (for the census frame line).
    uint64_t AppliedVertex();
    uint64_t AppliedTranslation();
}
