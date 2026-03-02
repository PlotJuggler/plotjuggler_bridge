/*
 * Copyright (C) 2026 Davide Faconti
 *
 * This file is part of pj_bridge.
 *
 * pj_bridge is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pj_bridge is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with pj_bridge. If not, see <https://www.gnu.org/licenses/>.
 */

#include "pj_bridge/bridge_server.hpp"

#include <spdlog/spdlog.h>

#include <map>
#include <nlohmann/json.hpp>

#include "pj_bridge/message_serializer.hpp"
#include "pj_bridge/protocol_constants.hpp"

using json = nlohmann::json;

namespace pj_bridge {

BridgeServer::BridgeServer(
    std::shared_ptr<TopicSourceInterface> topic_source,
    std::shared_ptr<SubscriptionManagerInterface> subscription_manager, std::shared_ptr<MiddlewareInterface> middleware,
    int port, double session_timeout, double publish_rate)
    : topic_source_(std::move(topic_source)),
      subscription_manager_(std::move(subscription_manager)),
      middleware_(std::move(middleware)),
      port_(port),
      session_timeout_(session_timeout),
      publish_rate_(publish_rate),
      initialized_(false),
      total_messages_published_(0),
      total_bytes_published_(0),
      publish_cycles_(0) {}

BridgeServer::~BridgeServer() {
  // Shut down the middleware before members are destroyed so that
  // disconnect callbacks (which capture `this`) cannot fire into
  // a partially destroyed BridgeServer.
  if (middleware_) {
    middleware_->shutdown();
  }
}

bool BridgeServer::initialize() {
  if (initialized_) {
    spdlog::warn("Bridge server already initialized");
    return true;
  }

  spdlog::info("Initializing bridge server...");

  if (publish_rate_ <= 0.0) {
    spdlog::error("Invalid publish_rate: {:.1f} (must be > 0)", publish_rate_);
    return false;
  }

  message_buffer_ = std::make_shared<MessageBuffer>();
  session_manager_ = std::make_unique<SessionManager>(session_timeout_);

  spdlog::info("Core components created");

  // Set the global message callback to feed into message buffer
  subscription_manager_->set_message_callback(
      [this](const std::string& topic_name, std::shared_ptr<std::vector<std::byte>> cdr_data, uint64_t timestamp_ns) {
        {
          std::lock_guard<std::mutex> lock(stats_mutex_);
          topic_receive_counts_[topic_name]++;
        }
        if (message_buffer_) {
          message_buffer_->add_message(topic_name, timestamp_ns, std::move(cdr_data));
        }
      });

  // Register disconnect callback before middleware init to avoid missing events
  middleware_->set_on_disconnect([this](const std::string& client_id) {
    spdlog::info("Client disconnected: '{}'", client_id);
    cleanup_session(client_id);
  });

  // Initialize middleware (may start accepting connections immediately)
  auto result = middleware_->initialize(static_cast<uint16_t>(port_));
  if (!result) {
    spdlog::error("Failed to initialize middleware: {}", result.error());
    return false;
  }
  spdlog::info("Middleware initialized (port: {})", port_);

  initialized_ = true;
  spdlog::info("Bridge server initialization complete");

  return true;
}

bool BridgeServer::process_requests() {
  if (!initialized_) {
    spdlog::error("Bridge server not initialized");
    return false;
  }

  // Receive request (non-blocking with timeout)
  std::vector<uint8_t> request_data;
  std::string client_id;
  bool received = middleware_->receive_request(request_data, client_id);

  if (!received || request_data.empty()) {
    return false;
  }

  std::string request(request_data.begin(), request_data.end());
  if (client_id.empty()) {
    spdlog::warn("Received request with no client identity");
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

      spdlog::debug("Processing '{}' command from client '{}'", command, client_id);

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
    spdlog::error("JSON parse error: {}", e.what());
    response = create_error_response("INVALID_JSON", "Failed to parse JSON request", json::object());
  } catch (const std::exception& e) {
    spdlog::error("Error processing request: {}", e.what());
    response = create_error_response("INTERNAL_ERROR", "Internal server error", request_json);
  }

  // Send response to the specific client
  std::vector<uint8_t> response_data(response.begin(), response.end());
  if (!middleware_->send_reply(client_id, response_data)) {
    spdlog::warn("Failed to send response to client '{}', cleaning up session", client_id);
    cleanup_session(client_id);
  }
  return true;
}

std::string BridgeServer::handle_get_topics(const std::string& client_id, const nlohmann::json& request) {
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    spdlog::info("Created new session for client '{}'", client_id);
  }

  session_manager_->update_heartbeat(client_id);

  auto topics = topic_source_->get_topics();

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

  spdlog::info("Returning {} topics to client '{}'", topics.size(), client_id);

  return response.dump();
}

std::string BridgeServer::handle_subscribe(const std::string& client_id, const nlohmann::json& request) {
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    spdlog::info("Created new session for client '{}'", client_id);
  }

  session_manager_->update_heartbeat(client_id);

  if (!request.contains("topics")) {
    return create_error_response("INVALID_REQUEST", "Missing 'topics' field", request);
  }

  if (!request["topics"].is_array()) {
    return create_error_response("INVALID_REQUEST", "'topics' must be an array", request);
  }

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

  // Determine which topics to add (additive model — subscribe only adds, never removes)
  std::vector<std::string> topics_to_add;
  for (const auto& [topic, rate] : requested_topics) {
    if (current_subs.find(topic) == current_subs.end()) {
      topics_to_add.push_back(topic);
    }
  }

  // Build topic name → type lookup from topic source
  auto available_topics = topic_source_->get_topics();
  std::unordered_map<std::string, std::string> topic_types;
  for (const auto& topic : available_topics) {
    topic_types[topic.name] = topic.type;
  }

  // Subscribe to new topics — track successes and failures
  json schemas = json::object();
  json failures = json::array();

  for (const auto& topic_name : topics_to_add) {
    if (topic_types.find(topic_name) == topic_types.end()) {
      spdlog::warn("Client '{}' requested non-existent topic '{}'", client_id, topic_name);
      json failure;
      failure["topic"] = topic_name;
      failure["reason"] = "Topic does not exist";
      failures.push_back(failure);
      continue;
    }

    std::string topic_type = topic_types[topic_name];

    // Get schema BEFORE subscribing to avoid corrupting the reference count
    // if schema extraction fails after subscribe() increments it.
    std::string schema;
    try {
      schema = topic_source_->get_schema(topic_name);
    } catch (const std::exception& e) {
      spdlog::error("Failed to get schema for topic '{}': {}", topic_name, e.what());
      json failure;
      failure["topic"] = topic_name;
      failure["reason"] = std::string("Schema extraction failed: ") + e.what();
      failures.push_back(failure);
      continue;
    }

    if (schema.empty()) {
      spdlog::error("Empty schema for topic '{}' (type: {})", topic_name, topic_type);
      json failure;
      failure["topic"] = topic_name;
      failure["reason"] = "Schema extraction returned empty definition";
      failures.push_back(failure);
      continue;
    }

    // Subscribe via subscription manager (ref-counted)
    bool success = subscription_manager_->subscribe(topic_name, topic_type);
    if (!success) {
      spdlog::error("Failed to subscribe to topic '{}'", topic_name);
      json failure;
      failure["topic"] = topic_name;
      failure["reason"] = "Subscription manager failed to create subscription";
      failures.push_back(failure);
      continue;
    }

    // Immediately add to session. If session is gone (client disconnected
    // between subscribe and now), roll back the ref count.
    if (!session_manager_->add_subscription(client_id, topic_name, requested_topics[topic_name])) {
      spdlog::warn("Session gone for client '{}', rolling back subscribe for '{}'", client_id, topic_name);
      subscription_manager_->unsubscribe(topic_name);
      continue;
    }

    nlohmann::json schema_obj;
    schema_obj["encoding"] = topic_source_->schema_encoding();
    schema_obj["definition"] = schema;
    schemas[topic_name] = schema_obj;

    spdlog::info("Client '{}' subscribed to topic '{}' (type: {})", client_id, topic_name, topic_type);
  }

  // Update rates for topics that were already subscribed but may have a new rate
  for (const auto& [topic, rate] : requested_topics) {
    if (current_subs.find(topic) != current_subs.end()) {
      session_manager_->add_subscription(client_id, topic, rate);
    }
  }

  // Build response
  json response;
  if (failures.empty()) {
    response["status"] = "success";
  } else if (schemas.empty()) {
    response["status"] = "error";
    response["error_code"] = "ALL_SUBSCRIPTIONS_FAILED";
    response["message"] = "Failed to subscribe to all requested topics";
  } else {
    response["status"] = "partial_success";
    response["message"] = "Some subscriptions failed";
  }

  response["schemas"] = schemas;
  if (!failures.empty()) {
    response["failures"] = failures;
  }

  // Include rate limits in response for topics with non-zero rates
  json rate_limits = json::object();
  auto updated_subs = session_manager_->get_subscriptions(client_id);
  for (const auto& [topic, rate] : updated_subs) {
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
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    spdlog::info("Created new session for client '{}'", client_id);
  }

  session_manager_->update_heartbeat(client_id);

  if (!request.contains("topics") || !request["topics"].is_array()) {
    return create_error_response("INVALID_REQUEST", "Missing or invalid 'topics' array", request);
  }

  auto current_subs = session_manager_->get_subscriptions(client_id);

  std::vector<std::string> removed;

  for (const auto& topic_item : request["topics"]) {
    std::string topic_name;
    if (topic_item.is_string()) {
      topic_name = topic_item.get<std::string>();
    } else if (topic_item.is_object() && topic_item.contains("name") && topic_item["name"].is_string()) {
      topic_name = topic_item["name"].get<std::string>();
    } else {
      continue;
    }

    if (current_subs.find(topic_name) != current_subs.end()) {
      subscription_manager_->unsubscribe(topic_name);
      current_subs.erase(topic_name);
      removed.push_back(topic_name);

      spdlog::info("Client '{}' unsubscribed from topic '{}'", client_id, topic_name);
    }
  }

  session_manager_->update_subscriptions(client_id, current_subs);

  json response;
  response["status"] = "success";
  response["removed"] = removed;
  inject_response_fields(response, request);

  return response.dump();
}

std::string BridgeServer::handle_heartbeat(const std::string& client_id, const nlohmann::json& request) {
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    spdlog::info("Created new session for client '{}'", client_id);
  }

  session_manager_->update_heartbeat(client_id);

  spdlog::debug("Heartbeat received from client '{}'", client_id);

  json response;
  response["status"] = "ok";
  inject_response_fields(response, request);

  return response.dump();
}

std::string BridgeServer::handle_pause(const std::string& client_id, const nlohmann::json& request) {
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    spdlog::info("Created new session for client '{}'", client_id);
  }

  session_manager_->update_heartbeat(client_id);

  if (session_manager_->is_paused(client_id)) {
    spdlog::debug("Client '{}' already paused", client_id);
    json response;
    response["status"] = "ok";
    response["paused"] = true;
    inject_response_fields(response, request);
    return response.dump();
  }

  session_manager_->set_paused(client_id, true);

  // Decrement ref counts for all subscribed topics
  auto subs = session_manager_->get_subscriptions(client_id);
  for (const auto& [topic, rate] : subs) {
    subscription_manager_->unsubscribe(topic);
    spdlog::debug("Decremented ref count for topic '{}' (client '{}' paused)", topic, client_id);
  }

  spdlog::info("Client '{}' paused ({} topics refs decremented)", client_id, subs.size());

  json response;
  response["status"] = "ok";
  response["paused"] = true;
  inject_response_fields(response, request);
  return response.dump();
}

std::string BridgeServer::handle_resume(const std::string& client_id, const nlohmann::json& request) {
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    spdlog::info("Created new session for client '{}'", client_id);
  }

  session_manager_->update_heartbeat(client_id);

  if (!session_manager_->is_paused(client_id)) {
    spdlog::debug("Client '{}' not paused", client_id);
    json response;
    response["status"] = "ok";
    response["paused"] = false;
    inject_response_fields(response, request);
    return response.dump();
  }

  session_manager_->set_paused(client_id, false);

  // Build topic name → type lookup for subscribe calls
  auto available_topics = topic_source_->get_topics();
  std::unordered_map<std::string, std::string> topic_types;
  for (const auto& t : available_topics) {
    topic_types[t.name] = t.type;
  }

  // Increment ref counts for all subscribed topics
  auto subs = session_manager_->get_subscriptions(client_id);
  std::vector<std::string> failed_topics;
  for (const auto& [topic, rate] : subs) {
    auto type_it = topic_types.find(topic);
    if (type_it == topic_types.end()) {
      spdlog::warn("Topic '{}' no longer exists, cannot re-subscribe for client '{}'", topic, client_id);
      failed_topics.push_back(topic);
      continue;
    }
    bool ok = subscription_manager_->subscribe(topic, type_it->second);
    if (ok) {
      spdlog::debug("Incremented ref count for topic '{}' (client '{}' resumed)", topic, client_id);
    } else {
      spdlog::warn("Topic '{}' subscription failed on resume for client '{}'", topic, client_id);
      failed_topics.push_back(topic);
    }
  }

  // Remove failed topics from session to prevent ref-count underflow on disconnect
  for (const auto& topic : failed_topics) {
    session_manager_->remove_subscription(client_id, topic);
  }

  spdlog::info(
      "Client '{}' resumed ({} topics re-subscribed, {} failed)", client_id, subs.size() - failed_topics.size(),
      failed_topics.size());

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
  if (!initialized_) {
    return;
  }

  auto timed_out_sessions = session_manager_->get_timed_out_sessions();

  for (const auto& client_id : timed_out_sessions) {
    spdlog::warn("Session timeout for client '{}'", client_id);
    cleanup_session(client_id);
  }
}

void BridgeServer::cleanup_session(const std::string& client_id) {
  std::lock_guard<std::mutex> lock(cleanup_mutex_);

  if (!session_manager_->session_exists(client_id)) {
    return;
  }

  auto subscriptions = session_manager_->get_subscriptions(client_id);
  bool was_paused = session_manager_->is_paused(client_id);

  // Unsubscribe from all topics (only if not paused — paused clients already decremented ref counts)
  if (!was_paused) {
    for (const auto& [topic, rate] : subscriptions) {
      subscription_manager_->unsubscribe(topic);
      spdlog::debug("Unsubscribed client '{}' from topic '{}'", client_id, topic);
    }
  }

  session_manager_->remove_session(client_id);

  {
    std::lock_guard<std::mutex> sent_lock(last_sent_mutex_);
    last_sent_times_.erase(client_id);
  }

  spdlog::info("Cleaned up session for client '{}' ({} topics unsubscribed)", client_id, subscriptions.size());
}

size_t BridgeServer::get_active_session_count() const {
  return session_manager_->session_count();
}

std::pair<uint64_t, uint64_t> BridgeServer::get_publish_stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return {total_messages_published_, total_bytes_published_};
}

BridgeServer::StatsSnapshot BridgeServer::snapshot_and_reset_stats() {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  StatsSnapshot snapshot;
  snapshot.topic_receive_counts = std::move(topic_receive_counts_);
  topic_receive_counts_.clear();
  snapshot.topic_forward_counts = std::move(topic_forward_counts_);
  topic_forward_counts_.clear();
  snapshot.publish_cycles = publish_cycles_;
  publish_cycles_ = 0;
  snapshot.total_bytes_published = total_bytes_published_;
  total_bytes_published_ = 0;
  return snapshot;
}

void BridgeServer::publish_aggregated_messages() {
  if (!initialized_) {
    return;
  }

  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  message_buffer_->move_messages(messages);

  if (messages.empty()) {
    return;
  }

  try {
    auto active_sessions = session_manager_->get_active_sessions();
    if (active_sessions.empty()) {
      return;
    }

    // Group clients by subscription config (topic + rate) to avoid redundant serialization.
    using GroupKey = std::map<std::string, int>;
    std::map<GroupKey, std::vector<std::string>> subscription_groups;
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
    std::unordered_map<std::string, uint64_t> forward_counts;

    // Collect paused state outside of last_sent_mutex_ to avoid lock ordering issues.
    std::unordered_map<std::string, bool> paused_state;
    for (const auto& [group_key, client_ids] : subscription_groups) {
      for (const auto& client_id : client_ids) {
        paused_state[client_id] = session_manager_->is_paused(client_id);
      }
    }

    // Per-group serialized frames, built under last_sent_mutex_ for rate limiting
    struct GroupFrame {
      std::vector<uint8_t> compressed_data;
      size_t msg_count;
      std::vector<std::string> client_ids;
    };
    std::vector<GroupFrame> frames;

    {
      std::lock_guard<std::mutex> sent_lock(last_sent_mutex_);

      for (const auto& [group_key, client_ids] : subscription_groups) {
        AggregatedMessageSerializer serializer;
        size_t group_msg_count = 0;

        const auto& representative_subs = client_subs[client_ids.front()];
        auto& rep_last_sent = last_sent_times_[client_ids.front()];

        for (const auto& [topic, msgs] : messages) {
          auto sub_it = representative_subs.find(topic);
          if (sub_it == representative_subs.end()) {
            continue;
          }

          double rate_hz = sub_it->second;
          int rate_mhz = static_cast<int>(rate_hz * 1000);

          if (rate_mhz == 0) {
            for (const auto& msg : msgs) {
              serializer.serialize_message(topic, msg.timestamp_ns, msg.data->data(), msg.data->size());
              group_msg_count++;
              forward_counts[topic]++;
            }
          } else {
            uint64_t min_interval_ns = static_cast<uint64_t>(1'000'000'000'000) / static_cast<uint64_t>(rate_mhz);
            uint64_t last_sent = rep_last_sent[topic];

            for (const auto& msg : msgs) {
              if (msg.timestamp_ns >= last_sent + min_interval_ns) {
                serializer.serialize_message(topic, msg.timestamp_ns, msg.data->data(), msg.data->size());
                last_sent = msg.timestamp_ns;
                group_msg_count++;
                forward_counts[topic]++;
              }
            }
            rep_last_sent[topic] = last_sent;
          }
        }

        if (group_msg_count == 0) {
          continue;
        }

        GroupFrame frame;
        frame.compressed_data = serializer.finalize();
        frame.msg_count = group_msg_count;
        frame.client_ids = client_ids;

        // Propagate rate-limiting state to other clients in the group
        for (const auto& client_id : client_ids) {
          if (client_id != client_ids.front()) {
            last_sent_times_[client_id] = rep_last_sent;
          }
        }

        frames.push_back(std::move(frame));
      }
    }  // last_sent_mutex_ released here

    // Send frames outside of any lock
    for (const auto& frame : frames) {
      bool any_sent = false;
      for (const auto& client_id : frame.client_ids) {
        auto it = paused_state.find(client_id);
        if (it != paused_state.end() && it->second) {
          continue;
        }
        if (middleware_->send_binary(client_id, frame.compressed_data)) {
          any_sent = true;
        } else {
          spdlog::debug("Failed to send binary frame to client '{}'", client_id);
        }
      }

      if (any_sent) {
        total_msg_count += frame.msg_count;
        total_bytes += frame.compressed_data.size();
      }
    }

    if (total_msg_count > 0) {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      total_messages_published_ += total_msg_count;
      total_bytes_published_ += total_bytes;
      publish_cycles_++;
      for (const auto& [topic, count] : forward_counts) {
        topic_forward_counts_[topic] += count;
      }
    }
  } catch (const std::exception& e) {
    spdlog::error("Error publishing aggregated messages: {}", e.what());
  }
}

}  // namespace pj_bridge
