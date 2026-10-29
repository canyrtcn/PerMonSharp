cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(PerMonSharp VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT (/MT) so PerMonSharp.exe runs portably on clean Windows installations
# without requiring the Visual C++ Redistributable package.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    add_compile_options(/W4 /MP /utf-8 /EHsc /permissive-)
    add_compile_options("$<$<CONFIG:Release>:/O2;/GL;/Gw;/Gy>")
    add_link_options("$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>")
endif()

# Locate fxc.exe from the Windows SDK to precompile HLSL into zero-overhead C++ bytecode headers
find_program(FXC_EXECUTABLE fxc REQUIRED)

set(GEN_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_SHADER_DIR}")

set(SHADER_VS_HDR "${GEN_SHADER_DIR}/luma_cas_vs.h")
set(SHADER_PS_HDR "${GEN_SHADER_DIR}/luma_cas_ps.h")
set(SHADER_CS_HDR "${GEN_SHADER_DIR}/diff_check_cs.h")

add_custom_command(
    OUTPUT "${SHADER_VS_HDR}"
    COMMAND "${FXC_EXECUTABLE}" /nologo /T vs_5_0 /E VSMain /O3 /Qstrip_reflect /Qstrip_debug
            /Vn g_LumaCasVS /Fh "${SHADER_VS_HDR}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/luma_cas.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/luma_cas.hlsl"
    COMMENT "Compiling Luma-CAS Vertex Shader (vs_5_0)"
    VERBATIM
)

add_custom_command(
    OUTPUT "${SHADER_PS_HDR}"
    COMMAND "${FXC_EXECUTABLE}" /nologo /T ps_5_0 /E PSMain /O3 /Qstrip_reflect /Qstrip_debug
            /Vn g_LumaCasPS /Fh "${SHADER_PS_HDR}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/luma_cas.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/luma_cas.hlsl"
    COMMENT "Compiling Luma-CAS Pixel Shader (ps_5_0)"
    VERBATIM
)

add_custom_command(
    OUTPUT "${SHADER_CS_HDR}"
    COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_0 /E CSMain /O3 /Qstrip_reflect /Qstrip_debug
            /Vn g_DiffCheckCS /Fh "${SHADER_CS_HDR}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/diff_check.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/diff_check.hlsl"
    COMMENT "Compiling DiffCheck Compute Shader (cs_5_0)"
    VERBATIM
)

add_custom_target(CompiledShaders DEPENDS "${SHADER_VS_HDR}" "${SHADER_PS_HDR}" "${SHADER_CS_HDR}")

add_library(PerMonSharpCore STATIC
    src/config/Config.cpp
    src/display/DisplayManager.cpp
    src/display/FullscreenDetector.cpp
    src/engine/GpuSharpenProcessor.cpp
    src/engine/MonitorWorker.cpp
    src/engine/EngineController.cpp
)

add_dependencies(PerMonSharpCore CompiledShaders)

target_include_directories(PerMonSharpCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "${GEN_SHADER_DIR}"
)

target_link_libraries(PerMonSharpCore PUBLIC
    user32
    gdi32
    shell32
    advapi32
    ole32
    runtimeobject
    dwmapi
    shcore
    d3d11
    dxgi
    dcomp
    d2d1
    dwrite
)

set_source_files_properties(resources/PerMonSharp.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/PerMonSharp.ico;${CMAKE_CURRENT_SOURCE_DIR}/resources/PerMonSharp.manifest"
)

add_executable(PerMonSharp WIN32
    src/main.cpp
    src/ui/IdentifyOverlay.cpp
    src/ui/TrayIcon.cpp
    src/ui/MainWindow.cpp
    resources/PerMonSharp.rc
)

target_link_libraries(PerMonSharp PRIVATE PerMonSharpCore comctl32)
set_target_properties(PerMonSharp PROPERTIES
    OUTPUT_NAME "PerMonSharp"
    LINK_FLAGS "/MANIFEST:NO"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/TestMain.cpp")
    enable_testing()
    add_executable(PerMonSharpTests
        tests/TestMain.cpp
    )
    target_link_libraries(PerMonSharpTests PRIVATE PerMonSharpCore)
    add_test(NAME PerMonSharpUnitAndGpuTests COMMAND PerMonSharpTests)
endif()
