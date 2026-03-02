cmake_minimum_required(VERSION 3.14)
project(pj_bridge)

# Compiler settings
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sanitizer options
option(ENABLE_TSAN "Enable Thread Sanitizer" OFF)
if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

option(ENABLE_ASAN "Enable Address Sanitizer" OFF)
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=address)
endif()

# Custom cmake modules
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ============================================================================
# Backend detection
# ============================================================================
find_package(ament_cmake QUIET)
# RTI Connext DDS — only look for it if explicitly requested or available
option(ENABLE_RTI "Build RTI Connext DDS backend" OFF)

if(ament_cmake_FOUND)
  message(STATUS "ROS2 (ament_cmake) found — will build pj_bridge_ros2")
endif()

if(ENABLE_RTI)
  find_package(RTIConnextDDS QUIET)
  if(RTIConnextDDS_FOUND)
    message(STATUS "RTI Connext DDS found — will build pj_bridge_rti")
  else()
    message(WARNING "ENABLE_RTI=ON but RTIConnextDDS not found — skipping RTI backend")
  endif()
endif()

# ============================================================================
# Common dependencies (for app layer)
# ============================================================================
find_package(ZSTD REQUIRED)

include(FetchContent)

# IXWebSocket — try system/conda first, fall back to FetchContent
find_package(ixwebsocket QUIET)
if(NOT ixwebsocket_FOUND)
  message(STATUS "IXWebSocket not found — fetching via FetchContent")
  FetchContent_Declare(
    ixwebsocket
    URL https://github.com/machinezone/IXWebSocket/archive/refs/tags/v11.4.6.tar.gz
    URL_HASH SHA256=c024334f8e45980836c67008979a884d6dcc5ef067dd2eb1fa7241f4c17ddc32
  )
  set(USE_TLS OFF CACHE BOOL "" FORCE)
  set(USE_ZLIB ON CACHE BOOL "" FORCE)
  set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(ixwebsocket)
else()
  message(STATUS "Using system IXWebSocket")
endif()

# spdlog — available via rosdep (libspdlog-dev) or pixi (conda-forge)
find_package(spdlog REQUIRED)

# nlohmann/json — available via rosdep (nlohmann-json-dev) or pixi (conda-forge)
find_package(nlohmann_json REQUIRED)

# ============================================================================
# Data path for tests
# ============================================================================
get_filename_component(DATA_PATH "${CMAKE_CURRENT_LIST_DIR}/DATA" ABSOLUTE)
configure_file(data_path.hpp.in "${CMAKE_BINARY_DIR}/include/data_path.hpp" @ONLY)
add_library(data_path INTERFACE IMPORTED)
target_include_directories(data_path INTERFACE ${CMAKE_BINARY_DIR}/include)

# ============================================================================
# pj_bridge_app — backend-agnostic core library
# ============================================================================
add_library(pj_bridge_app STATIC
  app/src/middleware/websocket_middleware.cpp
  app/src/message_buffer.cpp
  app/src/session_manager.cpp
  app/src/message_serializer.cpp
  app/src/bridge_server.cpp
  app/src/standalone_event_loop.cpp
)

target_include_directories(pj_bridge_app PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/app/include
  ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty
)

target_link_libraries(pj_bridge_app PUBLIC
  zstd::libzstd_shared
  ixwebsocket::ixwebsocket
  spdlog::spdlog
  nlohmann_json::nlohmann_json
)

# ============================================================================
# pj_bridge_ros2 — ROS2 backend (only if ament_cmake found)
# ============================================================================
if(ament_cmake_FOUND)
  find_package(ament_index_cpp REQUIRED)
  find_package(rclcpp REQUIRED)
  find_package(sensor_msgs REQUIRED)
  find_package(nav_msgs REQUIRED)

  # ROS2 plugin library (sources shared between executable and tests)
  add_library(pj_bridge_ros2_lib STATIC
    ros2/src/topic_discovery.cpp
    ros2/src/schema_extractor.cpp
    ros2/src/generic_subscription_manager.cpp
    ros2/src/message_stripper.cpp
    ros2/src/ros2_topic_source.cpp
    ros2/src/ros2_subscription_manager.cpp
  )

  target_include_directories(pj_bridge_ros2_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ros2/include
  )

  target_link_libraries(pj_bridge_ros2_lib PUBLIC
    pj_bridge_app
  )

  ament_target_dependencies(pj_bridge_ros2_lib PUBLIC
    ament_index_cpp
    rclcpp
    sensor_msgs
    nav_msgs
  )

  # ROS2 executable
  add_executable(pj_bridge_ros2
    ros2/src/main.cpp
  )

  target_link_libraries(pj_bridge_ros2
    pj_bridge_ros2_lib
  )

  ament_target_dependencies(pj_bridge_ros2
    rclcpp
  )

  # Install ROS2 executable
  install(TARGETS pj_bridge_ros2
    DESTINATION lib/${PROJECT_NAME}
  )
endif()

# ============================================================================
# pj_bridge_rti — RTI DDS backend (disabled — replaced by FastDDS backend)
# ============================================================================
# if(ENABLE_RTI AND RTIConnextDDS_FOUND)
#   find_package(CLI11 REQUIRED)
#
#   add_library(pj_bridge_rti_lib STATIC
#     rti/src/dds_topic_discovery.cpp
#     rti/src/dds_subscription_manager.cpp)
#   target_include_directories(pj_bridge_rti_lib PUBLIC
#     ${CMAKE_CURRENT_SOURCE_DIR}/rti/include)
#   target_link_libraries(pj_bridge_rti_lib PUBLIC
#     pj_bridge_app RTIConnextDDS::cpp2_api)
#
#   add_executable(pj_bridge_rti rti/src/main.cpp)
#   target_link_libraries(pj_bridge_rti pj_bridge_rti_lib CLI11::CLI11)
#   install(TARGETS pj_bridge_rti DESTINATION lib/${PROJECT_NAME})
# endif()

# ============================================================================
# pj_bridge_fastdds — eProsima Fast DDS backend (Conan-managed)
# ============================================================================
option(ENABLE_FASTDDS "Build eProsima FastDDS backend" OFF)

if(ENABLE_FASTDDS)
  find_package(fastcdr REQUIRED)
  find_package(fastdds REQUIRED)
  find_package(CLI11 REQUIRED)

  add_library(pj_bridge_fastdds_lib STATIC
    fastdds/src/fastdds_topic_source.cpp
    fastdds/src/fastdds_subscription_manager.cpp
  )

  target_include_directories(pj_bridge_fastdds_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/fastdds/include
  )

  target_link_libraries(pj_bridge_fastdds_lib PUBLIC
    pj_bridge_app
    fastdds
    fastcdr
  )

  add_executable(pj_bridge_fastdds fastdds/src/main.cpp)
  target_link_libraries(pj_bridge_fastdds pj_bridge_fastdds_lib CLI11::CLI11)
  install(TARGETS pj_bridge_fastdds DESTINATION lib/${PROJECT_NAME})
endif()

# ============================================================================
# Install common
# ============================================================================
install(
  DIRECTORY app/include/
  DESTINATION include
)

install(
  TARGETS pj_bridge_app
  EXPORT ${PROJECT_NAME}_appTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

# Install launch files (if they exist)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/launch)
  install(
    DIRECTORY launch/
    DESTINATION share/${PROJECT_NAME}/launch
  )
endif()

# ============================================================================
# Testing
# ============================================================================
if(BUILD_TESTING AND ament_cmake_FOUND)
  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(${PROJECT_NAME}_tests
    tests/unit/test_websocket_middleware.cpp
    tests/unit/test_message_buffer.cpp
    tests/unit/test_session_manager.cpp
    tests/unit/test_message_serializer.cpp
    tests/unit/test_bridge_server.cpp
    tests/unit/test_protocol_constants.cpp
    tests/unit/test_topic_discovery.cpp
    tests/unit/test_schema_extractor.cpp
    tests/unit/test_generic_subscription_manager.cpp
    tests/unit/test_message_stripper.cpp
  )

  target_link_libraries(${PROJECT_NAME}_tests
    pj_bridge_ros2_lib
    data_path
  )

  ament_target_dependencies(${PROJECT_NAME}_tests
    rclcpp
    sensor_msgs
    nav_msgs
  )

  # Sanitizer environment settings for CTest
  if(ENABLE_TSAN)
    set_tests_properties(${PROJECT_NAME}_tests PROPERTIES
      ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan_suppressions.txt")
  endif()

  if(ENABLE_ASAN)
    set_tests_properties(${PROJECT_NAME}_tests PROPERTIES
      ENVIRONMENT "ASAN_OPTIONS=new_delete_type_mismatch=0;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/asan_suppressions.txt")
  endif()
endif()

# ============================================================================
# ament package (must come last)
# ============================================================================
if(ament_cmake_FOUND)
  ament_package()
endif()
