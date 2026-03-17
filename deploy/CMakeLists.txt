cmake_minimum_required(VERSION 3.10)
project(agora_repro CXX)

# SDK at repo root: auto-detect layout by locating IAgoraService.h anywhere
# under ../agora_rtc_sdk and deriving AGORA_SDK_DIR from its parent "include"
# directory. Supports:
# - agora_rtc_sdk/agora_sdk/include/IAgoraService.h
# - agora_rtc_sdk/include/IAgoraService.h
# - and similar layouts where the header is in an "include" dir.
if(NOT DEFINED AGORA_SDK_DIR)
  file(GLOB_RECURSE _IA_HDR
    "${CMAKE_CURRENT_SOURCE_DIR}/../agora_rtc_sdk/**/IAgoraService.h")
  list(LENGTH _IA_HDR _IA_COUNT)
  if(_IA_COUNT EQUAL 0)
    message(FATAL_ERROR
      "Could not find IAgoraService.h anywhere under ../agora_rtc_sdk.\n"
      "Ensure your Agora SDK is unpacked into ../agora_rtc_sdk with an include/IAgoraService.h.")
  endif()
  list(GET _IA_HDR 0 _IA_HDR_PATH)
  # _IA_HDR_PATH is .../include/IAgoraService.h -> go up to include/, then up one to SDK root.
  get_filename_component(_IA_INCLUDE_DIR "${_IA_HDR_PATH}" DIRECTORY)
  get_filename_component(AGORA_SDK_DIR "${_IA_INCLUDE_DIR}" DIRECTORY)
endif()

add_executable(repro_pthread_init repro_pthread_init.cpp)
target_include_directories(repro_pthread_init PRIVATE
  "${AGORA_SDK_DIR}/include"
  "${AGORA_SDK_DIR}/include/c"
  "${AGORA_SDK_DIR}/include/c/base"
)
target_compile_definitions(repro_pthread_init PRIVATE TARGET_OS_LINUX)
target_link_directories(repro_pthread_init PRIVATE "${AGORA_SDK_DIR}")
target_link_libraries(repro_pthread_init PRIVATE pthread dl)
set_target_properties(repro_pthread_init PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  BUILD_RPATH "${AGORA_SDK_DIR}"
  INSTALL_RPATH "\$ORIGIN"
)
