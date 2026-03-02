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
#include <atomic>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>
#include <vector>

#include "pj_bridge/bridge_server.hpp"
#include "pj_bridge/middleware/websocket_middleware.hpp"
#include "pj_bridge_rti/dds_subscription_manager.hpp"
#include "pj_bridge_rti/dds_topic_discovery.hpp"

namespace {
std::atomic<bool> g_shutdown{false};

void signal_handler(int /*signum*/) {
  g_shutdown.store(true);
}
}  // namespace

int main(int argc, char* argv[]) {
  CLI::App app{"pj_bridge RTI DDS Backend"};

  std::vector<int32_t> domain_ids;
  int port = 8080;
  double publish_rate = 50.0;
  double session_timeout = 10.0;
  std::string qos_profile;
  std::string log_level = "info";
  bool stats_enabled = false;

  app.add_option("--domains,-d", domain_ids, "DDS domain IDs")->required()->expected(1, -1);
  app.add_option("--port,-p", port, "WebSocket port")->default_val(8080)->check(CLI::Range(1, 65535));
  app.add_option("--publish-rate", publish_rate, "Aggregation publish rate in Hz")->default_val(50.0);
  app.add_option("--session-timeout", session_timeout, "Session timeout in seconds")->default_val(10.0);
  app.add_option("--qos-profile", qos_profile, "QoS profile XML file path");
  app.add_option("--log-level", log_level, "Log level (trace, debug, info, warn, error)")->default_val("info");
  app.add_flag("--stats", stats_enabled, "Print statistics every 5 seconds");

  CLI11_PARSE(app, argc, argv);

  spdlog::set_level(spdlog::level::from_str(log_level));

  spdlog::info("pj_bridge (RTI DDS backend) starting...");
  spdlog::info("  Domains: {}", fmt::join(domain_ids, ", "));
  spdlog::info("  Port: {}", port);
  spdlog::info("  Publish rate: {:.1f} Hz", publish_rate);
  spdlog::info("  Session timeout: {:.1f} s", session_timeout);
  if (!qos_profile.empty()) {
    spdlog::info("  QoS profile: {}", qos_profile);
  }

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  try {
    // Create DDS components (implement TopicSourceInterface and SubscriptionManagerInterface directly)
    auto topic_source = std::make_shared<pj_bridge::DdsTopicDiscovery>(domain_ids, qos_profile);
    auto sub_manager = std::make_shared<pj_bridge::DdsSubscriptionManager>(*topic_source);

    // Create WebSocket middleware
    auto middleware = std::make_shared<pj_bridge::WebSocketMiddleware>();

    // Create bridge server
    pj_bridge::BridgeServer server(topic_source, sub_manager, middleware, port, session_timeout, publish_rate);

    if (!server.initialize()) {
      spdlog::error("Failed to initialize bridge server");
      return 1;
    }

    spdlog::info("Bridge server initialized, entering event loop");

    // Event loop
    using clock = std::chrono::steady_clock;
    auto publish_interval =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(1.0 / publish_rate));
    auto last_publish = clock::now();
    auto last_timeout_check = clock::now();
    auto last_stats_print = clock::now();
    uint64_t prev_bytes_published = 0;

    while (!g_shutdown.load()) {
      server.process_requests();

      auto now = clock::now();

      if (now - last_publish >= publish_interval) {
        server.publish_aggregated_messages();
        last_publish += publish_interval;
      }

      if (now - last_timeout_check >= std::chrono::seconds(1)) {
        server.check_session_timeouts();
        last_timeout_check = now;
      }

      if (stats_enabled && now - last_stats_print >= std::chrono::seconds(5)) {
        auto elapsed = std::chrono::duration<double>(now - last_stats_print).count();
        auto snapshot = server.snapshot_and_reset_stats();

        std::string stats_msg = "=== Stats ===\n  DDS receive rates:";
        if (snapshot.topic_receive_counts.empty()) {
          stats_msg += "\n    (no subscriptions)";
        } else {
          for (const auto& [topic, count] : snapshot.topic_receive_counts) {
            stats_msg += fmt::format("\n    {}: {:.1f} msg/s", topic, static_cast<double>(count) / elapsed);
          }
        }

        stats_msg +=
            fmt::format("\n  Publish frequency: {:.1f} Hz", static_cast<double>(snapshot.publish_cycles) / elapsed);
        uint64_t delta_bytes = snapshot.total_bytes_published - prev_bytes_published;
        prev_bytes_published = snapshot.total_bytes_published;
        stats_msg +=
            fmt::format("\n  Sent: {:.2f} MB/s", static_cast<double>(delta_bytes) / elapsed / (1024.0 * 1024.0));

        spdlog::info(stats_msg);
        last_stats_print = now;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    spdlog::info("Shutting down...");

    // Explicit ordered shutdown to avoid use-after-free:
    // 1. Clear DDS message callback
    spdlog::debug("[shutdown] Clearing DDS message callback...");
    sub_manager->set_message_callback(nullptr);
    // 2. Unsubscribe all DDS readers
    spdlog::debug("[shutdown] Unsubscribing all DDS readers...");
    sub_manager->unsubscribe_all();
    // 3. Shutdown WebSocket server
    spdlog::debug("[shutdown] Shutting down WebSocket middleware...");
    middleware->shutdown();
    spdlog::debug("[shutdown] Cleanup complete, exiting scope (destructors will run)...");
  } catch (const std::exception& e) {
    spdlog::error("Fatal error: {}", e.what());
    return 1;
  }

  return 0;
}
