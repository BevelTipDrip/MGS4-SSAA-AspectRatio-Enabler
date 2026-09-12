#include "pch.hpp"
#include "post_scale.hpp"

#include "log.hpp"

#if MGS4E_LAB_BUILD

#include <d3dcompiler.h>

namespace
{
    ID3D11Device* g_Device = nullptr;
    ID3D11VertexShader* g_Vs = nullptr;
    ID3D11PixelShader* g_Ps = nullptr;
    ID3D11SamplerState* g_Sampler = nullptr;
    ID3D11Buffer* g_Constants = nullptr;
    ID3D11RasterizerState* g_Raster = nullptr;
    ID3D11DepthStencilState* g_NoDepth = nullptr;
    ID3D11BlendState* g_NoBlend = nullptr;
    bool g_Failed = false;
    std::atomic<uint64_t> g_Draws { 0 };

    // Cached views per resource (the game reuses the same textures every frame).
    std::mutex g_Mutex;
    std::unordered_map<ID3D11Resource*, ID3D11RenderTargetView*> g_Rtvs;
    std::unordered_map<ID3D11Resource*, ID3D11ShaderResourceView*> g_Srvs;

    const char* kShader = R"(
        cbuffer C : register(b0) { float2 srcTexel; float2 taps; float2 tapStep; float2 pad; };
        Texture2D src : register(t0);
        SamplerState smp : register(s0);
        struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
        VSOut VS(uint id : SV_VertexID)
        {
            VSOut o;
            float2 uv = float2((id << 1) & 2, id & 2);
            o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
            o.uv = uv;
            return o;
        }
        // Box filter over the source footprint of one destination pixel: taps.x by taps.y
        // bilinear samples, each covering 2x2 source texels, spread over the footprint.
        float4 PS(VSOut i) : SV_Target
        {
            float4 sum = 0;
            float2 start = i.uv - tapStep * (taps - 1) * 0.5;
            for (int y = 0; y < (int)taps.y; y++)
                for (int x = 0; x < (int)taps.x; x++)
                    sum += src.Sample(smp, start + tapStep * float2(x, y));
            return sum / (taps.x * taps.y);
        }
    )";

    typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

    bool Compile(const char* entry, const char* target, ID3DBlob** out)
    {
        HMODULE compiler = GetModuleHandleW(L"d3dcompiler_47.dll");
        if (!compiler) { compiler = LoadLibraryW(L"d3dcompiler_47.dll"); }
        const auto compile = compiler ? reinterpret_cast<PFN_D3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile) { spdlog::error("PW post scale: D3DCompile not available."); return false; }
        ID3DBlob* errors = nullptr;
        const HRESULT r = compile(kShader, std::strlen(kShader), "post_scale", nullptr, nullptr, entry, target, 0, 0, out, &errors);
        if (FAILED(r))
        {
            spdlog::error("PW post scale: {} failed to compile: {}", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            if (errors) { errors->Release(); }
            return false;
        }
        if (errors) { errors->Release(); }
        return true;
    }

    bool EnsureResources()
    {
        if (g_Vs) { return true; }
        if (g_Failed || !g_Device) { return false; }
        ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr;
        if (!Compile("VS", "vs_5_0", &vs) || !Compile("PS", "ps_5_0", &ps)) { g_Failed = true; return false; }
        HRESULT r = g_Device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_Vs);
        if (SUCCEEDED(r)) { r = g_Device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_Ps); }
        vs->Release(); ps->Release();
        D3D11_SAMPLER_DESC sd {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (SUCCEEDED(r)) { r = g_Device->CreateSamplerState(&sd, &g_Sampler); }
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(r)) { r = g_Device->CreateBuffer(&bd, nullptr, &g_Constants); }
        D3D11_RASTERIZER_DESC rd {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        if (SUCCEEDED(r)) { r = g_Device->CreateRasterizerState(&rd, &g_Raster); }
        D3D11_DEPTH_STENCIL_DESC dd {};
        dd.DepthEnable = FALSE;
        if (SUCCEEDED(r)) { r = g_Device->CreateDepthStencilState(&dd, &g_NoDepth); }
        D3D11_BLEND_DESC bl {};
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (SUCCEEDED(r)) { r = g_Device->CreateBlendState(&bl, &g_NoBlend); }
        if (FAILED(r)) { spdlog::error("PW post scale: resource creation failed ({:#x}).", static_cast<uint32_t>(r)); g_Failed = true; return false; }
        spdlog::info("PW post scale: downscale shader and states ready.");
        return true;
    }

    bool Desc(ID3D11Resource* res, D3D11_TEXTURE2D_DESC& d)
    {
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex) { return false; }
        tex->GetDesc(&d);
        tex->Release();
        return true;
    }

    ID3D11RenderTargetView* RtvFor(ID3D11Resource* res)
    {
        const auto it = g_Rtvs.find(res);
        if (it != g_Rtvs.end()) { return it->second; }
        ID3D11RenderTargetView* v = nullptr;
        g_Device->CreateRenderTargetView(res, nullptr, &v);
        g_Rtvs[res] = v;
        return v;
    }

    ID3D11ShaderResourceView* SrvFor(ID3D11Resource* res)
    {
        const auto it = g_Srvs.find(res);
        if (it != g_Srvs.end()) { return it->second; }
        ID3D11ShaderResourceView* v = nullptr;
        g_Device->CreateShaderResourceView(res, nullptr, &v);
        g_Srvs[res] = v;
        return v;
    }
}

namespace PostScale
{
    void SetDevice(ID3D11Device* device) { g_Device = device; }

    bool Downscale(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src)
    {
        D3D11_TEXTURE2D_DESC sd {}, dd {};
        if (!Desc(src, sd) || !Desc(dst, dd)) { return false; }
        if (sd.Width == dd.Width && sd.Height == dd.Height) { return false; }
        if (sd.SampleDesc.Count > 1) { static bool once = false; if (!once) { once = true; spdlog::warn("PW post scale: source is multisampled; the downscale needs MSAA off."); } return false; }
        if (!(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || !(dd.BindFlags & D3D11_BIND_RENDER_TARGET)) { return false; }
        std::lock_guard lock(g_Mutex);
        if (!EnsureResources()) { return false; }
        ID3D11RenderTargetView* rtv = RtvFor(dst);
        ID3D11ShaderResourceView* srv = SrvFor(src);
        if (!rtv || !srv) { return false; }

        // Save the context's state we touch.
        ID3D11RenderTargetView* oldRtv[8] {}; ID3D11DepthStencilView* oldDsv = nullptr;
        ctx->OMGetRenderTargets(8, oldRtv, &oldDsv);
        UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_VIEWPORT oldVp[16] {};
        ctx->RSGetViewports(&nvp, oldVp);
        UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_RECT oldSc[16] {};
        ctx->RSGetScissorRects(&nsc, oldSc);
        ID3D11VertexShader* oldVs = nullptr; ID3D11PixelShader* oldPs = nullptr; ID3D11GeometryShader* oldGs = nullptr;
        ctx->VSGetShader(&oldVs, nullptr, nullptr); ctx->PSGetShader(&oldPs, nullptr, nullptr); ctx->GSGetShader(&oldGs, nullptr, nullptr);
        ID3D11ShaderResourceView* oldSrv = nullptr; ctx->PSGetShaderResources(0, 1, &oldSrv);
        ID3D11SamplerState* oldSmp = nullptr; ctx->PSGetSamplers(0, 1, &oldSmp);
        ID3D11Buffer* oldCb = nullptr; ctx->PSGetConstantBuffers(0, 1, &oldCb);
        ID3D11InputLayout* oldLayout = nullptr; ctx->IAGetInputLayout(&oldLayout);
        D3D11_PRIMITIVE_TOPOLOGY oldTopo {}; ctx->IAGetPrimitiveTopology(&oldTopo);
        ID3D11RasterizerState* oldRs = nullptr; ctx->RSGetState(&oldRs);
        ID3D11DepthStencilState* oldDss = nullptr; UINT oldRef = 0; ctx->OMGetDepthStencilState(&oldDss, &oldRef);
        ID3D11BlendState* oldBs = nullptr; float oldBf[4] {}; UINT oldMask = 0; ctx->OMGetBlendState(&oldBs, oldBf, &oldMask);

        // Box filter: taps per axis = ceil(ratio / 2), each bilinear tap covering 2x2 source texels.
        const float rx = static_cast<float>(sd.Width) / static_cast<float>(dd.Width);
        const float ry = static_cast<float>(sd.Height) / static_cast<float>(dd.Height);
        const float tx = std::max(1.0f, std::ceil(rx / 2.0f)), ty = std::max(1.0f, std::ceil(ry / 2.0f));
        const float c[8] = { 1.0f / sd.Width, 1.0f / sd.Height, tx, ty, (rx / tx) / sd.Width, (ry / ty) / sd.Height, 0, 0 };
        ctx->UpdateSubresource(g_Constants, 0, nullptr, c, 0, 0);

        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp { 0, 0, static_cast<float>(dd.Width), static_cast<float>(dd.Height), 0, 1 };
        ctx->RSSetViewports(1, &vp);
        D3D11_RECT sc { 0, 0, static_cast<LONG>(dd.Width), static_cast<LONG>(dd.Height) };
        ctx->RSSetScissorRects(1, &sc);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_Vs, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(g_Ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->PSSetSamplers(0, 1, &g_Sampler);
        ctx->PSSetConstantBuffers(0, 1, &g_Constants);
        ctx->RSSetState(g_Raster);
        ctx->OMSetDepthStencilState(g_NoDepth, 0);
        ctx->OMSetBlendState(g_NoBlend, nullptr, 0xffffffff);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &nullSrv);

        // Restore.
        ctx->OMSetRenderTargets(8, oldRtv, oldDsv);
        for (auto* v : oldRtv) { if (v) { v->Release(); } }
        if (oldDsv) { oldDsv->Release(); }
        if (nvp) { ctx->RSSetViewports(nvp, oldVp); }
        if (nsc) { ctx->RSSetScissorRects(nsc, oldSc); }
        ctx->VSSetShader(oldVs, nullptr, 0); if (oldVs) { oldVs->Release(); }
        ctx->PSSetShader(oldPs, nullptr, 0); if (oldPs) { oldPs->Release(); }
        ctx->GSSetShader(oldGs, nullptr, 0); if (oldGs) { oldGs->Release(); }
        ctx->PSSetShaderResources(0, 1, &oldSrv); if (oldSrv) { oldSrv->Release(); }
        ctx->PSSetSamplers(0, 1, &oldSmp); if (oldSmp) { oldSmp->Release(); }
        ctx->PSSetConstantBuffers(0, 1, &oldCb); if (oldCb) { oldCb->Release(); }
        ctx->IASetInputLayout(oldLayout); if (oldLayout) { oldLayout->Release(); }
        ctx->IASetPrimitiveTopology(oldTopo);
        ctx->RSSetState(oldRs); if (oldRs) { oldRs->Release(); }
        ctx->OMSetDepthStencilState(oldDss, oldRef); if (oldDss) { oldDss->Release(); }
        ctx->OMSetBlendState(oldBs, oldBf, oldMask); if (oldBs) { oldBs->Release(); }

        if (g_Draws.fetch_add(1) == 0) { spdlog::info("PW post scale: first downscale {}x{} -> {}x{} ({}x{} taps).", sd.Width, sd.Height, dd.Width, dd.Height, static_cast<int>(tx), static_cast<int>(ty)); }
        return true;
    }
}

#endif
