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

#include <memory>

#include "pj_bridge/bridge_server.hpp"
#include "pj_bridge/middleware/websocket_middleware.hpp"
#include "pj_bridge/subscription_manager_interface.hpp"

namespace pj_bridge {

struct StandaloneConfig {
  int port;
  double publish_rate;
  double session_timeout;
  bool stats_enabled;
};

/// Runs the standalone event loop until SIGINT/SIGTERM.
///
/// Handles:
/// - process_requests() every iteration
/// - publish_aggregated_messages() at publish_rate
/// - check_session_timeouts() every 1s
/// - optional stats printing every 5s
/// - ordered shutdown: clear callback -> unsubscribe_all -> middleware shutdown
void run_standalone_event_loop(
    BridgeServer& server, std::shared_ptr<SubscriptionManagerInterface> sub_manager,
    std::shared_ptr<WebSocketMiddleware> middleware, const StandaloneConfig& config);

}  // namespace pj_bridge
