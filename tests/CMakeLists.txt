find_package(doctest 2.5 REQUIRED CONFIG)

add_executable(dvv_media_tests
    main.cpp
    source_harness.cpp
    store_fakes.cpp
    test_clip_json.cpp
    test_colour_preset.cpp
    test_display.cpp
    test_display_store.cpp
    test_file_name.cpp
    test_format_json.cpp
    test_frame_store.cpp
    test_gpu_unpack.cpp
    test_layout_goldens.cpp
    test_original_read.cpp
    test_pixel_format.cpp
    test_planner.cpp
    test_provider_conformance.cpp
    test_raw_source.cpp
    test_reference.cpp
    test_synthetic_source.cpp
    test_to_display.cpp
    test_types.cpp
    test_uri_colour.cpp
    test_uri_params.cpp
    test_y4m_source.cpp)
target_link_libraries(dvv_media_tests PRIVATE dvv::media doctest::doctest)
# The real source providers, for the conformance case (whole archive: nothing references them).
dvv_link_components(dvv_media_tests dvv::media_sources)
# The unit tests also check private parts (src/), e.g. the format table and the pattern generators.
target_include_directories(dvv_media_tests PRIVATE ${dvv_media_SOURCE_DIR}/src)
# doctest otherwise only forward-declares std::ostream, which breaks printing std types.
target_compile_definitions(dvv_media_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
dvv_set_compiler_options(dvv_media_tests)
add_test(NAME dvv_media.unit COMMAND dvv_media_tests)
# The timeout turns a hang into a failure long before the CI job limit.
set_tests_properties(dvv_media.unit PROPERTIES
    TIMEOUT 300
    ENVIRONMENT DVV_LEAK_CHECK=fail
    ENVIRONMENT_MODIFICATION "${DVV_TEST_ENVIRONMENT}")
# Cycle labels: `sanity` marks the fast inner-loop subset (ctest --preset vs2026-sanity); `package`
# marks install and consumer tests, which only the full cycle runs (vs2026-small excludes them).
set_property(TEST dvv_media.unit APPEND PROPERTY LABELS sanity)

# The first creation of the unpacked formats races only in a fresh process: the unit tests create
# them all earlier, so this case has its own executable.
add_executable(dvv_media_race_tests
    main.cpp
    test_unpacked_race.cpp)
target_link_libraries(dvv_media_race_tests PRIVATE dvv::media doctest::doctest)
target_compile_definitions(dvv_media_race_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
dvv_set_compiler_options(dvv_media_race_tests)
add_test(NAME dvv_media.unpacked_race COMMAND dvv_media_race_tests)
set_tests_properties(dvv_media.unpacked_race PROPERTIES
    TIMEOUT 300
    ENVIRONMENT DVV_LEAK_CHECK=fail
    ENVIRONMENT_MODIFICATION "${DVV_TEST_ENVIRONMENT}")

# The source factory's decision rules, driven by three scripted fake providers. This executable
# does not link dvv_media_sources, so the real providers never take part in the decisions.
add_executable(dvv_media_factory_tests
    main.cpp
    fake_providers.cpp
    test_source_factory.cpp)
target_link_libraries(dvv_media_factory_tests PRIVATE dvv::media doctest::doctest)
target_compile_definitions(dvv_media_factory_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
dvv_set_compiler_options(dvv_media_factory_tests)
add_test(NAME dvv_media.factory COMMAND dvv_media_factory_tests)
set_tests_properties(dvv_media.factory PROPERTIES
    TIMEOUT 300
    ENVIRONMENT DVV_LEAK_CHECK=fail
    ENVIRONMENT_MODIFICATION "${DVV_TEST_ENVIRONMENT}")

# GPU tests (spec §12): a separate executable, CTest label gpu. It exits with 77 (reported as
# skipped) when no usable Vulkan 1.4 device exists, unless DVV_REQUIRE_GPU=1. Every test checks at
# its end that no validation message was counted. The loader is pointed at the validation layer of
# the third-party prefix (VK_ADD_LAYER_PATH); a system's layer may be too old for Vulkan 1.4.
find_file(DVV_VK_VALIDATION_LAYER_MANIFEST VkLayer_khronos_validation.json
    PATHS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES bin share/vulkan/explicit_layer.d NO_DEFAULT_PATH)
set(gpu_test_environment DVV_LEAK_CHECK=fail)
if(DVV_VK_VALIDATION_LAYER_MANIFEST)
    cmake_path(GET DVV_VK_VALIDATION_LAYER_MANIFEST PARENT_PATH layer_dir)
    list(APPEND gpu_test_environment "VK_ADD_LAYER_PATH=${layer_dir}")
endif()
# The software driver is not instrumented; see tsan.supp.
if(DVV_SANITIZE STREQUAL "thread")
    list(APPEND gpu_test_environment
         "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp")
endif()
add_executable(dvv_media_gpu_tests
    gpu_main.cpp gpu_fixture.cpp gpu_test_store.cpp gpu_test_to_display.cpp gpu_test_unpack.cpp)
target_link_libraries(dvv_media_gpu_tests PRIVATE dvv::media doctest::doctest)
target_include_directories(dvv_media_gpu_tests PRIVATE ${dvv_media_SOURCE_DIR}/src)
target_compile_definitions(dvv_media_gpu_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
dvv_set_compiler_options(dvv_media_gpu_tests)
add_test(NAME dvv_media.gpu COMMAND dvv_media_gpu_tests)
set_tests_properties(dvv_media.gpu PROPERTIES
    LABELS gpu
    SKIP_RETURN_CODE 77
    TIMEOUT 600
    ENVIRONMENT "${gpu_test_environment}"
    ENVIRONMENT_MODIFICATION "${DVV_TEST_ENVIRONMENT}")
set_property(TEST dvv_media.gpu APPEND PROPERTY LABELS sanity)
# Package test (spec §3.1): install, then build a consumer with find_package(dvv_media) and another
# with dvv_media in-tree. The whole build tree is installed (CMAKE_BINARY_DIR), so the prefix also
# holds dvv_base, dvv_registry, dvv_platform and dvv_gpu, which dvv_dependencies() always adds as
# subprojects. Skipped for sanitizer builds, whose consumers would need the same runtime flags.
if(NOT DVV_SANITIZE)
    string(REPLACE ";" "," deps_prefix_path "${CMAKE_PREFIX_PATH}")
    add_test(NAME dvv_media.package
             COMMAND ${CMAKE_COMMAND}
                     -DBUILD_DIR=${CMAKE_BINARY_DIR}
                     -DCONFIG=$<CONFIG>
                     -DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/package-prefix
                     -DSOURCE_DIR=${dvv_media_SOURCE_DIR}
                     -DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/package
                     -DCONSUMER_BUILD=${CMAKE_CURRENT_BINARY_DIR}/package-consumer
                     -DCONSUMER_TREE_BUILD=${CMAKE_CURRENT_BINARY_DIR}/pkg-tree
                     -DGENERATOR=${CMAKE_GENERATOR}
                     -DPLATFORM=${CMAKE_GENERATOR_PLATFORM}
                     -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
                     -DDEPS_PREFIX_PATH=${deps_prefix_path}
                     -DDVV_BUILD_CMAKE_DIR=${DVV_BUILD_CMAKE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/package/run-package-test.cmake)
    # Package and compile-fail tests of every dvv repository drive the same build tree in a
    # dependent build with DVV_TEST_DEPENDENCIES, so they share one lock name.
    set_tests_properties(dvv_media.package PROPERTIES
        RESOURCE_LOCK dvv_build_tree
        LABELS package)
endif()

# Compile-fail tests (spec §9.4), run by dvv-base's shared runner. Case 0 is the control and must
# build; cases 1-5 must fail to compile with the expected diagnostic: a frame store and a host
# frame constructed without a key, a store key forged outside the store, a source key forged
# without an OpenTicket, and an OpenTicket made outside the source factory.
set(compile_fail_source ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/direct_construction.cpp)
add_executable(dvv_media_compile_ok ${compile_fail_source})
target_compile_definitions(dvv_media_compile_ok PRIVATE DVV_COMPILE_FAIL_CASE=0)
target_link_libraries(dvv_media_compile_ok PRIVATE dvv::media)
dvv_set_compiler_options(dvv_media_compile_ok)
if(MSVC)
    set(compile_fail_patterns "C2665: 'dvv::media::FrameStore::FrameStore'"
        "C2661: 'dvv::media::HostFrame::HostFrame'" "C2248" "C2248" "C2248")
else()
    set(compile_fail_patterns
        "no matching constructor for initialization of 'dvv::media::FrameStore'"
        "no matching constructor for initialization of 'dvv::media::HostFrame'" "private"
        "private constructor" "private constructor")
endif()
foreach(case IN ITEMS 1 2 3 4 5)
    set(target dvv_media_compile_fail_${case})
    add_executable(${target} EXCLUDE_FROM_ALL ${compile_fail_source})
    target_compile_definitions(${target} PRIVATE DVV_COMPILE_FAIL_CASE=${case})
    target_link_libraries(${target} PRIVATE dvv::media)
    dvv_set_compiler_options(${target})
    math(EXPR pattern_index "${case} - 1")
    list(GET compile_fail_patterns ${pattern_index} pattern)
    # cmake --build needs the top-level tree; in a dependent build (another dvv library or the app)
    # dvv-media is a subproject.
    add_test(NAME dvv_media.compile_fail.${case}
             COMMAND ${CMAKE_COMMAND}
                     -DBUILD_DIR=${CMAKE_BINARY_DIR}
                     -DCONFIG=$<CONFIG>
                     -DTARGET=${target}
                     "-DPATTERN=${pattern}"
                     -P ${DVV_CMAKE_DIR}/run-compile-fail.cmake)
    set_tests_properties(dvv_media.compile_fail.${case} PROPERTIES RESOURCE_LOCK dvv_build_tree)
endforeach()
