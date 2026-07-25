# Find the ONNX Runtime C++ library from an official binary distribution.
#
# Defines:
#   onnxruntime_FOUND
#   onnxruntime::onnxruntime
#
# Optional hint:
#   ONNXRUNTIME_ROOT or the ONNXRUNTIME_ROOT environment variable

find_path(
  onnxruntime_INCLUDE_DIR
  NAMES onnxruntime_cxx_api.h
  HINTS "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
  PATH_SUFFIXES include
)
find_library(
  onnxruntime_LIBRARY
  NAMES onnxruntime
  HINTS "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
  PATH_SUFFIXES lib
)

if(WIN32)
  find_file(
    onnxruntime_RUNTIME_LIBRARY
    NAMES onnxruntime.dll
    HINTS "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
    PATH_SUFFIXES bin lib
  )
endif()

include(FindPackageHandleStandardArgs)
set(_onnxruntime_required_vars onnxruntime_INCLUDE_DIR onnxruntime_LIBRARY)
if(WIN32)
  list(APPEND _onnxruntime_required_vars onnxruntime_RUNTIME_LIBRARY)
endif()
find_package_handle_standard_args(
  onnxruntime
  REQUIRED_VARS ${_onnxruntime_required_vars}
)
unset(_onnxruntime_required_vars)

if(onnxruntime_FOUND AND NOT TARGET onnxruntime::onnxruntime)
  if(WIN32)
    add_library(onnxruntime::onnxruntime SHARED IMPORTED)
    set_target_properties(
      onnxruntime::onnxruntime
      PROPERTIES
        IMPORTED_IMPLIB "${onnxruntime_LIBRARY}"
        IMPORTED_LOCATION "${onnxruntime_RUNTIME_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_INCLUDE_DIR}"
    )
  else()
    add_library(onnxruntime::onnxruntime UNKNOWN IMPORTED)
    set_target_properties(
      onnxruntime::onnxruntime
      PROPERTIES
        IMPORTED_LOCATION "${onnxruntime_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(
  onnxruntime_INCLUDE_DIR
  onnxruntime_LIBRARY
  onnxruntime_RUNTIME_LIBRARY
)
