cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../components")
# PROJECT_VER from the nearest tag (v1.6.1-3-gabc1234 -> 1.6.1-3-gabc1234)
execute_process(COMMAND git describe --tags
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE GIT_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE GIT_DESCRIBE_RESULT ERROR_QUIET)
if(GIT_DESCRIBE_RESULT EQUAL 0 AND GIT_DESCRIBE)
  string(REGEX REPLACE "^v" "" PROJECT_VER "${GIT_DESCRIBE}")
else()
  set(PROJECT_VER "0.0.0-dev")
endif()
# Re-run cmake (new version string) after each commit or checkout
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../.git/logs/HEAD")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../.git/logs/HEAD")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cnc_transmitter)
