cmake_minimum_required(VERSION 4.2)
project(dvv_media VERSION 0.1.0 DESCRIPTION "dvv media layer" LANGUAGES CXX)
# Version string; a pre-release build adds its release-stage label (user decision U-M4ORDER).
set(dvv_media_VERSION_STRING "${PROJECT_VERSION}")

# dvv-build provides the shared CMake modules and the dependency check (dvv-deps.txt); the dvv
# presets set DVV_BUILD_CMAKE_DIR to its cmake folder.
if(NOT DVV_BUILD_CMAKE_DIR OR NOT IS_ABSOLUTE "${DVV_BUILD_CMAKE_DIR}"
   OR NOT EXISTS "${DVV_BUILD_CMAKE_DIR}/DvvDeps.cmake")
    message(FATAL_ERROR "dvv_media: DVV_BUILD_CMAKE_DIR is not set, not absolute or "
                        "has no DvvDeps.cmake (it must be dvv-build's cmake folder); configure "
                        "with a dvv-build preset (e.g. `cmake --preset vs2026`, see README.md) "
                        "or pass -DDVV_BUILD_CMAKE_DIR=<absolute path of dvv-build>/cmake")
endif()
include("${DVV_BUILD_CMAKE_DIR}/DvvDeps.cmake")
# Adds dvv-base, dvv-registry, dvv-platform and dvv-gpu (dvv-deps.txt) and checks their versions;
# a stand-alone build tests only this repository.
dvv_dependencies()

option(DVV_BUILD_TESTS "Build the dvv tests" ${PROJECT_IS_TOP_LEVEL})

# dvv-base sets DVV_CMAKE_DIR to the shared modules (in-tree: dvv-build's cmake folder).
if(NOT DVV_CMAKE_DIR)
    message(FATAL_ERROR "dvv_base did not define DVV_CMAKE_DIR")
endif()
include("${DVV_CMAKE_DIR}/DvvCompilerOptions.cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(dvv_media STATIC
    src/catalogue.cpp
    src/clip_json.cpp
    src/colour_preset.cpp
    src/converters.cpp
    src/display.cpp
    src/file_name.cpp
    src/format_json.cpp
    src/frame_store.cpp
    src/frames.cpp
    src/gpu_unpack.cpp
    src/patterns.cpp
    src/pixel_format.cpp
    src/planner.cpp
    src/reference.cpp
    src/source_factory.cpp
    src/sources/uri_params.cpp
    src/to_display.cpp
    src/types.cpp
    src/uri_colour.cpp)
add_library(dvv::media ALIAS dvv_media)
set_target_properties(dvv_media PROPERTIES EXPORT_NAME media)
target_include_directories(dvv_media PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dvv_media PUBLIC dvv::base dvv::registry dvv::platform dvv::gpu)
# src/sources/uri_params.h: shared by the sources and parseFileName() (file_name.h).
target_include_directories(dvv_media PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
dvv_set_compiler_options(dvv_media)
# The GPU unpack shader (M2 design §5.4) and the ToDisplay shader (M3 design D3-D5), embedded as
# dvv_media_shaders::unpack() and dvv_media_shaders::toDisplay().
dvv_add_shaders(dvv_media NAMESPACE dvv_media_shaders
    SHADERS shaders/unpack.slang shaders/to_display.slang)

# Built-in source providers (D1): a component library. Consumers link it only with
# dvv_link_components(<target> dvv::media_sources), so the DVV_REGISTER lines survive linking.
add_library(dvv_media_sources STATIC
    src/sources/file_read.cpp
    src/sources/raw_file.cpp
    src/sources/synthetic.cpp
    src/sources/y4m.cpp)
add_library(dvv::media_sources ALIAS dvv_media_sources)
set_target_properties(dvv_media_sources PROPERTIES EXPORT_NAME media_sources)
target_link_libraries(dvv_media_sources PUBLIC dvv::media)
target_include_directories(dvv_media_sources PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
dvv_set_compiler_options(dvv_media_sources)

install(TARGETS dvv_media EXPORT dvv_mediaTargets)
install(TARGETS dvv_media_sources EXPORT dvv_mediaTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
set(config_dir ${CMAKE_INSTALL_LIBDIR}/cmake/dvv_media)
install(EXPORT dvv_mediaTargets NAMESPACE dvv:: DESTINATION ${config_dir})
configure_package_config_file(cmake/dvv_mediaConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/dvv_mediaConfig.cmake INSTALL_DESTINATION ${config_dir})
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/dvv_mediaConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/dvv_mediaConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/dvv_mediaConfigVersion.cmake
    DESTINATION ${config_dir})

if(DVV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
