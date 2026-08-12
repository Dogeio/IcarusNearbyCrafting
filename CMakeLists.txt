cmake_minimum_required(VERSION 3.22)

project(NearbyCrafting VERSION 1.3.5 LANGUAGES CXX)

include(CTest)

set(UE4SS_RUNTIME_ROOT "" CACHE PATH "Shared UE4SS runtime root")
if(NOT UE4SS_RUNTIME_ROOT)
    message(FATAL_ERROR "UE4SS_RUNTIME_ROOT is required. Run scripts/setup-ue4ss.ps1 first.")
endif()
set(UE4SS_SOURCE_ROOT "${UE4SS_RUNTIME_ROOT}/source")
set(UE4SS_BUILD_ROOT "${UE4SS_RUNTIME_ROOT}/build")
set(UE4SS_CONFIG "Game__Shipping__Win64")
file(GLOB UE4SS_FIRST_PARTY_INCLUDE_DIRS "${UE4SS_SOURCE_ROOT}/deps/first/*/include")
if(NOT EXISTS "${UE4SS_BUILD_ROOT}/${UE4SS_CONFIG}/lib/UE4SS.lib")
    message(FATAL_ERROR "Shared UE4SS import library is missing. Run scripts/setup-ue4ss.ps1 first.")
endif()
add_library(UE4SS SHARED IMPORTED GLOBAL)
set_target_properties(UE4SS PROPERTIES
    IMPORTED_IMPLIB "${UE4SS_BUILD_ROOT}/${UE4SS_CONFIG}/lib/UE4SS.lib"
    IMPORTED_LOCATION "${UE4SS_BUILD_ROOT}/${UE4SS_CONFIG}/bin/UE4SS.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${UE4SS_SOURCE_ROOT}/UE4SS/include;${UE4SS_SOURCE_ROOT}/UE4SS/generated_include;${UE4SS_FIRST_PARTY_INCLUDE_DIRS};${UE4SS_SOURCE_ROOT}/deps/first/Unreal/generated_include;${UE4SS_SOURCE_ROOT}/deps/first/Unreal/include/Unreal;${UE4SS_SOURCE_ROOT}/deps/first/Unreal/include/Unreal/Core;${UE4SS_BUILD_ROOT}/_deps/polyhook2-src;${UE4SS_BUILD_ROOT}/_deps/polyhook2-src/asmjit/src;${UE4SS_BUILD_ROOT}/_deps/zydis-src/include;${UE4SS_BUILD_ROOT}/_deps/zydis-build;${UE4SS_BUILD_ROOT}/_deps/zydis-src/dependencies/zycore/include;${UE4SS_BUILD_ROOT}/_deps/imgui-src;${UE4SS_BUILD_ROOT}/_deps/imguitextedit-src;${UE4SS_BUILD_ROOT}/_deps/fmt-src/include;${UE4SS_BUILD_ROOT}/_deps/glaze-src/include")
link_directories("${UE4SS_BUILD_ROOT}/${UE4SS_CONFIG}/lib")

# Produce deterministic PE metadata for the loader files. /Brepro makes the PE
# timestamp content-derived, while /PDBALTPATH prevents the local PDB directory
# from being embedded in the DLL. These are deliberately applied to UE4SS and its
# proxy, not only to this mod DLL.
set(TARGET NearbyCrafting)

add_library(${TARGET} SHARED
    src/Config.cpp
    src/Features.cpp
    src/InventorySources.cpp
    src/NativeHooks.cpp
    src/NearbyCraftingMod.cpp
    src/RuntimeSafety.cpp
    src/dllmain.cpp
)

target_include_directories(${TARGET} PRIVATE include)
target_link_libraries(${TARGET} PUBLIC UE4SS Unreal DynamicOutput fmt PolyHook_2 Zydis Zycore asmjit asmtk)
target_compile_features(${TARGET} PRIVATE cxx_std_23)
target_compile_definitions(${TARGET} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UE_BUILD_SHIPPING=1
    UE_GAME
    UBT_COMPILED_PLATFORM=Win64
    OVERRIDE_PLATFORM_HEADER_NAME=Windows
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    PLATFORM_WINDOWS=1
    PLATFORM_MICROSOFT=1
    UNICODE
    _UNICODE
    NEARBYCRAFTING_VERSION="${PROJECT_VERSION}"
)
target_compile_options(${TARGET} PRIVATE /utf-8)

option(NEARBYCRAFTING_DEBUG_LOGGING
    "Compile NearbyCrafting with high-volume diagnostic console logging" OFF)
if(NEARBYCRAFTING_DEBUG_LOGGING)
    target_compile_definitions(${TARGET} PRIVATE NEARBYCRAFTING_DEBUG=1)
endif()

set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME "NearbyCrafting"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

set(NEARBYCRAFTING_PACKAGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/dist/NearbyCrafting-Release" CACHE PATH
    "Output directory for the packaged NearbyCrafting mod")
set(PACKAGE_ROOT "${NEARBYCRAFTING_PACKAGE_ROOT}")
add_custom_target(NearbyCraftingPackage
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PACKAGE_ROOT}/dlls"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PACKAGE_ROOT}/config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET}>" "${PACKAGE_ROOT}/dlls/main.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/package/Mods/NearbyCrafting/config/NearbyCrafting.ini"
        "${PACKAGE_ROOT}/config/NearbyCrafting.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/package/Mods/NearbyCrafting/enabled.txt"
        "${PACKAGE_ROOT}/enabled.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${PACKAGE_ROOT}/README.md"
    DEPENDS ${TARGET}
    VERBATIM
)

if(BUILD_TESTING)
    add_executable(NearbyCraftingConfigTests
        tests/ConfigTests.cpp
        src/Config.cpp
    )
    target_include_directories(NearbyCraftingConfigTests PRIVATE include)
    target_compile_features(NearbyCraftingConfigTests PRIVATE cxx_std_23)
    add_test(NAME NearbyCraftingConfigTests COMMAND NearbyCraftingConfigTests)
endif()
