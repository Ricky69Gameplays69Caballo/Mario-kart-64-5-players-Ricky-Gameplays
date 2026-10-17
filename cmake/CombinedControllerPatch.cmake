# Keep the upstream submodule revision while applying this fork's controller changes.
find_package(Git REQUIRED)
set(COMBINED_CONTROLLER_PATCH "${CMAKE_CURRENT_LIST_DIR}/../patches/libultraship-combined.patch")
set(COMBINED_CONTROLLER_SOURCE "${CMAKE_SOURCE_DIR}/libultraship")
if(NOT EXISTS "${COMBINED_CONTROLLER_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR "Initialize dependencies with: git submodule update --init --recursive")
endif()
execute_process(
  COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${COMBINED_CONTROLLER_PATCH}"
  WORKING_DIRECTORY "${COMBINED_CONTROLLER_SOURCE}"
  RESULT_VARIABLE COMBINED_PATCH_ALREADY_APPLIED
  OUTPUT_QUIET ERROR_QUIET)
if(NOT COMBINED_PATCH_ALREADY_APPLIED EQUAL 0)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --check "${COMBINED_CONTROLLER_PATCH}"
    WORKING_DIRECTORY "${COMBINED_CONTROLLER_SOURCE}"
    RESULT_VARIABLE COMBINED_PATCH_CHECK
    ERROR_VARIABLE COMBINED_PATCH_ERROR)
  if(NOT COMBINED_PATCH_CHECK EQUAL 0)
    message(FATAL_ERROR "Cannot apply Combined controller patch: ${COMBINED_PATCH_ERROR}")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply "${COMBINED_CONTROLLER_PATCH}"
    WORKING_DIRECTORY "${COMBINED_CONTROLLER_SOURCE}"
    RESULT_VARIABLE COMBINED_PATCH_RESULT
    ERROR_VARIABLE COMBINED_PATCH_ERROR)
  if(NOT COMBINED_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Combined controller patch failed: ${COMBINED_PATCH_ERROR}")
  endif()
  message(STATUS "Applied Combined expanded-controller patch")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${COMBINED_CONTROLLER_PATCH}")
