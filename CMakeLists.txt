cmake_minimum_required(VERSION 3.25)
project(FarCryInstinctsRecomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# exact guest state at faults (c.pc per instruction, no __restrict) for debugging, slower
option(FCI_PRECISE "Precise guest state for debugging" OFF)
if(FCI_PRECISE)
    add_compile_definitions(FCI_PRECISE)
endif()

include(FetchContent)

# x86 decoder for the recompiler
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG v4.1.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(zydis)

# SDL3 for controllers (any pad SDL knows: Xbox, PlayStation, Switch, generic), static so it links into the exe;
# settings as in SCDA-XBERecomp (MIT)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.16
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)
# settings menu (F10): Dear ImGui, the version SCDA-XBERecomp uses, on the Direct3D 11 presenter
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)

# shared by the recompiler and the game executable
add_library(xbe STATIC src/xbe/xbe.cpp)
target_include_directories(xbe PUBLIC src)

add_executable(fci-recomp
    tools/recomp/main.cpp
    tools/recomp/analysis.cpp
    tools/recomp/codegen.cpp
    tools/recomp/codegen_insn.cpp
    tools/recomp/xmm_live.cpp
    tools/recomp/semtest.cpp)
target_link_libraries(fci-recomp PRIVATE xbe Zydis)

# the recompiled game code, written by fci-recomp into generated/
set(FCI_PARTITIONS 128)
set(GAME_CODE ${CMAKE_SOURCE_DIR}/generated/table.cpp)
math(EXPR last "${FCI_PARTITIONS} - 1")
foreach(i RANGE ${last})
    string(LENGTH "${i}" len)
    if(len EQUAL 1)
        set(i "00${i}")
    elseif(len EQUAL 2)
        set(i "0${i}")
    endif()
    list(APPEND GAME_CODE ${CMAKE_SOURCE_DIR}/generated/code_${i}.cpp)
endforeach()

add_custom_command(
    OUTPUT ${GAME_CODE} ${CMAKE_SOURCE_DIR}/generated/funcs.h ${CMAKE_SOURCE_DIR}/generated/semtest_cases.cpp
           ${CMAKE_SOURCE_DIR}/generated/image_symbols.h
    COMMAND fci-recomp ${CMAKE_SOURCE_DIR}/assets/default.xbe ${CMAKE_SOURCE_DIR}/generated ${CMAKE_SOURCE_DIR}/config
    DEPENDS fci-recomp ${CMAKE_SOURCE_DIR}/assets/default.xbe ${CMAKE_SOURCE_DIR}/config/symbols.txt
            ${CMAKE_SOURCE_DIR}/config/symbols_fci.txt ${CMAKE_SOURCE_DIR}/config/hle.txt
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Recompiling default.xbe")

add_library(game_code STATIC ${GAME_CODE})
target_include_directories(game_code PRIVATE src generated)
# generated code is warning-free by construction only in spirit: unused labels and temporaries are expected
target_compile_options(game_code PRIVATE /W0)
# LLVM stops following the uses of a pointer after 100 and then assumes it escaped; a recompiled function uses its
# Cpu thousands of times, so without a higher limit every guest memory store may alias the Cpu and the guest registers
# are reloaded from and stored to memory around every access (see FCI_NOESCAPE in src/runtime/guest.h)
target_compile_options(game_code PRIVATE "SHELL:-mllvm -capture-tracking-max-uses-to-explore=1000000")

# profile guided optimization of the recompiled code: FCI_PGO=generate builds it instrumented (run it with
# FCI_PGO_WRITE=<seconds>, then llvm-profdata merge; tools/pgo.ps1 does all of it), FCI_PGO=<file.profdata> optimizes
# with that profile, empty (the default) with config/game_code.profdata if it exists; OFF without one
set(FCI_PGO "" CACHE STRING "generate, the .profdata file the game code is optimized with, empty: config/game_code.profdata, OFF")
set(pgo "${FCI_PGO}")
if(pgo STREQUAL "" AND EXISTS "${CMAKE_SOURCE_DIR}/config/game_code.profdata")
    set(pgo "${CMAKE_SOURCE_DIR}/config/game_code.profdata")
endif()
if(pgo STREQUAL "generate")
    target_compile_options(game_code PRIVATE -fprofile-instr-generate)
    get_filename_component(llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB profile_rt "${llvm_bin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
    set(FCI_PGO_RUNTIME "${profile_rt}")
elseif(pgo)
    message(STATUS "game code optimized with the profile ${pgo}")
    target_compile_options(game_code PRIVATE "-fprofile-instr-use=${pgo}" -Wno-profile-instr-unprofiled
                           -Wno-profile-instr-out-of-date)
    set_source_files_properties(${GAME_CODE} PROPERTIES OBJECT_DEPENDS "${pgo}")
endif()
# AVX2/BMI2/FMA code for the recompiled code (Haswell / Zen and later); no FMA contraction, results stay bit-identical
option(FCI_X86_64_V3 "x86-64-v3 code for the game code" OFF)
if(FCI_X86_64_V3)
    target_compile_options(game_code PRIVATE /clang:-march=x86-64-v3 /clang:-ffp-contract=off)
endif()
# ThinLTO of the recompiled code (lld-link does the code generation)
option(FCI_LTO "ThinLTO for the game code" OFF)
if(FCI_LTO)
    target_compile_options(game_code PRIVATE -flto=thin)
endif()

add_executable(FarCryInstincts
    src/app/main.cpp
    src/audio/apu.cpp
    src/gpu/nv2a.cpp
    src/gpu/pgraph.cpp
    src/gpu/kelvin.cpp
    src/gpu/renderer.cpp
    src/gpu/vsh.cpp
    src/gpu/psh.cpp
    src/gpu/texture.cpp
    src/runtime/log.cpp
    src/runtime/mmio.cpp
    src/runtime/profiler.cpp
    src/runtime/hitch.cpp
    src/runtime/memory.cpp
    src/runtime/runtime.cpp
    src/runtime/scheduler.cpp
    src/runtime/thread.cpp
    src/video/display.cpp
    # XDVDFS disc images for D:, adapted from SCDA-XBERecomp (MIT)
    src/kernel/disc.cpp
    src/kernel/exports.cpp
    src/kernel/k_file.cpp
    src/kernel/k_launch.cpp
    src/kernel/k_memory.cpp
    src/kernel/k_misc.cpp
    src/kernel/k_sync.cpp
    src/kernel/k_thread.cpp
    src/hle/xinput.cpp
    # host controllers through SDL3 (parts adapted from SCDA-XBERecomp, MIT)
    src/hle/host_pad.cpp
    src/xdk/xdk.cpp
    src/xdk/smoke.cpp
    # DirectSound HLE, adapted from SCDA-XBERecomp (MIT)
    src/xdk/dsound.cpp
    src/audio/hle_output.cpp
    # graphics HLE
    src/xdk/d3d8.cpp
    src/gpu/present.cpp
    src/app/settings.cpp
    # PC features: widescreen (Hor+), field of view, frame rate
    src/app/pcmode.cpp
    src/xdk/game.cpp
    src/xdk/shadows.cpp
    src/xdk/lod.cpp
    # PC comfort features: key bindings, mouse look, focus pause (parts adapted from SCDA-XBERecomp, MIT)
    src/hle/pc_input.cpp
    src/hle/skip_intro.cpp
    src/hle/start_level.cpp
    src/app/focus_pause.cpp
    src/runtime/guest_time.cpp
    # texture dump/replacement and fps counter (parts adapted from SCDA-XBERecomp, MIT)
    src/gpu/texture_pack.cpp
    src/gpu/fps_overlay.cpp
    # fmv_scale, colour_blind (parts adapted from SCDA-XBERecomp, MIT)
    src/gpu/fmv.cpp
    # ambient occlusion (SSAO/GTAO), adapted from SCDA-XBERecomp (MIT)
    src/gpu/ao.cpp
    src/gpu/water.cpp
    src/gpu/fc1_data.cpp
    # display output options: display mode, monitor, resolution, HDR10 (parts adapted from SCDA-XBERecomp, MIT)
    src/app/output.cpp
    # settings menu (F10 / Back+Start), adapted from SCDA-XBERecomp (MIT)
    src/app/menu.cpp
    ${IMGUI_SOURCES})
target_include_directories(FarCryInstincts PRIVATE src generated)
# SDL's headers go to the controller code alone (linked LINK_ONLY)
set_source_files_properties(src/hle/host_pad.cpp PROPERTIES INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include")
target_include_directories(FarCryInstincts SYSTEM PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# the menu feeds the controller to ImGui itself (every XInput pad, Back+Start)
target_compile_definitions(FarCryInstincts PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_link_libraries(FarCryInstincts PRIVATE game_code xbe dbghelp winmm d3d11 dxgi d3dcompiler xinput xaudio2 ole32
                      version)
# Microsoft's HLSL compiler (Windows SDK redistributable) next to the exe: under Wine/Proton (Steam Deck) the builtin
# one lacks parts of HLSL; Windows loads this copy too (application directory first)
find_file(FCI_D3DCOMPILER_DLL d3dcompiler_47.dll
    PATHS "$ENV{WindowsSdkDir}Redist/D3D/x64" "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64" NO_DEFAULT_PATH)
if(FCI_D3DCOMPILER_DLL)
    add_custom_command(TARGET FarCryInstincts POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FCI_D3DCOMPILER_DLL} $<TARGET_FILE_DIR:FarCryInstincts>)
else()
    message(WARNING "d3dcompiler_47.dll (Windows SDK Redist/D3D/x64) not found: not copied next to the exe")
endif()
target_link_libraries(FarCryInstincts PRIVATE windowscodecs)  # texture dump/replacement: PNG through WIC
target_link_libraries(FarCryInstincts PRIVATE $<LINK_ONLY:SDL3::SDL3-static>)  # controllers (src/hle/host_pad.cpp)
# symbols make crash stacks readable: recompiled functions are named f_<guest address>
# no high entropy ASLR: thread stacks that DLLs create at load time (before our first static initializer)
# could otherwise land anywhere in 64 bit space, sometimes in the guest window at 0x2000000000
target_link_options(FarCryInstincts PRIVATE /DEBUG /HIGHENTROPYVA:NO)
# line tables for the host code (no runtime cost): the profiler (FCI_PROFILE) names functions in anonymous
# namespaces and the source lines of inlined code
target_compile_options(FarCryInstincts PRIVATE -gline-tables-only)
# a GUI program with the usual main(): no console window (none under Wine/Proton either); the log attaches to the
# console it was started from, FCI_CONSOLE=1 opens one (src/runtime/log.cpp)
target_link_options(FarCryInstincts PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
if(FCI_LTO)
    target_link_options(FarCryInstincts PRIVATE "/lldltocache:${CMAKE_BINARY_DIR}/lto.cache")
endif()
if(pgo STREQUAL "generate")
    target_compile_definitions(FarCryInstincts PRIVATE FCI_PGO_GENERATE)
    target_link_libraries(FarCryInstincts PRIVATE "${FCI_PGO_RUNTIME}")
endif()

# compares the generated instruction semantics against the host CPU (run: fci-semtest [iterations])
add_executable(fci-semtest tools/semtest/main.cpp ${CMAKE_SOURCE_DIR}/generated/semtest_cases.cpp)
target_include_directories(fci-semtest PRIVATE src tools)
target_link_libraries(fci-semtest PRIVATE Zydis)
