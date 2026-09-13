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

    // The unmapped buffer is a constant buffer of `size` bytes, still writable. Returns true
    // when it is a UI upload (the 480x272 ortho in its first row), for the draw that follows.
    bool OnConstantUnmap(void* data, size_t size, float* shiftUnits = nullptr);

    // Wide-scene experiment (2026-09-13): the scene target is `wideUnits` canvas units wide at
    // integer `scale` while the UI keeps its 480-unit canvas. Installs the camera hooks; the
    // census then calls BeforeDraw/AfterDraw around every draw with `uiUpload` from
    // OnConstantUnmap, and UI draws get a centred 480-unit viewport.
    void ConfigureWideScene(int canvasW, int canvasH, int scale);
    bool WideSceneActive();
    // psHash: the bound pixel shader's hash (0 if unknown); the post chain's shaders stay wide.
    // shiftUnits: the wide-scene move (canvas units) recorded for the bound constants by
    // OnConstantUnmap; the element viewport slides by it instead of the translation.
    void BeforeDraw(ID3D11DeviceContext* ctx, bool uiUpload, uint64_t psHash, float shiftUnits);
    // What BeforeDraw would do with this draw, for the census line: "3d" (no UI ortho),
    // "post" (post-chain shader), "full" (full-canvas quad), "element" (centred).
    const char* Classify(ID3D11DeviceContext* ctx, bool uiUpload, uint64_t psHash);
    void AfterDraw(ID3D11DeviceContext* ctx, bool uiUpload);
    // The game sets its viewport and scissor from the HUD scale width (480 x scale) at the
    // origin, so on the wide target everything lands in the left part. These shift such a
    // rectangle to the centre of the wide target (in place; true when anything changed).
    bool AdjustViewports(UINT count, D3D11_VIEWPORT* viewports);
    bool AdjustScissors(UINT count, D3D11_RECT* rects);

    // The unmapped buffer is a vertex buffer of `size` bytes, still writable; `texture` is
    // the census's description of the texture bound to pixel-shader slot 0 ("WxH fmtN 0x...").
    void OnVertexUnmap(ID3D11DeviceContext* ctx, const std::string& texture, void* data, size_t size);

    // True once the mission HUD has been drawn (the timer group's constant upload seen); logged
    // once as "GAMEPLAY HUD" so a run script can wait for it before measuring.
    bool GameplaySeen();

    // How many uploads each kind of bias has rewritten so far (for the census frame line).
    uint64_t AppliedVertex();
    uint64_t AppliedTranslation();
}
