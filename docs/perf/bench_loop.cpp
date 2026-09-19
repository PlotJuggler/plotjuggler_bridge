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

#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;
static double cpu_ms() {
  rusage r;
  getrusage(RUSAGE_SELF, &r);
  return r.ru_utime.tv_sec * 1000.0 + r.ru_utime.tv_usec / 1000.0 + r.ru_stime.tv_sec * 1000.0 +
         r.ru_stime.tv_usec / 1000.0;
}

// run_standalone_event_loop(): 1 ms sleep, process_requests() every iteration.
static void idle_loop_today(double seconds) {
  std::mutex state_mu, queue_mu;
  std::queue<int> incoming;
  bool initialized = true;
  auto end = clk::now() + std::chrono::duration<double>(seconds);
  double c0 = cpu_ms();
  auto last_publish = clk::now();
  auto interval = std::chrono::nanoseconds(20'000'000);  // 50 Hz
  long iters = 0;
  while (clk::now() < end) {
    for (int i = 0; i < 256; i++) {  // process_requests() drain loop
      {
        std::lock_guard<std::mutex> l(state_mu);
        if (!initialized) {
          break;
        }
      }
      std::lock_guard<std::mutex> l(queue_mu);
      if (incoming.empty()) {
        break;
      }
    }
    auto now = clk::now();
    if (now - last_publish >= interval) {
      last_publish += interval;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    iters++;
  }
  printf(
      "  today  (1 ms sleep) : %8.1f ms CPU over %.0fs idle = %5.2f%% of one core  (%ld wakeups/s)\n", cpu_ms() - c0,
      seconds, (cpu_ms() - c0) / (seconds * 1000) * 100, (long)(iters / seconds));
}

// Deadline-driven: sleep until the next scheduled event instead of every 1 ms.
static void idle_loop_deadline(double seconds) {
  auto end = clk::now() + std::chrono::duration<double>(seconds);
  double c0 = cpu_ms();
  auto interval = std::chrono::nanoseconds(20'000'000);
  auto next_publish = clk::now() + interval;
  long iters = 0;
  while (clk::now() < end) {
    std::this_thread::sleep_until(next_publish);
    next_publish += interval;
    iters++;
  }
  printf(
      "  deadline-driven     : %8.1f ms CPU over %.0fs idle = %5.2f%% of one core  (%ld wakeups/s)\n", cpu_ms() - c0,
      seconds, (cpu_ms() - c0) / (seconds * 1000) * 100, (long)(iters / seconds));
}

// send_binary(): every frame is copied into a std::string before sendBinary(), per client.
static void bench_send_copy(size_t frame_bytes, int clients, int iters) {
  std::vector<uint8_t> frame(frame_bytes, 0xAB);
  volatile size_t sink = 0;
  auto t0 = clk::now();
  for (int i = 0; i < iters; i++) {
    for (int c = 0; c < clients; c++) {
      std::string s(reinterpret_cast<const char*>(frame.data()), frame.size());  // the extra copy
      sink += s.size();
    }
  }
  double a = std::chrono::duration<double, std::milli>(clk::now() - t0).count() / iters;
  printf(
      "  frame %8zu B x %d clients : %7.4f ms/frame of pure std::string copy -> %4.1f%% of a core @50Hz\n", frame_bytes,
      clients, a, a * 50 / 10);
}

int main() {
  printf("=== standalone event loop, fully idle (no clients, no messages) ===\n");
  idle_loop_today(3.0);
  idle_loop_deadline(3.0);
  printf("\n=== send_binary(): vector -> std::string copy per client ===\n");
  bench_send_copy(8 * 1024, 1, 20000);
  bench_send_copy(256 * 1024, 4, 2000);
  bench_send_copy(2 * 1024 * 1024, 4, 200);
  return 0;
}
