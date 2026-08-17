include(FetchContent)
find_package(Git REQUIRED)

FetchContent_Declare(
  AudioDeviceLib
  GIT_REPOSITORY https://github.com/fredemmott/AudioDeviceLib
  GIT_TAG 8cef359dd7920659a24aa1aabf9539fe2dffb01b
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  PATCH_COMMAND
    "${CMAKE_COMMAND}"
    "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    "-DSOURCE_DIR=<SOURCE_DIR>"
    "-DPATCH_FILE=${CMAKE_CURRENT_LIST_DIR}/cmake/AudioDeviceLib-Windows-reliability.patch"
    -P "${CMAKE_CURRENT_LIST_DIR}/cmake/ApplyPatch.cmake"
)

FetchContent_GetProperties(AudioDeviceLib)
if(NOT audiodevicelib_POPULATED)
  FetchContent_Populate(AudioDeviceLib)
  add_subdirectory("${audiodevicelib_SOURCE_DIR}" "${audiodevicelib_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()
