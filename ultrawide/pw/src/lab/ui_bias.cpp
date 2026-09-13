#include "pch.hpp"
#include <atomic>
#include "ui_bias.hpp"

#include "game.hpp"
#include "lab.hpp"
#include "log.hpp"
#include "mem.hpp"

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

    // ---- wide scene ------------------------------------------------------------------------
    // The camera builder (+8EE22: `movaps xmm3, xmm2; divss xmm3, [rbx+0x250]`) reads the
    // canvas's inverse aspect at [rbx+0x250] (272/480 natively) and the FOV argument arrives in
    // xmm2 at +8EBB1 (`movaps xmm6, xmm2`, stored to [rcx+0x254] and used for the projection and
    // the culling). Widening the aspect alone stretches the world horizontally (Hor+ at the same
    // horizontal FOV would be Vert-); dividing the FOV by the widening keeps the vertical FOV.
    constexpr uintptr_t kCameraAspect = 0x8EE22;
    constexpr uintptr_t kCameraFov = 0x8EBB1;
    SafetyHookMid g_CameraAspect {}, g_CameraFov {};
    int g_WideUnits = 0, g_WideScale = 0;
    std::atomic<uint32_t> g_DrawCalls { 0 }, g_ElementDraws { 0 }, g_Narrowed { 0 }, g_WideOrthoShifted { 0 };
    struct LastRect { bool valid = false; size_t count = 0; float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
    std::unordered_map<ID3D11DeviceContext*, LastRect> g_LastRect;
    float g_WideInvAspect = 0, g_WideRatio = 1;
    struct SavedViewport { D3D11_VIEWPORT vp[16]; UINT n; D3D11_RECT sc[16]; UINT nsc; bool scChanged; };
    std::unordered_map<ID3D11DeviceContext*, SavedViewport> g_SavedViewports;   // under g_Mutex

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

    void ConfigureWideScene(int wideUnits, int scale)
    {
        if (wideUnits <= 480 || scale <= 0) { return; }
        g_WideUnits = wideUnits; g_WideScale = scale;
        g_WideInvAspect = 272.0f / static_cast<float>(wideUnits);
        g_WideRatio = static_cast<float>(wideUnits) / 480.0f;
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        const uint8_t* a = reinterpret_cast<const uint8_t*>(base + kCameraAspect);
        const uint8_t* f = reinterpret_cast<const uint8_t*>(base + kCameraFov);
        if (a[0] == 0x0F && a[1] == 0x28 && a[2] == 0xDA && f[0] == 0x0F && f[1] == 0x28 && f[2] == 0xF2)
        {
            g_CameraAspect = safetyhook::create_mid(base + kCameraAspect, [](SafetyHookContext& ctx)
            {
                *reinterpret_cast<float*>(ctx.rbx + 0x250) = g_WideInvAspect;
            });
            g_CameraFov = safetyhook::create_mid(base + kCameraFov, [](SafetyHookContext& ctx)
            {
                ctx.xmm2.f32[0] /= g_WideRatio;
            });
            spdlog::info("PW wide scene: {} canvas units at scale {}: inverse aspect {:.4f}, FOV / {:.4f}; UI draws in a centred 480-unit viewport ({}/{}).",
                wideUnits, scale, g_WideInvAspect, g_WideRatio, g_CameraAspect ? "ok" : "FAILED", g_CameraFov ? "ok" : "FAILED");
        }
        else { spdlog::warn("PW wide scene: camera sites do not look as expected; not installed."); g_WideUnits = 0; }
    }

    bool WideSceneActive() { return g_WideUnits > 0; }


    // The post chain's pixel shaders (resolve, grading, fade, depth of field, final blit) and
    // the fill: their quads cover the canvas and must cover the wide target too.
    constexpr uint64_t kPostShaders[] = { 0x6c27b4cfull, 0x2752864aull, 0xbaa9f4b4ull, 0xfcb43266ull, 0x493494fdull, 0x7023c633ull, 0x4c3c9a5full };

    const char* Classify(ID3D11DeviceContext* ctx, bool uiUpload, uint64_t psHash)
    {
        if (!uiUpload) { return "3d"; }
        const uint64_t ps32 = psHash >> 32;
        for (const uint64_t h : kPostShaders) { if (ps32 == h) { return "post"; } }
        std::lock_guard lock(g_Mutex);
        const auto it = g_LastRect.find(ctx);
        if (it != g_LastRect.end() && it->second.valid)
        {
            const LastRect& r = it->second;
            // one quad covering the canvas; a batch of many quads (a whole menu panel) that
            // happens to span it is a set of elements
            if (r.count <= 6 && r.x0 <= 1.0f && r.y0 <= 1.0f && r.x1 >= 479.0f && r.y1 >= 271.0f) { return "full"; }
        }
        return "element";
    }

    void BeforeDraw(ID3D11DeviceContext* ctx, bool uiUpload, uint64_t psHash, float shiftUnits)
    {
        if (g_WideUnits <= 0) { return; }
        const uint32_t n = g_DrawCalls.fetch_add(1) + 1;
        if (n % 5000 == 0) { spdlog::info("PW wide scene: {} draws seen, {} element draws centred, {} of them slid.", n, g_ElementDraws.load(), g_Narrowed.load()); }
        // Only elements: full-canvas quads (fades) cover the wide viewport as they are, the
        // post chain's own vertex shaders place their quads themselves, 3D has no UI ortho.
        if (Classify(ctx, uiUpload, psHash)[0] != 'e') { return; }
        SavedViewport saved {};
        saved.n = 16;
        ctx->RSGetViewports(&saved.n, saved.vp);
        if (saved.n == 0) { return; }
        const float wideW = static_cast<float>(g_WideUnits * g_WideScale);
        if (std::fabs(saved.vp[0].Width - wideW) > 1.0f) { return; }   // only on the full wide target
        g_ElementDraws.fetch_add(1);
        // The element draws into the centred 480-unit part of the wide viewport, at the same
        // scale, slid sideways by its registered move (the 21:9 edge is +-85 units).
        const float hudW = static_cast<float>(480 * g_WideScale);
        const float shift = (wideW - hudW) * 0.5f + shiftUnits * static_cast<float>(g_WideScale);
        D3D11_VIEWPORT ui = saved.vp[0];
        ui.Width = hudW;
        ui.TopLeftX = saved.vp[0].TopLeftX + shift;
        // Its scissor is in the same 480-unit space: a whole-width clip opens to the wide
        // target, anything narrower slides with the viewport.
        saved.nsc = 16; saved.scChanged = false;
        ctx->RSGetScissorRects(&saved.nsc, saved.sc);
        D3D11_RECT sc[16] {};
        const LONG ishift = static_cast<LONG>(shift);
        for (UINT i = 0; i < saved.nsc; i++)
        {
            sc[i] = saved.sc[i];
            if (sc[i].left == 0 && sc[i].right >= static_cast<LONG>(wideW)) { continue; }
            if (sc[i].left == 0 && sc[i].right >= static_cast<LONG>(hudW)) { sc[i].right = static_cast<LONG>(wideW); saved.scChanged = true; continue; }
            sc[i].left += ishift; sc[i].right += ishift; saved.scChanged = true;
        }
        { std::lock_guard lock(g_Mutex); g_SavedViewports[ctx] = saved; }
        if (shiftUnits != 0 && g_Narrowed.fetch_add(1) == 0) { spdlog::info("PW wide scene: first slid element: viewport {}x{} at {} (move {} units).", ui.Width, ui.Height, ui.TopLeftX, shiftUnits); }
        ctx->RSSetViewports(1, &ui);
        if (saved.scChanged) { ctx->RSSetScissorRects(saved.nsc, sc); }
    }

    void AfterDraw(ID3D11DeviceContext* ctx, bool uiUpload)
    {
        if (g_WideUnits <= 0) { return; }
        SavedViewport saved {};
        {
            std::lock_guard lock(g_Mutex);
            g_LastRect[ctx].valid = false;   // one upload, one draw; a text draw has none
            const auto it = g_SavedViewports.find(ctx);
            if (it == g_SavedViewports.end()) { return; }
            saved = it->second;
            g_SavedViewports.erase(it);
        }
        ctx->RSSetViewports(saved.n, saved.vp);
        if (saved.scChanged) { ctx->RSSetScissorRects(saved.nsc, saved.sc); }
    }

    bool AdjustViewports(UINT count, D3D11_VIEWPORT* vp)
    {
        if (g_WideUnits <= 0 || !vp) { return false; }
        const float hudW = static_cast<float>(480 * g_WideScale);
        const float hudH = static_cast<float>(272 * g_WideScale);
        const float shift = static_cast<float>((g_WideUnits - 480) * g_WideScale) * 0.5f;
        bool changed = false;
        for (UINT i = 0; i < count; i++)
        {
            if (std::fabs(vp[i].Width - hudW) < 1.0f && std::fabs(vp[i].Height - hudH) < 1.0f && vp[i].TopLeftX == 0.0f)
            {
                vp[i].Width = hudW + 2 * shift; changed = true;   // the world gets the whole wide target; UI draws narrow it again
            }
        }
        return changed;
    }

    bool AdjustScissors(UINT count, D3D11_RECT* r)
    {
        if (g_WideUnits <= 0 || !r) { return false; }
        const LONG hudW = 480 * g_WideScale, hudH = 272 * g_WideScale;
        const LONG shift = (g_WideUnits - 480) * g_WideScale / 2;
        bool changed = false;
        for (UINT i = 0; i < count; i++)
        {
            if (r[i].left == 0 && r[i].right == hudW && r[i].bottom - r[i].top == hudH)
            {
                r[i].right = hudW + 2 * shift; changed = true;
            }
        }
        return changed;
    }

    bool OnConstantUnmap(void* data, size_t size, float* shiftUnits)
    {
        if (shiftUnits) { *shiftUnits = 0; }
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
        const bool ui = size >= 32 * sizeof(float) && std::fabs(static_cast<const float*>(data)[0] - 2.0f / 480.0f) <= 1e-5f;
        // Every draw's constants start with the UI ortho (the post chain's full-canvas quads
        // included); BeforeDraw tells elements from full-canvas quads by the vertex rectangle.
        std::lock_guard lock(g_Mutex);
        if (g_Translation.empty() || !ui) { return ui; }
        float* f = static_cast<float*>(data);
        // Only the UI ortho (row 1 = 2/480): leave 3D alone. The world basis (rows 5-7) may
        // be rotated (vertical text) or scaled (gauges); the translation row is canvas units
        // regardless, so it is always the handle.
        for (const TranslationBias& b : g_Translation)
        {
            if (!b.any && (std::fabs(f[28] - b.tx) > 0.5f || std::fabs(f[29] - b.ty) > 0.5f)) { continue; }
            if (g_WideUnits > 480 && shiftUnits)
            {
                // Wide scene: the element keeps its layout; its centred viewport slides by dx
                // (dy still moves the translation). Nothing is clipped by the 480-unit viewport.
                *shiftUnits += b.dx;
                f[29] += b.dy;
            }
            else
            {
                f[28] += b.dx;
                f[29] += b.dy;
            }
            g_AppliedTranslation.fetch_add(1);
        }
        return ui;
    }


    // The rectangle (canvas units) of the last vertex upload on each context, consumed by the
    // draw that follows: a quad spanning the whole 480x272 canvas is a fill, a fade or a post
    // pass and stays wide; anything else is an element and is centred.

    void OnVertexUnmap(ID3D11DeviceContext* ctx, const std::string& texture, void* data, size_t size)
    {
        std::lock_guard lock(g_Mutex);
        const size_t stride = GuessStride(size);
        if (stride < 12 || size < stride) { return; }
        const size_t count = size / stride;
        auto* p = static_cast<uint8_t*>(data);
        if (g_WideUnits > 0)
        {
            LastRect r; r.valid = true; r.count = count; r.x0 = r.y0 = 1e9f; r.x1 = r.y1 = -1e9f;
            for (size_t i = 0; i < count; i++)
            {
                float xy[2];
                std::memcpy(xy, p + i * stride + stride - 12, sizeof(xy));
                r.x0 = std::min(r.x0, xy[0]); r.x1 = std::max(r.x1, xy[0]);
                r.y0 = std::min(r.y0, xy[1]); r.y1 = std::max(r.y1, xy[1]);
            }
            g_LastRect[ctx] = r;
        }
        if (g_Vertex.empty()) { return; }
        UINT w = 0, h = 0;
        if (sscanf_s(texture.c_str(), "%ux%u", &w, &h) != 2) { return; }
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
