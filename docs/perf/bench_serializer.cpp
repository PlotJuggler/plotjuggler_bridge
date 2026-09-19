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

// Replicates AggregatedMessageSerializer::finalize() exactly vs. a pooled variant.
#include <zstd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>

using clk = std::chrono::steady_clock;
static double ms_since(clk::time_point t) {
  return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}
static constexpr size_t kHdr = 16;

// Semi-compressible payload, like CDR sensor data.
static std::vector<uint8_t> payload(size_t n) {
  std::vector<uint8_t> v(n);
  std::mt19937 rng(1);
  for (size_t i = 0; i < n; i++) {
    v[i] = (i % 97 == 0) ? static_cast<uint8_t>(rng()) : static_cast<uint8_t>(i & 0x3F);
  }
  return v;
}

// AS WRITTEN TODAY: a fresh AggregatedMessageSerializer per frame => createCCtx/freeCCtx
// per frame, and result(kHdr + compressBound) value-initializes (zeroes) the whole bound.
static double today(const std::vector<uint8_t>& data, int iters) {
  auto t0 = clk::now();
  volatile size_t sink = 0;
  for (int i = 0; i < iters; i++) {
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    size_t bound = ZSTD_compressBound(data.size());
    std::vector<uint8_t> result(kHdr + bound);  // zeroes ~bound bytes
    size_t cs = ZSTD_compressCCtx(cctx, result.data() + kHdr, bound, data.data(), data.size(), 1);
    if (ZSTD_isError(cs)) {
      throw std::runtime_error("zstd");
    }
    result.resize(kHdr + cs);
    sink += result.size();
    ZSTD_freeCCtx(cctx);
  }
  return ms_since(t0) / iters;
}

// POOLED: one long-lived cctx + one reused output buffer (no per-frame alloc, no zeroing).
static double pooled(const std::vector<uint8_t>& data, int iters) {
  ZSTD_CCtx* cctx = ZSTD_createCCtx();
  std::vector<uint8_t> result;
  auto t0 = clk::now();
  volatile size_t sink = 0;
  for (int i = 0; i < iters; i++) {
    size_t bound = ZSTD_compressBound(data.size());
    if (result.capacity() < kHdr + bound) {
      result.reserve(kHdr + bound);
    }
    result.resize(kHdr + bound);  // capacity already there, no realloc
    size_t cs = ZSTD_compressCCtx(cctx, result.data() + kHdr, bound, data.data(), data.size(), 1);
    if (ZSTD_isError(cs)) {
      throw std::runtime_error("zstd");
    }
    result.resize(kHdr + cs);
    sink += result.size();
  }
  double r = ms_since(t0) / iters;
  ZSTD_freeCCtx(cctx);
  return r;
}

// No compression at all (what a heavy/already-compressed frame could do).
static double raw_copy(const std::vector<uint8_t>& data, int iters) {
  std::vector<uint8_t> out;
  auto t0 = clk::now();
  volatile size_t sink = 0;
  for (int i = 0; i < iters; i++) {
    out.resize(kHdr + data.size());
    std::memcpy(out.data() + kHdr, data.data(), data.size());
    sink += out.size();
  }
  return ms_since(t0) / iters;
}

int main() {
  printf("=== finalize() cost: fresh-serializer-per-frame vs pooled ===\n");
  printf("%10s %12s %12s %12s %10s\n", "frame", "today(ms)", "pooled(ms)", "memcpy(ms)", "speedup");
  for (size_t n : {4096ul, 65536ul, 262144ul, 1048576ul, 4194304ul}) {
    auto d = payload(n);
    int iters = n > 1000000 ? 200 : 2000;
    double a = today(d, iters), b = pooled(d, iters), c = raw_copy(d, iters);
    printf("%9zuB %12.4f %12.4f %12.4f %9.2fx\n", n, a, b, c, a / b);
  }
  // A 30 Hz camera at the default 256 KiB heavy threshold: per-frame cost -> % of one core.
  printf("\n=== 1 MiB heavy frame @30Hz (one camera topic) ===\n");
  auto d = payload(1048576);
  double a = today(d, 300), b = pooled(d, 300), c = raw_copy(d, 300);
  printf("  today  %6.3f ms/frame -> %5.1f%% of one core\n", a, a * 30 / 10);
  printf("  pooled %6.3f ms/frame -> %5.1f%% of one core\n", b, b * 30 / 10);
  printf("  no-zstd%6.3f ms/frame -> %5.1f%% of one core\n", c, c * 30 / 10);
  return 0;
}
