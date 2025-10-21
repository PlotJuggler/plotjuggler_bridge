cmake_minimum_required(VERSION 3.8)
project(pj_ros_bridge)

# Compiler settings
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Set C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add cmake module path for custom find modules
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Find ROS2 dependencies
find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rosbag2_cpp REQUIRED)
find_package(rosidl_typesupport_introspection_cpp REQUIRED)
find_package(rosidl_runtime_cpp REQUIRED)

# Find external dependencies
find_package(ZSTD REQUIRED)

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
  src/middleware/zmq_middleware.cpp
  src/topic_discovery.cpp
  src/schema_extractor.cpp
  src/message_buffer.cpp
  src/generic_subscription_manager.cpp
)

target_link_libraries(${PROJECT_NAME}_lib
  zstd::libzstd_shared
  zmq
  dl  # For dlopen/dlsym used in schema extraction
)

ament_target_dependencies(${PROJECT_NAME}_lib
  ament_index_cpp
  rclcpp
  rosbag2_cpp
  rosidl_typesupport_introspection_cpp
  rosidl_runtime_cpp
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
  find_package(ament_lint_auto REQUIRED)

  # Skip copyright and cpplint for now
  set(ament_cmake_copyright_FOUND TRUE)
  set(ament_cmake_cpplint_FOUND TRUE)

  ament_lint_auto_find_test_dependencies()

  # Add unit tests
  ament_add_gtest(${PROJECT_NAME}_tests
    tests/unit/test_middleware.cpp
    tests/unit/test_topic_discovery.cpp
    tests/unit/test_schema_extractor.cpp
    tests/unit/test_message_buffer.cpp
    tests/unit/test_generic_subscription_manager.cpp
  )

  target_link_libraries(${PROJECT_NAME}_tests
    ${PROJECT_NAME}_lib
    data_path
  )

  ament_target_dependencies(${PROJECT_NAME}_tests
    rclcpp
  )
endif()

ament_package()
