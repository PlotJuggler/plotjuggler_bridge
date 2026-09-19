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

// Replicates the exact code shapes from pj_bridge hot paths to quantify cost.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using clk = std::chrono::steady_clock;
static double ms_since(clk::time_point t) {
  return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

static std::vector<std::string> make_topics(int n) {
  std::vector<std::string> v;
  for (int i = 0; i < n; i++) {
    v.push_back("/robot/sensors/module_" + std::to_string(i) + "/measurement");
  }
  return v;
}

// ---- 1. publish_aggregated_messages() grouping, as written today ----
static void bench_grouping(int n_topics, int n_clients, int cycles) {
  std::unordered_map<std::string, std::unordered_map<std::string, double>> sessions;
  auto topics = make_topics(n_topics);
  for (int c = 0; c < n_clients; c++) {
    auto& s = sessions["client-" + std::to_string(c)];
    for (auto& t : topics) {
      s[t] = 0.0;
    }
  }
  std::vector<std::string> active;
  for (auto& [k, v] : sessions) {
    active.push_back(k);
  }

  auto t0 = clk::now();
  volatile size_t sink = 0;
  for (int cy = 0; cy < cycles; cy++) {
    using GroupKey = std::map<std::string, int>;
    std::map<GroupKey, std::vector<std::string>> groups;
    std::unordered_map<std::string, std::unordered_map<std::string, double>> client_subs;
    for (const auto& client_id : active) {
      auto subs = sessions[client_id];  // get_subscriptions(): deep copy
      GroupKey key;
      for (const auto& [topic, rate_hz] : subs) {
        key[topic] = static_cast<int>(rate_hz * 1000);
      }
      groups[key].push_back(client_id);
      client_subs[client_id] = std::move(subs);
    }
    sink += groups.size();
  }
  double total = ms_since(t0);
  printf(
      "  grouping  %4d topics x %d clients : %7.3f ms/cycle  -> %5.1f%% of one core @50Hz\n", n_topics, n_clients,
      total / cycles, (total / cycles) * 50.0 / 10.0);
}

// ---- 2. MessageBuffer::add_message() cleanup scan, per message ----
struct Msg {
  uint64_t ts, rx;
  std::shared_ptr<std::vector<std::byte>> data;
};
static void bench_cleanup(int n_topics, int msgs) {
  std::unordered_map<std::string, std::deque<Msg>> buffers;
  auto topics = make_topics(n_topics);
  auto payload = std::make_shared<std::vector<std::byte>>(256);
  for (auto& t : topics) {
    buffers[t].push_back({1, 1, payload});
  }

  auto t0 = clk::now();
  for (int i = 0; i < msgs; i++) {
    uint64_t now = 2;  // nothing is stale: pure scan cost
    for (auto& [topic, buf] : buffers) {
      while (!buf.empty()) {
        if (now > buf.front().rx && now - buf.front().rx > 1000000000ull) {
          buf.pop_front();
        } else {
          break;
        }
      }
    }
    for (auto it = buffers.begin(); it != buffers.end();) {
      if (it->second.empty()) {
        it = buffers.erase(it);
      } else {
        ++it;
      }
    }
  }
  double total = ms_since(t0);
  printf(
      "  cleanup scan  %4d topics : %8.4f ms/message -> at 5k msg/s = %5.1f%% of one core\n", n_topics, total / msgs,
      (total / msgs) * 5000.0 / 10.0);
}

// ---- 3. vector<byte>(n) zero-init + memcpy  vs  reserve + insert ----
static void bench_alloc(size_t bytes, int iters) {
  std::vector<std::byte> src(bytes);
  auto t0 = clk::now();
  volatile size_t sink = 0;
  for (int i = 0; i < iters; i++) {
    auto d = std::make_shared<std::vector<std::byte>>(bytes);  // zeroes
    std::memcpy(d->data(), src.data(), bytes);                 // then overwrites
    sink += d->size();
  }
  double a = ms_since(t0);
  t0 = clk::now();
  for (int i = 0; i < iters; i++) {
    auto d = std::make_shared<std::vector<std::byte>>();
    d->reserve(bytes);
    d->insert(d->end(), src.data(), src.data() + bytes);  // no zeroing
    sink += d->size();
  }
  double b = ms_since(t0);
  printf(
      "  ingest copy %8zu B : zero+memcpy %7.4f ms  reserve+insert %7.4f ms  (%.2fx)\n", bytes, a / iters, b / iters,
      a / b);
}

// ---- 4. per-message std::function copy + global stats mutex ----
using MessageCallback = std::function<void(const std::string&, std::shared_ptr<std::vector<std::byte>>, uint64_t)>;
static void bench_callback(int msgs) {
  std::mutex cb_mu, stats_mu;
  std::unordered_map<std::string, uint64_t> counts;
  std::string big_capture(64, 'x');
  MessageCallback stored = [big_capture](const std::string&, std::shared_ptr<std::vector<std::byte>>, uint64_t) {};
  std::string topic = "/robot/sensors/module_7/measurement";

  auto t0 = clk::now();
  for (int i = 0; i < msgs; i++) {
    MessageCallback cb;
    {
      std::lock_guard<std::mutex> l(cb_mu);
      cb = stored;
    }  // copies the std::function
    {
      std::lock_guard<std::mutex> l(stats_mu);
      counts[topic]++;
    }  // global stats mutex + hash
    if (cb) {
      cb(topic, nullptr, 0);
    }
  }
  double a = ms_since(t0);
  t0 = clk::now();
  uint64_t local = 0;
  for (int i = 0; i < msgs; i++) {
    stored(topic, nullptr, 0);
    local++;
  }
  double b = ms_since(t0);
  printf(
      "  per-msg cb copy + stats mutex : %8.4f us/msg  vs direct %8.4f us/msg  (%.1fx)\n", a * 1000 / msgs,
      b * 1000 / msgs, a / b);
}

int main() {
  printf("=== pj_bridge hot-path micro-benchmarks (replicated code shapes) ===\n");
  bench_grouping(100, 1, 200);
  bench_grouping(500, 1, 100);
  bench_grouping(500, 4, 50);
  bench_grouping(2000, 2, 20);
  printf("\n");
  bench_cleanup(50, 20000);
  bench_cleanup(200, 10000);
  bench_cleanup(1000, 3000);
  printf("\n");
  bench_alloc(1024, 20000);
  bench_alloc(64 * 1024, 5000);
  bench_alloc(2 * 1024 * 1024, 300);
  printf("\n");
  bench_callback(500000);
  return 0;
}
