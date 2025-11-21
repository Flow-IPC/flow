/* Flow
 * Copyright 2023 Akamai Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy
 * of the License at
 *
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in
 * writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing
 * permissions and limitations under the License. */

#include "flow/log/log.hpp"
#include "flow/log/simple_ostream_logger.hpp"
#include "flow/util/util.hpp"
#include <gtest/gtest.h>
#include <type_traits>

namespace flow::log::test
{

namespace
{
using std::string;
} // Anonymous namespace

// Yes... this is very cheesy... but this is a test, so I don't really care.
#define CTX util::ostream_op_string("Caller context [", FLOW_UTIL_WHERE_AM_I_STR(), "].")

// @todo Many more things to test in log.?pp surely.  Gotta start somewhere though!

TEST(Log_context, Interface)
{
  using std::swap; // ADL-swap.

  /* Log_context[_mt] is simple, and its essential aspects will be tested here -- aside from the thread-safety
   * aspects of Log_context_mt (@todo).  One area that is somewhat finicky is Log_context_mt's copy, move, swap
   * facilities; due to mutex details they're not as straightforward as one normally sees; so we check those;
   * it would've been easy to make a mistake that would not be obviously visible. */

  const auto test_type = [](auto type_specimen)
  {
    using Log_context_t = decltype(type_specimen); // Log_context_t is either Log_context or ..._mt; identical APIs.
    Config cfg;
    Simple_ostream_logger logger1{&cfg};
    Simple_ostream_logger logger2{&cfg};
    const auto comp1 = Flow_log_component::S_UTIL;
    const auto comp2 = Flow_log_component::S_LOG;
    const auto comp0 = Component{};
    EXPECT_TRUE(comp0.empty());

    // @todo Maybe should implement operator==(Component, Component)?  Then use it/test it here?

    /* This isn't a test of Component functionality; it assumes that works; it just checks whether they're
     * equal and ensures this matches what our test is expecting that for those particular c1 and c2. */
    const auto comps_equal = [](Component c1, Component c2, const string& ctx)
    {
      if (c1.empty() && c2.empty())
      {
        return; // So equal then.
      }
      // Better both be not-empty.
      EXPECT_EQ(c1.empty(), c2.empty()) << ctx;

      EXPECT_EQ(c1.payload_type(), c2.payload_type()) << ctx;
      EXPECT_EQ(int(c1.payload_enum_raw_value()), int(c2.payload_enum_raw_value())) << ctx;
    };

    Log_context_t ctx1;
    EXPECT_TRUE(ctx1.get_log_component().empty());
    comps_equal(ctx1.get_log_component(), comp0, CTX);
    EXPECT_EQ(ctx1.get_logger(), nullptr);
    ctx1.set_logger(&logger2);
    EXPECT_EQ(ctx1.get_logger(), &logger2);
    ctx1 = Log_context_t{&logger1, comp1};
    EXPECT_FALSE(ctx1.get_log_component().empty());
    comps_equal(ctx1.get_log_component(), comp1, CTX);
    EXPECT_EQ(ctx1.get_logger(), &logger1);

    Log_context_t ctx2{&logger2};
    EXPECT_TRUE(ctx2.get_log_component().empty());
    comps_equal(ctx2.get_log_component(), comp0, CTX);
    EXPECT_EQ(ctx2.get_logger(), &logger2);
    ctx2.set_logger(&logger1);
    EXPECT_EQ(ctx2.get_logger(), &logger1);
    ctx2 = Log_context_t{&logger2, comp2};
    EXPECT_FALSE(ctx2.get_log_component().empty());
    comps_equal(ctx2.get_log_component(), comp2, CTX);
    EXPECT_EQ(ctx2.get_logger(), &logger2);

    swap(ctx1, ctx2);
    comps_equal(ctx1.get_log_component(), comp2, CTX);
    EXPECT_EQ(ctx1.get_logger(), &logger2);
    comps_equal(ctx2.get_log_component(), comp1, CTX);
    EXPECT_EQ(ctx2.get_logger(), &logger1);

    ctx1 = ctx2; // Copy-assign.
    comps_equal(ctx1.get_log_component(), comp1, CTX);
    EXPECT_EQ(ctx1.get_logger(), &logger1);
    comps_equal(ctx2.get_log_component(), comp1, CTX);
    EXPECT_EQ(ctx2.get_logger(), &logger1);
    ctx1 = Log_context_t{&logger2, comp2};

    ctx2 = std::move(ctx1); // Move-assign.
    EXPECT_TRUE(ctx1.get_log_component().empty());
    EXPECT_EQ(ctx1.get_logger(), nullptr);
    comps_equal(ctx2.get_log_component(), comp2, CTX);
    EXPECT_EQ(ctx2.get_logger(), &logger2);

    Log_context_t ctx3{ctx2}; // Copy-ct.
    comps_equal(ctx3.get_log_component(), comp2, CTX);
    EXPECT_EQ(ctx3.get_logger(), &logger2);
    comps_equal(ctx2.get_log_component(), comp2, CTX);
    EXPECT_EQ(ctx2.get_logger(), &logger2);

    Log_context_t ctx4{std::move(ctx3)}; // Move-ct.
    EXPECT_TRUE(ctx3.get_log_component().empty());
    EXPECT_EQ(ctx3.get_logger(), nullptr);
    comps_equal(ctx4.get_log_component(), comp2, CTX);
    EXPECT_EQ(ctx4.get_logger(), &logger2);
  }; // const auto test_type =

  test_type(Log_context{});
  test_type(Log_context_mt{});
} // TEST(Log_context, Interface)

} // namespace flow::log::test
