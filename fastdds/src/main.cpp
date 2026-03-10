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

#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <string>
#include <vector>

#include "pj_bridge/middleware/websocket_middleware.hpp"
#include "pj_bridge/standalone_event_loop.hpp"
#include "pj_bridge_fastdds/fastdds_subscription_manager.hpp"
#include "pj_bridge_fastdds/fastdds_topic_source.hpp"

int main(int argc, char* argv[]) {
  CLI::App app{"pj_bridge eProsima FastDDS Backend"};

  std::vector<int32_t> domain_ids;
  int port = 9090;
  double publish_rate = 50.0;
  double session_timeout = 10.0;
  std::string log_level = "info";
  bool stats_enabled = false;

  app.add_option("--domains,-d", domain_ids, "DDS domain IDs")->required()->expected(1, -1);
  app.add_option("--port,-p", port, "WebSocket port")->default_val(9090)->check(CLI::Range(1, 65535));
  app.add_option("--publish-rate", publish_rate, "Aggregation publish rate in Hz")->default_val(50.0);
  app.add_option("--session-timeout", session_timeout, "Session timeout in seconds")->default_val(10.0);
  app.add_option("--log-level", log_level, "Log level (trace, debug, info, warn, error)")->default_val("info");
  app.add_flag("--stats", stats_enabled, "Print statistics every 5 seconds");

  CLI11_PARSE(app, argc, argv);

  spdlog::set_level(spdlog::level::from_str(log_level));

  spdlog::info("pj_bridge (FastDDS backend) starting...");
  spdlog::info("  Domains: {}", fmt::join(domain_ids, ", "));
  spdlog::info("  Port: {}", port);
  spdlog::info("  Publish rate: {:.1f} Hz", publish_rate);
  spdlog::info("  Session timeout: {:.1f} s", session_timeout);

  try {
    auto topic_source = std::make_shared<pj_bridge::FastDdsTopicSource>(domain_ids);
    auto sub_manager = std::make_shared<pj_bridge::FastDdsSubscriptionManager>(*topic_source);
    auto middleware = std::make_shared<pj_bridge::WebSocketMiddleware>();

    pj_bridge::BridgeServer server(topic_source, sub_manager, middleware, port, session_timeout, publish_rate);

    pj_bridge::run_standalone_event_loop(
        server, sub_manager, middleware, {port, publish_rate, session_timeout, stats_enabled});
  } catch (const std::exception& e) {
    spdlog::error("Fatal error: {}", e.what());
    return 1;
  }

  return 0;
}
