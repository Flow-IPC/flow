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

#include "flow/util/default_init_allocator.hpp"
#include <gtest/gtest.h>
#include <list>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace flow::util::test
{

namespace
{

using uint = unsigned int;
static uint s_n_adaptee_constructs = 0;

/* Adaptee allocator that counts calls to its construct().  Default_init_allocator must route 0-arg (value-init)
 * construction away from it and everything else to it.  Note: it derives from std::allocator, which (in C++17) has
 * its own `rebind`; so we define ours -- else rebinding it would yield plain std::allocator.  Conversely, if
 * Default_init_allocator lacked its own `rebind`, it would inherit ours, and rebinding would silently drop the
 * adaptor (and all constructs, including 0-arg ones, would reach us). */
template<typename T>
struct Counting_allocator : public std::allocator<T>
{
  template<typename U>
  struct rebind { using other = Counting_allocator<U>; };

  Counting_allocator() = default;
  template<typename U>
  Counting_allocator(const Counting_allocator<U>&) {}

  template<typename U, typename... Args>
  void construct(U* ptr, Args&&... args)
  {
    ++s_n_adaptee_constructs;
    ::new(static_cast<void*>(ptr)) U(std::forward<Args>(args)...);
  }
};

} // Anonymous namespace

TEST(Default_init_allocator, Interface)
{
  using std::allocator_traits;
  using std::is_same_v;
  using std::vector;
  using std::list;

  using Alloc = Default_init_allocator<int, Counting_allocator<int>>;

  // Rebinding must keep the adaptor (and rebind the adaptee).
  static_assert(is_same_v<allocator_traits<Alloc>::rebind_alloc<int>, Alloc>);
  static_assert(is_same_v<allocator_traits<Alloc>::rebind_alloc<long>,
                          Default_init_allocator<long, Counting_allocator<long>>>);
  static_assert(is_same_v<allocator_traits<Default_init_allocator<int>>::rebind_alloc<int>,
                          Default_init_allocator<int>>);

  { // vector: value-init (n-arg ctor, resize()) skips the adaptee; emplace with args reaches it.
    s_n_adaptee_constructs = 0;
    vector<int, Alloc> v(10);
    EXPECT_EQ(s_n_adaptee_constructs, 0u);
    v.reserve(20); // Relocation move-constructs the existing 10 via the adaptee (1-arg construct()); that's correct.
    s_n_adaptee_constructs = 0;
    v.resize(20); // No reallocation now; so only the 10 new elements are constructed (value-init => skips adaptee).
    EXPECT_EQ(s_n_adaptee_constructs, 0u);
    v.push_back(5);
    EXPECT_GE(s_n_adaptee_constructs, 1u); // (Reallocation may copy/move-construct more via the adaptee.)
    EXPECT_EQ(v.back(), 5);
  }

  { // list: rebinds to its node type; same expectations.
    s_n_adaptee_constructs = 0;
    list<int, Alloc> l(10);
    EXPECT_EQ(s_n_adaptee_constructs, 0u);
    l.emplace_back(5);
    EXPECT_EQ(s_n_adaptee_constructs, 1u);
    EXPECT_EQ(l.back(), 5);
  }
} // TEST(Default_init_allocator, Interface)

} // namespace flow::util::test
