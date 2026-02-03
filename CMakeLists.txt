cmake_minimum_required(VERSION 3.8)
project(pj_ros_bridge)

# Compiler settings
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Set C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Thread Sanitizer option
option(ENABLE_TSAN "Enable Thread Sanitizer" OFF)
if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

# Address Sanitizer option
option(ENABLE_ASAN "Enable Address Sanitizer" OFF)
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=address)
endif()

# Add cmake module path for custom find modules
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Find ROS2 dependencies
find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)

# Find external dependencies
find_package(ZSTD REQUIRED)

# Try to find ixwebsocket (Conan or system install), fall back to FetchContent
find_package(ixwebsocket QUIET)
if(NOT ixwebsocket_FOUND)
  message(STATUS "ixwebsocket not found, fetching from GitHub...")
  include(FetchContent)
  FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v11.4.6
  )
  # Configure ixwebsocket options
  set(USE_TLS OFF CACHE BOOL "" FORCE)
  set(USE_ZLIB ON CACHE BOOL "" FORCE)
  set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(ixwebsocket)
endif()

# Include directories
include_directories(
  include
  ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty
)

##################################################################################

# ----  Trick to pass an absolute path to C++ code  ----
get_filename_component(DATA_PATH "${CMAKE_CURRENT_LIST_DIR}/DATA" ABSOLUTE)
configure_file(data_path.hpp.in "${CMAKE_BINARY_DIR}/include/data_path.hpp" @ONLY)
add_library(data_path INTERFACE IMPORTED)
target_include_directories(data_path INTERFACE ${CMAKE_BINARY_DIR}/include)

##################################################################################

# Create library
add_library(${PROJECT_NAME}_lib
  src/middleware/websocket_middleware.cpp
  src/topic_discovery.cpp
  src/schema_extractor.cpp
  src/message_buffer.cpp
  src/generic_subscription_manager.cpp
  src/session_manager.cpp
  src/message_serializer.cpp
  src/bridge_server.cpp
)

target_link_libraries(${PROJECT_NAME}_lib
  zstd::libzstd_shared
  ixwebsocket::ixwebsocket
)

ament_target_dependencies(${PROJECT_NAME}_lib
  ament_index_cpp
  rclcpp
)

# Create executable
add_executable(${PROJECT_NAME}_node
  src/main.cpp
)

target_link_libraries(${PROJECT_NAME}_node
  ${PROJECT_NAME}_lib
)

ament_target_dependencies(${PROJECT_NAME}_node
  rclcpp
)

# Install executable
install(TARGETS ${PROJECT_NAME}_node
  DESTINATION lib/${PROJECT_NAME}
)

# Install launch files
install(
  DIRECTORY launch/
  DESTINATION share/${PROJECT_NAME}/launch
)

# Install headers
install(
  DIRECTORY include/
  DESTINATION include
)

# Install library
install(
  TARGETS ${PROJECT_NAME}_lib
  EXPORT ${PROJECT_NAME}_libTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

# Testing
if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)

  # Add unit tests
  ament_add_gtest(${PROJECT_NAME}_tests
    tests/unit/test_websocket_middleware.cpp
    tests/unit/test_topic_discovery.cpp
    tests/unit/test_schema_extractor.cpp
    tests/unit/test_message_buffer.cpp
    tests/unit/test_generic_subscription_manager.cpp
    tests/unit/test_session_manager.cpp
    tests/unit/test_message_serializer.cpp
    tests/unit/test_bridge_server.cpp
    tests/unit/test_protocol_constants.cpp
  )

  target_link_libraries(${PROJECT_NAME}_tests
    ${PROJECT_NAME}_lib
    data_path
  )

  ament_target_dependencies(${PROJECT_NAME}_tests
    rclcpp
  )

  # When TSAN is enabled, suppress known third-party warnings during CTest runs
  if(ENABLE_TSAN)
    set_tests_properties(${PROJECT_NAME}_tests PROPERTIES
      ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan_suppressions.txt")
  endif()

  # When ASAN is enabled, suppress known third-party issues during CTest runs
  if(ENABLE_ASAN)
    set_tests_properties(${PROJECT_NAME}_tests PROPERTIES
      ENVIRONMENT "ASAN_OPTIONS=new_delete_type_mismatch=0;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/asan_suppressions.txt")
  endif()
endif()

ament_package()
