#include "pch.hpp"
#include "ui_bias.hpp"

#include "lab.hpp"
#include "log.hpp"

#if MGS4E_LAB_BUILD

// Two rewrites of an upload the game already made, keyed on identity, applied at Unmap on the
// deferred context before the draw is recorded (see pw/docs/ui-elements.md, section 3):
//
//   tbias <tx> <ty> <dx> <dy>          move every UI draw whose world-matrix translation row is
//                                       (tx, ty) by (dx, dy) canvas units (constant upload)
//   tbias any <dx> <dy> | tbias clear
//   bias <W>x<H> <dx> <dy> [x0 y0 x1 y1] move every quad drawn with a WxH texture, optionally
//                                       only the one whose canvas rectangle matches (vertex upload)
//   bias clear

namespace
{
    struct VertexBias { UINT texW, texH; bool hasRect; float x0, y0, x1, y1; float dx, dy; };
    struct TranslationBias { bool any; float tx, ty; float dx, dy; };

    std::mutex g_Mutex;
    std::vector<VertexBias> g_Vertex;
    std::vector<TranslationBias> g_Translation;
    std::atomic<uint64_t> g_AppliedVertex { 0 };
    std::atomic<uint64_t> g_AppliedTranslation { 0 };
    std::atomic<bool> g_GameplaySeen { false };

    // Stride of a UI upload without the draw count: whole quads of 24-byte textured vertices
    // (144 B each) or 16-byte untextured ones (96 B each); otherwise by divisibility.
    size_t GuessStride(size_t size)
    {
        if (size % 144 == 0) { return 24; }
        if (size % 96 == 0) { return 16; }
        return (size % 24 == 0) ? 24 : 16;
    }
}

namespace UiBias
{
    bool Active()
    {
        std::lock_guard lock(g_Mutex);
        return !g_Vertex.empty() || !g_Translation.empty();
    }

    uint64_t AppliedVertex() { return g_AppliedVertex.load(); }
    uint64_t AppliedTranslation() { return g_AppliedTranslation.load(); }

    bool GameplaySeen() { return g_GameplaySeen.load(); }

    void OnConstantUnmap(void* data, size_t size)
    {
        // Gameplay marker: the mission timer / remaining-enemies group draws with translation
        // (-11, -8) and the UI ortho; nothing outside a mission uses that row.
        if (!g_GameplaySeen.load() && size >= 32 * sizeof(float))
        {
            const float* f = static_cast<const float*>(data);
            if (std::fabs(f[0] - 2.0f / 480.0f) <= 1e-5f && std::fabs(f[28] + 11.0f) < 0.5f && std::fabs(f[29] + 8.0f) < 0.5f && !g_GameplaySeen.exchange(true))
            {
                spdlog::info("PW census: GAMEPLAY HUD: the mission timer group is being drawn.");
            }
        }
        std::lock_guard lock(g_Mutex);
        if (g_Translation.empty() || size < 32 * sizeof(float)) { return; }
        float* f = static_cast<float*>(data);
        // Only the UI ortho (row 1 = 2/480): leave 3D alone. The world basis (rows 5-7) may
        // be rotated (vertical text) or scaled (gauges); the translation row is canvas units
        // regardless, so it is always the handle.
        if (std::fabs(f[0] - 2.0f / 480.0f) > 1e-5f) { return; }
        for (const TranslationBias& b : g_Translation)
        {
            if (!b.any && (std::fabs(f[28] - b.tx) > 0.5f || std::fabs(f[29] - b.ty) > 0.5f)) { continue; }
            f[28] += b.dx;
            f[29] += b.dy;
            g_AppliedTranslation.fetch_add(1);
        }
    }

    void OnVertexUnmap(const std::string& texture, void* data, size_t size)
    {
        std::lock_guard lock(g_Mutex);
        if (g_Vertex.empty()) { return; }
        UINT w = 0, h = 0;
        if (sscanf_s(texture.c_str(), "%ux%u", &w, &h) != 2) { return; }
        const size_t stride = GuessStride(size);
        if (stride < 12 || size < stride) { return; }
        const size_t count = size / stride;
        auto* p = static_cast<uint8_t*>(data);
        for (const VertexBias& b : g_Vertex)
        {
            if (b.texW != w || b.texH != h) { continue; }
            if (b.hasRect)
            {
                float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
                for (size_t i = 0; i < count; i++)
                {
                    float xy[2];
                    std::memcpy(xy, p + i * stride + stride - 12, sizeof(xy));
                    minX = std::min(minX, xy[0]); maxX = std::max(maxX, xy[0]);
                    minY = std::min(minY, xy[1]); maxY = std::max(maxY, xy[1]);
                }
                if (std::fabs(minX - b.x0) > 0.5f || std::fabs(minY - b.y0) > 0.5f || std::fabs(maxX - b.x1) > 0.5f || std::fabs(maxY - b.y1) > 0.5f) { continue; }
            }
            for (size_t i = 0; i < count; i++)
            {
                float* xy = reinterpret_cast<float*>(p + i * stride + stride - 12);
                xy[0] += b.dx;
                xy[1] += b.dy;
            }
            g_AppliedVertex.fetch_add(1);
        }
    }

    bool Command(const std::string& line)
    {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        if (cmd == "bias")
        {
            std::string tex;
            in >> tex;
            std::lock_guard lock(g_Mutex);
            if (tex == "clear") { g_Vertex.clear(); spdlog::info("PW live: vertex biases cleared."); return true; }
            VertexBias b {};
            if (sscanf_s(tex.c_str(), "%ux%u", &b.texW, &b.texH) != 2 || !(in >> b.dx >> b.dy))
            {
                spdlog::warn("PW live: bad bias '{}' (bias <W>x<H> <dx> <dy> [x0 y0 x1 y1] | bias clear).", line);
                return true;
            }
            if (in >> b.x0 >> b.y0 >> b.x1 >> b.y1) { b.hasRect = true; }
            g_Vertex.push_back(b);
            spdlog::info("PW live: vertex bias on texture {}x{}{} by ({}, {}); {} active.", b.texW, b.texH,
                b.hasRect ? std::format(" rect ({},{})-({},{})", b.x0, b.y0, b.x1, b.y1) : std::string(""), b.dx, b.dy, g_Vertex.size());
            return true;
        }
        if (cmd == "tbias")
        {
            std::string first;
            in >> first;
            std::lock_guard lock(g_Mutex);
            if (first == "clear") { g_Translation.clear(); spdlog::info("PW live: translation biases cleared."); return true; }
            TranslationBias b {};
            bool ok = false;
            if (first == "any") { b.any = true; ok = static_cast<bool>(in >> b.dx >> b.dy); }
            else
            {
                try { b.tx = std::stof(first); ok = static_cast<bool>(in >> b.ty >> b.dx >> b.dy); } catch (...) { ok = false; }
            }
            if (!ok) { spdlog::warn("PW live: bad tbias '{}' (tbias <tx> <ty> <dx> <dy> | tbias any <dx> <dy> | tbias clear).", line); return true; }
            g_Translation.push_back(b);
            if (b.any) { spdlog::info("PW live: translation bias on every UI draw by ({}, {}); {} active.", b.dx, b.dy, g_Translation.size()); }
            else { spdlog::info("PW live: translation bias on ({}, {}) by ({}, {}); {} active.", b.tx, b.ty, b.dx, b.dy, g_Translation.size()); }
            return true;
        }
        return false;
    }
}

#endif
