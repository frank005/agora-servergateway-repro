cmake_minimum_required(VERSION 3.10)
project(agora_repro CXX)

# SDK at repo root. Support both layouts:
# - agora_rtc_sdk/agora_sdk/include/IAgoraService.h
# - agora_rtc_sdk/include/IAgoraService.h
if(NOT DEFINED AGORA_SDK_DIR)
  set(_SDK_DIR1 "${CMAKE_CURRENT_SOURCE_DIR}/../agora_rtc_sdk/agora_sdk")
  set(_SDK_DIR2 "${CMAKE_CURRENT_SOURCE_DIR}/../agora_rtc_sdk")
  if(EXISTS "${_SDK_DIR1}/include/IAgoraService.h")
    set(AGORA_SDK_DIR "${_SDK_DIR1}")
  elseif(EXISTS "${_SDK_DIR2}/include/IAgoraService.h")
    set(AGORA_SDK_DIR "${_SDK_DIR2}")
  else()
    message(FATAL_ERROR "Could not find IAgoraService.h under ../agora_rtc_sdk. "
                        "Expected either ../agora_rtc_sdk/agora_sdk/include or ../agora_rtc_sdk/include.")
  endif()
endif()

add_executable(repro_pthread_init repro_pthread_init.cpp)
target_include_directories(repro_pthread_init PRIVATE "${AGORA_SDK_DIR}/include")
target_compile_definitions(repro_pthread_init PRIVATE TARGET_OS_LINUX)
target_link_directories(repro_pthread_init PRIVATE "${AGORA_SDK_DIR}")
target_link_libraries(repro_pthread_init PRIVATE
  agora_rtc_sdk
  pthread
  dl
  rt
  m
)
set_target_properties(repro_pthread_init PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  BUILD_RPATH "${AGORA_SDK_DIR}"
  INSTALL_RPATH "\$ORIGIN"
)
