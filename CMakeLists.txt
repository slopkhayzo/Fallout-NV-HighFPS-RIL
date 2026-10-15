cmake_minimum_required(VERSION 3.20)
project(FalloutNVHFR LANGUAGES CXX)

# FalloutNVHFR.asi: an ASI plugin for an ASI loader. 32-bit only.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "FalloutNVHFR is 32-bit; configure with the x86 toolchain.")
endif()

function(falloutnvhfr_settings target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    set_property(TARGET ${target} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endfunction()

add_library(FalloutNVHFR SHARED
    src/FalloutNVHFR.cpp
)
falloutnvhfr_settings(FalloutNVHFR)
set_target_properties(FalloutNVHFR PROPERTIES
    OUTPUT_NAME FalloutNVHFR
    PREFIX ""
    SUFFIX ".asi"
)

add_executable(asi_smoke tests/asi_smoke.cpp)
falloutnvhfr_settings(asi_smoke)

# Includes the plugin source and runs its per-draw classifiers on synthetic
# values; installs nothing.
add_executable(classifier_test tests/classifier_test.cpp)
falloutnvhfr_settings(classifier_test)
