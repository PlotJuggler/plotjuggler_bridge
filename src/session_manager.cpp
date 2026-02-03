// Copyright 2025
// ROS2 Bridge - Session Manager Implementation

#include "pj_ros_bridge/session_manager.hpp"

#include <algorithm>

namespace pj_ros_bridge {

SessionManager::SessionManager(double timeout_seconds) : timeout_duration_(timeout_seconds) {}

bool SessionManager::create_session(const std::string& client_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Check if session already exists
  if (sessions_.find(client_id) != sessions_.end()) {
    return false;
  }

  // Create new session
  Session session;
  session.client_id = client_id;
  session.created_at = std::chrono::steady_clock::now();
  session.last_heartbeat = session.created_at;

  sessions_[client_id] = session;
  return true;
}

bool SessionManager::update_heartbeat(const std::string& client_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = sessions_.find(client_id);
  if (it == sessions_.end()) {
    return false;
  }

  it->second.last_heartbeat = std::chrono::steady_clock::now();
  return true;
}

bool SessionManager::get_session(const std::string& client_id, Session& session) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = sessions_.find(client_id);
  if (it == sessions_.end()) {
    return false;
  }

  session = it->second;
  return true;
}

bool SessionManager::update_subscriptions(
    const std::string& client_id, const std::unordered_map<std::string, double>& topics) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = sessions_.find(client_id);
  if (it == sessions_.end()) {
    return false;
  }

  it->second.subscribed_topics = topics;
  return true;
}

std::unordered_map<std::string, double> SessionManager::get_subscriptions(const std::string& client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = sessions_.find(client_id);
  if (it == sessions_.end()) {
    return {};
  }

  return it->second.subscribed_topics;
}

bool SessionManager::remove_session(const std::string& client_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = sessions_.find(client_id);
  if (it == sessions_.end()) {
    return false;
  }

  sessions_.erase(it);
  return true;
}

std::vector<std::string> SessionManager::get_timed_out_sessions() {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::string> timed_out_clients;
  auto now = std::chrono::steady_clock::now();

  for (const auto& [client_id, session] : sessions_) {
    auto time_since_heartbeat = now - session.last_heartbeat;
    if (time_since_heartbeat > timeout_duration_) {
      timed_out_clients.push_back(client_id);
    }
  }

  return timed_out_clients;
}

std::vector<std::string> SessionManager::get_active_sessions() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::string> active_sessions;
  active_sessions.reserve(sessions_.size());

  for (const auto& [client_id, session] : sessions_) {
    active_sessions.push_back(client_id);
  }

  return active_sessions;
}

size_t SessionManager::session_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

bool SessionManager::session_exists(const std::string& client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.find(client_id) != sessions_.end();
}

bool SessionManager::set_paused(const std::string& client_id, bool paused) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = sessions_.find(client_id);
  if (it == sessions_.end()) {
    return false;
  }

  it->second.paused = paused;
  return true;
}

bool SessionManager::is_paused(const std::string& client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = sessions_.find(client_id);
  if (it == sessions_.end()) {
    return false;  // Non-existent sessions are not paused
  }

  return it->second.paused;
}

}  // namespace pj_ros_bridge
