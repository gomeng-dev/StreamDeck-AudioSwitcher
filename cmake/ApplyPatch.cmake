execute_process(
  COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH_FILE}"
  WORKING_DIRECTORY "${SOURCE_DIR}"
  RESULT_VARIABLE already_applied
  OUTPUT_QUIET
  ERROR_QUIET
)
if(already_applied EQUAL 0)
  return()
endif()

execute_process(
  COMMAND "${GIT_EXECUTABLE}" apply "${PATCH_FILE}"
  WORKING_DIRECTORY "${SOURCE_DIR}"
  RESULT_VARIABLE apply_result
  ERROR_VARIABLE apply_error
)
if(NOT apply_result EQUAL 0)
  message(FATAL_ERROR "Failed to apply ${PATCH_FILE}: ${apply_error}")
endif()
