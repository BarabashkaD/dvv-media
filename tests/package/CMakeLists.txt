# Consumer of dvv_media; built by run-package-test.cmake. With DVV_MEDIA_SOURCE_DIR it takes
# dvv_media in-tree through FetchContent (as a dependent library or the app takes it; then
# DVV_BUILD_CMAKE_DIR names dvv-build's cmake folder), otherwise the installed package.
cmake_minimum_required(VERSION 4.2)
project(dvv_media_package_consumer LANGUAGES CXX)

if(DVV_MEDIA_SOURCE_DIR)
    include(FetchContent)
    FetchContent_Declare(dvv_media SOURCE_DIR "${DVV_MEDIA_SOURCE_DIR}")
    FetchContent_MakeAvailable(dvv_media)
else()
    find_package(dvv_media 0.1 REQUIRED)
endif()
if(NOT DVV_CMAKE_DIR)
    message(FATAL_ERROR "dvv_base did not define DVV_CMAKE_DIR")
endif()
include("${DVV_CMAKE_DIR}/DvvCompilerOptions.cmake")

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE dvv::media)
# The built-in source providers (a component library: whole archive, so the registrations survive).
dvv_link_components(consumer dvv::media_sources)
dvv_set_compiler_options(consumer)
