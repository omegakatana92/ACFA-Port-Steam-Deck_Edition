cmake_minimum_required(VERSION 3.25)
# RC is the Windows resource compiler. Linux has no equivalent and the .rc files carry Windows-only
# resources, so the language and those sources are selected per platform.
if(WIN32)
  project(acfa LANGUAGES C CXX RC)
else()
  project(acfa LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX UNICODE _UNICODE)
endif()

# The port's version (VERSION; the release's changelog names it), shown by the launcher and logged
# by the game.
file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION ACFA_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/VERSION)
add_compile_definitions(ACFA_VERSION="${ACFA_VERSION}")

# Developer diagnostics (control-file commands, profiler, memory search and watchpoints, traces,
# frame reports, A/B switches, test autopilot). Off in player builds: compiled out, and the
# release export (tools\release) removes their code.
option(ACFA_DEV "Build with developer diagnostics" OFF)
if(ACFA_DEV)
  add_compile_definitions(ACFA_DEV=1)
else()
  add_compile_definitions(ACFA_DEV=0)
endif()

# Linux runtime: off until the platform layer and a graphics backend exist. Windows builds are
# unaffected by this option.
option(ACFA_BUILD_LINUX_RUNTIME "Build the native Linux runtime (requires the platform layer)" OFF)
option(ACFA_USE_SYSTEM_SDL3 "Use an installed SDL3 instead of the pinned release" OFF)
# Host-side tests of portable code (no game data): hashes, paths, settings parsing.
option(ACFA_BUILD_TESTS "Build the host-side unit tests" OFF)

# Self-contained executables (no libc++/libunwind DLLs next to them), reproducible: no build
# paths or link timestamps in the binaries.
if(MINGW)
  add_link_options(-static -Wl,--no-insert-timestamp)
endif()
if(NOT WIN32)
  # The Linux runtime and tools use pthreads and dlopen (Vulkan loader, SDL3).
  find_package(Threads REQUIRED)
  add_compile_definitions(_FILE_OFFSET_BITS=64 _GNU_SOURCE)
endif()
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)
# UTF-8 process code page for every executable (paths with any characters).
if(WIN32)
  set(ACFA_UTF8_RC ${CMAKE_SOURCE_DIR}/src/common/utf8.rc)
else()
  set(ACFA_UTF8_RC "")
endif()

# ---- third party ----
file(GLOB ZLIB_SRC ${CMAKE_SOURCE_DIR}/third_party/zlib/*.c)
add_library(acfa_zlib STATIC ${ZLIB_SRC})
target_include_directories(acfa_zlib PUBLIC ${CMAKE_SOURCE_DIR}/third_party/zlib)
target_compile_options(acfa_zlib PRIVATE -w)

# ---- common ----
add_library(acfa_common STATIC
  src/common/bytes.cpp
  src/common/crypto.cpp
  src/common/known_inputs.cpp
  src/common/file.cpp
  src/common/ini.cpp
  src/common/privacy.cpp
  src/common/settings_file.cpp
  src/common/sfo.cpp)
target_include_directories(acfa_common PUBLIC ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
  target_link_libraries(acfa_common PUBLIC acfa_zlib bcrypt)
else()
  # SHA-1/SHA-256 are implemented in crypto.cpp on POSIX (no CNG, no OpenSSL dependency).
  target_link_libraries(acfa_common PUBLIC acfa_zlib Threads::Threads)
endif()
target_compile_options(acfa_common PRIVATE -Wall -Wextra)

# ---- PPU decoding (shared by recompiler and runtime) ----
add_library(acfa_ppu STATIC src/ppu/ppu_decode.cpp)
target_link_libraries(acfa_ppu PUBLIC acfa_common)
target_compile_options(acfa_ppu PRIVATE -Wall -Wextra)

# ---- SPU ISA, chunk discovery (shared by the SPU recompiler and the runtime) ----
add_library(acfa_spu STATIC src/spu/spu_isa.cpp)
target_link_libraries(acfa_spu PUBLIC acfa_common)
target_compile_options(acfa_spu PRIVATE -Wall -Wextra)

add_executable(acfa-spurecomp src/spurecomp/main.cpp ${ACFA_UTF8_RC})
target_link_libraries(acfa-spurecomp PRIVATE acfa_spu)
target_compile_options(acfa-spurecomp PRIVATE -Wall -Wextra)

# ---- recompiler ----
add_executable(acfa-recomp
  src/recomp/main.cpp
  src/recomp/image.cpp
  src/recomp/analysis.cpp
  src/recomp/emit.cpp
  ${ACFA_UTF8_RC})
target_link_libraries(acfa-recomp PRIVATE acfa_ppu)
target_compile_options(acfa-recomp PRIVATE -Wall -Wextra)

# ---- generated PPU code (game-derived, from local/gen/ppu; never distributed) ----
set(ACFA_GEN_DIR "${CMAKE_SOURCE_DIR}/local/gen/ppu" CACHE PATH "Directory with acfa-recomp output")
file(GLOB ACFA_GEN_SRC CONFIGURE_DEPENDS "${ACFA_GEN_DIR}/*.cpp")
set(ACFA_CPU_FLAGS "-march=haswell" CACHE STRING "CPU flags for generated code and the runtime (x86-64-v3: AVX2 + FMA + BMI2)")

if(ACFA_GEN_SRC)
  add_library(acfa_ppu_gen STATIC ${ACFA_GEN_SRC})
  target_include_directories(acfa_ppu_gen PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa_ppu_gen PRIVATE -O2 ${ACFA_CPU_FLAGS} -fno-strict-aliasing -w)

    file(GLOB ACFA_RT_SRC CONFIGURE_DEPENDS
      ${CMAKE_SOURCE_DIR}/src/runtime/*.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/hle/*.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/lv2/*.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/rsx/*.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/gpu/*.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/audio/*.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/media/*.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/game/*.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/spu/*.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/ui/*.cpp)
    if(NOT WIN32)
      # Windows-only backends. The Linux build uses the SDL3 platform layer and the placeholder
      # renderer until the Vulkan backend lands; keeping the D3D11 files out avoids dragging
      # Direct3D types into the Linux build.
      list(REMOVE_ITEM ACFA_RT_SRC
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/renderer.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/submit.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/d3d11_device.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/window.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/display.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/rsx_texture.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/shader_warm.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/png.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/vp_hlsl.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/fp_hlsl.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/ui/overlay.cpp)
      list(APPEND ACFA_RT_SRC
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/linux/window_sdl.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/linux/display_clock.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/linux/device_stub.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/linux/renderer_stub.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/gpu/vulkan/vk_presenter.cpp
        ${CMAKE_SOURCE_DIR}/src/runtime/ui/overlay_stub.cpp)
    endif()
  # Recompiled SPU code (game-derived, from local/gen/spu; never distributed).
  set(ACFA_SPU_GEN_DIR "${CMAKE_SOURCE_DIR}/local/gen/spu" CACHE PATH "Directory with acfa-spurecomp output")
  file(GLOB ACFA_SPU_GEN_SRC CONFIGURE_DEPENDS "${ACFA_SPU_GEN_DIR}/*.cpp")
  if(NOT ACFA_SPU_GEN_SRC)
    set(ACFA_SPU_GEN_SRC ${CMAKE_SOURCE_DIR}/src/spu/spu_gen_empty.cpp)
  endif()
  add_library(acfa_spu_gen STATIC ${ACFA_SPU_GEN_SRC})
  target_include_directories(acfa_spu_gen PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa_spu_gen PRIVATE -O2 ${ACFA_CPU_FLAGS} -fno-strict-aliasing -w)

  if(WIN32)
    list(REMOVE_ITEM ACFA_RT_SRC ${CMAKE_SOURCE_DIR}/src/runtime/ui/overlay_stub.cpp)
    add_executable(acfa ${ACFA_RT_SRC} ${ACFA_UTF8_RC} src/runtime/game.rc)
    if(NOT ACFA_DEV)  # players' builds open no console window (the run log holds every message)
      set_target_properties(acfa PROPERTIES WIN32_EXECUTABLE ON)
    endif()
    target_include_directories(acfa PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(acfa PRIVATE ${ACFA_CPU_FLAGS} -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -g)
    # Whole-archive: the function table references everything anyway; keeps HLE registrars alive.
    target_link_libraries(acfa PRIVATE acfa_ppu_gen acfa_spu_gen acfa_spu acfa_ppu acfa_common d3d11 dxgi d3dcompiler d2d1 dwrite ole32 psapi
                          synchronization hid setupapi)
  elseif(ACFA_BUILD_LINUX_RUNTIME)
    # SDL3 provides the window, controller and audio device layer on Linux. Fetch a pinned release
    # by default (WSL can otherwise find a Windows SDL3 CMake package through the /mnt prefix).
    if(ACFA_USE_SYSTEM_SDL3)
      find_package(SDL3 REQUIRED CONFIG)
    else()
      include(FetchContent)
      set(SDL_SHARED OFF CACHE BOOL "" FORCE)
      set(SDL_STATIC ON CACHE BOOL "" FORCE)
      set(SDL_TEST OFF CACHE BOOL "" FORCE)
      FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP OFF)
      FetchContent_MakeAvailable(SDL3)
    endif()
    find_package(Vulkan REQUIRED)
    add_executable(acfa ${ACFA_RT_SRC})
    target_include_directories(acfa PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(acfa PRIVATE ${ACFA_CPU_FLAGS} -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -g)
    target_link_libraries(acfa PRIVATE acfa_ppu_gen acfa_spu_gen acfa_spu acfa_ppu acfa_common Threads::Threads ${CMAKE_DL_LIBS})
    if(TARGET SDL3::SDL3)
      target_link_libraries(acfa PRIVATE SDL3::SDL3)
    elseif(TARGET SDL3::SDL3-static)
      target_link_libraries(acfa PRIVATE SDL3::SDL3-static)
    endif()
    if(Vulkan_FOUND)
      target_link_libraries(acfa PRIVATE Vulkan::Vulkan)
    endif()
    if(ACFA_BUILD_TESTS AND TARGET SDL3::SDL3 AND Vulkan_FOUND)
      add_executable(acfa-vk-present
        src/runtime/gpu/vulkan/vk_present_test.cpp
        src/runtime/gpu/vulkan/vk_presenter.cpp)
      target_include_directories(acfa-vk-present PRIVATE ${CMAKE_SOURCE_DIR}/src)
      target_compile_options(acfa-vk-present PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
      target_link_libraries(acfa-vk-present PRIVATE SDL3::SDL3 Vulkan::Vulkan)
    endif()
  else()
    message(STATUS "Generated PPU code found, but the Linux runtime is not enabled yet "
                   "(configure with -DACFA_BUILD_LINUX_RUNTIME=ON after the platform layer is complete).")
  endif()
endif()

# ---- preparation tool ----
add_executable(acfa-prep
  src/prep/main.cpp
  src/prep/import.cpp
  src/prep/prx.cpp
  src/prep/sfo.cpp
  src/prep/spu_records.cpp
  ${ACFA_UTF8_RC})
target_link_libraries(acfa-prep PRIVATE acfa_common)
target_compile_options(acfa-prep PRIVATE -Wall -Wextra)

# ---- launcher (players' setup and start; no game data or game-derived code) ----
if(WIN32)
add_executable(acfa-launcher WIN32
  src/launcher/main.cpp
  src/launcher/setup.cpp
  src/launcher/inputs.cpp
  src/launcher/process.cpp
  src/launcher/rpcs3.cpp
  src/launcher/download.cpp
  src/launcher/source.cpp
  src/launcher/install.cpp
  src/launcher/launcher.rc)
# The launcher's file version (its properties): the numbers of VERSION, for its resource file only.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" acfa_version_numbers "${ACFA_VERSION}")
set_source_files_properties(src/launcher/launcher.rc PROPERTIES COMPILE_DEFINITIONS
  "ACFA_VERSION_MAJOR=${CMAKE_MATCH_1};ACFA_VERSION_MINOR=${CMAKE_MATCH_2};ACFA_VERSION_PATCH=${CMAKE_MATCH_3}")
# A release launcher carries the port's sources (a zip of the exported tree, made by
# tools/release/export.py) and unpacks them into its install folder (src/launcher/source.h).
set(ACFA_SOURCE_ZIP "" CACHE FILEPATH "Zip of the port's sources to embed in the launcher (releases)")
if(ACFA_SOURCE_ZIP)
  file(TO_CMAKE_PATH "${ACFA_SOURCE_ZIP}" acfa_source_zip)
  file(WRITE ${CMAKE_BINARY_DIR}/launcher_source.rc "2 RCDATA \"${acfa_source_zip}\"\n")
  target_sources(acfa-launcher PRIVATE ${CMAKE_BINARY_DIR}/launcher_source.rc)
  set_source_files_properties(${CMAKE_BINARY_DIR}/launcher_source.rc PROPERTIES OBJECT_DEPENDS "${acfa_source_zip}")
endif()
target_include_directories(acfa-launcher PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(acfa-launcher PRIVATE acfa_common comctl32 winhttp shell32 ole32 uuid advapi32)
target_link_options(acfa-launcher PRIVATE -municode)
target_compile_options(acfa-launcher PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
set_target_properties(acfa-launcher PROPERTIES OUTPUT_NAME ACFA-Launcher)
endif()  # WIN32 launcher

# ---- host-side tests (portable code only; no game data) ----
if(ACFA_BUILD_TESTS)
  enable_testing()
  add_executable(acfa-host-tests tests/host_tests.cpp)
  target_link_libraries(acfa-host-tests PRIVATE acfa_common)
  target_compile_options(acfa-host-tests PRIVATE -Wall -Wextra)
  add_test(NAME host_tests COMMAND acfa-host-tests)
  if(TARGET acfa-vk-present)
    add_test(NAME vulkan_present COMMAND acfa-vk-present --frames 60 --verify)
    set_tests_properties(vulkan_present PROPERTIES SKIP_RETURN_CODE 77)
  endif()

  # Vulkan foundation gate: instance/device/image/clear/readback, headless (lavapipe works).
  if(NOT WIN32)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
      add_executable(acfa-vk-smoke src/runtime/gpu/vulkan/vk_clear_test.cpp)
      target_link_libraries(acfa-vk-smoke PRIVATE Vulkan::Vulkan)
      add_test(NAME vulkan_clear COMMAND acfa-vk-smoke)
    else()
      message(STATUS "Vulkan not found: the Vulkan clear smoke test is not built")
    endif()

    # Stage 2 gate: DXC -> SPIR-V, shifted bindings and a synthetic draw.
    # Set ACFA_DXC_EXECUTABLE (and ACFA_DXC_LIB_DIR if the loader is not found) to enable it.
    set(ACFA_DXC_EXECUTABLE "" CACHE FILEPATH "dxc executable for the Vulkan Stage 2 gate")
    set(ACFA_DXC_LIB_DIR "" CACHE PATH "Directory containing libdxcompiler.so (empty: system/default)")
    if(NOT ACFA_DXC_EXECUTABLE)
      find_program(_ACFA_DXC_FOUND NAMES dxc)
      if(_ACFA_DXC_FOUND)
        set(ACFA_DXC_EXECUTABLE "${_ACFA_DXC_FOUND}" CACHE FILEPATH "dxc executable for the Vulkan Stage 2 gate" FORCE)
      endif()
    endif()
    if(ACFA_DXC_EXECUTABLE AND Vulkan_FOUND)
      if(NOT ACFA_DXC_LIB_DIR)
        get_filename_component(_dxc_bin_dir "${ACFA_DXC_EXECUTABLE}" DIRECTORY)
        get_filename_component(_dxc_root "${_dxc_bin_dir}" DIRECTORY)
        if(EXISTS "${_dxc_root}/lib/libdxcompiler.so")
          set(ACFA_DXC_LIB_DIR "${_dxc_root}/lib")
        endif()
      endif()
      set(_stage2_dir "${CMAKE_BINARY_DIR}/stage2")
      set(_stage2_vs "${_stage2_dir}/stage2_vs.spv")
      set(_stage2_ps "${_stage2_dir}/stage2_ps.spv")
      set(_stage3_vs "${_stage2_dir}/stage3_vs.spv")
      set(_stage3_ps "${_stage2_dir}/stage3_ps.spv")
      set(_dxc_shifts -fvk-b-shift 0 0 -fvk-t-shift 1000 0 -fvk-s-shift 2000 0 -fvk-u-shift 3000 0)
      set(_dxc_env "")
      if(ACFA_DXC_LIB_DIR)
        set(_dxc_env "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${ACFA_DXC_LIB_DIR}")
      endif()
      add_custom_command(
        OUTPUT ${_stage2_vs} ${_stage2_ps} ${_stage3_vs} ${_stage3_ps}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_stage2_dir}
        COMMAND ${_dxc_env} ${ACFA_DXC_EXECUTABLE} -T vs_6_2 -E vs_main -spirv -fspv-target-env=vulkan1.1
                -fspv-entrypoint-name=main -fvk-use-dx-layout ${_dxc_shifts} -Fo ${_stage2_vs} ${CMAKE_SOURCE_DIR}/tests/shaders/stage2.hlsl
        COMMAND ${_dxc_env} ${ACFA_DXC_EXECUTABLE} -T ps_6_2 -E ps_main -spirv -fspv-target-env=vulkan1.1
                -fspv-entrypoint-name=main -fvk-use-dx-layout ${_dxc_shifts} -Fo ${_stage2_ps} ${CMAKE_SOURCE_DIR}/tests/shaders/stage2.hlsl
        COMMAND ${_dxc_env} ${ACFA_DXC_EXECUTABLE} -T vs_6_2 -E vs_main -spirv -fspv-target-env=vulkan1.1
                -fspv-entrypoint-name=main -fvk-use-dx-layout ${_dxc_shifts} -Fo ${_stage3_vs} ${CMAKE_SOURCE_DIR}/tests/shaders/stage3.hlsl
        COMMAND ${_dxc_env} ${ACFA_DXC_EXECUTABLE} -T ps_6_2 -E ps_main -spirv -fspv-target-env=vulkan1.1
                -fspv-entrypoint-name=main -fvk-use-dx-layout ${_dxc_shifts} -Fo ${_stage3_ps} ${CMAKE_SOURCE_DIR}/tests/shaders/stage3.hlsl
        DEPENDS ${CMAKE_SOURCE_DIR}/tests/shaders/stage2.hlsl ${CMAKE_SOURCE_DIR}/tests/shaders/stage3.hlsl
        COMMENT "DXC: Stage 2/3 HLSL -> SPIR-V (shifted bindings)"
        VERBATIM)
      add_custom_target(acfa-stage2-spirv DEPENDS ${_stage2_vs} ${_stage2_ps} ${_stage3_vs} ${_stage3_ps})
      add_executable(acfa-vk-draw src/runtime/gpu/vulkan/vk_draw_test.cpp)
      target_compile_definitions(acfa-vk-draw PRIVATE
        "STAGE2_VS_SPV=\"${_stage2_vs}\""
        "STAGE2_PS_SPV=\"${_stage2_ps}\"")
      target_link_libraries(acfa-vk-draw PRIVATE Vulkan::Vulkan)
      add_dependencies(acfa-vk-draw acfa-stage2-spirv)
      add_test(NAME vulkan_stage2_draw COMMAND acfa-vk-draw --validate)
      set_tests_properties(vulkan_stage2_draw PROPERTIES SKIP_RETURN_CODE 77)
      add_test(NAME vulkan_stage2_flip_sensitivity COMMAND acfa-vk-draw --no-flip)
      set_tests_properties(vulkan_stage2_flip_sensitivity PROPERTIES SKIP_RETURN_CODE 77)

      add_executable(acfa-vk-state src/runtime/gpu/vulkan/vk_state_test.cpp)
      target_compile_definitions(acfa-vk-state PRIVATE
        "STAGE3_VS_SPV=\"${_stage3_vs}\""
        "STAGE3_PS_SPV=\"${_stage3_ps}\"")
      target_link_libraries(acfa-vk-state PRIVATE Vulkan::Vulkan)
      add_dependencies(acfa-vk-state acfa-stage2-spirv)
      add_test(NAME vulkan_stage3_state COMMAND acfa-vk-state --validate)
      set_tests_properties(vulkan_stage3_state PROPERTIES SKIP_RETURN_CODE 77)
      find_program(SPIRV_DIS_EXECUTABLE NAMES spirv-dis)
      if(SPIRV_DIS_EXECUTABLE)
        add_test(NAME stage2_bindings
          COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/check-stage2-bindings.sh ${_stage2_vs} ${_stage2_ps})
        set_tests_properties(stage2_bindings PROPERTIES SKIP_RETURN_CODE 77)
      endif()
      message(STATUS "Stage 2 Vulkan draw test enabled with DXC: ${ACFA_DXC_EXECUTABLE}")
    else()
      message(STATUS "DXC not found: the Vulkan Stage 2 draw test is not built (set ACFA_DXC_EXECUTABLE)")
    endif()
  endif()
endif()

# ---- media decoder test tool (development: checks the MPEG-2 decoder on the user's movies) ----
