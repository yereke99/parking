cmake_minimum_required(VERSION 3.10)

project(kz_anpr VERSION 1.0.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

option(KZ_ANPR_BUILD_RUNTIME "Build the OpenCV-based ANPR runtime" ON)
option(KZ_ANPR_BUILD_TESTS "Build deterministic unit tests" ON)
option(KZ_ANPR_WITH_ONNXRUNTIME "Enable the ONNX Runtime inference backend" ON)
option(KZ_ANPR_WITH_TENSORRT "Enable the native TensorRT inference backend when available" ON)
option(KZ_ANPR_REQUIRE_TENSORRT "Fail configuration when native TensorRT is unavailable" OFF)
option(KZ_ANPR_SANITIZERS "Enable address and undefined-behaviour sanitizers" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Warnings are applied only to this project's own targets so third-party headers stay quiet.
if(MSVC)
    set(KZ_ANPR_WARNINGS /W4 /permissive-)
else()
    set(KZ_ANPR_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
endif()

function(kz_anpr_configure_target target)
    target_compile_options(${target} PRIVATE ${KZ_ANPR_WARNINGS})
    if(KZ_ANPR_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.13)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        else()
            set_property(TARGET ${target} APPEND_STRING PROPERTY
                LINK_FLAGS " -fsanitize=address,undefined")
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Core: configuration, validation, tracking, state machine, consensus.
# No OpenCV and no inference runtime, so it builds and is tested anywhere.
# ---------------------------------------------------------------------------
add_library(kz_anpr_core
    src/common/config.cpp
    src/common/logging.cpp
    src/common/metrics.cpp
    src/common/yaml.cpp
    src/pipeline/plate_consensus.cpp
    src/pipeline/recognition_event.cpp
    src/pipeline/vehicle_state_machine.cpp
    src/tracking/plate_tracker.cpp
    src/tracking/stop_detector.cpp
    src/validation/plate_validator.cpp
)
target_include_directories(kz_anpr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(kz_anpr_core PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    # JetPack 4.x ships GCC 7. std::filesystem lives in a separate compatibility library there.
    target_link_libraries(kz_anpr_core PUBLIC stdc++fs)
endif()
kz_anpr_configure_target(kz_anpr_core)

# ---------------------------------------------------------------------------
# Runtime: camera, inference backends, detector, OCR, pipeline.
# ---------------------------------------------------------------------------
if(KZ_ANPR_BUILD_RUNTIME)
    find_package(OpenCV QUIET COMPONENTS core imgproc videoio dnn imgcodecs)
    if(NOT OpenCV_FOUND)
        message(WARNING
            "OpenCV C++ development package not found. The runtime targets are skipped. "
            "Install libopencv-dev (Debian/JetPack) or the Homebrew opencv formula, or configure "
            "with -DKZ_ANPR_BUILD_RUNTIME=OFF to build only the core library and tests.")
    else()
        set(KZ_ANPR_RUNTIME_SOURCES
            src/camera/camera_source.cpp
            src/camera/frame_pump.cpp
            src/detection/plate_detector.cpp
            src/inference/inference_session.cpp
            src/inference/opencv_dnn_session.cpp
            src/ocr/fast_plate_ocr.cpp
            src/ocr/nomeroff_ocr.cpp
            src/ocr/plate_ocr.cpp
            src/ocr/research_ocr.cpp
            src/pipeline/anpr_pipeline.cpp
            src/pipeline/motion_detector.cpp
            src/pipeline/quality_assessor.cpp
        )

        set(KZ_ANPR_ONNXRUNTIME_FOUND OFF)
        if(KZ_ANPR_WITH_ONNXRUNTIME)
            find_package(onnxruntime QUIET CONFIG)
            if(onnxruntime_FOUND)
                set(KZ_ANPR_ONNXRUNTIME_FOUND ON)
            else()
                # JetPack and the upstream tarballs install headers and the shared library
                # without a CMake package config, so fall back to a direct search.
                find_path(ONNXRUNTIME_INCLUDE_DIR
                    NAMES onnxruntime_cxx_api.h
                    PATH_SUFFIXES onnxruntime onnxruntime/core/session include/onnxruntime
                    HINTS ${ONNXRUNTIME_ROOT} /usr/include /usr/local/include /opt/onnxruntime/include)
                find_library(ONNXRUNTIME_LIBRARY
                    NAMES onnxruntime
                    HINTS ${ONNXRUNTIME_ROOT} /usr/lib /usr/local/lib /opt/onnxruntime/lib
                    PATH_SUFFIXES aarch64-linux-gnu x86_64-linux-gnu)
                if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
                    set(KZ_ANPR_ONNXRUNTIME_FOUND ON)
                endif()
            endif()
        endif()

        if(KZ_ANPR_ONNXRUNTIME_FOUND)
            list(APPEND KZ_ANPR_RUNTIME_SOURCES src/inference/onnxruntime_session.cpp)
        else()
            message(WARNING
                "ONNX Runtime not found. Building without the ONNX Runtime backends. "
                "Native TensorRT may still be enabled below. "
                "The default Nomeroff OCR worker remains usable; the legacy Fast Plate OCR "
                "comparison backend will refuse to start because OpenCV DNN cannot supply its "
                "uint8 input tensor.")
        endif()

        set(KZ_ANPR_TENSORRT_FOUND OFF)
        if(KZ_ANPR_WITH_TENSORRT)
            find_path(TENSORRT_INCLUDE_DIR NvInfer.h
                HINTS /usr/include/aarch64-linux-gnu /usr/include /usr/local/include)
            find_library(TENSORRT_NVINFER_LIBRARY nvinfer
                HINTS /usr/lib/aarch64-linux-gnu /usr/lib /usr/local/lib)
            find_library(TENSORRT_ONNX_PARSER_LIBRARY nvonnxparser
                HINTS /usr/lib/aarch64-linux-gnu /usr/lib /usr/local/lib)
            find_path(CUDA_RUNTIME_INCLUDE_DIR cuda_runtime_api.h
                HINTS /usr/local/cuda/include)
            find_library(CUDA_RUNTIME_LIBRARY cudart
                HINTS /usr/local/cuda/lib64 /usr/local/cuda/targets/aarch64-linux/lib)
            if(TENSORRT_INCLUDE_DIR AND TENSORRT_NVINFER_LIBRARY AND
               TENSORRT_ONNX_PARSER_LIBRARY AND CUDA_RUNTIME_INCLUDE_DIR AND
               CUDA_RUNTIME_LIBRARY)
                set(KZ_ANPR_TENSORRT_FOUND ON)
                list(APPEND KZ_ANPR_RUNTIME_SOURCES src/inference/tensorrt_session.cpp)
            elseif(KZ_ANPR_REQUIRE_TENSORRT)
                message(FATAL_ERROR
                    "Native TensorRT/CUDA development libraries were not found (NvInfer.h, "
                    "nvonnxparser, nvinfer, cudart). On JetPack 4 the NVIDIA container runtime "
                    "mounts them from the host: build with Docker's default runtime set to nvidia "
                    "on a Jetson with JetPack's CUDA and TensorRT components installed.")
            else()
                message(STATUS "Native TensorRT not found; the portable build will omit it.")
            endif()
        endif()

        add_library(kz_anpr_runtime ${KZ_ANPR_RUNTIME_SOURCES})
        target_include_directories(kz_anpr_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(kz_anpr_runtime PUBLIC kz_anpr_core Threads::Threads ${OpenCV_LIBS})
        target_include_directories(kz_anpr_runtime SYSTEM PUBLIC ${OpenCV_INCLUDE_DIRS})
        kz_anpr_configure_target(kz_anpr_runtime)

        # Optional: the debug window. Headless server builds of OpenCV ship without highgui, and
        # the runtime must still build there.
        find_package(OpenCV QUIET COMPONENTS highgui)
        if(TARGET opencv_highgui)
            target_compile_definitions(kz_anpr_runtime PUBLIC KZ_ANPR_WITH_HIGHGUI=1)
            target_link_libraries(kz_anpr_runtime PUBLIC opencv_highgui)
        else()
            message(STATUS "OpenCV highgui not found; --visualize will be disabled at runtime.")
        endif()

        if(KZ_ANPR_ONNXRUNTIME_FOUND)
            target_compile_definitions(kz_anpr_runtime PUBLIC KZ_ANPR_WITH_ONNXRUNTIME=1)
            if(onnxruntime_FOUND)
                target_link_libraries(kz_anpr_runtime PUBLIC onnxruntime::onnxruntime)
            else()
                target_include_directories(kz_anpr_runtime SYSTEM PUBLIC ${ONNXRUNTIME_INCLUDE_DIR})
                target_link_libraries(kz_anpr_runtime PUBLIC ${ONNXRUNTIME_LIBRARY})
            endif()
        endif()
        if(KZ_ANPR_TENSORRT_FOUND)
            target_compile_definitions(kz_anpr_runtime PUBLIC KZ_ANPR_WITH_TENSORRT=1)
            target_include_directories(kz_anpr_runtime SYSTEM PUBLIC
                ${TENSORRT_INCLUDE_DIR} ${CUDA_RUNTIME_INCLUDE_DIR})
            target_link_libraries(kz_anpr_runtime PUBLIC
                ${TENSORRT_NVINFER_LIBRARY} ${TENSORRT_ONNX_PARSER_LIBRARY}
                ${CUDA_RUNTIME_LIBRARY})
        endif()

        add_executable(kz_anpr src/main.cpp)
        target_link_libraries(kz_anpr PRIVATE kz_anpr_runtime)
        kz_anpr_configure_target(kz_anpr)

        add_executable(kz_anpr_benchmark src/benchmark_main.cpp)
        target_link_libraries(kz_anpr_benchmark PRIVATE kz_anpr_runtime)
        kz_anpr_configure_target(kz_anpr_benchmark)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(KZ_ANPR_BUILD_TESTS)
    enable_testing()

    add_executable(kz_anpr_core_tests
        tests/test_main.cpp
        tests/test_config.cpp
        tests/test_consensus.cpp
        tests/test_state_machine.cpp
        tests/test_tracking.cpp
        tests/test_validation.cpp
        tests/test_yaml.cpp
    )
    target_link_libraries(kz_anpr_core_tests PRIVATE kz_anpr_core)
    target_include_directories(kz_anpr_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    kz_anpr_configure_target(kz_anpr_core_tests)
    add_test(NAME kz_anpr_core_tests COMMAND kz_anpr_core_tests)
    set_tests_properties(kz_anpr_core_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    if(TARGET kz_anpr_runtime)
        add_executable(kz_anpr_runtime_tests
            tests/test_main.cpp
            tests/test_ocr_decoding.cpp
            tests/test_quality.cpp
        )
        target_link_libraries(kz_anpr_runtime_tests PRIVATE kz_anpr_runtime)
        target_include_directories(kz_anpr_runtime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        kz_anpr_configure_target(kz_anpr_runtime_tests)
        add_test(NAME kz_anpr_runtime_tests COMMAND kz_anpr_runtime_tests)
        set_tests_properties(kz_anpr_runtime_tests PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()
