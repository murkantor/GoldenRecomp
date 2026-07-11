cmake_minimum_required(VERSION 3.20)
project(GoldenRecomp)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-everything /W4")
endif()

# Opt out of constexpr mutex constructor on windows to prevent vcredist issues
if (WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
endif()


# Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake 3.24:
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if (WIN32)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/lib/")
endif()

set(RT64_STATIC TRUE)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)

# set(BUILD_SHARED_LIBS_SAVED "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)

# SDL2 must be available before RecompFrontend: recompinput consumes
# SDL2_INCLUDE_DIRS directly. On Windows we fetch the prebuilt SDL2 here
# (moved up from the GoldenRecomp target section below).
if (WIN32)
    include(FetchContent)
    if (DEFINED ENV{SDL2_VERSION})
        set(SDL2_VERSION $ENV{SDL2_VERSION})
    else()
        set(SDL2_VERSION "2.30.3")
    endif()
    FetchContent_Declare(
        sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip
    )
    FetchContent_MakeAvailable(sdl2)
    set(SDL2_INCLUDE_DIRS ${sdl2_SOURCE_DIR}/include)
endif()

# DXC options + binary selection must exist BEFORE RecompFrontend is added:
# recompui's shader rules (build_vertex_shader/build_pixel_shader) expand
# ${DXC} at add_subdirectory time. (Previously lived after the GoldenRecomp
# target; an empty DXC made the shader commands ShellExecute the .hlsl
# sources — opening them in the default editor — instead of compiling.)
set (DXC_COMMON_OPTS "-I${PROJECT_SOURCE_DIR}/src")
set (DXC_DXIL_OPTS "-Wno-ignored-attributes")
set (DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
set (DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0" "-D DYNAMIC_RENDER_PARAMS")
set (DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-D DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
set (DXC_CS_OPTS "${DXC_COMMON_OPTS}" "-E" "CSMain" "-T cs_6_0")
set (DXC_GS_OPTS "${DXC_COMMON_OPTS}" "-E" "GSMain" "-T gs_6_0")
set (DXC_RT_OPTS "${DXC_COMMON_OPTS}" "-D" "RT_SHADER" "-T" "lib_6_3" "-fspv-target-env=vulkan1.1spirv1.4" "-fspv-extension=SPV_KHR_ray_tracing" "-fspv-extension=SPV_EXT_descriptor_indexing")

if (WIN32)
    set (DXC "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc.exe")
    add_compile_definitions(NOMINMAX)
else()
    if (APPLE)
        # Apple's binary is universal, so it'll work on both x86_64 and arm64
        set (DXC "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/arm64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-macos")
    else()
        if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
            set (DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/x64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc-linux")
        else()
            set (DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/arm64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-linux")
        endif()
    endif()
endif()

# RecompFrontend (recompui + recompinput) replaces the vendored src/ui and
# input stack. It bundles its own RmlUi/lunasvg/freetype under recompui/lib,
# so the old lib/RmlUi and lib/lunasvg subdirectories are no longer built.
set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)
set(RECOMP_FRONTEND_RT64_PATH ${CMAKE_SOURCE_DIR}/lib/rt64)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/RecompFrontend)

# librecomp bundles nlohmann json 3.9 while RT64 (and therefore recompui)
# bundles 3.12 - the ABI version is part of the mangled names, so recompui's
# references to recomp::config::Config methods would not link. Force the
# runtime targets onto RT64's copy.
target_include_directories(librecomp BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib)
target_include_directories(ultramodern BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib)

# The bundled fmt 10.x fails its FMT_STRING consteval parse under clang >= 21
# (string-literal pointer arithmetic no longer constant-foldable); building it
# as C++17 disables fmt's consteval path without touching the submodule.
if (TARGET fmt)
    set_target_properties(fmt PROPERTIES CXX_STANDARD 17)
endif()

target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)

# RecompiledFuncs - Library containing the primary recompiler output
add_library(RecompiledFuncs STATIC)

target_compile_options(RecompiledFuncs PRIVATE
    # -Wno-unused-but-set-variable
    -fno-strict-aliasing
    # The regenerated funcs declare game functions whose names collide with
    # libc builtins (isspace et al.).
    -fno-builtin
    -Wno-unused-variable
    -Wno-implicit-function-declaration
)

target_include_directories(RecompiledFuncs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    # New runtime namespaces recomp.h under librecomp/; generated code
    # still includes the flat "recomp.h".
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include/librecomp
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)

file(GLOB FUNC_C_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c)
file(GLOB FUNC_CXX_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.cpp)

target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})

# PatchesLib - Library containing the recompiled output for any custom function patches
add_library(PatchesLib STATIC)

target_compile_options(PatchesLib PRIVATE
    # -Wno-unused-but-set-variable
    -fno-strict-aliasing
    -fno-builtin
    -Wno-unused-variable
    -Wno-implicit-function-declaration
)

target_include_directories(PatchesLib PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    # New runtime namespaces recomp.h under librecomp/; generated code
    # still includes the flat "recomp.h".
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include/librecomp
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)

target_sources(PatchesLib PRIVATE
    ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches.c
    ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.c
)

set_source_files_properties(${CMAKE_SOURCE_DIR}/RecompiledPatches/patches.c PROPERTIES COMPILE_FLAGS -fno-strict-aliasing)

# Build patches elf
if(NOT DEFINED PATCHES_C_COMPILER)
    set(PATCHES_C_COMPILER clang)
endif()

if(NOT DEFINED PATCHES_LD)
    set(PATCHES_LD ld.lld)
endif()

if(NOT DEFINED PATCHES_OBJCOPY)
    set(PATCHES_OBJCOPY llvm-objcopy)
endif()

if(WIN32)
    # No native make/mips-clang on Windows; build the patches inside WSL.
    add_custom_target(PatchesBin
        COMMAND wsl make CC=${PATCHES_C_COMPILER} LD=${PATCHES_LD} OBJCOPY=${PATCHES_OBJCOPY}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/patches
        BYPRODUCTS ${CMAKE_SOURCE_DIR}/patches/patches.elf
    )
else()
    add_custom_target(PatchesBin
        COMMAND ${CMAKE_COMMAND} -E env CC=${PATCHES_C_COMPILER} LD=${PATCHES_LD} OBJCOPY=${PATCHES_OBJCOPY} make
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/patches
        BYPRODUCTS ${CMAKE_SOURCE_DIR}/patches/patches.elf
    )
endif()

# Generate patches_bin.c from patches.bin
add_custom_command(OUTPUT ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.c
    COMMAND file_to_c ${CMAKE_SOURCE_DIR}/patches/patches.bin mm_patches_bin ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.c ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.h 
    DEPENDS ${CMAKE_SOURCE_DIR}/patches/patches.bin
)

# Recompile patches elf into patches.c and patches.bin
add_custom_command(OUTPUT
                       ${CMAKE_SOURCE_DIR}/patches/patches.bin
                       ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches.c
                       ${CMAKE_SOURCE_DIR}/RecompiledPatches/recomp_overlays.inl
                       ${CMAKE_SOURCE_DIR}/RecompiledPatches/funcs.h
                   # TODO: Look into why modifying patches requires two builds to take
                   COMMAND ./N64Recomp patches.toml
                   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                   DEPENDS ${CMAKE_SOURCE_DIR}/patches/patches.elf
)

# Download controller db file for controller support via SDL2
set(GAMECONTROLLERDB_COMMIT "b1e4090b3d4266e55feb0793efa35792e05faf66")
set(GAMECONTROLLERDB_URL "https://raw.githubusercontent.com/gabomdq/SDL_GameControllerDB/${GAMECONTROLLERDB_COMMIT}/gamecontrollerdb.txt")

file(DOWNLOAD ${GAMECONTROLLERDB_URL} ${CMAKE_SOURCE_DIR}/gamecontrollerdb.txt
     TLS_VERIFY ON)

add_custom_target(DownloadGameControllerDB
    DEPENDS ${CMAKE_SOURCE_DIR}/gamecontrollerdb.txt)

# Main executable
add_executable(GoldenRecomp)
add_dependencies(GoldenRecomp DownloadGameControllerDB)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    # The vendored SDL 2.26 headers redefine clang's _m_prefetch builtin (a
    # workaround for clang 11 that breaks clang >= 15). Pre-including intrin.h
    # defines the real intrinsic first, so SDL's fallback is skipped.
    target_compile_options(rt64 PRIVATE /FIintrin.h)
    target_compile_options(GoldenRecomp PRIVATE /FIintrin.h)
    # RecompFrontend targets include SDL headers too.
    target_compile_options(recompinput PRIVATE /FIintrin.h)
    target_compile_options(recompui PRIVATE /FIintrin.h)
    # rt64 builds its vendored imgui with -Werror; clang >= 21 introduces this
    # warning, which imgui 1.x trips.
    target_compile_options(rt64 PRIVATE -Wno-uninitialized-const-pointer)
endif()

# Generate mm_shader_cache.c from the MM shader cache if it exists
if (EXISTS ${CMAKE_SOURCE_DIR}/shadercache/mm_shader_cache.bin)
    set(HAS_MM_SHADER_CACHE TRUE)
    target_compile_definitions(GoldenRecomp PRIVATE HAS_MM_SHADER_CACHE)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mm_shader_cache.c ${CMAKE_CURRENT_BINARY_DIR}/mm_shader_cache.h
        COMMAND file_to_c ${CMAKE_SOURCE_DIR}/shadercache/mm_shader_cache.bin mm_shader_cache_bytes ${CMAKE_CURRENT_BINARY_DIR}/mm_shader_cache.c ${CMAKE_CURRENT_BINARY_DIR}/mm_shader_cache.h
        DEPENDS ${CMAKE_SOURCE_DIR}/shadercache/mm_shader_cache.bin
    )
endif()

set (SOURCES
    ${CMAKE_SOURCE_DIR}/src/main/main.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_patches.cpp
    # src/main/rt64_render_context.cpp superseded by recompui's renderer

    # src/game/input.cpp and src/ui/* are replaced by RecompFrontend
    # (recompinput + recompui); game-side glue below is ported to its APIs.
    ${CMAKE_SOURCE_DIR}/src/game/input_compat.cpp
    ${CMAKE_SOURCE_DIR}/src/game/controls.cpp
    ${CMAKE_SOURCE_DIR}/src/game/config.cpp
    ${CMAKE_SOURCE_DIR}/src/game/scene_table.cpp
    ${CMAKE_SOURCE_DIR}/src/game/debug.cpp
    ${CMAKE_SOURCE_DIR}/src/game/quicksaving.cpp
    ${CMAKE_SOURCE_DIR}/src/game/recomp_api.cpp

    ${CMAKE_SOURCE_DIR}/rsp/aspMain.cpp
    #${CMAKE_SOURCE_DIR}/rsp/njpgdspMain.cpp
)

if (HAS_MM_SHADER_CACHE)
    list(APPEND SOURCES ${CMAKE_CURRENT_BINARY_DIR}/mm_shader_cache.c)
endif()

target_include_directories(GoldenRecomp PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/concurrentqueue
    ${CMAKE_SOURCE_DIR}/lib/GamepadMotionHelpers
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/inc
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/rhi
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/render
    ${CMAKE_SOURCE_DIR}/lib/sse2neon
    ${CMAKE_SOURCE_DIR}/lib/freetype-windows-binaries/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include
    ${CMAKE_BINARY_DIR}/shaders
    ${CMAKE_CURRENT_BINARY_DIR}
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    target_compile_options(GoldenRecomp PRIVATE
        #-march=nehalem
        -march=core2
        -fno-strict-aliasing
        -fms-extensions
        -msse4.1 # added for core2
    )
else()
    target_compile_options(GoldenRecomp PRIVATE
        -fno-strict-aliasing
        -fms-extensions
    )
endif()

if (WIN32)
    # SDL2 is fetched near the top of this file (before RecompFrontend).
    target_include_directories(GoldenRecomp PRIVATE
        ${sdl2_SOURCE_DIR}/include
    )
    target_link_directories(GoldenRecomp PRIVATE
        ${sdl2_SOURCE_DIR}/lib/x64
    )

    # Copy SDL2 and dxc DLLs to output folder as post build step
    add_custom_command(TARGET GoldenRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:GoldenRecomp>)

    set_target_properties(
        GoldenRecomp
        PROPERTIES
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
            LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
            LINK_FLAGS_MINSIZEREL "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )

    target_sources(GoldenRecomp PRIVATE ${CMAKE_SOURCE_DIR}/icons/app.rc)
endif()

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    find_package(SDL2 REQUIRED)
    find_package(X11 REQUIRED)

    # Generate icon_bytes.c from the app icon PNG.
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.c ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.h
        COMMAND file_to_c ${CMAKE_SOURCE_DIR}/icons/512.png icon_bytes ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.c ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.h 
        DEPENDS ${CMAKE_SOURCE_DIR}/icons/512.png
    )
    target_sources(GoldenRecomp PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.c)

    message(STATUS "SDL2_FOUND = ${SDL2_FOUND}")
    message(STATUS "SDL2_INCLUDE_DIRS = ${SDL2_INCLUDE_DIRS}")

    target_include_directories(GoldenRecomp PRIVATE ${SDL2_INCLUDE_DIRS})

    message(STATUS "X11_FOUND = ${X11_FOUND}")
    message(STATUS "X11_Xrandr_FOUND = ${X11_Xrandr_FOUND}")
    message(STATUS "X11_INCLUDE_DIR = ${X11_INCLUDE_DIR}")
    message(STATUS "X11_LIBRARIES = ${X11_LIBRARIES}")

    target_include_directories(GoldenRecomp PRIVATE ${X11_INCLUDE_DIR} ${X11_Xrandr_INCLUDE_PATH})
    target_link_libraries(GoldenRecomp PRIVATE ${X11_LIBRARIES} ${X11_Xrandr_LIB})

    find_package(Freetype REQUIRED)

    message(STATUS "FREETYPE_FOUND = ${FREETYPE_FOUND}")
    message(STATUS "FREETYPE_INCLUDE_DIRS = ${FREETYPE_INCLUDE_DIRS}")
    message(STATUS "FREETYPE_LIBRARIES = ${FREETYPE_LIBRARIES}")

    include_directories(${FREETYPE_LIBRARIES})
    target_link_libraries(GoldenRecomp PRIVATE ${FREETYPE_LIBRARIES})

    set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)

    target_link_libraries(GoldenRecomp PRIVATE "-latomic -static-libstdc++" ${CMAKE_DL_LIBS} Threads::Threads)
endif()

target_link_libraries(GoldenRecomp PRIVATE
    PatchesLib
    RecompiledFuncs
    SDL2
    librecomp
    ultramodern
    rt64
    recompui
    recompinput
    nfd
)

# Every RECOMP_PATCH function exists twice in this link: the recompiled
# original in RecompiledFuncs and the patch in PatchesLib. A full link
# resolves each duplicate to PatchesLib (listed first); INCREMENTAL linking
# re-binds them unpredictably on partial relinks — observed 2026-07-11 as
# patches silently reverting to the original code (no clouds, the Start-Game
# crash returning) after builds that only touched the patches lib. Force
# full links.
if(WIN32)
    target_link_options(GoldenRecomp PRIVATE "/INCREMENTAL:NO")
endif()

# Interface UI shaders are built by recompui now (it declares the same
# output paths — duplicating them here breaks the ninja graph). The DXC
# variable block moved above the RecompFrontend add_subdirectory: recompui's
# shader rules expand it there, and an empty DXC made the generated commands
# ShellExecute the .hlsl files (opening them in the default editor) instead
# of compiling them.

target_sources(GoldenRecomp PRIVATE ${SOURCES})

set_property(TARGET GoldenRecomp PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

