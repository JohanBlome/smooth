if(NOT DEFINED MODULE OR NOT EXISTS "${MODULE}")
  message(FATAL_ERROR "MODULE must name the built Smooth plugin")
endif()
if(NOT DEFINED OUTPUT_DIR)
  message(FATAL_ERROR "OUTPUT_DIR is required")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(GET_RUNTIME_DEPENDENCIES
  LIBRARIES "${MODULE}"
  RESOLVED_DEPENDENCIES_VAR resolved_dependencies
  UNRESOLVED_DEPENDENCIES_VAR unresolved_dependencies
)

set(copied 0)
foreach(dependency IN LISTS resolved_dependencies)
  get_filename_component(name "${dependency}" NAME)
  if(name MATCHES "^libopencv_.*\\.dylib$")
    file(COPY "${dependency}" DESTINATION "${OUTPUT_DIR}" FOLLOW_SYMLINK_CHAIN)
    math(EXPR copied "${copied} + 1")
  endif()
endforeach()

if(copied EQUAL 0)
  message(FATAL_ERROR
    "No OpenCV runtime libraries were resolved for ${MODULE}. "
    "Check that OpenCV_DIR refers to a complete C++ SDK.")
endif()

message(STATUS "Bundled ${copied} OpenCV runtime libraries")
