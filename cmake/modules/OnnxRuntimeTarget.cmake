include_guard(GLOBAL)

# ONNX Runtime 1.23.x does not propagate its installed public-header directory
# from the exported target when it is built as a static library. Normalize the
# target here so consumers work with both vcpkg and official binary packages.
function(mixxx_ensure_onnxruntime_target)
  if(NOT TARGET onnxruntime::onnxruntime)
    message(
      FATAL_ERROR
      "ONNX Runtime was found without an onnxruntime::onnxruntime target"
    )
  endif()

  get_target_property(
    _onnxruntime_interface_includes
    onnxruntime::onnxruntime
    INTERFACE_INCLUDE_DIRECTORIES
  )
  if(_onnxruntime_interface_includes)
    return()
  endif()

  find_path(
    _onnxruntime_public_include_dir
    NAMES onnxruntime_cxx_api.h
    HINTS "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
    PATH_SUFFIXES include include/onnxruntime onnxruntime
  )
  if(NOT _onnxruntime_public_include_dir)
    message(
      FATAL_ERROR
      "ONNX Runtime was found, but its C++ public headers were not found"
    )
  endif()

  set_property(
    TARGET onnxruntime::onnxruntime
    APPEND
    PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_onnxruntime_public_include_dir}"
  )
endfunction()
