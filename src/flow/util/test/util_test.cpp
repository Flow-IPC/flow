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

#include "flow/util/util.hpp"
#include <gtest/gtest.h>
#include <iomanip>
#include <memory>

namespace flow::util::test
{

namespace
{
using std::string;
} // Anonymous namespace

// Yes... this is very cheesy... but this is a test, so I don't really care.
#define CTX FLOW_UTIL_WHERE_AM_I_STR()

TEST(Util_scoped_setter, Interface)
{
  string str;
  int num = 0;
  EXPECT_TRUE(str.empty());
  EXPECT_EQ(num, 0);
  {
    Scoped_setter<string> set{&str, "abc"};
    Scoped_setter<int> set2{&num, 10};
    EXPECT_EQ(str, "abc");
    EXPECT_EQ(num, 10);
    {
      // Test out Scoped_setter move ctor... in 1-2 ways (return, direct).
      auto set2
        = ([&]() -> auto { return std::make_unique<Scoped_setter<int>>(&num, 5); })
          ();
      auto set
        = ([&]() -> auto { return std::make_unique<Scoped_setter<string>>(&str, "def"); })
          ();
      Scoped_setter<int> set2_a{std::move(*set2)};
      Scoped_setter<string> set_a{std::move(*set)};
      EXPECT_EQ(str, "def");
      EXPECT_EQ(num, 5);
      set2.reset(); // Destroy the moved-from guys -- this should have no effect.
      set.reset();
      EXPECT_EQ(str, "def");
      EXPECT_EQ(num, 5);
    }
    EXPECT_EQ(str, "abc");
    EXPECT_EQ(num, 10);
  }
  EXPECT_TRUE(str.empty());
  EXPECT_EQ(num, 0);
} // TEST(Util_scoped_setter, Interface)

TEST(Util_misc, Interface)
{
  // ostream_op_string() is built on top of vaious things, so this alone is a pretty decent check.  @todo More.
  EXPECT_EQ(ostream_op_string("abc[", 2, "] flag[", true, "]:", std::hex, 12),
            "abc[2] flag[1]:c");

  EXPECT_EQ(ceil_div(0, 1024), 0);
  EXPECT_EQ(ceil_div(1, 1024u), 1);
  EXPECT_EQ(ceil_div(2, 1024l), 1);
  EXPECT_EQ(ceil_div(1023, 1024ul), 1);
  EXPECT_EQ(ceil_div(1024, 1024ll), 1);
  EXPECT_EQ(ceil_div(1025, 1024ull), 2);
  EXPECT_EQ(ceil_div(1026, 1024ll), 2);
  EXPECT_EQ(ceil_div(2047, 1024ul), 2);
  EXPECT_EQ(ceil_div(2048, 1024l), 2);
  EXPECT_EQ(ceil_div(2049, 1024u), 3);
  EXPECT_EQ(ceil_div(2050, 1024), 3);
  EXPECT_EQ(ceil_div(8192, 1024u), 8);
  EXPECT_EQ(ceil_div(8193, 1024l), 9);

  EXPECT_EQ(round_to_multiple(0, 1024), 0 * 1024);
  EXPECT_EQ(round_to_multiple(1, 1024u), 1 * 1024);
  EXPECT_EQ(round_to_multiple(2, 1024l), 1 * 1024);
  EXPECT_EQ(round_to_multiple(1023, 1024ul), 1 * 1024);
  EXPECT_EQ(round_to_multiple(1024, 1024ll), 1 * 1024);
  EXPECT_EQ(round_to_multiple(1025, 1024ull), 2 * 1024);
  EXPECT_EQ(round_to_multiple(1026, 1024ll), 2 * 1024);
  EXPECT_EQ(round_to_multiple(2047, 1024ul), 2 * 1024);
  EXPECT_EQ(round_to_multiple(2048, 1024l), 2 * 1024);
  EXPECT_EQ(round_to_multiple(2049, 1024u), 3 * 1024);
  EXPECT_EQ(round_to_multiple(2050, 1024), 3 * 1024);
  EXPECT_EQ(round_to_multiple(8192, 1024u), 8 * 1024);
  EXPECT_EQ(round_to_multiple(8193, 1024l), 9 * 1024);
} // TEST(Util_misc, Interface)

} // namespace flow::util::test
