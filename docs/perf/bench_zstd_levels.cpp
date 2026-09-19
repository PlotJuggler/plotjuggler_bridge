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

#include <zstd.h>

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
using clk = std::chrono::steady_clock;
// Mixed payload: ~float-array CDR (compressible) and image-like (barely compressible).
static std::vector<uint8_t> mk(size_t n, bool imagey) {
  std::vector<uint8_t> v(n);
  std::mt19937 rng(7);
  for (size_t i = 0; i < n; i++) {
    v[i] = imagey ? static_cast<uint8_t>(rng()) : static_cast<uint8_t>((i % 8 < 2) ? rng() : 0);
  }
  return v;
}
int main() {
  printf("%-10s %6s %12s %12s %10s\n", "payload", "level", "ms/MiB", "ratio", "MB/s");
  for (bool imagey : {false, true}) {
    auto d = mk(1 << 20, imagey);
    for (int lvl : {1, -1, -3, -5}) {
      ZSTD_CCtx* c = ZSTD_createCCtx();
      std::vector<uint8_t> out(ZSTD_compressBound(d.size()));
      size_t cs = 0;
      auto t0 = clk::now();
      for (int i = 0; i < 300; i++) {
        cs = ZSTD_compressCCtx(c, out.data(), out.size(), d.data(), d.size(), lvl);
      }
      double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count() / 300;
      printf(
          "%-10s %6d %12.4f %12.2f %10.0f\n", imagey ? "image-like" : "float-CDR", lvl, ms, (double)d.size() / cs,
          d.size() / ms / 1000.0);
      ZSTD_freeCCtx(c);
    }
  }
  return 0;
}
