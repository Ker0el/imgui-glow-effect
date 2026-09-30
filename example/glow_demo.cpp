// ============================================================================
//  glow_demo.cpp — 独立演示 / 接入参考实现
// ============================================================================
//
//  一个自带窗口的 Dear ImGui + D3D11 程序，启用了辉光效果。不依赖任何宿主程序，
//  双击就能跑，滑块实时调参。
//
//  这个文件同时是接入的参考实现，重点看三处：
//
//    * CreateDevice()  — DirectComposition + 预乘 alpha 交换链。
//                        就是这一块让辉光走「纯加法」而不是糊出一圈黑边，
//                        移植到别处之前先把那段注释读了。
//    * DrawTitleBar()  — 无边框窗口怎么拖才不抖。
//    * wWinMain()      — 辉光的三次调用，顺序不能错。
//
//  构建：见 build_msvc.bat 或上一层的 CMakeLists.txt。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <stdio.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "ImGuiGlowEffect.hpp"

#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")

#define DEMO_W 1000
#define DEMO_H 660
#define DEMO_TITLE L"ImGui 辉光演示"
#define DEMO_TITLE_U8 "ImGui 辉光演示 — 按住这条标题栏可以拖动窗口"
#define BAR_H 28.0f   // 自绘标题栏高度

static ID3D11Device*           g_dev  = NULL;
static ID3D11DeviceContext*    g_ctx  = NULL;
static IDXGISwapChain1*        g_swap = NULL;
static ID3D11RenderTargetView* g_rtv  = NULL;
static HWND                    g_wnd  = NULL;

static IDXGIFactory2*       g_factory = NULL;
static IDCompositionDevice* g_dcomp   = NULL;
static IDCompositionTarget* g_target  = NULL;
static IDCompositionVisual* g_visual  = NULL;

static ImGuiGlowNS::GlowEffect g_glow;
static ImGuiGlowNS::GlowParams g_p;

static float g_accent[4] = { 0.92f, 0.18f, 0.29f, 1.0f };
static bool  g_quit   = false;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// 配色。暗底 + 高饱和，辉光最好看；效果本身跟主题无关。
// ---------------------------------------------------------------------------
static const char* kThemeNames[] = { "暗色·猩红", "暗色·青蓝", "亮色", "ImGui 默认" };

enum ThemeId { THEME_CRIMSON = 0, THEME_OCEAN, THEME_LIGHT, THEME_IMGUI_DARK };

static int g_theme = THEME_CRIMSON;

static void ApplyTheme(int id) {
    ImGuiStyle& s = ImGui::GetStyle();

    // 形状/间距比颜色更影响"好不好看"，统一设一遍
    s.WindowPadding     = ImVec2(10, 10);
    s.WindowRounding    = 5.0f;
    s.FramePadding      = ImVec2(8, 4);
    s.FrameRounding     = 4.0f;
    s.ItemSpacing       = ImVec2(8, 6);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.IndentSpacing     = 20.0f;
    s.ScrollbarSize     = 13.0f;
    s.ScrollbarRounding = 9.0f;
    s.GrabMinSize       = 12.0f;
    s.GrabRounding      = 4.0f;
    s.WindowTitleAlign  = ImVec2(0.02f, 0.5f);
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupRounding     = 4.0f;
    s.TabRounding       = 4.0f;

    ImVec4* c = s.Colors;
    switch (id) {
    case THEME_OCEAN:
        c[ImGuiCol_Text]                 = ImVec4(0.88f, 0.92f, 0.98f, 1.00f);
        c[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.56f, 0.64f, 1.00f);
        c[ImGuiCol_WindowBg]             = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);
        c[ImGuiCol_ChildBg]              = ImVec4(0.08f, 0.10f, 0.13f, 1.00f);
        c[ImGuiCol_PopupBg]              = ImVec4(0.12f, 0.14f, 0.19f, 0.97f);
        c[ImGuiCol_Border]               = ImVec4(0.24f, 0.30f, 0.40f, 0.80f);
        c[ImGuiCol_FrameBg]              = ImVec4(0.16f, 0.19f, 0.25f, 1.00f);
        c[ImGuiCol_FrameBgHovered]       = ImVec4(0.24f, 0.48f, 0.78f, 0.70f);
        c[ImGuiCol_FrameBgActive]        = ImVec4(0.24f, 0.52f, 0.88f, 0.95f);
        c[ImGuiCol_TitleBg]              = ImVec4(0.12f, 0.14f, 0.19f, 1.00f);
        c[ImGuiCol_TitleBgActive]        = ImVec4(0.20f, 0.42f, 0.72f, 1.00f);
        c[ImGuiCol_MenuBarBg]            = ImVec4(0.14f, 0.17f, 0.22f, 1.00f);
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.28f, 0.33f, 0.42f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.36f, 0.44f, 0.56f, 1.00f);
        c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.44f, 0.56f, 0.74f, 1.00f);
        c[ImGuiCol_CheckMark]            = ImVec4(0.36f, 0.72f, 1.00f, 1.00f);
        c[ImGuiCol_SliderGrab]           = ImVec4(0.36f, 0.60f, 0.88f, 0.90f);
        c[ImGuiCol_SliderGrabActive]     = ImVec4(0.44f, 0.76f, 1.00f, 1.00f);
        c[ImGuiCol_Button]               = ImVec4(0.24f, 0.44f, 0.70f, 0.55f);
        c[ImGuiCol_ButtonHovered]        = ImVec4(0.30f, 0.56f, 0.88f, 0.85f);
        c[ImGuiCol_ButtonActive]         = ImVec4(0.36f, 0.66f, 1.00f, 1.00f);
        c[ImGuiCol_Header]               = ImVec4(0.24f, 0.44f, 0.70f, 0.55f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(0.30f, 0.56f, 0.88f, 0.80f);
        c[ImGuiCol_HeaderActive]         = ImVec4(0.36f, 0.66f, 1.00f, 1.00f);
        c[ImGuiCol_Separator]            = ImVec4(0.24f, 0.30f, 0.40f, 0.80f);
        c[ImGuiCol_SeparatorHovered]     = ImVec4(0.36f, 0.60f, 0.88f, 0.70f);
        c[ImGuiCol_SeparatorActive]      = ImVec4(0.36f, 0.72f, 1.00f, 1.00f);
        c[ImGuiCol_TextSelectedBg]       = ImVec4(0.36f, 0.66f, 1.00f, 0.40f);
        break;

    case THEME_LIGHT:
        ImGui::StyleColorsLight();
        c[ImGuiCol_WindowBg]      = ImVec4(0.95f, 0.95f, 0.96f, 1.00f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        c[ImGuiCol_CheckMark]     = ImVec4(0.22f, 0.52f, 0.90f, 1.00f);
        break;

    case THEME_IMGUI_DARK:
        ImGui::StyleColorsDark();
        c[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);
        break;

    default: // THEME_CRIMSON：暗色 + 猩红
        c[ImGuiCol_Text]                 = ImVec4(0.86f, 0.93f, 0.89f, 1.00f);
        c[ImGuiCol_TextDisabled]         = ImVec4(0.86f, 0.93f, 0.89f, 0.35f);
        c[ImGuiCol_WindowBg]             = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
        c[ImGuiCol_ChildBg]              = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
        c[ImGuiCol_PopupBg]              = ImVec4(0.20f, 0.22f, 0.27f, 0.96f);
        c[ImGuiCol_Border]               = ImVec4(0.31f, 0.31f, 0.42f, 0.55f);
        c[ImGuiCol_FrameBg]              = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
        c[ImGuiCol_FrameBgHovered]       = ImVec4(0.92f, 0.18f, 0.29f, 0.60f);
        c[ImGuiCol_FrameBgActive]        = ImVec4(0.92f, 0.18f, 0.29f, 0.90f);
        c[ImGuiCol_TitleBg]              = ImVec4(0.17f, 0.18f, 0.22f, 1.00f);
        c[ImGuiCol_TitleBgActive]        = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        c[ImGuiCol_MenuBarBg]            = ImVec4(0.20f, 0.22f, 0.27f, 0.60f);
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.15f, 0.16f, 0.20f, 1.00f);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.35f, 0.36f, 0.42f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.92f, 0.18f, 0.29f, 0.70f);
        c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        c[ImGuiCol_CheckMark]            = ImVec4(0.95f, 0.30f, 0.38f, 1.00f);
        c[ImGuiCol_SliderGrab]           = ImVec4(0.62f, 0.64f, 0.70f, 0.90f);
        c[ImGuiCol_SliderGrabActive]     = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        c[ImGuiCol_Button]               = ImVec4(0.47f, 0.77f, 0.83f, 0.16f);
        c[ImGuiCol_ButtonHovered]        = ImVec4(0.92f, 0.18f, 0.29f, 0.75f);
        c[ImGuiCol_ButtonActive]         = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        c[ImGuiCol_Header]               = ImVec4(0.92f, 0.18f, 0.29f, 0.55f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(0.92f, 0.18f, 0.29f, 0.80f);
        c[ImGuiCol_HeaderActive]         = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        c[ImGuiCol_Separator]            = ImVec4(0.30f, 0.31f, 0.37f, 0.80f);
        c[ImGuiCol_SeparatorHovered]     = ImVec4(0.92f, 0.18f, 0.29f, 0.70f);
        c[ImGuiCol_SeparatorActive]      = ImVec4(0.92f, 0.18f, 0.29f, 1.00f);
        c[ImGuiCol_TextSelectedBg]       = ImVec4(0.92f, 0.18f, 0.29f, 0.43f);
        break;
    }
}

// ---------------------------------------------------------------------------
// 字体。按顺序试几个系统中文字体，全都没有就退回 ImGui 内置（中文会变方块）。
// 想换字体：把任意 .ttf 改名成 font.ttf 丢到 exe 同目录即可，不用重新编译。
// ---------------------------------------------------------------------------
static void LoadFont(ImGuiIO& io) {
    char exeDir[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exeDir, MAX_PATH);
    { char* q = strrchr(exeDir, '\\'); if (q) *(q + 1) = 0; else exeDir[0] = 0; }

    static const char* kCandidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",          // 微软雅黑
        "C:\\Windows\\Fonts\\NotoSansSC-VF.ttf", // 思源黑体
        "C:\\Windows\\Fonts\\simhei.ttf",        // 黑体
        "C:\\Windows\\Fonts\\segoeui.ttf",       // 非中文系统的兜底
    };

    ImFontConfig cfg;
    cfg.OversampleH = 1; cfg.OversampleV = 1; cfg.PixelSnapH = true;

    char local[MAX_PATH];
    _snprintf_s(local, MAX_PATH, _TRUNCATE, "%sfont.ttf", exeDir);
    if (GetFileAttributesA(local) != INVALID_FILE_ATTRIBUTES &&
        io.Fonts->AddFontFromFileTTF(local, 17.0f, &cfg, io.Fonts->GetGlyphRangesChineseSimplifiedCommon()))
        return;

    for (int i = 0; i < (int)(sizeof(kCandidates) / sizeof(kCandidates[0])); i++) {
        if (GetFileAttributesA(kCandidates[i]) == INVALID_FILE_ATTRIBUTES) continue;
        if (io.Fonts->AddFontFromFileTTF(kCandidates[i], 17.0f, &cfg,
                                         io.Fonts->GetGlyphRangesChineseSimplifiedCommon()))
            return;
    }
    io.Fonts->AddFontDefault();
}

// ---------------------------------------------------------------------------
// 设备 + 交换链。
//
// **辉光能不能用，就看这一段。** 窗口走 DirectComposition 逐像素 alpha 合成，
// 辉光才能以纯加法叠上去。彩色键窗口为什么不行，见 ImGuiGlowEffect.hpp 顶部。
// ---------------------------------------------------------------------------
static bool CreateDevice() {
    // 1) 裸 D3D11 设备。不能走 D3D11CreateDeviceAndSwapChain —— 交换链必须由
    //    CreateSwapChainForComposition 创建，才能挂进 DWM 的视觉树。
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
            want, 3, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx)))
        return false;

    IDXGIDevice* dxgiDev = NULL;
    if (FAILED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return false;
    IDXGIAdapter* adapter = NULL;
    dxgiDev->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&g_factory);
    adapter->Release();

    // 2) DirectComposition 设备 + 视觉树：把窗口的内容来源从"重定向位图"换成
    //    我们自己的交换链，DWM 就会拿交换链的 alpha 通道做逐像素混合。
    if (FAILED(DCompositionCreateDevice(dxgiDev, __uuidof(IDCompositionDevice), (void**)&g_dcomp))) {
        dxgiDev->Release();
        return false;
    }
    dxgiDev->Release();

    if (FAILED(g_dcomp->CreateTargetForHwnd(g_wnd, TRUE, &g_target))) return false;
    if (FAILED(g_dcomp->CreateVisual(&g_visual))) return false;

    // 3) 合成用交换链。FLIP_SEQUENTIAL + PREMULTIPLIED 都是硬要求：
    //      - 老的 DISCARD 模型会被 CreateSwapChainForComposition 直接拒绝
    //      - alpha 不是 PREMULTIPLIED 的话 DWM 会把 alpha 当 1，整窗不透明
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width       = DEMO_W;
    sd.Height      = DEMO_H;
    sd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(g_factory->CreateSwapChainForComposition(g_dev, &sd, NULL, &g_swap))) return false;

    if (FAILED(g_visual->SetContent(g_swap)))      return false;
    if (FAILED(g_target->SetRoot(g_visual)))       return false;
    if (FAILED(g_dcomp->Commit()))                 return false;

    // 合成交换链的 Present **不受 vsync 节流** —— DWM 按自己的节奏来取帧，
    // Present 会立刻返回。不节流的话主循环空转到几千 FPS、白烧一个多核（实测 108%）。
    // 节流放在主循环末尾用 DwmFlush()。IDXGISwapChain2 的帧延迟等待对象对
    // CreateSwapChainForComposition 出来的交换链**不生效**，别在那上面浪费时间。
    //
    // 后台缓冲的 RTV 特意不在这里建：翻转模型下它每帧都会换一张，见主循环。
    return true;
}

// ---------------------------------------------------------------------------
// 自绘标题栏 + 拖动整个窗口。
//
// 窗口是 WS_EX_NOREDIRECTIONBITMAP 的无边框窗口（这个样式没法配系统标题栏），
// 所以标题栏得自己画一条。
//
// 拖动**不能**用 ImGui 的 io.MouseDelta：窗口一移动，客户区坐标就跟着变，
// delta 会一帧偏大一帧偏小，拖起来会抖甚至往回跳。
// 改用「光标屏幕坐标 − 窗口左上角」的固定偏移 —— 屏幕坐标不受窗口移动影响，
// 每帧把窗口摆回"光标仍落在同一个相对位置"的地方，跟手且不抖。
// ---------------------------------------------------------------------------
static bool  s_dragging = false;
static POINT s_dragOff  = { 0, 0 };

static void DrawTitleBar() {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)DEMO_W, BAR_H), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 3.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.16f, 0.17f, 0.21f, 1.0f));
    ImGui::Begin("##titlebar", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextUnformatted(DEMO_TITLE_U8);
    ImGui::SameLine((float)DEMO_W - 76.0f);
    if (ImGui::Button("退出", ImVec2(56, 21))) g_quit = true;

    // 按在标题条空白处（不是按钮上）才开始拖。
    // 松手才清 s_dragging —— 这样光标拖出标题条甚至拖出窗口都还能继续跟手。
    const bool overItem = ImGui::IsAnyItemHovered();
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!s_dragging && !overItem && ImGui::IsWindowHovered()) {
            POINT p; GetCursorPos(&p);
            RECT  r; GetWindowRect(g_wnd, &r);
            s_dragOff.x = p.x - r.left;
            s_dragOff.y = p.y - r.top;
            s_dragging  = true;
        }
    } else {
        s_dragging = false;
    }
    if (s_dragging) {
        POINT p; GetCursorPos(&p);
        SetWindowPos(g_wnd, NULL, p.x - s_dragOff.x, p.y - s_dragOff.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

// ---------------------------------------------------------------------------
// 演示界面：左边控制面板，右边一块"展示板"，放各种亮度/颜色的元素。
// ---------------------------------------------------------------------------
static void DrawDemoUI() {
    DrawTitleBar();

    // 两个面板都加 NoMove：位置固定，免得被拖到标题栏上挡住拖动区
    ImGui::SetNextWindowPos(ImVec2(16, BAR_H + 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_Always);
    ImGui::Begin("辉光参数", NULL,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Checkbox("开启辉光", &g_p.enabled);
    ImGui::SliderFloat("发光阈值", &g_p.threshold, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("阈值过渡", &g_p.knee,      0.01f, 1.0f, "%.2f");
    ImGui::SliderFloat("光晕半径", &g_p.radius,    1.0f, 48.0f, "%.0f");
    ImGui::SliderFloat("光晕强度", &g_p.intensity, 0.0f, 3.0f, "%.2f");
    ImGui::TextDisabled("半径按 1/4 分辨率算，等效屏幕 %.0f px", g_p.radius * 4.0f);

    if (ImGui::Button("重置")) {
        const ImGuiGlowNS::GlowParams d;
        g_p.threshold = d.threshold; g_p.knee = d.knee;
        g_p.radius = d.radius; g_p.intensity = d.intensity;
    }
    ImGui::SameLine();
    if (ImGui::Button("猛一点"))   { g_p.threshold = 0.20f; g_p.radius = 24.0f; g_p.intensity = 1.8f; }
    ImGui::SameLine();
    if (ImGui::Button("只留高光")) { g_p.threshold = 0.70f; g_p.knee = 0.10f; g_p.radius = 16.0f; g_p.intensity = 2.0f; }
    ImGui::SameLine();
    if (ImGui::Button("全屏泛光")) { g_p.threshold = 0.10f; g_p.knee = 0.40f; g_p.radius = 40.0f; g_p.intensity = 1.4f; }

    ImGui::Separator();
    ImGui::Combo("主题", &g_theme, kThemeNames, 4);
    {
        static int applied = -1;
        if (applied != g_theme) { ApplyTheme(g_theme); applied = g_theme; }
    }
    ImGui::TextDisabled("%.1f FPS", ImGui::GetIO().Framerate);
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(462, BAR_H + 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(522, DEMO_H - BAR_H - 24.0f), ImGuiCond_Always);
    ImGui::Begin("展示板", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    ImGui::TextColored(ImVec4(1, 1, 1, 1), "亮色文字会发光");
    ImGui::TextDisabled("暗色文字达不到阈值，基本不发光");
    ImGui::TextColored(ImVec4(g_accent[0], g_accent[1], g_accent[2], 1), "用强调色写的标题");
    ImGui::Separator();

    ImGui::ColorEdit4("强调色", g_accent, ImGuiColorEditFlags_NoInputs);

    // 一排亮度不同的按钮：把「发光阈值」往上拉，能看出谁先掉出去
    const float lv[] = { 1.0f, 0.8f, 0.6f, 0.4f, 0.25f, 0.12f };
    for (int i = 0; i < 6; i++) {
        char lbl[32]; sprintf_s(lbl, "亮度 %.2f", lv[i]);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(lv[i], lv[i], lv[i], 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(lv[i], lv[i], lv[i], 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(lv[i], lv[i], lv[i], 1.0f));
        float t = (lv[i] > 0.5f) ? 0.1f : 0.9f;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(t, t, t, 1.0f));
        ImGui::Button(lbl, ImVec2(160, 34));
        ImGui::PopStyleColor(4);
        if (i % 3 != 2) ImGui::SameLine();
    }
    ImGui::Separator();

    // 高饱和色块：看辉光会不会互相串色
    const ImVec4 sw[6] = {
        ImVec4(1.00f, 0.20f, 0.20f, 1), ImVec4(0.20f, 1.00f, 0.35f, 1), ImVec4(0.25f, 0.55f, 1.00f, 1),
        ImVec4(1.00f, 0.85f, 0.20f, 1), ImVec4(0.80f, 0.30f, 1.00f, 1), ImVec4(0.20f, 1.00f, 0.95f, 1),
    };
    for (int i = 0; i < 6; i++) {
        ImGui::PushStyleColor(ImGuiCol_Button,        sw[i]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sw[i]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  sw[i]);
        char id[16]; sprintf_s(id, "##sw%d", i);
        ImGui::Button(id, ImVec2(72, 44));
        ImGui::PopStyleColor(3);
        if (i != 5) ImGui::SameLine();
    }

    ImGui::Separator();
    ImGui::TextWrapped("辉光是加法的：越亮的地方溢出的光越强。把「发光阈值」往上拉，"
                       "只有最亮的元素会发光；往下拉，整个界面都会晕开。");
    ImGui::TextDisabled("本窗口走 DirectComposition 逐像素 alpha 合成，");
    ImGui::TextDisabled("所以光晕能糊到桌面上，而不是在周围糊出一圈黑边。");
    ImGui::End();
}

static LRESULT WINAPI WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    if (m == WM_DESTROY || m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { PostQuitMessage(0); return 0; }   // 无边框窗口留个键盘后路
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetProcessDPIAware();

    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"ImGuiGlowDemoWnd";
    RegisterClassExW(&wc);

    // WS_EX_NOREDIRECTIONBITMAP：窗口不去申请重定向位图，内容全部由 DWM 从
    // DirectComposition 视觉树（也就是我们的交换链）取，并按 alpha 混合。
    // 这个样式跟 WS_EX_LAYERED 互斥，两个不能同时用。
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, DEMO_TITLE,
                            WS_POPUP, 120, 80, DEMO_W, DEMO_H, NULL, NULL, hInst, NULL);
    if (!g_wnd) { MessageBoxW(NULL, L"建窗口失败", L"辉光演示", MB_ICONERROR); return 1; }

    if (!CreateDevice()) { MessageBoxW(NULL, L"D3D11 / DirectComposition 初始化失败", L"辉光演示", MB_ICONERROR); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;          // 不落地 imgui.ini
    LoadFont(io);
    ApplyTheme(g_theme);
    ImGui_ImplWin32_Init(g_wnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    if (!g_glow.Initialize(g_dev, g_ctx))
        MessageBoxA(g_wnd, g_glow.LastError(), "辉光初始化失败", MB_ICONERROR);

    ShowWindow(g_wnd, SW_SHOW);
    UpdateWindow(g_wnd);

    // 预乘 alpha 的底：全透明。ImGui 的混合（SRC_ALPHA / INV_SRC_ALPHA 叠到
    // 透明底上）累积出来的正好就是 DWM 要的预乘格式。
    const float clear[4] = { 0, 0, 0, 0 };

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running || g_quit) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawDemoUI();
        ImGui::Render();

        // 翻转模型的"后台缓冲"在 Present 之后会换成另一张，所以每帧重新取。
        // 缓存一个 RTV 反复用的话，从第二帧起就是在往正在显示的那张上画。
        if (g_rtv) { g_rtv->Release(); g_rtv = NULL; }
        ID3D11Texture2D* back = NULL;
        if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) break;
        g_dev->CreateRenderTargetView(back, NULL, &g_rtv);
        back->Release();

        g_ctx->OMSetRenderTargets(1, &g_rtv, NULL);
        g_ctx->ClearRenderTargetView(g_rtv, clear);

        // ---- 辉光就这三下 ----
        bool glowDone = false;
        if (g_p.enabled && g_glow.IsInitialized() &&
            g_glow.EnsureSize(DEMO_W, DEMO_H) && g_glow.BeginUIPass()) {
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_glow.EndUIPass();
            if (g_glow.Process(g_p)) {
                g_glow.Composite(g_rtv, g_p);
                glowDone = true;
            }
        }
        if (!glowDone) {
            // 兜底：没开辉光 / 半径 0 / 初始化失败 —— 直接画，界面不会因为辉光消失
            g_ctx->OMSetRenderTargets(1, &g_rtv, NULL);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        g_swap->Present(1, 0);
        DwmFlush();   // 压到屏幕刷新率，见 CreateDevice() 的注释
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_glow.Release();
    if (g_rtv)     g_rtv->Release();
    if (g_swap)    g_swap->Release();
    if (g_visual)  g_visual->Release();
    if (g_target)  g_target->Release();
    if (g_dcomp)   g_dcomp->Release();
    if (g_factory) g_factory->Release();
    if (g_ctx)     g_ctx->Release();
    if (g_dev)     g_dev->Release();
    return 0;
}
