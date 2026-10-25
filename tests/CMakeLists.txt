# --- Warnings applied only to this project's own test targets (not forced
# onto downstream consumers of the header). ---

add_library(cbor_warnings INTERFACE)
target_compile_options(cbor_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU>:
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wold-style-cast -Wcast-qual -Wnon-virtual-dtor -Woverloaded-virtual
        -Wnull-dereference -Wdouble-promotion -Wuseless-cast -Wlogical-op
        -Wduplicated-cond -Wduplicated-branches
        -Werror>
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wold-style-cast -Wcast-qual -Wnon-virtual-dtor -Woverloaded-virtual
        -Wnull-dereference -Wdouble-promotion
        -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# --- Positive tests: one file per major type / concern ---

set(positive_test_sources
    test_decode_length.cpp
    test_uint.cpp
    test_negint.cpp
    test_bytestring.cpp
    test_textstring.cpp
    test_simple.cpp
    test_array.cpp
    test_map.cpp
    test_tag.cpp
    test_indefinite_length.cpp
    test_nesting_depth.cpp
    test_malformed_input.cpp
)

foreach(src ${positive_test_sources})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE constexpr_cbor compile_time_ut cbor_warnings)
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# noexcept_tests.cpp: proves the decoder/encoder surface compiles under
# -fno-exceptions -fno-rtti (several embedded toolchains, ESP-IDF's default
# among them, build this way). Compile-only, like the sibling library's own
# convention: standalone (no compile_time_ut dependency), so it matches
# exactly what the bare-metal docker images build.
add_library(noexcept_tests OBJECT noexcept_tests.cpp)
target_link_libraries(noexcept_tests PRIVATE constexpr_cbor cbor_warnings)
target_compile_options(noexcept_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-exceptions -fno-rtti>)

# --- Expected-compile-failure test helpers (copied from compile_time_ut's
# own tests/CMakeLists.txt convention) ---

function(add_compile_failure_test name source)
    cmake_parse_arguments(ACFT "" "" "LABELS;DEFINES" ${ARGN})
    add_library(${name} OBJECT EXCLUDE_FROM_ALL ${source})
    target_link_libraries(${name} PRIVATE constexpr_cbor compile_time_ut cbor_warnings)
    if(ACFT_DEFINES)
        target_compile_definitions(${name} PRIVATE ${ACFT_DEFINES})
    endif()
    add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND}
            -DOBJ_DIR=${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${name}.dir
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=${name}
            -DCONFIG=$<CONFIG>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/try_compile.cmake)
    set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
    if(ACFT_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${ACFT_LABELS}")
    endif()
endfunction()

function(add_compile_failure_test_with_values name source)
    cmake_parse_arguments(ACFTV "" "" "LABELS;DEFINES;EXPECT" ${ARGN})
    add_library(${name} OBJECT EXCLUDE_FROM_ALL ${source})
    target_link_libraries(${name} PRIVATE constexpr_cbor compile_time_ut cbor_warnings)
    if(ACFTV_DEFINES)
        target_compile_definitions(${name} PRIVATE ${ACFTV_DEFINES})
    endif()
    add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND}
            -DOBJ_DIR=${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${name}.dir
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=${name}
            -DCONFIG=$<CONFIG>
            "-DEXPECT_SUBSTRINGS=${ACFTV_EXPECT}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/try_compile_grep.cmake)
    if(ACFTV_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${ACFTV_LABELS}")
    endif()
endfunction()

add_compile_failure_test_with_values(wrong_shape_utf8_policy
    expected_failures/wrong_shape_utf8_policy.cpp LABELS policy
    EXPECT "Utf8Policy must provide static bool is_valid")

add_compile_failure_test_with_values(wrong_shape_duplicate_key_policy
    expected_failures/wrong_shape_duplicate_key_policy.cpp LABELS policy
    EXPECT "KeyPolicy must provide static bool on_duplicate")

add_compile_failure_test(expect_eq_wrong_decoded_value
    expected_failures/expect_eq_wrong_decoded_value.cpp LABELS sanity)
