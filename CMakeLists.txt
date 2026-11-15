cmake_minimum_required(VERSION 3.16)
project(ASTRAL VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Statically link the MSVC runtime. The cheat DLL is manually mapped, so it
# must not depend on vcruntime/ucrt API-set imports (those cannot be resolved
# reliably in the target process).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(EXTERNAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external)

# ============================================================
# Download MinHook if not present
# ============================================================
if(NOT EXISTS ${EXTERNAL_DIR}/minhook/include/MinHook.h)
    message(STATUS "[ASTRAL] Cloning MinHook...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env git clone --depth 1 https://github.com/mmozeiko/MinHook.git ${EXTERNAL_DIR}/minhook
        RESULT_VARIABLE MH_RESULT
    )
    if(NOT MH_RESULT EQUAL 0)
        execute_process(
            COMMAND git clone --depth 1 https://github.com/TsudaKagehi/MinHook.git ${EXTERNAL_DIR}/minhook
        )
    endif()
endif()

# ============================================================
# Download ImGui if not present
# ============================================================
if(NOT EXISTS ${EXTERNAL_DIR}/imgui/imgui.cpp)
    message(STATUS "[ASTRAL] Cloning Dear ImGui...")
    execute_process(
        COMMAND git clone --depth 1 https://github.com/ocornut/imgui.git ${EXTERNAL_DIR}/imgui
    )
endif()

# ============================================================
# MinHook Library
# ============================================================
add_library(minhook STATIC
    ${EXTERNAL_DIR}/minhook/src/hook.c
    ${EXTERNAL_DIR}/minhook/src/buffer.c
    ${EXTERNAL_DIR}/minhook/src/trampoline.c
    ${EXTERNAL_DIR}/minhook/src/hde/hde64.c
)
set_target_properties(minhook PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)
target_include_directories(minhook PUBLIC ${EXTERNAL_DIR}/minhook/include)

# ============================================================
# ImGui Library
# ============================================================
add_library(imgui STATIC
    ${EXTERNAL_DIR}/imgui/imgui.cpp
    ${EXTERNAL_DIR}/imgui/imgui_draw.cpp
    ${EXTERNAL_DIR}/imgui/imgui_widgets.cpp
    ${EXTERNAL_DIR}/imgui/imgui_tables.cpp
    ${EXTERNAL_DIR}/imgui/imgui_demo.cpp
    ${EXTERNAL_DIR}/imgui/backends/imgui_impl_dx11.cpp
    ${EXTERNAL_DIR}/imgui/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC
    ${EXTERNAL_DIR}/imgui
    ${EXTERNAL_DIR}/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(imgui PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)
target_link_libraries(imgui PUBLIC d3d11 dxgi winmm)

# ============================================================
# CS2Internal DLL (the cheat module)
# ============================================================
file(GLOB_RECURSE CS2_SOURCES
    "CS2Internal/core/*.cpp"
    "CS2Internal/features/*.cpp"
    "CS2Internal/ui/*.cpp"
)
add_library(CS2Internal SHARED ${CS2_SOURCES})
target_compile_definitions(CS2Internal PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(CS2Internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/CS2Internal
    ${EXTERNAL_DIR}/imgui
    ${EXTERNAL_DIR}/imgui/backends
)

target_link_options(CS2Internal PRIVATE /ENTRY:DllMain /IGNORE:4216)

target_link_libraries(CS2Internal PRIVATE
    kernel32.lib
    user32.lib
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
    shlwapi.lib
    minhook
    imgui
    libvcruntime.lib
    libucrt.lib
)

target_compile_options(CS2Internal PRIVATE /EHa /GS-)

set_target_properties(CS2Internal PROPERTIES
    OUTPUT_NAME "CS2Internal"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ============================================================
# ASTRAL Loader (injector executable)
# ============================================================
file(GLOB_RECURSE LOADER_SOURCES
    "CS2Internal/loader/*.cpp"
)

add_executable(ASTRAL_Loader ${LOADER_SOURCES})
target_include_directories(ASTRAL_Loader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/CS2Internal
)

target_link_libraries(ASTRAL_Loader PRIVATE
    d3d11
    d3dcompiler
    dxgi
    winhttp
    ws2_32
)

target_compile_options(ASTRAL_Loader PRIVATE /EHa)

set_target_properties(ASTRAL_Loader PROPERTIES
    OUTPUT_NAME "ASTRAL_Loader"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ---- temporary manual-map validation harness (deleted after validation) ----
add_executable(mmap_self selftest/mmap_self.cpp)
set_target_properties(mmap_self PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
