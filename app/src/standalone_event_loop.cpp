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

#include "pj_bridge/standalone_event_loop.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

namespace pj_bridge {

namespace {
std::atomic<bool> g_shutdown{false};

void signal_handler(int /*signum*/) {
  g_shutdown.store(true);
}
}  // namespace

void run_standalone_event_loop(
    BridgeServer& server, std::shared_ptr<SubscriptionManagerInterface> sub_manager,
    std::shared_ptr<WebSocketMiddleware> middleware, const StandaloneConfig& config) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  if (!server.initialize()) {
    spdlog::error("Failed to initialize bridge server");
    return;
  }

  spdlog::info("Bridge server initialized, entering event loop");

  using clock = std::chrono::steady_clock;
  auto publish_interval =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(1.0 / config.publish_rate));
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

    if (config.stats_enabled && now - last_stats_print >= std::chrono::seconds(5)) {
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
      stats_msg += fmt::format("\n  Sent: {:.2f} MB/s", static_cast<double>(delta_bytes) / elapsed / (1024.0 * 1024.0));

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
}

}  // namespace pj_bridge
