cmake_minimum_required(VERSION 3.20)
project(pypraat LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG REQUIRED)

set(PRAAT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/praat)
if(NOT EXISTS ${PRAAT_DIR}/Makefile)
  message(FATAL_ERROR "Praat submodule is missing. Run: git submodule update --init praat")
endif()

set(PRAAT_SOURCES_CMAKE ${CMAKE_CURRENT_BINARY_DIR}/praat_sources.cmake)
execute_process(
  COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/praat_sources.py ${PRAAT_DIR}
  OUTPUT_FILE ${PRAAT_SOURCES_CMAKE}
  COMMAND_ERROR_IS_FATAL ANY
)
include(${PRAAT_SOURCES_CMAKE})

# The Linux/Windows Makefile does not name the CoreAudio host. The Mac
# application is built from Xcode, which compiles these files instead of the
# ALSA/JACK host. They are the Mac counterparts of pa_unix_hostapis.c.
if(APPLE)
  list(APPEND PRAAT_GROUP_external_portaudio_SOURCES
    ${PRAAT_DIR}/external/portaudio/pa_mac_hostapis.c
    ${PRAAT_DIR}/external/portaudio/pa_mac_core.c
    ${PRAAT_DIR}/external/portaudio/pa_mac_core_utilities.c
    ${PRAAT_DIR}/external/portaudio/pa_mac_core_blocking.c
  )
endif()

set(PRAAT_OBJECTS)
foreach(group ${PRAAT_GROUPS})
  set(target praat_${group})
  add_library(${target} OBJECT ${PRAAT_GROUP_${group}_SOURCES})
  target_include_directories(${target} PRIVATE ${PRAAT_GROUP_${group}_INCLUDES})
  # macintosh selects the Cocoa sources Praat actually maintains.
# Batch init never opens a window; NO_GRAPHICS would take dead Carbon branches.
target_compile_definitions(${target} PRIVATE
    macintosh
  )
  target_compile_options(${target} PRIVATE
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/praat_prefix.h"
  )
  target_compile_options(${target} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-deprecated-declarations
    -Wno-missing-field-initializers
  )
  set_source_files_properties(${PRAAT_GROUP_${group}_SOURCES} PROPERTIES LANGUAGE OBJCXX)
  # C sources must stay C. OBJCXX above is then overridden per C file.
  foreach(source ${PRAAT_GROUP_${group}_SOURCES})
    if(source MATCHES "\\.c$")
      set_source_files_properties(${source} PROPERTIES LANGUAGE C)
    endif()
  endforeach()
  list(APPEND PRAAT_OBJECTS $<TARGET_OBJECTS:${target}>)
endforeach()

add_library(praat STATIC ${PRAAT_OBJECTS} src/gestalt_stub.c)
set_target_properties(praat PROPERTIES LINKER_LANGUAGE CXX)
if(APPLE)
  target_link_libraries(praat PUBLIC
    "-framework Cocoa"
    "-framework CoreFoundation"
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework AudioUnit"
    "-framework Accelerate"
    "-framework Metal"
    "-framework MetalPerformanceShaders"
    "-framework OpenCL"
    "-framework CoreGraphics"
    "-framework CoreText"
    "-framework QuartzCore"
  )
endif()

python_add_library(_pypraat MODULE src/module.cpp WITH_SOABI)
set_source_files_properties(src/module.cpp PROPERTIES LANGUAGE OBJCXX)
target_link_libraries(_pypraat PRIVATE pybind11::headers praat)
target_include_directories(_pypraat PRIVATE
  ${PRAAT_DIR}/sys
  ${PRAAT_DIR}/melder
  ${PRAAT_DIR}/kar
  ${PRAAT_DIR}/dwsys
  ${PRAAT_DIR}/fon
  ${PRAAT_DIR}/stat
  ${PRAAT_DIR}/main
  ${PRAAT_DIR}/external/gsl
  ${PRAAT_DIR}/dwtools
)
target_compile_definitions(_pypraat PRIVATE macintosh)
target_compile_features(_pypraat PRIVATE cxx_std_17)

install(TARGETS _pypraat DESTINATION pypraat)
