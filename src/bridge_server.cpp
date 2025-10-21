// Copyright 2025
// ROS2 Bridge - Bridge Server Implementation

#include "pj_ros_bridge/bridge_server.hpp"

#include <nlohmann/json.hpp>

#include "pj_ros_bridge/message_serializer.hpp"

using json = nlohmann::json;

namespace pj_ros_bridge {

BridgeServer::BridgeServer(
    std::shared_ptr<rclcpp::Node> node, std::shared_ptr<MiddlewareInterface> middleware, int req_port, int pub_port,
    double session_timeout, double publish_rate)
    : node_(node),
      middleware_(middleware),
      req_port_(req_port),
      pub_port_(pub_port),
      session_timeout_(session_timeout),
      publish_rate_(publish_rate),
      initialized_(false),
      total_messages_published_(0),
      total_bytes_published_(0) {}

bool BridgeServer::initialize() {
  if (initialized_) {
    RCLCPP_WARN(node_->get_logger(), "Bridge server already initialized");
    return true;
  }

  RCLCPP_INFO(node_->get_logger(), "Initializing bridge server...");

  // Initialize middleware
  try {
    middleware_->initialize(req_port_, pub_port_);
    RCLCPP_INFO(node_->get_logger(), "Middleware initialized (REQ port: %d, PUB port: %d)", req_port_, pub_port_);
  } catch (const std::exception& e) {
    RCLCPP_ERROR(node_->get_logger(), "Failed to initialize middleware: %s", e.what());
    return false;
  }

  // Create components
  topic_discovery_ = std::make_unique<TopicDiscovery>(node_);
  schema_extractor_ = std::make_unique<SchemaExtractor>();
  message_buffer_ = std::make_shared<MessageBuffer>();
  subscription_manager_ = std::make_unique<GenericSubscriptionManager>(node_);
  session_manager_ = std::make_unique<SessionManager>(session_timeout_);

  RCLCPP_INFO(node_->get_logger(), "Core components created");

  // Create session timeout timer (check every 1 second)
  session_timeout_timer_ = node_->create_wall_timer(std::chrono::seconds(1), [this]() { check_session_timeouts(); });

  RCLCPP_INFO(node_->get_logger(), "Session timeout monitoring started");

  // Create message publisher timer (default: 50 Hz)
  auto publish_period_ms = static_cast<int64_t>(1000.0 / publish_rate_);
  publish_timer_ = node_->create_wall_timer(
      std::chrono::milliseconds(publish_period_ms), [this]() { publish_aggregated_messages(); });

  RCLCPP_INFO(node_->get_logger(), "Message aggregation publisher started at %.1f Hz", publish_rate_);

  initialized_ = true;
  RCLCPP_INFO(node_->get_logger(), "Bridge server initialization complete");

  return true;
}

bool BridgeServer::process_requests() {
  if (!initialized_) {
    RCLCPP_ERROR(node_->get_logger(), "Bridge server not initialized");
    return false;
  }

  // Receive request (non-blocking)
  std::vector<uint8_t> request_data;
  std::string client_id;
  bool received = middleware_->receive_request(request_data, client_id);

  if (!received || request_data.empty()) {
    return false;  // No request pending
  }

  // Convert request data to string
  std::string request(request_data.begin(), request_data.end());
  if (client_id.empty()) {
    RCLCPP_WARN(node_->get_logger(), "Received request with no client identity");
    std::string error = create_error_response("INVALID_CLIENT", "No client identity provided");
    std::vector<uint8_t> error_data(error.begin(), error.end());
    middleware_->send_reply(error_data);
    return true;
  }

  // Parse request JSON
  std::string response;
  try {
    json request_json = json::parse(request);

    if (!request_json.contains("command")) {
      response = create_error_response("INVALID_REQUEST", "Missing 'command' field");
    } else {
      std::string command = request_json["command"];

      RCLCPP_DEBUG(node_->get_logger(), "Processing '%s' command from client '%s'", command.c_str(), client_id.c_str());

      // Route to appropriate handler
      if (command == "get_topics") {
        response = handle_get_topics(client_id);
      } else if (command == "subscribe") {
        response = handle_subscribe(client_id, request);
      } else if (command == "heartbeat") {
        response = handle_heartbeat(client_id);
      } else {
        response = create_error_response("UNKNOWN_COMMAND", "Unknown command: " + command);
      }
    }
  } catch (const json::exception& e) {
    RCLCPP_ERROR(node_->get_logger(), "JSON parse error: %s", e.what());
    response = create_error_response("INVALID_JSON", "Failed to parse JSON request");
  } catch (const std::exception& e) {
    RCLCPP_ERROR(node_->get_logger(), "Error processing request: %s", e.what());
    response = create_error_response("INTERNAL_ERROR", "Internal server error");
  }

  // Send response
  std::vector<uint8_t> response_data(response.begin(), response.end());
  middleware_->send_reply(response_data);
  return true;
}

std::string BridgeServer::handle_get_topics(const std::string& client_id) {
  // Create session if it doesn't exist
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    RCLCPP_INFO(node_->get_logger(), "Created new session for client '%s'", client_id.c_str());
  }

  // Update heartbeat
  session_manager_->update_heartbeat(client_id);

  // Discover topics
  auto topics = topic_discovery_->discover_topics();

  // Build JSON response
  json response;
  response["status"] = "success";
  response["topics"] = json::array();

  for (const auto& topic : topics) {
    json topic_entry;
    topic_entry["name"] = topic.name;
    topic_entry["type"] = topic.type;
    response["topics"].push_back(topic_entry);
  }

  RCLCPP_INFO(node_->get_logger(), "Returning %zu topics to client '%s'", topics.size(), client_id.c_str());

  return response.dump();
}

std::string BridgeServer::handle_subscribe(const std::string& client_id, const std::string& request_json) {
  // Create session if it doesn't exist
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    RCLCPP_INFO(node_->get_logger(), "Created new session for client '%s'", client_id.c_str());
  }

  // Update heartbeat
  session_manager_->update_heartbeat(client_id);

  // Parse request
  json request = json::parse(request_json);

  if (!request.contains("topics")) {
    return create_error_response("INVALID_REQUEST", "Missing 'topics' field");
  }

  if (!request["topics"].is_array()) {
    return create_error_response("INVALID_REQUEST", "'topics' must be an array");
  }

  // Get current subscriptions
  auto current_subs = session_manager_->get_subscriptions(client_id);

  // Parse requested topics
  std::unordered_set<std::string> requested_topics;
  for (const auto& topic : request["topics"]) {
    if (topic.is_string()) {
      requested_topics.insert(topic);
    }
  }

  // Determine which topics to add and remove
  std::vector<std::string> topics_to_add;
  std::vector<std::string> topics_to_remove;

  // Find topics to add (in requested but not in current)
  for (const auto& topic : requested_topics) {
    if (current_subs.find(topic) == current_subs.end()) {
      topics_to_add.push_back(topic);
    }
  }

  // Find topics to remove (in current but not in requested)
  for (const auto& topic : current_subs) {
    if (requested_topics.find(topic) == requested_topics.end()) {
      topics_to_remove.push_back(topic);
    }
  }

  // Get all available topics
  auto available_topics = topic_discovery_->discover_topics();
  std::unordered_map<std::string, std::string> topic_types;
  for (const auto& topic : available_topics) {
    topic_types[topic.name] = topic.type;
  }

  // Subscribe to new topics
  json schemas = json::object();
  for (const auto& topic_name : topics_to_add) {
    // Check if topic exists
    if (topic_types.find(topic_name) == topic_types.end()) {
      RCLCPP_WARN(
          node_->get_logger(), "Client '%s' requested non-existent topic '%s'", client_id.c_str(), topic_name.c_str());
      continue;
    }

    std::string topic_type = topic_types[topic_name];

    // Create callback to add messages to buffer
    auto callback = [this, topic_name](
                        const std::string& topic, const std::shared_ptr<rclcpp::SerializedMessage>& msg,
                        uint64_t receive_time_ns) {
      // Extract message data
      const auto& rcl_msg = msg->get_rcl_serialized_message();
      std::vector<uint8_t> data(rcl_msg.buffer, rcl_msg.buffer + rcl_msg.buffer_length);

      // For now, use receive time as publish time (TODO: extract from message header)
      message_buffer_->add_message(topic, receive_time_ns, receive_time_ns, data);
    };

    // Subscribe via subscription manager
    bool success = subscription_manager_->subscribe(topic_name, topic_type, callback);
    if (!success) {
      RCLCPP_ERROR(node_->get_logger(), "Failed to subscribe to topic '%s'", topic_name.c_str());
      continue;
    }

    // Get schema
    try {
      std::string schema = schema_extractor_->get_message_definition(topic_type);
      schemas[topic_name] = schema;

      RCLCPP_INFO(
          node_->get_logger(), "Client '%s' subscribed to topic '%s' (type: %s)", client_id.c_str(), topic_name.c_str(),
          topic_type.c_str());
    } catch (const std::exception& e) {
      RCLCPP_ERROR(node_->get_logger(), "Failed to get schema for topic '%s': %s", topic_name.c_str(), e.what());
      subscription_manager_->unsubscribe(topic_name);
    }
  }

  // Unsubscribe from removed topics
  for (const auto& topic_name : topics_to_remove) {
    subscription_manager_->unsubscribe(topic_name);
    RCLCPP_INFO(node_->get_logger(), "Client '%s' unsubscribed from topic '%s'", client_id.c_str(), topic_name.c_str());
  }

  // Update session subscriptions
  session_manager_->update_subscriptions(client_id, requested_topics);

  // Build response
  json response;
  response["status"] = "success";
  response["schemas"] = schemas;

  return response.dump();
}

std::string BridgeServer::handle_heartbeat(const std::string& client_id) {
  // Create session if it doesn't exist
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    RCLCPP_INFO(node_->get_logger(), "Created new session for client '%s'", client_id.c_str());
  }

  // Update heartbeat
  session_manager_->update_heartbeat(client_id);

  RCLCPP_DEBUG(node_->get_logger(), "Heartbeat received from client '%s'", client_id.c_str());

  // Build response
  json response;
  response["status"] = "ok";

  return response.dump();
}

std::string BridgeServer::create_error_response(const std::string& error_code, const std::string& message) const {
  json response;
  response["status"] = "error";
  response["error_code"] = error_code;
  response["message"] = message;

  return response.dump();
}

void BridgeServer::check_session_timeouts() {
  auto timed_out_sessions = session_manager_->get_timed_out_sessions();

  for (const auto& client_id : timed_out_sessions) {
    RCLCPP_WARN(node_->get_logger(), "Session timeout for client '%s'", client_id.c_str());

    cleanup_session(client_id);
  }
}

void BridgeServer::cleanup_session(const std::string& client_id) {
  // Get client's subscriptions
  auto subscriptions = session_manager_->get_subscriptions(client_id);

  // Unsubscribe from all topics
  for (const auto& topic : subscriptions) {
    subscription_manager_->unsubscribe(topic);
    RCLCPP_DEBUG(node_->get_logger(), "Unsubscribed client '%s' from topic '%s'", client_id.c_str(), topic.c_str());
  }

  // Remove session
  session_manager_->remove_session(client_id);

  RCLCPP_INFO(
      node_->get_logger(), "Cleaned up session for client '%s' (%zu topics unsubscribed)", client_id.c_str(),
      subscriptions.size());
}

size_t BridgeServer::get_active_session_count() const {
  return session_manager_->session_count();
}

std::pair<uint64_t, uint64_t> BridgeServer::get_publish_stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return {total_messages_published_, total_bytes_published_};
}

void BridgeServer::publish_aggregated_messages() {
  // Get all new messages from buffer
  auto messages = message_buffer_->get_new_messages();

  if (messages.empty()) {
    // No new messages, skip publishing
    return;
  }

  try {
    // Create serializer and add all messages
    AggregatedMessageSerializer serializer;
    for (const auto& msg : messages) {
      serializer.add_message(msg.topic_name, msg.publish_timestamp_ns, msg.receive_timestamp_ns, msg.data);
    }

    // Serialize to binary format
    auto serialized_data = serializer.serialize();

    if (serialized_data.empty()) {
      RCLCPP_WARN(node_->get_logger(), "Serialized data is empty despite having messages");
      return;
    }

    // Compress with ZSTD
    auto compressed_data = AggregatedMessageSerializer::compress_zstd(serialized_data);

    // Publish via middleware
    bool success = middleware_->publish_data(compressed_data);

    if (success) {
      // Update statistics
      std::lock_guard<std::mutex> lock(stats_mutex_);
      total_messages_published_ += messages.size();
      total_bytes_published_ += compressed_data.size();

      RCLCPP_DEBUG(
          node_->get_logger(), "Published %zu messages (%zu bytes raw, %zu bytes compressed, %.1f%% ratio)",
          messages.size(), serialized_data.size(), compressed_data.size(),
          100.0 * static_cast<double>(compressed_data.size()) / static_cast<double>(serialized_data.size()));
    } else {
      RCLCPP_ERROR(node_->get_logger(), "Failed to publish aggregated messages");
    }
  } catch (const std::exception& e) {
    RCLCPP_ERROR(node_->get_logger(), "Error publishing aggregated messages: %s", e.what());
  }
}

}  // namespace pj_ros_bridge
