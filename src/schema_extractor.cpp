// Copyright 2025 Davide Faconti
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "pj_ros_bridge/schema_extractor.hpp"

#include <rosidl_runtime_cpp/message_type_support_decl.hpp>
#include <rosidl_typesupport_introspection_cpp/field_types.hpp>
#include <rosidl_typesupport_introspection_cpp/identifier.hpp>
#include <rosidl_typesupport_introspection_cpp/message_introspection.hpp>

#include <dlfcn.h>
#include <sstream>

namespace pj_ros_bridge
{

using MessageMembers = rosidl_typesupport_introspection_cpp::MessageMembers;
using MessageMember = rosidl_typesupport_introspection_cpp::MessageMember;

nlohmann::json SchemaExtractor::extract_schema(const std::string& message_type)
{
  std::string library_name;
  std::string type_name;

  if (!parse_message_type(message_type, library_name, type_name)) {
    return nlohmann::json();
  }

  // Construct the library file name
  std::string lib_file_name = "lib" + library_name + "__rosidl_typesupport_introspection_cpp.so";

  // Try to load the type support library
  void* lib_handle = dlopen(lib_file_name.c_str(), RTLD_LAZY | RTLD_GLOBAL);
  if (!lib_handle) {
    // Library not found - this is expected for types not currently available
    return nlohmann::json();
  }

  // Construct the symbol name for the type support function
  std::string symbol_name = "rosidl_typesupport_introspection_cpp__get_message_type_support_handle__" +
    library_name + "__msg__" + type_name;

  using TypeSupportFunc = const rosidl_message_type_support_t* (*)();
  auto type_support_func = reinterpret_cast<TypeSupportFunc>(dlsym(lib_handle, symbol_name.c_str()));

  if (!type_support_func) {
    dlclose(lib_handle);
    return nlohmann::json();
  }

  const rosidl_message_type_support_t* type_support = type_support_func();

  if (!type_support) {
    dlclose(lib_handle);
    return nlohmann::json();
  }

  auto schema = build_schema_from_introspection(type_support->data);

  // Don't close the library - it might be needed later
  // dlclose(lib_handle);

  return schema;
}

std::string SchemaExtractor::get_schema_json(const std::string& message_type)
{
  auto schema = extract_schema(message_type);
  if (schema.is_null()) {
    return "";
  }
  return schema.dump(2);  // Pretty print with 2-space indent
}

bool SchemaExtractor::parse_message_type(
  const std::string& message_type,
  std::string& library_name,
  std::string& type_name) const
{
  // Expected format: "package_name/msg/MessageType"
  size_t first_slash = message_type.find('/');
  if (first_slash == std::string::npos) {
    return false;
  }

  size_t second_slash = message_type.find('/', first_slash + 1);
  if (second_slash == std::string::npos) {
    return false;
  }

  library_name = message_type.substr(0, first_slash);
  type_name = message_type.substr(second_slash + 1);

  return true;
}

nlohmann::json SchemaExtractor::build_schema_from_introspection(const void* type_support)
{
  const auto* members = static_cast<const MessageMembers*>(type_support);
  if (!members) {
    return nlohmann::json();
  }

  nlohmann::json schema;
  schema["type"] = "object";
  schema["properties"] = nlohmann::json::object();

  for (size_t i = 0; i < members->member_count_; ++i) {
    const MessageMember& member = members->members_[i];
    nlohmann::json field_schema;

    // Handle different field types
    switch (member.type_id_) {
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_BOOL:
        field_schema["type"] = "boolean";
        break;
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_BYTE:
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT8:
        field_schema["type"] = "uint8";
        break;
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_CHAR:
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_INT8:
        field_schema["type"] = "int8";
        break;
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT16:
        field_schema["type"] = "uint16";
        break;
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_INT16:
        field_schema["type"] = "int16";
        break;
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT32:
        field_schema["type"] = "uint32";
        break;
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_INT32:
        field_schema["type"] = "int32";
        break;
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT64:
        field_schema["type"] = "uint64";
        break;
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_INT64:
        field_schema["type"] = "int64";
        break;
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_FLOAT:
        field_schema["type"] = "float32";
        break;
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_DOUBLE:
        field_schema["type"] = "float64";
        break;
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_STRING:
        field_schema["type"] = "string";
        break;
      case rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE:
        // Nested message - recursively extract schema
        if (member.members_) {
          field_schema = build_schema_from_introspection(member.members_->data);
        }
        break;
      default:
        field_schema["type"] = "unknown";
        break;
    }

    // Handle arrays
    if (member.is_array_) {
      nlohmann::json array_schema;
      array_schema["type"] = "array";
      array_schema["items"] = field_schema;

      if (!member.is_upper_bound_ && member.array_size_ > 0) {
        array_schema["maxItems"] = member.array_size_;
        array_schema["minItems"] = member.array_size_;
      }

      schema["properties"][member.name_] = array_schema;
    } else {
      schema["properties"][member.name_] = field_schema;
    }
  }

  return schema;
}

}  // namespace pj_ros_bridge
