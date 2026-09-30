#pragma once
// ImGuiGlowEffect.hpp — Dear ImGui 的 DX11 真·泛光（bloom）后处理
//
// 标准 bloom 三步：
//   1. 把这一帧的 ImGui 画进一张**离屏 RT**（透明黑底），而不是直接画到后台缓冲
//   2. 亮部提取 + 降采样到 1/4 -> 两趟分离高斯 -> glowSRV
//   3. 合成回后台缓冲：ui + glow * intensity
//
// 为什么降到 1/4：高斯模糊的代价正比于像素数。1/4 分辨率下半径 12 的模糊，等效
// 于全分辨率半径 48，成本只有 1/16。上采样时双线性插值顺带把光晕抹得更顺 ——
// 辉光本来就是糊的，完全不需要全分辨率。
//
// ---------------------------------------------------------------------------
// ⚠ 合成的 alpha 规则 —— 改这个文件前先读这段
//
// 本效果假定宿主窗口用「逐像素 alpha」合成（DirectComposition + 预乘 alpha 交换链，
// 见 glow_demo.cpp / ext/main.cpp 的窗口创建）。合成着色器输出的 alpha **只取自 UI**，
// 辉光那部分 alpha 恒为 0。这样 DWM 的合成公式
//
//     out = src.rgb + dst.rgb * (1 - src.a)
//
// 在辉光像素上退化成 src.a=0，也就是 out = 游戏画面 + 辉光 —— **纯加法**。
// 光晕可以平滑地糊到游戏/桌面上，外圈淡出时不会遮住背景。
//
// 反过来，如果窗口用的是彩色键透明（SetLayeredWindowAttributes + LWA_COLORKEY），
// 那只有纯黑透明、其余像素一律**替换**目标。加法辉光衰减到 5/255 的那一大圈就会
// 变成不透明深灰，把背后整片糊脏 —— 这种窗口下本效果**不能**直接用。
// ---------------------------------------------------------------------------
// 用法（必须在 ImGui::Render() 之后、Present() 之前）：
//
//   g_glow.EnsureSize(w, h);
//   if (g_glow.BeginUIPass()) {                       // 绑离屏 RT，清成透明黑
//       ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
//       g_glow.EndUIPass();                           // 解绑，让纹理能被当 SRV 读
//       if (g_glow.Process(g_glowParams))             // 亮部 -> 降采样 -> 模糊
//           g_glow.Composite(g_rtv, g_glowParams);    // 合成到后台缓冲
//   }
//
// 注意 ImGui_ImplDX11_RenderDrawData 会把它进来时的 RTV 还原回去，所以画完
// 之后仍然绑着 uiRTV —— 必须先 EndUIPass() 解绑，否则下一趟要读它当 SRV。
// ---------------------------------------------------------------------------

#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "imgui.h"   // 这份实现只用到 ImDrawList/ImDrawFlags/ImGui::GetIO，都在 imgui.h 里

#pragma comment(lib, "d3dcompiler.lib")

namespace ImGuiGlowNS {

// 辉光参数。全部暴露成运行时滑块，方便现场调。
struct GlowParams {
    bool  enabled   = true;
    float threshold = 0.30f;   // 亮部阈值：低于它的像素不发光（直通色，0~1）
    float knee      = 0.20f;   // 阈值软过渡宽度，避免出现生硬的亮度断层
    float radius    = 12.0f;   // 1/4 分辨率下的高斯半径，等效屏幕半径 = 4x
    float intensity = 1.10f;   // 辉光强度
};

// ============================================================================
// 着色器
// ============================================================================

// 全屏三角形：用 SV_VertexID 现算，不需要顶点缓冲也不需要输入布局。
// id=0 -> (0,0)  id=1 -> (2,0)  id=2 -> (0,2)，三个点撑满整个 NDC。
static const char* kFullscreenVS = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv  = p;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    return o;
}
)";

// 所有 pass 共用一块常量缓冲。命名成员（不用 float4 打包）是为了可读性，
// HLSL 的 cbuffer 会把它们按 4 个 float 一个寄存器自动排好。
static const char* kCommonCB = R"(
cbuffer Params : register(b0) {
    float g_Threshold;
    float g_Knee;
    float g_Radius;
    float g_Intensity;
    float g_TexelX;
    float g_TexelY;
    float g_Pad0;
    float g_Pad1;
};

Texture2D    g_Tex  : register(t0);
SamplerState g_Samp : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
)";

// 亮部提取 + 4x 降采样。
// 单点抽样会让 1px 的细边框在 1/4 分辨率下闪烁（走样），所以取 4 个点做盒式平均：
// 采样点分开半个目标纹素，在源图上正好覆盖 4x4 个像素。
static const char* kBrightPassPS = R"(
float4 main(VSOut i) : SV_Target {
    float2 o = float2(g_TexelX, g_TexelY) * 0.5;

    float3 c = g_Tex.Sample(g_Samp, i.uv + float2(-o.x, -o.y)).rgb
             + g_Tex.Sample(g_Samp, i.uv + float2( o.x, -o.y)).rgb
             + g_Tex.Sample(g_Samp, i.uv + float2(-o.x,  o.y)).rgb
             + g_Tex.Sample(g_Samp, i.uv + float2( o.x,  o.y)).rgb;
    c *= 0.25;

    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    float w = saturate((lum - g_Threshold) / max(g_Knee, 1e-4));
    w = w * w * (3.0 - 2.0 * w);   // smoothstep，过渡比线性干净

    return float4(c * w, 1.0);
}
)";

// 分离高斯。方向靠 g_TexelX/g_TexelY 传进来（横向 (1/w,0)，纵向 (0,1/h)），
// 一个着色器跑两趟，跟渲染无关。
static const char* kBlurPS = R"(
float4 main(VSOut i) : SV_Target {
    float  r   = clamp(g_Radius, 0.0, 64.0);
    float2 dir = float2(g_TexelX, g_TexelY);   // 已带「走 1 个纹素」的长度

    if (r < 0.5) return float4(g_Tex.Sample(g_Samp, i.uv).rgb, 1.0);

    float sigma = r / 3.0;
    float3 sum  = 0;
    float  wsum = 0;

    [loop]
    for (float x = -r; x <= r; x += 1.0) {
        float w = exp(-(x * x) / (2.0 * sigma * sigma));
        // dir 已经带了「1 像素」的长度，乘 x 就是走 x 个纹素
        sum  += g_Tex.Sample(g_Samp, i.uv + dir * x).rgb * w;
        wsum += w;
    }
    return float4(sum / wsum, 1.0);
}
)";

// 合成。
// 离屏 UI RT 里存的**已经是预乘 alpha**的颜色（见下面 BeginUIPass 的注释），
// 所以 rgb 直接相加就是最终颜色。
//
// alpha 只取自 UI，辉光那部分的 alpha 恒为 0 —— 这是整个效果的命门。
// DWM 合成逐像素 alpha 的公式是 out = src.rgb + dst.rgb*(1-src.a)，
// 令辉光像素的 src.a=0，它就退化成 out = 游戏画面 + 辉光，纯加法。
// 光晕外圈淡出时既不遮住游戏、也不会留下黑边；空白处 rgb 和 a 都是 0，完全透明。
static const char* kCompositePS = R"(
Texture2D g_UI : register(t1);

float4 main(VSOut i) : SV_Target {
    float4 ui = g_Tex.Sample(g_Samp, i.uv);
    float3 gl = g_UI.Sample(g_Samp, i.uv).rgb * g_Intensity;

    return float4(ui.rgb + gl, ui.a);
}
)";

// 和 HLSL 里那个 cbuffer 逐字段对应，sizeof 必须是 32。
struct GlowCB {
    float threshold;
    float knee;
    float radius;
    float intensity;
    float texelX;
    float texelY;
    float pad0;
    float pad1;
};

// ============================================================================
// 状态保存/还原
// ============================================================================
//
// 这个类跑在别人（ImGui 后端）的每一帧中间，会把管线状态改得面目全非。
// 虽然本项目是外部覆盖层、自己一个 D3D11 设备，但 ImGui 后端在同一帧里
// 前后都会设状态，乱改容易在后面某个 pass 上突然翻车，所以还是还原干净。
struct StateGuard {
    ID3D11DeviceContext* ctx = nullptr;

    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    UINT vpCount = 0;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT scCount = 0;
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11PixelShader*  ps = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11Buffer*       psCB[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    ID3D11ShaderResourceView* psSRV[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11SamplerState* samp = nullptr;
    ID3D11BlendState*   blend = nullptr;
    FLOAT blendFactor[4] = { 1, 1, 1, 1 };
    UINT  sampleMask = 0xffffffff;
    ID3D11DepthStencilState* dss = nullptr;
    UINT  stencilRef = 0;
    ID3D11RasterizerState* rs = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

    explicit StateGuard(ID3D11DeviceContext* c) : ctx(c) {
        if (!ctx) return;
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&vpCount, vp);
        scCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetScissorRects(&scCount, sc);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, psCB);
        ctx->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, psSRV);
        ctx->PSGetSamplers(0, 1, &samp);
        ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
        ctx->OMGetDepthStencilState(&dss, &stencilRef);
        ctx->RSGetState(&rs);
        ctx->IAGetPrimitiveTopology(&topo);
    }

    ~StateGuard() { Restore(); }

    void Restore() {
        if (!ctx) return;

        // 先把所有 SRV 解绑再还原，免得还原时纹理同时挂在 RTV 和 SRV 上
        ID3D11ShaderResourceView* nullSRV[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        ctx->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nullSRV);
        ID3D11RenderTargetView* nullRTV[1] = { nullptr };
        ctx->OMSetRenderTargets(1, nullRTV, nullptr);

        ctx->PSSetShader(ps, nullptr, 0);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, psCB);
        ctx->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, psSRV);
        ctx->PSSetSamplers(0, 1, &samp);
        ctx->OMSetBlendState(blend, blendFactor, sampleMask);
        ctx->OMSetDepthStencilState(dss, stencilRef);
        ctx->RSSetState(rs);
        ctx->IASetPrimitiveTopology(topo);
        if (vpCount) ctx->RSSetViewports(vpCount, vp);
        if (scCount) ctx->RSSetScissorRects(scCount, sc);
        ctx->OMSetRenderTargets(1, &rtv, dsv);

        // 释放这一轮 Query 出来的引用计数
        if (rtv) rtv->Release();
        if (dsv) dsv->Release();
        if (ps) ps->Release();
        if (vs) vs->Release();
        if (samp) samp->Release();
        if (blend) blend->Release();
        if (dss) dss->Release();
        if (rs) rs->Release();
        for (int i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; i++)
            if (psCB[i]) psCB[i]->Release();
        for (int i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; i++)
            if (psSRV[i]) psSRV[i]->Release();

        ctx = nullptr;   // 防重复还原
    }
};

// ============================================================================
// GlowEffect
// ============================================================================

class GlowEffect {
public:
    GlowEffect() = default;
    ~GlowEffect() { Release(); }

    bool Initialize(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
        Release();
        if (!dev || !ctx) { SetErr("device/context 为空"); return false; }
        device  = dev;
        context = ctx;

        if (!CompileShaders()) return false;
        if (!CreateStates())   return false;

        initialized = true;
        return true;
    }

    bool IsInitialized() const { return initialized; }
    const char* LastError() const { return err; }

    // ---- 1) UI 画进离屏 RT -------------------------------------------------
    //
    // 离屏 RT 清成 (0,0,0,0)。ImGui 的混合是 SrcBlend=SRC_ALPHA /
    // DestBlend=INV_SRC_ALPHA（PS 输出直通 alpha，不预乘），所以从全透明底
    // 一层层叠上去之后，RT 里的 RGB 天然就是「UI 叠在黑底上的颜色」，
    // 也就是相对于 alpha 的预乘值。合成时直接 ui+glow 即可。
    bool BeginUIPass() {
        if (!initialized || !uiRTV) return false;

        // 上一帧 Composite 采样过 bloomSRVA，但 uiSRV 可能还挂在 t0 上。
        // 现在就把它解掉，免得下面 ImGui 画进来时纹理同时是 SRV 和渲染目标。
        ID3D11ShaderResourceView* nullSRV[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        context->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nullSRV);

        ID3D11RenderTargetView* rt[1] = { uiRTV };
        context->OMSetRenderTargets(1, rt, nullptr);

        const float clear[4] = { 0, 0, 0, 0 };
        context->ClearRenderTargetView(uiRTV, clear);
        return true;
    }

    void EndUIPass() {
        if (!initialized) return;
        ID3D11RenderTargetView* nullRTV[1] = { nullptr };
        context->OMSetRenderTargets(1, nullRTV, nullptr);
        // 上一帧的 SRV 绑定可能还挂着 uiSRV，这里一起清掉
        ID3D11ShaderResourceView* nullSRV[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        context->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nullSRV);
    }

    // 尺寸变了就重建。返回 false 表示当前没有可用的纹理。
    bool EnsureSize(int w, int h) {
        if (!initialized || w <= 0 || h <= 0) return false;
        if (w == width && h == height && uiTex) return true;
        ReleaseSurfaces();
        width = w; height = h;
        bw = w / kDownsample; if (bw < 1) bw = 1;
        bh = h / kDownsample; if (bh < 1) bh = 1;

        if (!CreateSurface(w, h, &uiTex, &uiSRV, &uiRTV)) { SetErr("离屏 UI RT 创建失败"); return false; }
        if (!CreateSurface(bw, bh, &bloomTexA, &bloomSRVA, &bloomRTVA)) { SetErr("辉光 RT-A 创建失败"); return false; }
        if (!CreateSurface(bw, bh, &bloomTexB, &bloomSRVB, &bloomRTVB)) { SetErr("辉光 RT-B 创建失败"); return false; }
        return true;
    }

    // ---- 2) 亮部提取 -> 降采样 -> 两趟分离高斯 -> bloomSRVA ----------------
    bool Process(const GlowParams& p) {
        if (!initialized || !uiSRV || !bloomSRVA) return false;
        if (p.radius < 0.5f) return false;   // 半径 0 就当没开，省两趟

        StateGuard g(context);
        SetupCommon();

        const float cx = 1.0f / (float)bw;
        const float cy = 1.0f / (float)bh;

        // 亮部提取 + 4x 降采样：uiSRV -> bloomA
        {
            GlowCB cb = { p.threshold, p.knee, 0.0f, 0.0f, cx, cy, 0.0f, 0.0f };
            SetPass(bloomRTVA, bw, bh, psBrightPass);
            UploadCB(cb);
            ID3D11ShaderResourceView* srv[1] = { uiSRV };
            context->PSSetShaderResources(0, 1, srv);
            context->Draw(3, 0);
            UnbindSRV(0);
        }

        // 横向模糊：bloomA -> bloomB
        {
            GlowCB cb = { p.threshold, p.knee, p.radius, 0.0f, cx, 0.0f, 0.0f, 0.0f };
            SetPass(bloomRTVB, bw, bh, psBlur);
            UploadCB(cb);
            ID3D11ShaderResourceView* srv[1] = { bloomSRVA };
            context->PSSetShaderResources(0, 1, srv);
            context->Draw(3, 0);
            UnbindSRV(0);
        }

        // 纵向模糊：bloomB -> bloomA（最终辉光图就落在 bloomA）
        {
            GlowCB cb = { p.threshold, p.knee, p.radius, 0.0f, 0.0f, cy, 0.0f, 0.0f };
            SetPass(bloomRTVA, bw, bh, psBlur);
            UploadCB(cb);
            ID3D11ShaderResourceView* srv[1] = { bloomSRVB };
            context->PSSetShaderResources(0, 1, srv);
            context->Draw(3, 0);
            UnbindSRV(0);
        }

        return true;
    }

    // ---- 3) 合成到目标 RTV（ui + glow + 黑截断） ---------------------------
    void Composite(ID3D11RenderTargetView* dstRTV, const GlowParams& p) {
        if (!initialized || !dstRTV || !uiSRV || !bloomSRVA) return;

        StateGuard g(context);
        SetupCommon();

        GlowCB cb = { 0, 0, 0, p.intensity, 0, 0, 0, 0 };
        SetPass(dstRTV, width, height, psComposite);
        UploadCB(cb);

        ID3D11ShaderResourceView* srv[2] = { uiSRV, bloomSRVA };
        context->PSSetShaderResources(0, 2, srv);
        context->Draw(3, 0);
        UnbindSRV(2);
    }

    void Release() {
        ReleaseSurfaces();
        if (psBrightPass) { psBrightPass->Release(); psBrightPass = nullptr; }
        if (psBlur)       { psBlur->Release();       psBlur = nullptr; }
        if (psComposite)  { psComposite->Release();  psComposite = nullptr; }
        if (vsFullscreen) { vsFullscreen->Release(); vsFullscreen = nullptr; }
        if (sampler)      { sampler->Release();      sampler = nullptr; }
        if (cbuffer)      { cbuffer->Release();      cbuffer = nullptr; }
        initialized = false;
        width = height = bw = bh = 0;
    }

private:
    static const int kDownsample = 4;

    // 所有 pass 共用的管线设置：无混合、无深度、关剔除、全屏三角形。
    void SetupCommon() {
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vsFullscreen, nullptr, 0);
        context->PSSetSamplers(0, 1, &sampler);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        context->RSSetState(nullptr);
    }

    void SetPass(ID3D11RenderTargetView* rt, int w, int h, ID3D11PixelShader* ps) {
        ID3D11RenderTargetView* rtv[1] = { rt };
        context->OMSetRenderTargets(1, rtv, nullptr);
        D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
        context->RSSetViewports(1, &vp);
        context->PSSetShader(ps, nullptr, 0);
    }

    void UploadCB(const GlowCB& cb) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(context->Map(cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &cb, sizeof(cb));
            context->Unmap(cbuffer, 0);
        }
        context->PSSetConstantBuffers(0, 1, &cbuffer);
    }

    void UnbindSRV(UINT n) {
        ID3D11ShaderResourceView* nullSRV[2] = { nullptr, nullptr };
        if (n > 2) n = 2;
        context->PSSetShaderResources(0, n, nullSRV);
    }

    bool CompileShaders() {
        if (!Compile(kFullscreenVS, "vs_5_0", &vsFullscreen)) return false;

        // 三个像素着色器共用同一份 cbuffer/采样器/VSOut 声明，在这里拼一次。
        // 拼出来的 std::string 是个局部量，但 CompileBlob 是同步的，
        // D3DCompile 在它析构前就已经读完了整份源码。
        std::string psSrc = kCommonCB;
        const size_t base = psSrc.size();

        psSrc += kBrightPassPS;
        if (!Compile(psSrc.c_str(), "ps_5_0", &psBrightPass)) return false;

        psSrc.resize(base);
        psSrc += kBlurPS;
        if (!Compile(psSrc.c_str(), "ps_5_0", &psBlur)) return false;

        psSrc.resize(base);
        psSrc += kCompositePS;
        if (!Compile(psSrc.c_str(), "ps_5_0", &psComposite)) return false;

        return true;
    }

    bool Compile(const char* src, const char* target, ID3D11VertexShader** out) {
        ID3DBlob* blob = nullptr;
        if (!CompileBlob(src, target, &blob)) return false;
        HRESULT hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
        blob->Release();
        if (FAILED(hr)) { SetErr("CreateVertexShader 失败"); return false; }
        return true;
    }

    bool Compile(const char* src, const char* target, ID3D11PixelShader** out) {
        ID3DBlob* blob = nullptr;
        if (!CompileBlob(src, target, &blob)) return false;
        HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
        blob->Release();
        if (FAILED(hr)) { SetErr("CreatePixelShader 失败"); return false; }
        return true;
    }

    bool CompileBlob(const char* src, const char* target, ID3DBlob** out) {
        ID3DBlob* errBlob = nullptr;
        HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                                "main", target, D3DCOMPILE_ENABLE_STRICTNESS, 0, out, &errBlob);
        if (FAILED(hr)) {
            // 编译错误只在这里拿得到，日志里要留全，不然只剩一句"初始化失败"
            if (errBlob) {
                _snprintf_s(err, sizeof(err), _TRUNCATE, "着色器编译失败(%s): %.200s",
                            target, (const char*)errBlob->GetBufferPointer());
                errBlob->Release();
            } else {
                SetErr("着色器编译失败");
            }
            return false;
        }
        if (errBlob) errBlob->Release();
        return true;
    }

    bool CreateStates() {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MinLOD = 0;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, &sampler))) { SetErr("采样器创建失败"); return false; }

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth      = sizeof(GlowCB);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &cbuffer))) { SetErr("常量缓冲创建失败"); return false; }
        return true;
    }

    bool CreateSurface(int w, int h, ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv, ID3D11RenderTargetView** rtv) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, tex))) return false;
        if (FAILED(device->CreateShaderResourceView(*tex, nullptr, srv))) return false;
        if (FAILED(device->CreateRenderTargetView(*tex, nullptr, rtv))) return false;
        return true;
    }

    void ReleaseSurfaces() {
        if (context) {
            ID3D11RenderTargetView* nullRTV[1] = { nullptr };
            context->OMSetRenderTargets(1, nullRTV, nullptr);
            ID3D11ShaderResourceView* nullSRV[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
            context->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nullSRV);
        }
        #define REL(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)
        REL(uiTex);      REL(uiSRV);      REL(uiRTV);
        REL(bloomTexA);  REL(bloomSRVA);  REL(bloomRTVA);
        REL(bloomTexB);  REL(bloomSRVB);  REL(bloomRTVB);
        #undef REL
    }

    void SetErr(const char* s) { _snprintf_s(err, sizeof(err), _TRUNCATE, "%s", s); }

    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* context = nullptr;

    ID3D11VertexShader*  vsFullscreen  = nullptr;
    ID3D11PixelShader*   psBrightPass  = nullptr;
    ID3D11PixelShader*   psBlur        = nullptr;
    ID3D11PixelShader*   psComposite   = nullptr;
    ID3D11SamplerState*  sampler       = nullptr;
    ID3D11Buffer*        cbuffer       = nullptr;

    ID3D11Texture2D*          uiTex = nullptr;
    ID3D11ShaderResourceView* uiSRV = nullptr;
    ID3D11RenderTargetView*   uiRTV = nullptr;

    ID3D11Texture2D*          bloomTexA = nullptr;
    ID3D11ShaderResourceView* bloomSRVA = nullptr;
    ID3D11RenderTargetView*   bloomRTVA = nullptr;

    ID3D11Texture2D*          bloomTexB = nullptr;
    ID3D11ShaderResourceView* bloomSRVB = nullptr;
    ID3D11RenderTargetView*   bloomRTVB = nullptr;

    int  width = 0, height = 0;   // 全分辨率（离屏 UI RT）
    int  bw = 0, bh = 0;          // 1/4 分辨率（辉光链）
    bool initialized = false;
    char err[256] = { 0 };
};

} // namespace ImGuiGlowNS
