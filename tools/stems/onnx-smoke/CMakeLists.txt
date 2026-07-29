cmake_minimum_required(VERSION 3.21)

project(demucs-onnx-smoke LANGUAGES CXX)

list(
  APPEND
  CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/modules"
)
find_package(onnxruntime CONFIG QUIET)
if(NOT onnxruntime_FOUND)
  find_package(onnxruntime MODULE REQUIRED)
endif()
include(OnnxRuntimeTarget)
mixxx_ensure_onnxruntime_target()

add_executable(
  demucs-onnx-smoke
  ../../../src/stems/demucsonnxrunner.cpp
  ../../../src/stems/stemchunkpipeline.cpp
  main.cpp
)
add_executable(
  demucs-onnx-benchmark
  ../../../src/stems/demucsonnxrunner.cpp
  benchmark.cpp
)
target_compile_features(demucs-onnx-smoke PRIVATE cxx_std_20)
target_compile_features(demucs-onnx-benchmark PRIVATE cxx_std_20)
target_include_directories(demucs-onnx-smoke PRIVATE ../../../src)
target_include_directories(demucs-onnx-benchmark PRIVATE ../../../src)
target_link_libraries(demucs-onnx-smoke PRIVATE onnxruntime::onnxruntime)
target_link_libraries(demucs-onnx-benchmark PRIVATE onnxruntime::onnxruntime)

if(WIN32)
  target_link_libraries(demucs-onnx-benchmark PRIVATE psapi)
  add_custom_command(
    TARGET demucs-onnx-smoke
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:onnxruntime::onnxruntime>
      $<TARGET_FILE_DIR:demucs-onnx-smoke>
  )
  add_custom_command(
    TARGET demucs-onnx-benchmark
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:onnxruntime::onnxruntime>
      $<TARGET_FILE_DIR:demucs-onnx-benchmark>
  )
endif()
