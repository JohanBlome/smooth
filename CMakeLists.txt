cmake_minimum_required(VERSION 3.22)
project(smooth VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(OBS_STUDIO_DIR "" CACHE PATH "Path to obs-studio source checkout")
set(OBS_BUILD_DIR "" CACHE PATH "Path to configured obs-studio build directory")

if(APPLE AND (NOT OBS_STUDIO_DIR OR NOT EXISTS "${OBS_STUDIO_DIR}/libobs/obs-module.h"))
  foreach(dir
      "/opt/homebrew/include/obs-studio"
      "/usr/local/include/obs-studio"
      "/opt/homebrew/opt/obs/include"
      "/usr/local/opt/obs/include")
    if(EXISTS "${dir}/libobs/obs-module.h")
      set(OBS_STUDIO_DIR "${dir}" CACHE PATH "" FORCE)
      message(STATUS "Found OBS Studio headers at: ${OBS_STUDIO_DIR}")
      break()
    endif()
  endforeach()
endif()

if(NOT EXISTS "${OBS_STUDIO_DIR}/libobs/obs-module.h")
  message(FATAL_ERROR
    "OBS_STUDIO_DIR must point to an obs-studio source tree containing libobs/obs-module.h. "
    "Pass -DOBS_STUDIO_DIR=/path/to/obs-studio or use scripts/build_install.sh.")
endif()

if(NOT EXISTS "${OBS_BUILD_DIR}/config/obsconfig.h" AND NOT EXISTS "${OBS_BUILD_DIR}/obsconfig.h")
  message(FATAL_ERROR
    "OBS_BUILD_DIR must point to a configured obs-studio build tree containing config/obsconfig.h. "
    "Pass -DOBS_BUILD_DIR=/path/to/obs-studio/build or use scripts/build_install.sh.")
endif()

set(YUNET_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/data/models/face_detection_yunet_2023mar.onnx")
if(NOT EXISTS "${YUNET_MODEL}")
  message(FATAL_ERROR
    "The YuNet model is missing: ${YUNET_MODEL}\n"
    "Run scripts/install_third_party.sh, then configure again.")
endif()

find_package(OpenCV 4.5.4 REQUIRED COMPONENTS core imgproc objdetect dnn)

add_library(smooth MODULE
  src/plugin.cpp
  src/face_detector.cpp
  src/face_detector.hpp
)

target_include_directories(smooth PRIVATE
  "${OBS_STUDIO_DIR}/libobs"
  "${OBS_STUDIO_DIR}/libobs/graphics"
  "${OBS_STUDIO_DIR}/libobs/util"
  ${OpenCV_INCLUDE_DIRS}
)

# OBS source builds keep SIMDe and their other public header dependencies here.
if(EXISTS "${OBS_STUDIO_DIR}/../.deps")
  file(GLOB OBS_DEPS_INCLUDE_CANDIDATES "${OBS_STUDIO_DIR}/../.deps/obs-deps*/include")
elseif(EXISTS "${OBS_STUDIO_DIR}/.deps")
  file(GLOB OBS_DEPS_INCLUDE_CANDIDATES "${OBS_STUDIO_DIR}/.deps/obs-deps*/include")
endif()
if(OBS_DEPS_INCLUDE_CANDIDATES)
  list(GET OBS_DEPS_INCLUDE_CANDIDATES 0 OBS_DEPS_INCLUDE)
  target_include_directories(smooth PRIVATE "${OBS_DEPS_INCLUDE}")
endif()

if(EXISTS "${OBS_BUILD_DIR}/config/obsconfig.h")
  target_include_directories(smooth PRIVATE "${OBS_BUILD_DIR}/config")
else()
  target_include_directories(smooth PRIVATE "${OBS_BUILD_DIR}")
endif()

set(OBS_LIB_FOUND FALSE)
if(APPLE)
  foreach(config RelWithDebInfo Release Debug)
    foreach(candidate
        "${OBS_BUILD_DIR}/libobs/${config}/libobs.framework/Versions/A/libobs"
        "${OBS_BUILD_DIR}/libobs/${config}/libobs.framework/libobs"
        "${OBS_BUILD_DIR}/libobs/${config}/libobs.a")
      if(EXISTS "${candidate}")
        target_link_libraries(smooth PRIVATE "${candidate}")
        set(OBS_LIB_FOUND TRUE)
        message(STATUS "Linking against libobs: ${candidate}")
        break()
      endif()
    endforeach()
    if(OBS_LIB_FOUND)
      break()
    endif()
  endforeach()
  if(NOT OBS_LIB_FOUND)
    foreach(candidate
        "${OBS_BUILD_DIR}/libobs/libobs.dylib"
        "${OBS_BUILD_DIR}/libobs.dylib")
      if(EXISTS "${candidate}")
        target_link_libraries(smooth PRIVATE "${candidate}")
        set(OBS_LIB_FOUND TRUE)
        message(STATUS "Linking against libobs: ${candidate}")
        break()
      endif()
    endforeach()
  endif()
else()
  foreach(candidate
      "${OBS_BUILD_DIR}/libobs/libobs.so"
      "${OBS_BUILD_DIR}/libobs/RelWithDebInfo/libobs.so"
      "${OBS_BUILD_DIR}/libobs/Release/libobs.so"
      "${OBS_BUILD_DIR}/libobs/Debug/libobs.so"
      "${OBS_BUILD_DIR}/libobs.so")
    if(EXISTS "${candidate}")
      target_link_libraries(smooth PRIVATE "${candidate}")
      set(OBS_LIB_FOUND TRUE)
      message(STATUS "Linking against libobs: ${candidate}")
      break()
    endif()
  endforeach()
endif()

if(NOT OBS_LIB_FOUND)
  message(FATAL_ERROR
    "libobs was not found under OBS_BUILD_DIR. Build it first with: "
    "cmake --build \"${OBS_BUILD_DIR}\" --target libobs")
endif()

target_link_libraries(smooth PRIVATE ${OpenCV_LIBS})
target_compile_definitions(smooth PRIVATE SMOOTH_VERSION="${PROJECT_VERSION}")

if(UNIX AND NOT APPLE)
  target_link_libraries(smooth PRIVATE pthread dl m)
elseif(APPLE)
  target_link_libraries(smooth PRIVATE pthread)
endif()

if(MSVC)
  target_compile_options(smooth PRIVATE /W4 /permissive-)
else()
  target_compile_options(smooth PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Match spectra/scopes: build a bare OBS module and let build_install.sh package it.
set_target_properties(smooth PROPERTIES OUTPUT_NAME "smooth" PREFIX "")
if(APPLE)
  set_target_properties(smooth PROPERTIES SUFFIX ".so")
endif()

# Gate the build on the same OBS-effect compatibility checks used by scopes.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  file(GLOB SMOOTH_EFFECTS "${CMAKE_CURRENT_SOURCE_DIR}/data/effects/*.effect")
  set(EFFECT_LINT_STAMP "${CMAKE_CURRENT_BINARY_DIR}/.effect-lint.stamp")
  add_custom_command(
    OUTPUT "${EFFECT_LINT_STAMP}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/lint_effect.py" ${SMOOTH_EFFECTS}
    COMMAND ${CMAKE_COMMAND} -E touch "${EFFECT_LINT_STAMP}"
    DEPENDS ${SMOOTH_EFFECTS} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/lint_effect.py"
    COMMENT "Linting OBS .effect files"
    VERBATIM
  )
  add_custom_target(lint-effects ALL DEPENDS "${EFFECT_LINT_STAMP}")
  add_dependencies(smooth lint-effects)
else()
  message(WARNING "Python3 not found; skipping OBS effect linting")
endif()

install(TARGETS smooth LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/obs-plugins")
install(DIRECTORY data/ DESTINATION "${CMAKE_INSTALL_PREFIX}/share/obs/obs-plugins/smooth")
