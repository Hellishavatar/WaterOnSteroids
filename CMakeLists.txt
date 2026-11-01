cmake_minimum_required(VERSION 3.21)
project(WaterOnSteroids VERSION 1.5.0 LANGUAGES CXX)

set(PLUGIN_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
if(PLUGIN_VERSION_PATCH STREQUAL "")
    set(PLUGIN_VERSION_PATCH 0)
endif()

set(PLUGIN_AUTHOR "Hellishavatar")
set(PLUGIN_COPYRIGHT "Copyright © 2026 ${PLUGIN_AUTHOR}.")
set(PLUGIN_DESCRIPTION "Water On Steroids for Skyrim SE and AE.")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Keep local paths out of the binary. Source paths (__FILE__, std::source_location, as used by the
# log macros) are rewritten relative to the project, and the build directory, the vcpkg packages
# and anything else under the user profile go with them. Release builds produce no PDB; where one
# is produced, only its file name is embedded in the DLL.
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" WOS_NATIVE_SOURCE_DIR)
set(WOS_PATH_MAPS "${WOS_NATIVE_SOURCE_DIR}=${PROJECT_NAME}")
if(NOT CMAKE_SOURCE_DIR STREQUAL WOS_NATIVE_SOURCE_DIR)
    list(APPEND WOS_PATH_MAPS "${CMAKE_SOURCE_DIR}=${PROJECT_NAME}")
endif()
if(DEFINED ENV{USERPROFILE})
    file(TO_NATIVE_PATH "$ENV{USERPROFILE}" WOS_NATIVE_USERPROFILE)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" WOS_CMAKE_USERPROFILE)
    list(APPEND WOS_PATH_MAPS "${WOS_NATIVE_USERPROFILE}=user" "${WOS_CMAKE_USERPROFILE}=user")
endif()
list(REMOVE_DUPLICATES WOS_PATH_MAPS)

if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/experimental:deterministic)
    foreach(map IN LISTS WOS_PATH_MAPS)
        add_compile_options("/pathmap:${map}")
    endforeach()
elseif(MSVC)
    foreach(map IN LISTS WOS_PATH_MAPS)
        add_compile_options("/clang:-ffile-prefix-map=${map}")
    endforeach()
else()
    foreach(map IN LISTS WOS_PATH_MAPS)
        add_compile_options("-ffile-prefix-map=${map}")
    endforeach()
endif()

if(MSVC)
    add_link_options(
        "$<$<CONFIG:Release>:/DEBUG:NONE>"
        /PDBALTPATH:%_PDB%
    )
endif()

set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "Enable Skyrim SE" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "Enable Skyrim AE" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "Disable Skyrim VR" FORCE)
# CommonLib's prebuilt bundle is compiled for SE, AE, and VR together. Force its source
# build so the runtime macros in this plugin are limited to SE and AE.
set(COMMONLIB_PREBUILT OFF CACHE BOOL "Disable all-runtime CommonLib prebuilt" FORCE)
set(COMMONLIB_PREBUILT_DIR "" CACHE PATH "Disable all-runtime CommonLib prebuilt" FORCE)
set(COMMONLIB_PREBUILT_MULTICONFIG OFF CACHE BOOL "Disable all-runtime CommonLib prebuilt" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)

include(FetchContent)
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()
set(EXTERN_DIR "${CMAKE_SOURCE_DIR}/extern")
file(MAKE_DIRECTORY "${EXTERN_DIR}")

function(declare_dependency name folder repository tag)
    set(source_dir "${EXTERN_DIR}/${folder}")
    set(binary_dir "${CMAKE_BINARY_DIR}/${folder}")
    if(EXISTS "${source_dir}/CMakeLists.txt")
        message(STATUS "WaterOnSteroids: using local ${name} from ${source_dir}")
        FetchContent_Declare(${name}
            SOURCE_DIR "${source_dir}"
            BINARY_DIR "${binary_dir}"
            ${ARGN})
    else()
        message(STATUS "WaterOnSteroids: downloading ${name} ${tag}")
        FetchContent_Declare(${name}
            GIT_REPOSITORY "${repository}"
            GIT_TAG "${tag}"
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
            SOURCE_DIR "${source_dir}"
            BINARY_DIR "${binary_dir}"
            ${ARGN})
    endif()
endfunction()

declare_dependency(
    spdlog
    spdlog
    https://github.com/gabime/spdlog.git
    v1.16.0
    OVERRIDE_FIND_PACKAGE)
declare_dependency(CommonLibSSE CommonLibSSE-NG https://github.com/alandtse/CommonLibSSE-NG.git v13.1.0)

find_package(fmt CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h" REQUIRED)
find_library(DETOURS_LIBRARY NAMES detours REQUIRED)
FetchContent_MakeAvailable(spdlog)
set(_saved_skip "${CMAKE_SKIP_INSTALL_RULES}")
set(CMAKE_SKIP_INSTALL_RULES TRUE)
FetchContent_MakeAvailable(CommonLibSSE)
set(CMAKE_SKIP_INSTALL_RULES "${_saved_skip}")
unset(_saved_skip)

if(NOT TARGET CommonLibSSE::CommonLibSSE)
    message(FATAL_ERROR "CommonLibSSE-NG target was not created")
endif()
if(NOT COMMAND add_commonlibsse_plugin)
    include("${commonlibsse_SOURCE_DIR}/cmake/CommonLibSSE.cmake")
endif()
if(NOT COMMAND add_commonlibsse_plugin)
    message(FATAL_ERROR "add_commonlibsse_plugin() is unavailable")
endif()

# Dear ImGui has no CMake project; its sources are compiled into the plugin below. Pinned to the
# master commit the plugin is built and tested with (it reports itself as 1.93.0 WIP).
set(IMGUI_DIR "${EXTERN_DIR}/imgui")
set(WOS_IMGUI_COMMIT ea6d21687bec144dd7aee0f4db37f7c61a8799bb)
if(EXISTS "${IMGUI_DIR}/imgui.h")
    message(STATUS "WaterOnSteroids: using local imgui from ${IMGUI_DIR}")
else()
    message(STATUS "WaterOnSteroids: downloading imgui ${WOS_IMGUI_COMMIT}")
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(imgui
        URL "https://github.com/ocornut/imgui/archive/${WOS_IMGUI_COMMIT}.zip"
        URL_HASH SHA256=2d4bd69f6ed51011f8d070c2ab5df4c8ed7b580bef9c537c70111439187a7189
        SOURCE_DIR "${IMGUI_DIR}"
        BINARY_DIR "${CMAKE_BINARY_DIR}/imgui")
    FetchContent_MakeAvailable(imgui)
endif()

set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp")

set_source_files_properties(${IMGUI_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

set(SOURCES
    "${CMAKE_SOURCE_DIR}/src/CellWaterMap.cpp"
    "${CMAKE_SOURCE_DIR}/src/CellWaterMap.h"
    "${CMAKE_SOURCE_DIR}/src/LodWaterSplit.cpp"
    "${CMAKE_SOURCE_DIR}/src/LodWaterSplit.h"
    "${CMAKE_SOURCE_DIR}/src/Plugin.cpp"
    "${CMAKE_SOURCE_DIR}/src/RenderPassFinder.h"
    "${CMAKE_SOURCE_DIR}/src/Settings.h"
    "${CMAKE_SOURCE_DIR}/src/WaterPatcher.cpp"
    "${CMAKE_SOURCE_DIR}/src/WaterPatcher.h"
    "${CMAKE_SOURCE_DIR}/src/Menu.cpp"
    "${CMAKE_SOURCE_DIR}/src/Menu.h"
    "${CMAKE_SOURCE_DIR}/src/TypeMaterial.cpp"
    "${CMAKE_SOURCE_DIR}/src/TypeMaterial.h"
    "${CMAKE_SOURCE_DIR}/src/WaterShader.cpp"
    "${CMAKE_SOURCE_DIR}/src/WaterShader.h"
    "${CMAKE_SOURCE_DIR}/src/WaterWatch.cpp"
    "${CMAKE_SOURCE_DIR}/src/WaterWatch.h"
    "${CMAKE_SOURCE_DIR}/include/EditorID.h"
    "${CMAKE_SOURCE_DIR}/include/PCH.h"
    ${IMGUI_SOURCES})

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "WaterOnSteroids"
    AUTHOR "${PLUGIN_AUTHOR}"
    VERSION ${PROJECT_VERSION}
    USE_ADDRESS_LIBRARY
    SOURCES ${SOURCES})

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/Version.h"
    @ONLY)
target_sources(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    "${CMAKE_CURRENT_BINARY_DIR}/include/Version.h")
target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/include"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    ${DETOURS_INCLUDE_DIRS})
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE
    spdlog::spdlog
    fmt::fmt
    ZLIB::ZLIB
    ${DETOURS_LIBRARY}
    d3d11
    dxgi)

set(WOS_PACKED_INI
    "${CMAKE_SOURCE_DIR}/packed/SKSE/Plugins/WaterOnSteroids.ini")
set(WOS_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(WOS_EXCEPTIONS "${CMAKE_SOURCE_DIR}/EXCEPTIONS.md")
set(WOS_NOTICE "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt")
set(WOS_DIST_ROOT "${CMAKE_SOURCE_DIR}/dist/WaterOnSteroids")
set(WOS_DIST_PLUGIN_DIR "${WOS_DIST_ROOT}/SKSE/Plugins")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WOS_DIST_PLUGIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PROJECT_NAME}>"
        "${WOS_DIST_PLUGIN_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WOS_PACKED_INI}"
        "${WOS_DIST_PLUGIN_DIR}/WaterOnSteroids.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WOS_LICENSE}"
        "${WOS_DIST_PLUGIN_DIR}/WaterOnSteroids-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WOS_EXCEPTIONS}"
        "${WOS_DIST_PLUGIN_DIR}/WaterOnSteroids-EXCEPTIONS.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WOS_NOTICE}"
        "${WOS_DIST_PLUGIN_DIR}/WaterOnSteroids-THIRD_PARTY_NOTICES.txt"
    VERBATIM)

