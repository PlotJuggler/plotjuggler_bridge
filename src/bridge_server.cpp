// Copyright 2025
// ROS2 Bridge - Bridge Server Implementation

#include "pj_ros_bridge/bridge_server.hpp"

#include <map>
#include <nlohmann/json.hpp>
#include <unordered_set>

#include "pj_ros_bridge/message_serializer.hpp"
#include "pj_ros_bridge/protocol_constants.hpp"

using json = nlohmann::json;

namespace pj_ros_bridge {

BridgeServer::BridgeServer(
    std::shared_ptr<rclcpp::Node> node, std::shared_ptr<MiddlewareInterface> middleware, int port,
    double session_timeout, double publish_rate)
    : node_(node),
      middleware_(middleware),
      port_(port),
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

  // Create components before middleware init so callbacks can use them
  topic_discovery_ = std::make_unique<TopicDiscovery>(node_);
  schema_extractor_ = std::make_unique<SchemaExtractor>();
  message_buffer_ = std::make_shared<MessageBuffer>();
  subscription_manager_ = std::make_unique<GenericSubscriptionManager>(node_);
  session_manager_ = std::make_unique<SessionManager>(session_timeout_);

  RCLCPP_INFO(node_->get_logger(), "Core components created");

  // Register disconnect callback before middleware init to avoid missing events
  middleware_->set_on_disconnect([this](const std::string& client_id) {
    RCLCPP_INFO(node_->get_logger(), "Client disconnected: '%s'", client_id.c_str());
    cleanup_session(client_id);
  });

  // Initialize middleware (may start accepting connections immediately)
  auto result = middleware_->initialize(static_cast<uint16_t>(port_));
  if (!result) {
    RCLCPP_ERROR(node_->get_logger(), "Failed to initialize middleware: %s", result.error().c_str());
    return false;
  }
  RCLCPP_INFO(node_->get_logger(), "Middleware initialized (port: %d)", port_);

  // Create session timeout timer (check every 1 second)
  session_timeout_timer_ = node_->create_wall_timer(std::chrono::seconds(1), [this]() { check_session_timeouts(); });

  RCLCPP_INFO(node_->get_logger(), "Session timeout monitoring started");

  // Create message publisher timer (default: 50 Hz)
  if (publish_rate_ <= 0.0) {
    RCLCPP_ERROR(node_->get_logger(), "Invalid publish_rate: %.1f (must be > 0)", publish_rate_);
    return false;
  }
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
    return true;
  }

  // Parse request JSON
  std::string response;
  json request_json;
  try {
    request_json = json::parse(request);

    if (!request_json.contains("command")) {
      response = create_error_response("INVALID_REQUEST", "Missing 'command' field", request_json);
    } else {
      std::string command = request_json["command"];

      RCLCPP_DEBUG(node_->get_logger(), "Processing '%s' command from client '%s'", command.c_str(), client_id.c_str());

      // Route to appropriate handler
      if (command == "get_topics") {
        response = handle_get_topics(client_id, request_json);
      } else if (command == "subscribe") {
        response = handle_subscribe(client_id, request_json);
      } else if (command == "unsubscribe") {
        response = handle_unsubscribe(client_id, request_json);
      } else if (command == "heartbeat") {
        response = handle_heartbeat(client_id, request_json);
      } else if (command == "pause") {
        response = handle_pause(client_id, request_json);
      } else if (command == "resume") {
        response = handle_resume(client_id, request_json);
      } else {
        response = create_error_response("UNKNOWN_COMMAND", "Unknown command: " + command, request_json);
      }
    }
  } catch (const json::exception& e) {
    RCLCPP_ERROR(node_->get_logger(), "JSON parse error: %s", e.what());
    response = create_error_response("INVALID_JSON", "Failed to parse JSON request", json::object());
  } catch (const std::exception& e) {
    RCLCPP_ERROR(node_->get_logger(), "Error processing request: %s", e.what());
    response = create_error_response("INTERNAL_ERROR", "Internal server error", request_json);
  }

  // Send response to the specific client
  std::vector<uint8_t> response_data(response.begin(), response.end());
  middleware_->send_reply(client_id, response_data);
  return true;
}

std::string BridgeServer::handle_get_topics(const std::string& client_id, const nlohmann::json& request) {
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

  inject_response_fields(response, request);

  RCLCPP_INFO(node_->get_logger(), "Returning %zu topics to client '%s'", topics.size(), client_id.c_str());

  return response.dump();
}

std::string BridgeServer::handle_subscribe(const std::string& client_id, const nlohmann::json& request) {
  // Create session if it doesn't exist
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    RCLCPP_INFO(node_->get_logger(), "Created new session for client '%s'", client_id.c_str());
  }

  // Update heartbeat
  session_manager_->update_heartbeat(client_id);

  if (!request.contains("topics")) {
    return create_error_response("INVALID_REQUEST", "Missing 'topics' field", request);
  }

  if (!request["topics"].is_array()) {
    return create_error_response("INVALID_REQUEST", "'topics' must be an array", request);
  }

  // Get current subscriptions
  auto current_subs = session_manager_->get_subscriptions(client_id);

  // Parse requested topics — supports mixed array of strings and objects:
  //   ["topic1", {"name": "topic2", "max_rate_hz": 10.0}]
  std::unordered_map<std::string, double> requested_topics;
  for (const auto& topic : request["topics"]) {
    if (topic.is_string()) {
      requested_topics[topic.get<std::string>()] = 0.0;
    } else if (topic.is_object() && topic.contains("name") && topic["name"].is_string()) {
      double rate_hz = 0.0;
      if (topic.contains("max_rate_hz") && topic["max_rate_hz"].is_number()) {
        rate_hz = topic["max_rate_hz"].get<double>();
        if (rate_hz < 0.0) {
          rate_hz = 0.0;
        }
      }
      requested_topics[topic["name"].get<std::string>()] = rate_hz;
    }
  }

  // Determine which topics to add (additive model - subscribe only adds, never removes)
  std::vector<std::string> topics_to_add;

  // Find topics to add (in requested but not in current)
  for (const auto& [topic, rate] : requested_topics) {
    if (current_subs.find(topic) == current_subs.end()) {
      topics_to_add.push_back(topic);
    }
  }

  // Get all available topics
  auto available_topics = topic_discovery_->discover_topics();
  std::unordered_map<std::string, std::string> topic_types;
  for (const auto& topic : available_topics) {
    topic_types[topic.name] = topic.type;
  }

  // Subscribe to new topics - track successes and failures
  json schemas = json::object();
  json failures = json::array();
  std::unordered_set<std::string> successfully_subscribed;

  for (const auto& topic_name : topics_to_add) {
    // Check if topic exists
    if (topic_types.find(topic_name) == topic_types.end()) {
      RCLCPP_WARN(
          node_->get_logger(), "Client '%s' requested non-existent topic '%s'", client_id.c_str(), topic_name.c_str());
      json failure;
      failure["topic"] = topic_name;
      failure["reason"] = "Topic does not exist";
      failures.push_back(failure);
      continue;
    }

    std::string topic_type = topic_types[topic_name];

    // Create callback to add messages to buffer
    auto callback = [this](
                        const std::string& topic, const std::shared_ptr<rclcpp::SerializedMessage>& msg,
                        uint64_t receive_time_ns) { message_buffer_->add_message(topic, receive_time_ns, msg); };

    // Get schema BEFORE subscribing to avoid corrupting the reference count
    // if schema extraction fails after subscribe() increments it.
    std::string schema;
    try {
      schema = schema_extractor_->get_message_definition(topic_type);
    } catch (const std::exception& e) {
      RCLCPP_ERROR(node_->get_logger(), "Failed to get schema for topic '%s': %s", topic_name.c_str(), e.what());
      json failure;
      failure["topic"] = topic_name;
      failure["reason"] = std::string("Schema extraction failed: ") + e.what();
      failures.push_back(failure);
      continue;
    }

    // Subscribe via subscription manager
    bool success = subscription_manager_->subscribe(topic_name, topic_type, callback);
    if (!success) {
      RCLCPP_ERROR(node_->get_logger(), "Failed to subscribe to topic '%s'", topic_name.c_str());
      json failure;
      failure["topic"] = topic_name;
      failure["reason"] = "Subscription manager failed to create subscription";
      failures.push_back(failure);
      continue;
    }

    nlohmann::json schema_obj;
    schema_obj["encoding"] = kSchemaEncodingRos2Msg;
    schema_obj["definition"] = schema;
    schemas[topic_name] = schema_obj;
    successfully_subscribed.insert(topic_name);

    RCLCPP_INFO(
        node_->get_logger(), "Client '%s' subscribed to topic '%s' (type: %s)", client_id.c_str(), topic_name.c_str(),
        topic_type.c_str());
  }

  // Update session subscriptions with successfully subscribed topics (additive model)
  std::unordered_map<std::string, double> final_subscriptions = current_subs;
  for (const auto& topic : successfully_subscribed) {
    final_subscriptions[topic] = requested_topics[topic];
  }
  // Update rates for topics that were already subscribed but may have a new rate
  for (const auto& [topic, rate] : requested_topics) {
    if (final_subscriptions.find(topic) != final_subscriptions.end()) {
      final_subscriptions[topic] = rate;
    }
  }
  session_manager_->update_subscriptions(client_id, final_subscriptions);

  // Build response
  json response;
  if (failures.empty()) {
    response["status"] = "success";
  } else if (schemas.empty()) {
    // All subscriptions failed
    response["status"] = "error";
    response["error_code"] = "ALL_SUBSCRIPTIONS_FAILED";
    response["message"] = "Failed to subscribe to all requested topics";
  } else {
    // Partial success
    response["status"] = "partial_success";
    response["message"] = "Some subscriptions failed";
  }

  response["schemas"] = schemas;
  if (!failures.empty()) {
    response["failures"] = failures;
  }

  // Include rate limits in response for topics with non-zero rates
  json rate_limits = json::object();
  for (const auto& [topic, rate] : final_subscriptions) {
    if (rate > 0.0) {
      rate_limits[topic] = rate;
    }
  }
  if (!rate_limits.empty()) {
    response["rate_limits"] = rate_limits;
  }

  inject_response_fields(response, request);

  return response.dump();
}

std::string BridgeServer::handle_unsubscribe(const std::string& client_id, const nlohmann::json& request) {
  // Create session if it doesn't exist
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    RCLCPP_INFO(node_->get_logger(), "Created new session for client '%s'", client_id.c_str());
  }

  // Update heartbeat
  session_manager_->update_heartbeat(client_id);

  // Validate topics field
  if (!request.contains("topics") || !request["topics"].is_array()) {
    return create_error_response("INVALID_REQUEST", "Missing or invalid 'topics' array", request);
  }

  // Get current subscriptions
  auto current_subs = session_manager_->get_subscriptions(client_id);

  std::vector<std::string> removed;

  for (const auto& topic_item : request["topics"]) {
    std::string topic_name;
    if (topic_item.is_string()) {
      topic_name = topic_item.get<std::string>();
    } else {
      continue;  // Skip invalid entries
    }

    // Check if subscribed
    if (current_subs.find(topic_name) != current_subs.end()) {
      // Decrement subscription ref count
      subscription_manager_->unsubscribe(topic_name);

      // Remove from client subscriptions
      current_subs.erase(topic_name);

      removed.push_back(topic_name);

      RCLCPP_INFO(
          node_->get_logger(), "Client '%s' unsubscribed from topic '%s'", client_id.c_str(), topic_name.c_str());
    }
  }

  // Update session subscriptions
  session_manager_->update_subscriptions(client_id, current_subs);

  // Build response
  json response;
  response["status"] = "success";
  response["removed"] = removed;
  inject_response_fields(response, request);

  return response.dump();
}

std::string BridgeServer::handle_heartbeat(const std::string& client_id, const nlohmann::json& request) {
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
  inject_response_fields(response, request);

  return response.dump();
}

std::string BridgeServer::handle_pause(const std::string& client_id, const nlohmann::json& request) {
  // Create session if it doesn't exist
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    RCLCPP_INFO(node_->get_logger(), "Created new session for client '%s'", client_id.c_str());
  }

  // Update heartbeat
  session_manager_->update_heartbeat(client_id);

  // Check if already paused (idempotent)
  if (session_manager_->is_paused(client_id)) {
    RCLCPP_DEBUG(node_->get_logger(), "Client '%s' already paused", client_id.c_str());
    json response;
    response["status"] = "ok";
    response["paused"] = true;
    inject_response_fields(response, request);
    return response.dump();
  }

  // Set paused state
  session_manager_->set_paused(client_id, true);

  // Smart ROS2 management: decrement ref counts for all subscribed topics
  auto subs = session_manager_->get_subscriptions(client_id);
  for (const auto& [topic, rate] : subs) {
    subscription_manager_->unsubscribe(topic);
    RCLCPP_DEBUG(
        node_->get_logger(), "Decremented ref count for topic '%s' (client '%s' paused)", topic.c_str(),
        client_id.c_str());
  }

  RCLCPP_INFO(node_->get_logger(), "Client '%s' paused (%zu topics refs decremented)", client_id.c_str(), subs.size());

  json response;
  response["status"] = "ok";
  response["paused"] = true;
  inject_response_fields(response, request);
  return response.dump();
}

std::string BridgeServer::handle_resume(const std::string& client_id, const nlohmann::json& request) {
  // Create session if it doesn't exist
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    RCLCPP_INFO(node_->get_logger(), "Created new session for client '%s'", client_id.c_str());
  }

  // Update heartbeat
  session_manager_->update_heartbeat(client_id);

  // Check if not paused (idempotent)
  if (!session_manager_->is_paused(client_id)) {
    RCLCPP_DEBUG(node_->get_logger(), "Client '%s' not paused", client_id.c_str());
    json response;
    response["status"] = "ok";
    response["paused"] = false;
    inject_response_fields(response, request);
    return response.dump();
  }

  // Set unpaused state
  session_manager_->set_paused(client_id, false);

  // Smart ROS2 management: increment ref counts for all subscribed topics
  // Get all available topics for type lookup
  auto available_topics = topic_discovery_->discover_topics();
  std::unordered_map<std::string, std::string> topic_types;
  for (const auto& topic : available_topics) {
    topic_types[topic.name] = topic.type;
  }

  // Create callback for message buffer (same as in handle_subscribe)
  auto callback = [this](
                      const std::string& topic, const std::shared_ptr<rclcpp::SerializedMessage>& msg,
                      uint64_t receive_time_ns) { message_buffer_->add_message(topic, receive_time_ns, msg); };

  auto subs = session_manager_->get_subscriptions(client_id);
  for (const auto& [topic, rate] : subs) {
    auto type_it = topic_types.find(topic);
    if (type_it != topic_types.end()) {
      subscription_manager_->subscribe(topic, type_it->second, callback);
      RCLCPP_DEBUG(
          node_->get_logger(), "Incremented ref count for topic '%s' (client '%s' resumed)", topic.c_str(),
          client_id.c_str());
    } else {
      RCLCPP_WARN(
          node_->get_logger(), "Topic '%s' no longer exists, cannot re-subscribe for client '%s'", topic.c_str(),
          client_id.c_str());
    }
  }

  RCLCPP_INFO(node_->get_logger(), "Client '%s' resumed (%zu topics refs incremented)", client_id.c_str(), subs.size());

  json response;
  response["status"] = "ok";
  response["paused"] = false;
  inject_response_fields(response, request);
  return response.dump();
}

void BridgeServer::inject_response_fields(nlohmann::json& response, const nlohmann::json& request) const {
  response["protocol_version"] = kProtocolVersion;
  if (request.contains("id") && request["id"].is_string()) {
    response["id"] = request["id"];
  }
}

std::string BridgeServer::create_error_response(
    const std::string& error_code, const std::string& message, const nlohmann::json& request) const {
  json response;
  response["status"] = "error";
  response["error_code"] = error_code;
  response["message"] = message;
  inject_response_fields(response, request);

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
  std::lock_guard<std::mutex> lock(cleanup_mutex_);

  // Check if session still exists (idempotency guard)
  if (!session_manager_->session_exists(client_id)) {
    return;
  }

  // Get client's subscriptions
  auto subscriptions = session_manager_->get_subscriptions(client_id);

  // Unsubscribe from all topics
  for (const auto& [topic, rate] : subscriptions) {
    subscription_manager_->unsubscribe(topic);
    RCLCPP_DEBUG(node_->get_logger(), "Unsubscribed client '%s' from topic '%s'", client_id.c_str(), topic.c_str());
  }

  // Remove session
  session_manager_->remove_session(client_id);

  // Clean up rate-limit tracking for this client
  {
    std::lock_guard<std::mutex> lock(last_sent_mutex_);
    last_sent_times_.erase(client_id);
  }

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
  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  message_buffer_->move_messages(messages);

  if (messages.empty()) {
    // No new messages, skip publishing
    return;
  }

  try {
    // Get active sessions for per-client filtering
    auto active_sessions = session_manager_->get_active_sessions();
    if (active_sessions.empty()) {
      return;
    }

    // Group clients by subscription config (topic + rate) to avoid redundant serialization.
    // Key: map of topic -> rate_mhz (milli-Hz as int, avoids float equality issues).
    // Clients with identical topic+rate configs share the same compressed buffer.
    using GroupKey = std::map<std::string, int>;
    std::map<GroupKey, std::vector<std::string>> subscription_groups;
    // Also store the per-client subscription map for rate filtering
    std::unordered_map<std::string, std::unordered_map<std::string, double>> client_subs;

    for (const auto& client_id : active_sessions) {
      auto subs = session_manager_->get_subscriptions(client_id);
      if (subs.empty()) {
        continue;
      }
      GroupKey key;
      for (const auto& [topic, rate_hz] : subs) {
        key[topic] = static_cast<int>(rate_hz * 1000);
      }
      subscription_groups[key].push_back(client_id);
      client_subs[client_id] = std::move(subs);
    }

    size_t total_msg_count = 0;
    size_t total_bytes = 0;

    std::lock_guard<std::mutex> sent_lock(last_sent_mutex_);

    for (const auto& [group_key, client_ids] : subscription_groups) {
      // Serialize once for this subscription group
      AggregatedMessageSerializer serializer;
      size_t group_msg_count = 0;

      // Use the first client's subs to get rate info (all clients in group have same config)
      const auto& representative_subs = client_subs[client_ids.front()];
      // Get this representative client's last-sent times for rate filtering
      auto& rep_last_sent = last_sent_times_[client_ids.front()];

      for (const auto& [topic, msgs] : messages) {
        auto sub_it = representative_subs.find(topic);
        if (sub_it == representative_subs.end()) {
          continue;
        }

        double rate_hz = sub_it->second;
        int rate_mhz = static_cast<int>(rate_hz * 1000);

        if (rate_mhz == 0) {
          // Unlimited: serialize all messages
          for (const auto& msg : msgs) {
            serializer.serialize_message(topic, msg.timestamp_ns, *(msg.msg));
            group_msg_count++;
          }
        } else {
          // Rate limited: compute minimum interval in nanoseconds
          // rate_mhz is in milli-Hz, so interval = 1e12 / rate_mhz nanoseconds
          uint64_t min_interval_ns = 1'000'000'000'000ULL / static_cast<uint64_t>(rate_mhz);
          uint64_t last_sent = rep_last_sent[topic];  // 0 if not yet sent

          for (const auto& msg : msgs) {
            if (msg.timestamp_ns >= last_sent + min_interval_ns) {
              serializer.serialize_message(topic, msg.timestamp_ns, *(msg.msg));
              last_sent = msg.timestamp_ns;
              group_msg_count++;
            }
          }
          rep_last_sent[topic] = last_sent;
        }
      }

      if (group_msg_count == 0) {
        continue;
      }

      // Compress once for the group
      std::vector<uint8_t> compressed_data;
      AggregatedMessageSerializer::compress_zstd(serializer.get_serialized_data(), compressed_data);

      // Send the same buffer to all clients in this group
      bool any_sent = false;
      for (const auto& client_id : client_ids) {
        if (middleware_->send_binary(client_id, compressed_data)) {
          any_sent = true;
        }
        // Sync last-sent times from representative to all clients in group
        if (client_id != client_ids.front()) {
          last_sent_times_[client_id] = rep_last_sent;
        }
      }

      // Count messages once per group (not per client) to avoid inflation
      if (any_sent) {
        total_msg_count += group_msg_count;
        total_bytes += compressed_data.size();
      }
    }

    if (total_msg_count > 0) {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      total_messages_published_ += total_msg_count;
      total_bytes_published_ += total_bytes;
    }
  } catch (const std::exception& e) {
    RCLCPP_ERROR(node_->get_logger(), "Error publishing aggregated messages: %s", e.what());
  }
}

}  // namespace pj_ros_bridge
