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

# What a release ships beside the executable to build the game code on the player's PC (src/app/game_code.cpp):
# the recompiler, the executable's import library, the headers and the configuration. tools/dist.ps1 adds clang and
# lld; the developer build puts the rest here too, so its layout is the release's.
set(COMPILER_DIR ${CMAKE_BINARY_DIR}/compiler)

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

# compares the generated instruction semantics against the host CPU (run: fci-semtest [iterations]); the cases are
# fixed instructions, which fci-recomp writes without an XBE
set(SEMTEST_CASES ${CMAKE_BINARY_DIR}/semtest/semtest_cases.cpp)
add_custom_command(
    OUTPUT ${SEMTEST_CASES}
    COMMAND fci-recomp --semtest ${SEMTEST_CASES}
    DEPENDS fci-recomp
    COMMENT "Writing the semantics test cases")
add_executable(fci-semtest tools/semtest/main.cpp ${SEMTEST_CASES})
target_include_directories(fci-semtest PRIVATE src tools)
target_compile_definitions(fci-semtest PRIVATE FCI_GAME_CODE)
target_link_libraries(fci-semtest PRIVATE Zydis)

# only the recompiler and the semantics test (CI without game files, no SDL/ImGui fetched)
option(FCI_TOOLS_ONLY "Build fci-recomp and fci-semtest only, without default.xbe" OFF)
if(FCI_TOOLS_ONLY)
    return()
endif()

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

# ---- the game code DLL's interface (src/runtime/guest.h) ----

# How the recompiled code is compiled, here and on the first start (src/app/game_code.cpp gets the same list through
# fci_abi.h): without the C and C++ headers of MSVC and the Windows SDK (-nostdinc, src/runtime/freestanding), no
# stack cookies (no C runtime to check them) and no default libraries in the objects. LLVM stops following the uses of
# a pointer after 100 and then assumes it escaped; a recompiled function uses its Cpu thousands of times, so without a
# higher limit every guest memory store may alias the Cpu and the guest registers are reloaded from and stored to
# memory around every access (see FCI_NOESCAPE in src/runtime/guest.h).
set(FCI_GAME_FLAGS /GS- /Zl /W0 /clang:-nostdinc -DFCI_GAME_CODE -DFCI_GAME_DLL)
set(FCI_GAME_MLLVM "-mllvm -capture-tracking-max-uses-to-explore=1000000")
if(FCI_PRECISE)
    list(APPEND FCI_GAME_FLAGS -DFCI_PRECISE)
endif()
# AVX2/BMI2/FMA code for the recompiled code (Haswell / Zen and later); no FMA contraction, results stay bit-identical
option(FCI_X86_64_V3 "x86-64-v3 code for the game code" OFF)
if(FCI_X86_64_V3)
    list(APPEND FCI_GAME_FLAGS /clang:-march=x86-64-v3 /clang:-ffp-contract=off)
endif()
# the release flags CMake puts in front of every compile, which the first start repeats
set(FCI_GAME_BASE_FLAGS /DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG -std:c++20)

# FCI_ABI_HASH: what the executable and the DLL must agree on. A DLL built for another hash is stale and is built
# again; a hash changes with any of these files or the flags (re-run on changes through CMAKE_CONFIGURE_DEPENDS).
file(GLOB FREESTANDING_HEADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/runtime/freestanding/*)
file(GLOB RECOMP_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/recomp/*.cpp ${CMAKE_SOURCE_DIR}/tools/recomp/*.h)
set(FCI_ABI_FILES ${CMAKE_SOURCE_DIR}/src/runtime/guest.h ${FREESTANDING_HEADERS} ${RECOMP_FILES}
    ${CMAKE_SOURCE_DIR}/src/xbe/xbe.h ${CMAKE_SOURCE_DIR}/src/xbe/xbe.cpp ${CMAKE_SOURCE_DIR}/config/hle.txt
    ${CMAKE_SOURCE_DIR}/config/symbols.txt ${CMAKE_SOURCE_DIR}/config/symbols_fci.txt)
list(SORT FCI_ABI_FILES)
set(abi_text "${FCI_GAME_BASE_FLAGS} ${FCI_GAME_FLAGS} ${FCI_GAME_MLLVM}")
foreach(f ${FCI_ABI_FILES})
    file(READ ${f} content)
    string(REPLACE "\r" "" content "${content}")  # the same hash for either line ending of a checkout
    file(RELATIVE_PATH rel ${CMAKE_SOURCE_DIR} ${f})
    string(APPEND abi_text "\n--- ${rel}\n${content}")
endforeach()
string(SHA256 abi_sha "${abi_text}")
string(SUBSTRING ${abi_sha} 0 16 abi_hex)
set(FCI_ABI_HASH "0x${abi_hex}ull")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FCI_ABI_FILES})
message(STATUS "game code ABI ${FCI_ABI_HASH}")

# fci_abi.h for the executable: the hash, the flags and the version of the compiler that built it
list(JOIN FCI_GAME_BASE_FLAGS " " base_flags)
list(JOIN FCI_GAME_FLAGS " " game_flags)
set(FCI_HOST_GEN ${CMAKE_BINARY_DIR}/host_gen)
file(CONFIGURE OUTPUT ${FCI_HOST_GEN}/fci_abi.h CONTENT "// generated by CMakeLists.txt
#pragma once
#define FCI_ABI_HASH ${FCI_ABI_HASH}
#define FCI_GAME_FLAGS L\"${base_flags} ${game_flags} ${FCI_GAME_MLLVM}\"
#define FCI_CLANG_VERSION \"${CMAKE_CXX_COMPILER_VERSION}\"
")

# what the executable compiles against from the configuration alone (no xbe): SYM_<name> and the host replacements
# it exports (hle_hooks.h/.cpp)
add_custom_command(
    OUTPUT ${FCI_HOST_GEN}/image_symbols.h ${FCI_HOST_GEN}/hle_hooks.h ${FCI_HOST_GEN}/hle_hooks.cpp
    COMMAND fci-recomp --host-headers ${CMAKE_SOURCE_DIR}/config ${FCI_HOST_GEN}
    DEPENDS fci-recomp ${CMAKE_SOURCE_DIR}/config/symbols.txt ${CMAKE_SOURCE_DIR}/config/symbols_fci.txt
            ${CMAKE_SOURCE_DIR}/config/hle.txt
    COMMENT "Writing the host headers from config/")

# profile guided optimization of the recompiled code: FCI_PGO=generate builds it instrumented (run it with
# FCI_PGO_WRITE=<seconds>, then llvm-profdata merge; tools/pgo.ps1 does all of it), FCI_PGO=<file.profdata> optimizes
# with that profile, empty (the default) with config/game_code.profdata if it exists; OFF without one
set(FCI_PGO "" CACHE STRING "generate, the .profdata file the game code is optimized with, empty: config/game_code.profdata, OFF")
set(pgo "${FCI_PGO}")
if(pgo STREQUAL "" AND EXISTS "${CMAKE_SOURCE_DIR}/config/game_code.profdata")
    set(pgo "${CMAKE_SOURCE_DIR}/config/game_code.profdata")
endif()
# ThinLTO of the recompiled code (lld-link does the code generation)
option(FCI_LTO "ThinLTO for the game code" OFF)

# ---- the executable (runtime, kernel, HLE, renderer; no game code) ----

add_executable(FarCryInstincts
    src/app/main.cpp
    # builds the game code DLL from the player's xbe on the first start (parts adapted from SCDA-XBERecomp, MIT)
    src/app/game_code.cpp
    src/audio/apu.cpp
    src/gpu/nv2a.cpp
    src/gpu/pgraph.cpp
    src/gpu/kelvin.cpp
    src/gpu/renderer.cpp
    src/gpu/vsh.cpp
    src/gpu/psh.cpp
    src/gpu/texture.cpp
    src/runtime/game_dll.cpp
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
    ${FCI_HOST_GEN}/hle_hooks.cpp
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
target_include_directories(FarCryInstincts PRIVATE src ${FCI_HOST_GEN})
# SDL's headers go to the controller code alone (linked LINK_ONLY)
set_source_files_properties(src/hle/host_pad.cpp PROPERTIES INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include")
target_include_directories(FarCryInstincts SYSTEM PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# the menu feeds the controller to ImGui itself (every XInput pad, Back+Start)
target_compile_definitions(FarCryInstincts PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_link_libraries(FarCryInstincts PRIVATE xbe dbghelp winmm d3d11 dxgi d3dcompiler xinput xaudio2 ole32 version)
# what it exports to the game code DLL (FCI_RT_API, hle_hooks.h, the C runtime functions of runtime.cpp) goes into
# its import library, compiler/FarCryInstincts.lib, which the DLL links against
set_target_properties(FarCryInstincts PROPERTIES ENABLE_EXPORTS ON ARCHIVE_OUTPUT_DIRECTORY ${COMPILER_DIR})
# Microsoft's HLSL compiler (Windows SDK redistributable) next to the exe, which the loader takes first (application
# directory), under Wine/Proton too; a release ships it (tools/dist.ps1). Delete the copy to test Proton's builtin
# compiler (vkd3d-shader)
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
# no high entropy ASLR: thread stacks that DLLs create at load time (before our first static initializer)
# could otherwise land anywhere in 64 bit space, sometimes in the guest window at 0x2000000000
target_link_options(FarCryInstincts PRIVATE /DEBUG /HIGHENTROPYVA:NO)
# line tables for the host code (no runtime cost): the profiler (FCI_PROFILE) names functions in anonymous
# namespaces and the source lines of inlined code
target_compile_options(FarCryInstincts PRIVATE -gline-tables-only)
# a GUI program with the usual main(): no console window (none under Wine/Proton either); the log attaches to the
# console it was started from, FCI_CONSOLE=1 opens one (src/runtime/log.cpp)
target_link_options(FarCryInstincts PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)

# the rest of the compiler folder: the recompiler, the headers the generated code includes and the configuration
add_custom_target(compiler_files ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${COMPILER_DIR}/config ${COMPILER_DIR}/include/runtime
            ${COMPILER_DIR}/include/freestanding
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fci-recomp> ${COMPILER_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/runtime/guest.h ${COMPILER_DIR}/include/runtime
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FREESTANDING_HEADERS} ${COMPILER_DIR}/include/freestanding
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/config/hle.txt ${CMAKE_SOURCE_DIR}/config/symbols.txt
            ${CMAKE_SOURCE_DIR}/config/symbols_fci.txt ${COMPILER_DIR}/config
    VERBATIM)
add_dependencies(compiler_files fci-recomp)
if(EXISTS ${CMAKE_SOURCE_DIR}/config/game_code.profdata)
    add_custom_command(TARGET compiler_files POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/config/game_code.profdata ${COMPILER_DIR}/config)
endif()

# the release folder and zip (out/dist): cmake --build --preset release --target dist, with FCI_LLVM_DIR the folder
# tools/get_llvm.ps1 filled (the clang and lld the release bundles); never anything built from the game
set(FCI_LLVM_DIR "" CACHE PATH "tools/get_llvm.ps1's output folder: the LLVM release the dist target bundles")
if(FCI_LLVM_DIR)
    add_custom_target(dist
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tools/dist.ps1
                -Llvm ${FCI_LLVM_DIR} -Build ${CMAKE_BINARY_DIR} -Zip
        DEPENDS FarCryInstincts compiler_files
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM)
endif()

# ---- the game code DLL, from assets/default.xbe (left out without it: the release builds it on the first start) ----

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/assets/default.xbe)
    message(STATUS "no assets/default.xbe: the executable only, the game code is built on its first start")
    return()
endif()

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
    OUTPUT ${GAME_CODE} ${CMAKE_SOURCE_DIR}/generated/funcs.h
    COMMAND fci-recomp ${CMAKE_SOURCE_DIR}/assets/default.xbe ${CMAKE_SOURCE_DIR}/generated ${CMAKE_SOURCE_DIR}/config
    DEPENDS fci-recomp ${CMAKE_SOURCE_DIR}/assets/default.xbe ${CMAKE_SOURCE_DIR}/config/symbols.txt
            ${CMAKE_SOURCE_DIR}/config/symbols_fci.txt ${CMAKE_SOURCE_DIR}/config/hle.txt
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Recompiling default.xbe")

# game_code/game_code.dll beside the executable, as the first start builds it: no C runtime (/NOENTRY, nothing but
# what the executable exports), linked against the executable's import library
add_library(game_code SHARED ${GAME_CODE})
target_include_directories(game_code PRIVATE src/runtime/freestanding src generated)
target_compile_options(game_code PRIVATE ${FCI_GAME_FLAGS} "SHELL:${FCI_GAME_MLLVM}")
# the hash only where it is used (rt::GameCode in table.cpp), so a new hash does not recompile all of the code
set_source_files_properties(${CMAKE_SOURCE_DIR}/generated/table.cpp PROPERTIES COMPILE_DEFINITIONS FCI_ABI_HASH=${FCI_ABI_HASH})
target_link_libraries(game_code PRIVATE FarCryInstincts)
set_target_properties(game_code PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/game_code
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/game_code/lib)
# the symbols name f_<guest address> in crash stacks and the profiler
target_link_options(game_code PRIVATE /DEBUG /INCREMENTAL:NO)
if(pgo STREQUAL "generate")
    # instrumented: clang's profile runtime needs a C runtime of its own (the DLL's), which then also starts it
    target_compile_options(game_code PRIVATE -fprofile-instr-generate -DFCI_PGO_GENERATE)
    get_filename_component(llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB profile_rt "${llvm_bin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
    target_link_libraries(game_code PRIVATE "${profile_rt}" libcmt libucrt libvcruntime kernel32)
else()
    target_link_options(game_code PRIVATE /NOENTRY /NODEFAULTLIB)
    if(pgo)
        message(STATUS "game code optimized with the profile ${pgo}")
        target_compile_options(game_code PRIVATE "-fprofile-instr-use=${pgo}" -Wno-profile-instr-unprofiled
                               -Wno-profile-instr-out-of-date)
        set_source_files_properties(${GAME_CODE} PROPERTIES OBJECT_DEPENDS "${pgo}")
    endif()
endif()
if(FCI_LTO)
    target_compile_options(game_code PRIVATE -flto=thin)
    target_link_options(game_code PRIVATE "/lldltocache:${CMAKE_BINARY_DIR}/lto.cache")
endif()
