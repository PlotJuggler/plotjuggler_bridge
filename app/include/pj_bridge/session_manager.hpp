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
#include <vector>

namespace pj_bridge {

struct Session {
  std::string client_id;
  std::unordered_map<std::string, double> subscribed_topics;
  std::chrono::steady_clock::time_point last_heartbeat;
  std::chrono::steady_clock::time_point created_at;
  bool paused{false};
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

 private:
  std::unordered_map<std::string, Session> sessions_;
  mutable std::mutex mutex_;
  std::chrono::duration<double> timeout_duration_;
};

}  // namespace pj_bridge
