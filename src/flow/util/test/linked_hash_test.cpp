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

#include "flow/util/linked_hash_map.hpp"
#include "flow/util/linked_hash_set.hpp"
#include "flow/util/util.hpp"
#include <gtest/gtest.h>

namespace flow::util::test
{

namespace
{
using std::string;
using std::vector;

using uint = unsigned int;
static uint s_n_copies = 0;

struct Obj
{
  string m_str;
  Obj() = default;
  Obj(const char* str) : m_str(str) {}
  Obj(const Obj& src) : m_str(src.m_str) { ++s_n_copies; }
  Obj(Obj&&) = default;
  Obj& operator=(const Obj& src) { if (this != &src) { m_str = src.m_str; ++s_n_copies; }
                                   return *this; }
  Obj& operator=(Obj&&) = default;
  bool operator==(const Obj& rhs) const { return m_str == rhs.m_str; }
};

size_t hash_value(const Obj& obj) { return boost::hash_value(obj.m_str); };

} // Anonymous namespace

// Yes... this is very cheesy... but this is a test, so I don't really care.
#define CTX ostream_op_string("Caller context [", FLOW_UTIL_WHERE_AM_I_STR(), "].")

TEST(Linked_hash, Interface)
{
  /* @todo I am sure there's more stuff to torture in Linked_hash_* for complete coverage; like custom
   *       equality and hash predicates for example.  Also n_buckets.
   * As it stands, this at least tests a bunch of things which is by far better than nothing. */

  using std::swap; // This enables proper ADL.

  const auto n_copies_check = [&](uint n, const string& ctx)
  {
    EXPECT_EQ(s_n_copies, n) << ctx;
    s_n_copies = 0;
  };

  const auto keys_check_set = [](const auto& vals, const vector<string>& exp, const string& ctx)
  {
    ASSERT_EQ(vals.size(), exp.size()) << ctx;
    ASSERT_EQ(vals.size() == 0, vals.empty()) << ctx;
    size_t idx = 0;
    for (const auto& val : vals)
    {
      EXPECT_EQ(val.m_str, exp[idx]) << ctx;
      ++idx;
    }
    for (auto rit = vals.crbegin(); rit != vals.crend(); ++rit)
    {
      --idx;
      EXPECT_EQ(rit->m_str, exp[idx]) << ctx;
    }
  };

  const auto keys_check_map = [](const auto& vals, const vector<string>& exp, const string& ctx)
  {
    ASSERT_EQ(vals.size(), exp.size()) << ctx;
    ASSERT_EQ(vals.size() == 0, vals.empty()) << ctx;
    size_t idx = 0;
    for (const auto& val : vals)
    {
      EXPECT_EQ(val.first, exp[idx]) << ctx;
      ++idx;
    }
    for (auto rit = vals.crbegin(); rit != vals.crend(); ++rit)
    {
      --idx;
      EXPECT_EQ(rit->first, exp[idx]) << ctx;
    }
  };

  const auto vals_check_map = [](const auto& vals, const vector<string>& exp, const string& ctx)
  {
    ASSERT_EQ(vals.size(), exp.size()) << ctx;
    size_t idx = 0;
    for (const auto& val : vals)
    {
      EXPECT_EQ(val.second.m_str, exp[idx]) << ctx;
      ++idx;
    }
    for (auto rit = vals.crbegin(); rit != vals.crend(); ++rit)
    {
      --idx;
      EXPECT_EQ(rit->second.m_str, exp[idx]) << ctx;
    }
  };

  using Map = Linked_hash_map<string, Obj>; // Obj can count copies thereof which we can check via n_copies_check().
  using Set = Linked_hash_set<Obj>; // Might as well test a custom-ish key type.

  { // Map test block.
    Map map1;

    s_n_copies = 0;
    map1.insert(Map::Value_movable{"b", "X"});
    n_copies_check(0, CTX); // Move-cting .insert() should be chosen => no Obj{"X"} copy.

    map1["a"] = "A"; // Becomes newest (first) because inserted.
    /* Inserting operator[] should internally avoid copying even the default-cted Obj{}.
     * Then the assignment to the Obj& should also be move-assignment of Obj{"A"} temporary (sanity-check of Obj code;
     * not really checking anything inside Linked_hash_map).
     * Anyway no copies => counter should be 0. */
    n_copies_check(0, CTX);

    map1["b"] = "B"; // Does not become newest (first) because already present (but mapped-value is replaced, X->B).
    n_copies_check(0, CTX);

    Map map2{{ { "a", "A" }, { "b", "B" } }};

    keys_check_map(map1, { "a", "b" }, CTX);
    vals_check_map(map1, { "A", "B" }, CTX);
    keys_check_map(map2, { "a", "b" }, CTX);
    vals_check_map(map2, { "A", "B" }, CTX);

    Map::Value_movable val_pair1{"c", "C"};
    Map::Value val_pair2{"d", "D"};
    s_n_copies = 0;
    map2.insert(std::move(val_pair1));
    n_copies_check(0, CTX); // "C" should not be copied b/c move() -- .insert(&&) should be chosen.
    EXPECT_EQ(val_pair1.first, ""); // String key got destroyed too via move.
    EXPECT_EQ(val_pair1.second.m_str, "");
    map2.insert(val_pair2);
    n_copies_check(1, CTX); // "D" should be copied b/c no move() -- .insert(const&) should be chosen.
    keys_check_map(map2, { "d", "c", "a", "b" }, CTX);
    vals_check_map(map2, { "D", "C", "A", "B" }, CTX);
    EXPECT_EQ(val_pair2.first, "d"); // String key got destroyed too via move.
    EXPECT_EQ(val_pair2.second.m_str, "D");

    // We've tested operator[] OK already, higher up above, but haven't checked the move/copy of key aspect of it.
    string val1{"e"};
    string val2{"f"};
    string val3{"b"};
    auto& ref = map2[val3] = "X";
    EXPECT_EQ(val3, "b"); // Already present, so definitely key untouched.
    EXPECT_EQ(ref, "X");
    ref = "B"; // Just change it back to "steady state."
    map2[std::move(val1)] = "E"; // Not present and we used move() -- operator[&&] should be chosen.
    EXPECT_EQ(val1, "");
    map2[val2] = "F"; // Not present and we did not use move() -- operator[const&] should be chosen.
    EXPECT_EQ(val2, "f");
    // Get rid of the 2 new keys to get back to "steady state."  (We test .erase() varieties more fully lower-down.)
    map2.erase("e");
    map2.erase("f");

    s_n_copies = 0;
    swap(map1, map2);
    n_copies_check(0, CTX); // Not a single copy of `Obj`s.  @todo Somehow ensure other stuff (`string`s?) not copied?
    keys_check_map(map1, { "d", "c", "a", "b" }, CTX);
    vals_check_map(map1, { "D", "C", "A", "B" }, CTX);
    keys_check_map(map2, { "a", "b" }, CTX);
    vals_check_map(map2, { "A", "B" }, CTX);

    s_n_copies = 0;
    map2 = map1;
    n_copies_check(4, CTX); // All `Obj`s did get copied.  @todo Could also check post-copy independence of map1 vs map2.
    keys_check_map(map1, { "d", "c", "a", "b" }, CTX);
    vals_check_map(map1, { "D", "C", "A", "B" }, CTX);
    keys_check_map(map2, { "d", "c", "a", "b" }, CTX);
    vals_check_map(map2, { "D", "C", "A", "B" }, CTX);

    s_n_copies = 0;
    map1 = std::move(map2);
    n_copies_check(0, CTX); // Nothing should be copied.
    keys_check_map(map1, { "d", "c", "a", "b" }, CTX);
    vals_check_map(map1, { "D", "C", "A", "B" }, CTX);
    keys_check_map(map2, { }, CTX);
    vals_check_map(map2, { }, CTX);

    // Same deal but construct instead of assigning.

    s_n_copies = 0;
    const auto map4 = map1;
    n_copies_check(4, CTX);
    keys_check_map(map1, { "d", "c", "a", "b" }, CTX);
    vals_check_map(map1, { "D", "C", "A", "B" }, CTX);
    keys_check_map(map4, { "d", "c", "a", "b" }, CTX);
    vals_check_map(map4, { "D", "C", "A", "B" }, CTX);

    s_n_copies = 0;
    const auto map5 = std::move(map1);
    n_copies_check(0, CTX);
    keys_check_map(map1, { }, CTX);
    vals_check_map(map1, { }, CTX);
    keys_check_map(map5, { "d", "c", "a", "b" }, CTX);
    vals_check_map(map5, { "D", "C", "A", "B" }, CTX);

    auto map6 = std::move(map5);
    auto ret = map6.insert(Map::Value{"e", "E"});
    EXPECT_TRUE(ret.second);
    keys_check_map(map6, { "e", "d", "c", "a", "b" }, CTX);
    vals_check_map(map6, { "E", "D", "C", "A", "B" }, CTX);
    ret.first->second = "X"; // Note: ret.first is iterator; ret.first->first is key (which is const).
    keys_check_map(map6, { "e", "d", "c", "a", "b" }, CTX);
    vals_check_map(map6, { "X", "D", "C", "A", "B" }, CTX);
    ret = map6.insert(Map::Value{"e", "E"});
    EXPECT_FALSE(ret.second);
    keys_check_map(map6, { "e", "d", "c", "a", "b" }, CTX);
    vals_check_map(map6, { "X", "D", "C", "A", "B" }, CTX);
    ret.first->second = "E";
    keys_check_map(map6, { "e", "d", "c", "a", "b" }, CTX);
    vals_check_map(map6, { "E", "D", "C", "A", "B" }, CTX);

    /* @todo Repeat, tediously, the insert() tests as on map6 above, but with the copying-insert instead of moving-insert.
     * We did already test copying-insert, and the fact that it in facts inserts and moves, but we haven't tested its
     * return value pair, nor that it no-ops if key already in map. */

    const auto map7 = map6;
    EXPECT_EQ(map7.find("b"), --map7.end());
    EXPECT_NE(map7.find("a"), map7.end());
    EXPECT_EQ(map7.find("x"), map7.end());
    EXPECT_EQ(map7.find("b"), --map7.cend());
    EXPECT_NE(map7.find("a"), map7.cend());
    EXPECT_EQ(map7.find("x"), map7.cend());
    const Map map8;
    EXPECT_EQ(map8.find("a"), map8.end());
    Map map9;
    EXPECT_EQ(map9.find("a"), map9.end());

    auto map10 = map7;
    const auto it1 = map10.find("c");
    EXPECT_EQ(it1->second, "C");
    it1->second = "X";
    const auto it2 = map10.find("c");
    EXPECT_EQ(it1->second, "X");
    EXPECT_EQ(it2->second, "X");
    EXPECT_EQ(it1, it2);
    auto it3 = map10.cbegin();
    EXPECT_EQ(it3->first, "e");
    EXPECT_EQ(it3->second, "E");
    EXPECT_EQ(it3, map10.const_newest());
    it3 = --map10.cend();
    EXPECT_EQ(it3->first, "b");
    EXPECT_EQ(it3->second, "B");
    EXPECT_EQ(it3, --map10.const_past_oldest());
    auto it4 = map10.begin();
    EXPECT_EQ(it4->first, "e");
    EXPECT_EQ(it4->second, "E");
    EXPECT_EQ(it4, map10.const_newest());
    it4->second = "Z";
    keys_check_map(map10, { "e", "d", "c", "a", "b" }, CTX);
    vals_check_map(map10, { "Z", "D", "X", "A", "B" }, CTX);
    it4 = --map10.end();
    EXPECT_EQ(it4->first, "b");
    EXPECT_EQ(it4->second, "B");
    EXPECT_EQ(it4, --map10.past_oldest());
    EXPECT_EQ(it4, --map10.const_past_oldest());
    it4->second = "Y";
    keys_check_map(map10, { "e", "d", "c", "a", "b" }, CTX);
    vals_check_map(map10, { "Z", "D", "X", "A", "Y" }, CTX);

    // Note: reverse-iterator accessors are tested OK in *_check_map().  @todo Test the non-const varieties also though.

    EXPECT_FALSE(map10.touch("x"));
    EXPECT_TRUE(map10.touch("a"));
    keys_check_map(map10, { "a", "e", "d", "c", "b" }, CTX);
    vals_check_map(map10, { "A", "Z", "D", "X", "Y" }, CTX);
    map10.touch(map10.find("b"));
    keys_check_map(map10, { "b", "a", "e", "d", "c" }, CTX);
    vals_check_map(map10, { "Y", "A", "Z", "D", "X" }, CTX);
    map10.touch(map10.find("b"));
    keys_check_map(map10, { "b", "a", "e", "d", "c" }, CTX);
    vals_check_map(map10, { "Y", "A", "Z", "D", "X" }, CTX);

    EXPECT_EQ(map10.erase("x"), 0);
    EXPECT_EQ(map10.erase("c"), 1);
    EXPECT_EQ(map10.erase("c"), 0);
    EXPECT_EQ(map10.erase("a"), 1);
    EXPECT_EQ(map10.erase("a"), 0);
    EXPECT_EQ(map10.erase("b"), 1);
    EXPECT_EQ(map10.erase("b"), 0);
    keys_check_map(map10, { "e", "d" }, CTX);
    vals_check_map(map10, { "Z", "D" }, CTX);
    map10.clear();
    keys_check_map(map10, { }, CTX);
    vals_check_map(map10, { }, CTX);
    map10 = map7;
    keys_check_map(map10, { "e", "d", "c", "a", "b" }, CTX);
    vals_check_map(map10, { "E", "D", "C", "A", "B" }, CTX);
    auto it5 = map10.erase(map10.begin(), map10.find("c"));
    keys_check_map(map10, { "c", "a", "b" }, CTX);
    vals_check_map(map10, { "C", "A", "B" }, CTX);
    EXPECT_EQ(it5, map10.begin());
    map10 = map7;
    keys_check_map(map10, { "e", "d", "c", "a", "b" }, CTX);
    vals_check_map(map10, { "E", "D", "C", "A", "B" }, CTX);
    it5 = map10.erase(map10.find("c"), map10.end());
    keys_check_map(map10, { "e", "d" }, CTX);
    vals_check_map(map10, { "E", "D" }, CTX);
    EXPECT_EQ(it5, map10.end());
    map10 = map7;
    keys_check_map(map10, { "e", "d", "c", "a", "b" }, CTX);
    vals_check_map(map10, { "E", "D", "C", "A", "B" }, CTX);
    it5 = map10.erase(map10.find("d"), map10.find("b"));
    keys_check_map(map10, { "e", "b" }, CTX);
    vals_check_map(map10, { "E", "B" }, CTX);
    EXPECT_EQ(it5, --map10.end());
    EXPECT_EQ(it5->first, "b");
    EXPECT_EQ(it5->second, "B");

    EXPECT_EQ(map10.count("e"), 1);
    EXPECT_EQ(map10.count("b"), 1);
    EXPECT_EQ(map10.count("x"), 0);
    EXPECT_EQ(map10.count(""), 0);
    map10[""] = "Q";
    EXPECT_EQ(map10.count(""), 1);
    EXPECT_EQ(map10[""], "Q");
    EXPECT_EQ(map10["e"], "E");
    EXPECT_EQ(map10["b"], "B");

    // @todo Test .max_size(), I suppose.
  } // Map test block.

  // Test analogous stuff for Set, in the same order, omitting inapplicable things.  Comments kept (even) lighter.
  {
    Set set1;

    s_n_copies = 0;
    set1.insert("b");
    n_copies_check(0, CTX); // Move-cting .insert() should be chosen => no Obj{"b"} copy.

    set1.insert(Obj{"a"}); // Becomes newest (first) because inserted.
    n_copies_check(0, CTX);

    Set set2{{ "a", "b" }};

    keys_check_set(set1, { "a", "b" }, CTX);
    keys_check_set(set2, { "a", "b" }, CTX);

    Obj val1{"c"};
    Obj val2{"d"};
    s_n_copies = 0;
    set2.insert(std::move(val1));
    n_copies_check(0, CTX); // "c" should not be copied b/c move() -- .insert(&&) should be chosen.
    EXPECT_EQ(val1.m_str, ""); // String key got destroyed too via move.
    set2.insert(val2);
    n_copies_check(1, CTX); // "d" should be copied b/c no move() -- .insert(const&) should be chosen.
    keys_check_set(set2, { "d", "c", "a", "b" }, CTX);
    EXPECT_EQ(val2.m_str, "d"); // String key got destroyed too via move.

    s_n_copies = 0;
    swap(set1, set2);
    n_copies_check(0, CTX); // Not a single copy of `Obj`s.  @todo Somehow ensure other stuff (`string`s?) not copied?
    keys_check_set(set1, { "d", "c", "a", "b" }, CTX);
    keys_check_set(set2, { "a", "b" }, CTX);

    s_n_copies = 0;
    set2 = set1;
    n_copies_check(4, CTX); // All `Obj`s did get copied.  @todo Could also check post-copy independence of set1 vs set2.
    keys_check_set(set1, { "d", "c", "a", "b" }, CTX);
    keys_check_set(set2, { "d", "c", "a", "b" }, CTX);

    s_n_copies = 0;
    set1 = std::move(set2);
    n_copies_check(0, CTX); // Nothing should be copied.
    keys_check_set(set1, { "d", "c", "a", "b" }, CTX);
    keys_check_set(set2, { }, CTX);

    // Same deal but construct instead of assigning.

    s_n_copies = 0;
    const auto set4 = set1;
    n_copies_check(4, CTX);
    keys_check_set(set1, { "d", "c", "a", "b" }, CTX);
    keys_check_set(set4, { "d", "c", "a", "b" }, CTX);

    s_n_copies = 0;
    const auto set5 = std::move(set1);
    n_copies_check(0, CTX);
    keys_check_set(set1, { }, CTX);
    keys_check_set(set5, { "d", "c", "a", "b" }, CTX);

    auto set6 = std::move(set5);
    auto ret = set6.insert("e");
    EXPECT_TRUE(ret.second);
    EXPECT_EQ(ret.first->m_str, "e");
    keys_check_set(set6, { "e", "d", "c", "a", "b" }, CTX);
    ret = set6.insert("e");
    EXPECT_FALSE(ret.second);
    EXPECT_EQ(ret.first->m_str, "e");
    keys_check_set(set6, { "e", "d", "c", "a", "b" }, CTX);

    /* @todo Repeat, tediously, the insert() tests as on set6 above, but with the copying-insert instead of
     * moving-insert.
     * We did already test copying-insert, and the fact that it in facts inserts and moves, but we haven't tested its
     * return value pair, nor that it no-ops if key already in set. */

    const auto set7 = set6;
    EXPECT_EQ(set7.find("b"), --set7.end());
    EXPECT_NE(set7.find("a"), set7.end());
    EXPECT_EQ(set7.find("x"), set7.end());
    EXPECT_EQ(set7.find("b"), --set7.cend());
    EXPECT_NE(set7.find("a"), set7.cend());
    EXPECT_EQ(set7.find("x"), set7.cend());
    const Set set8;
    EXPECT_EQ(set8.find("a"), set8.end());
    Set set9;
    EXPECT_EQ(set9.find("a"), set9.end());

    auto set10 = set7;
    const auto it1 = set10.find("c");
    EXPECT_EQ(it1->m_str, "c");
    const auto it2 = set10.find("c");
    EXPECT_EQ(it2->m_str, "c");
    EXPECT_EQ(it1, it2);
    auto it3 = set10.cbegin();
    EXPECT_EQ(it3->m_str, "e");
    EXPECT_EQ(it3, set10.const_newest());
    it3 = --set10.cend();
    EXPECT_EQ(it3->m_str, "b");
    EXPECT_EQ(it3, --set10.const_past_oldest());
    auto it4 = set10.begin();
    EXPECT_EQ(it4->m_str, "e");
    EXPECT_EQ(it4, set10.const_newest());
    keys_check_set(set10, { "e", "d", "c", "a", "b" }, CTX);
    it4 = --set10.end();
    EXPECT_EQ(it4->m_str, "b");
    EXPECT_EQ(it4, --set10.past_oldest());
    EXPECT_EQ(it4, --set10.const_past_oldest());
    keys_check_set(set10, { "e", "d", "c", "a", "b" }, CTX);

    // Note: reverse-iterator accessors are tested OK in *_check_set().  @todo Test the non-const varieties also though.

    EXPECT_FALSE(set10.touch("x"));
    EXPECT_TRUE(set10.touch("a"));
    keys_check_set(set10, { "a", "e", "d", "c", "b" }, CTX);
    set10.touch(set10.find("b"));
    keys_check_set(set10, { "b", "a", "e", "d", "c" }, CTX);
    set10.touch(set10.find("b"));
    keys_check_set(set10, { "b", "a", "e", "d", "c" }, CTX);

    EXPECT_EQ(set10.erase("x"), 0);
    EXPECT_EQ(set10.erase("c"), 1);
    EXPECT_EQ(set10.erase("c"), 0);
    EXPECT_EQ(set10.erase("a"), 1);
    EXPECT_EQ(set10.erase("a"), 0);
    EXPECT_EQ(set10.erase("b"), 1);
    EXPECT_EQ(set10.erase("b"), 0);
    keys_check_set(set10, { "e", "d" }, CTX);
    set10.clear();
    keys_check_set(set10, { }, CTX);
    set10 = set7;
    keys_check_set(set10, { "e", "d", "c", "a", "b" }, CTX);
    auto it5 = set10.erase(set10.begin(), set10.find("c"));
    keys_check_set(set10, { "c", "a", "b" }, CTX);
    EXPECT_EQ(it5, set10.begin());
    set10 = set7;
    keys_check_set(set10, { "e", "d", "c", "a", "b" }, CTX);
    it5 = set10.erase(set10.find("c"), set10.end());
    keys_check_set(set10, { "e", "d" }, CTX);
    EXPECT_EQ(it5, set10.end());
    set10 = set7;
    keys_check_set(set10, { "e", "d", "c", "a", "b" }, CTX);
    it5 = set10.erase(set10.find("d"), set10.find("b"));
    keys_check_set(set10, { "e", "b" }, CTX);
    EXPECT_EQ(it5, --set10.end());
    EXPECT_EQ(it5->m_str, "b");

    EXPECT_EQ(set10.count("e"), 1);
    EXPECT_EQ(set10.count("b"), 1);
    EXPECT_EQ(set10.count("x"), 0);
    EXPECT_EQ(set10.count(""), 0);
    set10.insert("");
    EXPECT_EQ(set10.count(""), 1);
  } // Set test block.
} // TEST(Linked_hash, Interface)

} // namespace flow::util::test
