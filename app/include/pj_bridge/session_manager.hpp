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

#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pj_bridge {

struct Session {
  std::string client_id;
  std::unordered_map<std::string, double> subscribed_topics;
  /// Topics for which this session currently holds a middleware subscription
  /// ref. Invariant: a ref is held iff the topic is in this set — pause
  /// releases all refs, resume re-acquires them, and cleanup releases exactly
  /// the held ones (never relying on the paused flag).
  std::unordered_set<std::string> ref_held_topics;
  std::chrono::steady_clock::time_point last_heartbeat;
  std::chrono::steady_clock::time_point created_at;
  bool paused{false};
  /// True if this session opted in to server-initiated `topics_changed`
  /// notifications via the `subscribe_topic_updates` command.
  bool topic_updates{false};
};

class SessionManager {
 public:
  explicit SessionManager(double timeout_seconds = 10.0);

  bool create_session(const std::string& client_id);
  bool update_heartbeat(const std::string& client_id);
  bool get_session(const std::string& client_id, Session& session) const;
  bool update_subscriptions(const std::string& client_id, const std::unordered_map<std::string, double>& topics);
  bool add_subscription(const std::string& client_id, const std::string& topic, double rate);
  bool remove_subscription(const std::string& client_id, const std::string& topic);
  std::unordered_map<std::string, double> get_subscriptions(const std::string& client_id) const;
  bool remove_session(const std::string& client_id);
  std::vector<std::string> get_timed_out_sessions();
  std::vector<std::string> get_active_sessions() const;
  size_t session_count() const;
  bool session_exists(const std::string& client_id) const;
  bool set_paused(const std::string& client_id, bool paused);
  bool is_paused(const std::string& client_id) const;
  bool set_topic_updates(const std::string& client_id, bool enabled);
  bool wants_topic_updates(const std::string& client_id) const;
  bool set_ref_held(const std::string& client_id, const std::string& topic, bool held);
  std::unordered_set<std::string> get_ref_held_topics(const std::string& client_id) const;

 private:
  std::unordered_map<std::string, Session> sessions_;
  mutable std::mutex mutex_;
  std::chrono::duration<double> timeout_duration_;
};

}  // namespace pj_bridge
