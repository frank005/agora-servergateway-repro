cmake_minimum_required(VERSION 3.10)
project(agora_repro CXX)

# SDK at repo root:  agora_rtc_sdk/agora_sdk (override with -DAGORA_SDK_DIR=...)
if(NOT DEFINED AGORA_SDK_DIR)
  set(AGORA_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../agora_rtc_sdk/agora_sdk")
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
