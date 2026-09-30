cmake_minimum_required(VERSION 3.15)
project(imgui_glow_effect LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "ImGuiGlowEffect 依赖 Direct3D 11，只能在 Windows 上构建。")
endif()

# --- Dear ImGui -------------------------------------------------------------
# 指定了本地 checkout 就用本地的，否则自动拉一份
set(IMGUI_DIR "" CACHE PATH "本机 Dear ImGui 源码目录")
if(NOT IMGUI_DIR)
    include(FetchContent)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.9b)   # 1.91 及以上都行
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
endif()

add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp")
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")

# --- Demo -------------------------------------------------------------------
add_executable(GlowDemo WIN32 example/glow_demo.cpp)
target_include_directories(GlowDemo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(GlowDemo PRIVATE imgui d3d11 dxgi d3dcompiler dcomp dwmapi)
target_compile_definitions(GlowDemo PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)

# --- 效果本体是 header-only 的，单独给个 interface target，方便 target_link_libraries 直接挂
add_library(ImGuiGlowEffect INTERFACE)
target_include_directories(ImGuiGlowEffect INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ImGuiGlowEffect INTERFACE d3d11 d3dcompiler)
