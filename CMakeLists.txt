cmake_minimum_required(VERSION 3.14)
project(pj_bridge)

# Single source of truth for the server version: package.xml. Exposed to the
# code as PJ_BRIDGE_VERSION (the `server.version` field in get_topics).
file(STRINGS package.xml _pj_bridge_version_line REGEX "<version>[^<]+</version>" LIMIT_COUNT 1)
string(REGEX REPLACE ".*<version>([^<]+)</version>.*" "\\1" PJ_BRIDGE_VERSION "${_pj_bridge_version_line}")
if(NOT PJ_BRIDGE_VERSION MATCHES "^[0-9]")
  set(PJ_BRIDGE_VERSION "unknown")
endif()
add_compile_definitions(PJ_BRIDGE_VERSION="${PJ_BRIDGE_VERSION}")

# Compiler settings
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(CMAKE_CXX_STANDARD 20)
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
option(PJ_BRIDGE_TLS "Build IXWebSocket with TLS (OpenSSL) support" ON)

find_package(ixwebsocket QUIET)
if(NOT ixwebsocket_FOUND)
  message(STATUS "IXWebSocket not found — fetching via FetchContent")
  FetchContent_Declare(
    ixwebsocket
    URL https://github.com/machinezone/IXWebSocket/archive/refs/tags/v12.0.1.tar.gz
    URL_HASH SHA256=d23bdc91dbfe2b9ae13c322d539392d7a6b8b506560f41c90e227fa0f86a2405
  )
  set(USE_TLS ${PJ_BRIDGE_TLS} CACHE BOOL "" FORCE)
  if(PJ_BRIDGE_TLS)
    set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
  endif()
  set(USE_ZLIB ON CACHE BOOL "" FORCE)
  set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(ixwebsocket)
  if(PJ_BRIDGE_TLS)
    # The static library does not export its OpenSSL dependency to consumers.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(ixwebsocket INTERFACE OpenSSL::SSL OpenSSL::Crypto)
  endif()
else()
  message(STATUS "Using system IXWebSocket")
endif()

# Cloudini (optional) — point cloud compression transform
option(PJ_BRIDGE_FETCH_CLOUDINI "Fetch Cloudini when cloudini_lib is not installed" ON)
find_package(cloudini_lib QUIET)
if(NOT cloudini_lib_FOUND AND PJ_BRIDGE_FETCH_CLOUDINI)
  message(STATUS "cloudini_lib not found — fetching via FetchContent")
  FetchContent_Declare(
    cloudini
    URL https://github.com/facontidavide/cloudini/archive/refs/tags/1.3.0.tar.gz
    URL_HASH SHA256=26c9322d5a08694b9b24bbec975b7814b05cf7d4962327f35266b38bb15b5e66
  )
  set(CLOUDINI_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(CLOUDINI_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  # Use the zstd found above instead of a second, vendored copy in the same binary.
  # FindZSTD already defined zstd::libzstd_static; Cloudini's own
  # find_package(zstd CONFIG) would clash with those targets, so it is disabled.
  set(CLOUDINI_FORCE_VENDORED_DEPS OFF CACHE BOOL "" FORCE)
  set(CMAKE_DISABLE_FIND_PACKAGE_zstd ON)
  # Cloudini becomes a SHARED ament package (install rules, ament_package()) when
  # it finds ament_cmake. Hide it while its directory is processed: we want the
  # plain static library, and EXCLUDE_FROM_ALL keeps its install rules out of ours.
  set(_pj_build_testing ${BUILD_TESTING})
  set(BUILD_TESTING OFF)
  set(CMAKE_DISABLE_FIND_PACKAGE_ament_cmake ON)
  set(_pj_ament_cmake_found ${ament_cmake_FOUND})
  set(ament_cmake_FOUND FALSE)  # the disabled find_package() leaves our earlier result in place
  set(_pj_build_shared_libs ${BUILD_SHARED_LIBS})
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_GetProperties(cloudini)
  if(NOT cloudini_POPULATED)
    if(POLICY CMP0169)
      cmake_policy(SET CMP0169 OLD)  # FetchContent_Populate: needed for add_subdirectory(EXCLUDE_FROM_ALL) before CMake 3.28
    endif()
    FetchContent_Populate(cloudini)
    add_subdirectory(${cloudini_SOURCE_DIR}/cloudini_lib ${cloudini_BINARY_DIR} EXCLUDE_FROM_ALL)
  endif()
  set(BUILD_SHARED_LIBS ${_pj_build_shared_libs})
  set(ament_cmake_FOUND ${_pj_ament_cmake_found})
  set(CMAKE_DISABLE_FIND_PACKAGE_zstd OFF)
  set(CMAKE_DISABLE_FIND_PACKAGE_ament_cmake OFF)
  set(BUILD_TESTING ${_pj_build_testing})
  set_target_properties(cloudini_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(NOT TARGET cloudini::cloudini_lib)
    add_library(cloudini::cloudini_lib ALIAS cloudini_lib)
  endif()
  set(cloudini_lib_FOUND TRUE)
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
  app/src/whitelist_filter.cpp
  app/src/transform_set.cpp
  app/src/transforming_topic_source.cpp
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

if(cloudini_lib_FOUND)
  target_sources(pj_bridge_app PRIVATE app/src/cloudini_transform.cpp)
  target_link_libraries(pj_bridge_app PRIVATE cloudini::cloudini_lib)
  target_compile_definitions(pj_bridge_app PUBLIC PJ_BRIDGE_HAS_CLOUDINI)
  message(STATUS "Cloudini transform: enabled")
else()
  message(STATUS "Cloudini transform: disabled (cloudini_lib not found)")
endif()

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
    ros2/src/strip_transform.cpp
    ros2/src/ros2_topic_source.cpp
    ros2/src/ros2_subscription_manager.cpp
  )

  target_include_directories(pj_bridge_ros2_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ros2/include
  )

  target_link_libraries(pj_bridge_ros2_lib PUBLIC
    pj_bridge_app
  )

  # target_link_libraries, not ament_target_dependencies: removed in newer distros (Lyrical)
  target_link_libraries(pj_bridge_ros2_lib PUBLIC
    ament_index_cpp::ament_index_cpp
    rclcpp::rclcpp
    ${sensor_msgs_TARGETS}
    ${nav_msgs_TARGETS}
  )

  # ROS2 executable
  add_executable(pj_bridge_ros2
    ros2/src/main.cpp
  )

  target_link_libraries(pj_bridge_ros2
    pj_bridge_ros2_lib
  )

  # Install ROS2 executable
  install(TARGETS pj_bridge_ros2
    DESTINATION lib/${PROJECT_NAME}
  )
endif()

# ============================================================================
# pj_bridge_rti — RTI DDS backend
# ============================================================================
if(ENABLE_RTI AND RTIConnextDDS_FOUND)
  find_package(CLI11 REQUIRED)

  add_library(pj_bridge_rti_lib STATIC
    rti/src/dds_topic_discovery.cpp
    rti/src/dds_subscription_manager.cpp)
  target_include_directories(pj_bridge_rti_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/rti/include)
  target_link_libraries(pj_bridge_rti_lib PUBLIC
    pj_bridge_app RTIConnextDDS::cpp2_api)

  add_executable(pj_bridge_rti rti/src/main.cpp)
  target_link_libraries(pj_bridge_rti pj_bridge_rti_lib CLI11::CLI11)
  install(TARGETS pj_bridge_rti DESTINATION lib/${PROJECT_NAME})
endif()

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
    tests/unit/test_backpressure.cpp
    tests/unit/test_bounded_frame_queue.cpp
    tests/unit/test_message_buffer.cpp
    tests/unit/test_session_manager.cpp
    tests/unit/test_message_serializer.cpp
    tests/unit/test_bridge_server.cpp
    tests/unit/test_protocol_constants.cpp
    tests/unit/test_whitelist_filter.cpp
    tests/unit/test_transform_set.cpp
    tests/unit/test_transforming_topic_source.cpp
    tests/unit/test_topic_discovery.cpp
    tests/unit/test_schema_extractor.cpp
    tests/unit/test_generic_subscription_manager.cpp
    tests/unit/test_ros2_subscription_manager.cpp
    tests/unit/test_message_stripper.cpp
    tests/unit/test_cloudini_transform.cpp
  )

  target_link_libraries(${PROJECT_NAME}_tests
    pj_bridge_ros2_lib
    data_path
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
