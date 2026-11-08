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
# Release build (tools/build_release.sh): no stack audit, /debug compiled out,
# log level WARN from tools/sdkconfig.release.
if(FILEXFER_RELEASE)
  if(FILEXFER_STACK_AUDIT)
    message(FATAL_ERROR "FILEXFER_RELEASE and FILEXFER_STACK_AUDIT together: use tools/build_release.sh")
  endif()
  idf_build_set_property(COMPILE_DEFINITIONS "FILEXFER_RELEASE=1" APPEND)
endif()
project(cnc_receiver)
if(FILEXFER_RELEASE AND CONFIG_LOG_DEFAULT_LEVEL GREATER 2)
  message(FATAL_ERROR "Release build needs log level WARN or lower (tools/sdkconfig.release)")
endif()

# Signed apps (CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT, RSA-3072): the app is
# signed in place with the key at $FILEXFER_SIGNING_KEY, kept outside the repo.
# Without it the build is unsigned and a device will refuse it as an update.
if(CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT)
  if(DEFINED ENV{FILEXFER_SIGNING_KEY} AND EXISTS "$ENV{FILEXFER_SIGNING_KEY}")
    idf_build_get_property(build_dir BUILD_DIR)
    idf_build_get_property(python PYTHON)
    set(app_bin "${build_dir}/${CMAKE_PROJECT_NAME}.bin")
    add_custom_command(OUTPUT "${build_dir}/.filexfer_signed_timestamp"
      COMMAND ${python} "$ENV{IDF_PATH}/components/esptool_py/esptool/espsecure.py" sign_data "${app_bin}"
              --version 2 --keyfile "$ENV{FILEXFER_SIGNING_KEY}" # in place; --keyfile takes a list, so it goes last
      COMMAND ${CMAKE_COMMAND} -E md5sum "${app_bin}" > "${build_dir}/.filexfer_signed_timestamp"
      DEPENDS "${build_dir}/.bin_timestamp"
      VERBATIM
      COMMENT "Signing ${CMAKE_PROJECT_NAME}.bin with FILEXFER_SIGNING_KEY")
    add_custom_target(filexfer_sign_app DEPENDS "${build_dir}/.filexfer_signed_timestamp")
    add_dependencies(gen_project_binary filexfer_sign_app)
  else()
    message(WARNING "FILEXFER_SIGNING_KEY not set (or file missing): ${CMAKE_PROJECT_NAME}.bin is NOT signed; "
                    "devices reject it as an update and a USB-flashed unsigned app cannot accept updates.")
  endif()
endif()

