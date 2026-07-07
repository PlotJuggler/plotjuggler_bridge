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

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "pj_bridge/whitelist_filter.hpp"

namespace pj_bridge {
namespace {

TEST(WhitelistFilterTest, DefaultConstructedMatchesEverything) {
  WhitelistFilter filter;
  EXPECT_TRUE(filter.matches("/anything"));
  EXPECT_TRUE(filter.matches("/foo/bar"));
  EXPECT_TRUE(filter.matches(""));
}

TEST(WhitelistFilterTest, CreateSucceedsForValidPatterns) {
  auto result = WhitelistFilter::create({"/camera.*", "/imu"});
  ASSERT_TRUE(result.has_value());
}

TEST(WhitelistFilterTest, CreateFailsForInvalidRegexAndNamesOffendingPattern) {
  auto result = WhitelistFilter::create({"/valid", "("});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("("), std::string::npos);
}

TEST(WhitelistFilterTest, FullMatchSemanticsRejectPartialMatch) {
  auto result = WhitelistFilter::create({"/cam"});
  ASSERT_TRUE(result.has_value());
  WhitelistFilter filter = result.value();

  // "/cam" must NOT match "/camera" — full match, not prefix match.
  EXPECT_FALSE(filter.matches("/camera"));
  EXPECT_TRUE(filter.matches("/cam"));
}

TEST(WhitelistFilterTest, WildcardPatternMatchesSubPaths) {
  auto result = WhitelistFilter::create({"/camera.*"});
  ASSERT_TRUE(result.has_value());
  WhitelistFilter filter = result.value();

  EXPECT_TRUE(filter.matches("/camera"));
  EXPECT_TRUE(filter.matches("/camera/image"));
  EXPECT_FALSE(filter.matches("/other"));
}

TEST(WhitelistFilterTest, MatchesIfAnyPatternMatches) {
  auto result = WhitelistFilter::create({"/foo", "/bar.*"});
  ASSERT_TRUE(result.has_value());
  WhitelistFilter filter = result.value();

  EXPECT_TRUE(filter.matches("/foo"));
  EXPECT_TRUE(filter.matches("/bar/baz"));
  EXPECT_FALSE(filter.matches("/qux"));
}

TEST(WhitelistFilterTest, EmptyPatternListMatchesEverything) {
  auto result = WhitelistFilter::create({});
  ASSERT_TRUE(result.has_value());
  WhitelistFilter filter = result.value();

  EXPECT_TRUE(filter.matches("/anything"));
}

}  // namespace
}  // namespace pj_bridge
