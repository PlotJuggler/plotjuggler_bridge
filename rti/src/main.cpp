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
#include <optional>
#include <string>
#include <vector>

#include "pj_bridge/middleware/websocket_middleware.hpp"
#include "pj_bridge/standalone_event_loop.hpp"
#include "pj_bridge/whitelist_filter.hpp"
#include "pj_bridge_rti/dds_subscription_manager.hpp"
#include "pj_bridge_rti/dds_topic_discovery.hpp"

int main(int argc, char* argv[]) {
  CLI::App app{"pj_bridge RTI DDS Backend"};

  std::vector<int32_t> domain_ids;
  int port = 9090;
  double publish_rate = 50.0;
  double session_timeout = 10.0;
  std::string qos_profile;
  std::string log_level = "info";
  bool stats_enabled = false;
  std::vector<std::string> topic_whitelist{".*"};
  double topic_poll_interval = 1.0;
  int client_backlog_size = 100;
  int heavy_frame_threshold_bytes = 262144;
  std::string certfile;
  std::string keyfile;

  app.add_option("--domains,-d", domain_ids, "DDS domain IDs")->required()->expected(1, -1);
  app.add_option("--port,-p", port, "WebSocket port")->default_val(9090)->check(CLI::Range(1, 65535));
  app.add_option("--publish-rate", publish_rate, "Aggregation publish rate in Hz")->default_val(50.0);
  app.add_option("--session-timeout", session_timeout, "Session timeout in seconds")->default_val(10.0);
  app.add_option("--qos-profile", qos_profile, "QoS profile XML file path");
  app.add_option("--log-level", log_level, "Log level (trace, debug, info, warn, error)")->default_val("info");
  app.add_flag("--stats", stats_enabled, "Print statistics every 5 seconds");
  app.add_option(
         "--topic-poll-interval", topic_poll_interval,
         "Interval in seconds between topics_changed notification polls (0 disables)")
      ->default_val(1.0);
  app.add_option(
         "--client-backlog-size", client_backlog_size,
         "Max frames queued per slow client before dropping the oldest (backpressure)")
      ->default_val(100)
      ->check(CLI::Range(1, 1000000));
  app.add_option(
         "--heavy-frame-threshold-bytes", heavy_frame_threshold_bytes,
         "Isolate messages this size (bytes) or larger into their own size-class frames; "
         "0 disables (keep below the 1 MiB socket watermark)")
      ->default_val(262144)
      ->check(CLI::Range(0, 1000000000));
  // Bound variable is already initialized to {".*"} (match everything); CLI11
  // leaves it untouched if the flag is not passed, so no default_val() is
  // needed (and default_val() on a vector<string> would round-trip through a
  // single joined string, which is not what we want here).
  app.add_option("--topic-whitelist", topic_whitelist, "Topic whitelist regex patterns (full-match, ECMAScript)");

  auto certfile_opt =
      app.add_option("--certfile", certfile, "TLS server certificate file (enables wss://, requires --keyfile)");
  auto keyfile_opt = app.add_option("--keyfile", keyfile, "TLS private key file (enables wss://, requires --certfile)");
  certfile_opt->needs(keyfile_opt);
  keyfile_opt->needs(certfile_opt);

  CLI11_PARSE(app, argc, argv);

  bool tls_enabled = !certfile.empty();

  spdlog::set_level(spdlog::level::from_str(log_level));

  spdlog::info("pj_bridge (RTI DDS backend) starting...");
  spdlog::info("  Domains: {}", fmt::join(domain_ids, ", "));
  spdlog::info("  Port: {}", port);
  spdlog::info("  Publish rate: {:.1f} Hz", publish_rate);
  spdlog::info("  Session timeout: {:.1f} s", session_timeout);
  if (!qos_profile.empty()) {
    spdlog::info("  QoS profile: {}", qos_profile);
  }
  spdlog::info("  Topic whitelist: {}", fmt::join(topic_whitelist, ", "));
  spdlog::info("  Topic poll interval: {:.1f} s", topic_poll_interval);
  spdlog::info("  Client backlog size: {}", client_backlog_size);
  spdlog::info("  Heavy frame threshold: {} bytes", heavy_frame_threshold_bytes);
  spdlog::info("  TLS: {}", tls_enabled ? "enabled" : "disabled");

  auto whitelist_result = pj_bridge::WhitelistFilter::create(topic_whitelist);
  if (!whitelist_result) {
    spdlog::error("Invalid --topic-whitelist: {}", whitelist_result.error());
    return 1;
  }

  if (topic_poll_interval < 0.0) {
    spdlog::error("Invalid --topic-poll-interval: {:.1f} (must be >= 0; 0 disables polling)", topic_poll_interval);
    return 1;
  }

  if (heavy_frame_threshold_bytes > 0 &&
      static_cast<size_t>(heavy_frame_threshold_bytes) >= pj_bridge::WebSocketMiddleware::kSocketBufferHighWatermark) {
    spdlog::warn(
        "--heavy-frame-threshold-bytes ({}) >= socket watermark ({}): messages between the two sizes stay 'light' and "
        "queue instead of shedding under congestion — keep the threshold below the watermark",
        heavy_frame_threshold_bytes, pj_bridge::WebSocketMiddleware::kSocketBufferHighWatermark);
  }

  try {
    auto topic_source = std::make_shared<pj_bridge::DdsTopicDiscovery>(domain_ids, qos_profile);
    auto sub_manager = std::make_shared<pj_bridge::DdsSubscriptionManager>(*topic_source);
    std::optional<pj_bridge::TlsConfig> tls_config;
    if (tls_enabled) {
      tls_config = pj_bridge::TlsConfig{certfile, keyfile};
    }
    auto middleware =
        std::make_shared<pj_bridge::WebSocketMiddleware>(static_cast<size_t>(client_backlog_size), tls_config);

    pj_bridge::BridgeServer server(
        topic_source, sub_manager, middleware,
        {port, session_timeout, publish_rate, std::move(whitelist_result.value()),
         static_cast<size_t>(heavy_frame_threshold_bytes)});

    pj_bridge::run_standalone_event_loop(
        server, sub_manager, middleware, {port, publish_rate, session_timeout, stats_enabled, topic_poll_interval});
  } catch (const std::exception& e) {
    spdlog::error("Fatal error: {}", e.what());
    return 1;
  }

  return 0;
}
