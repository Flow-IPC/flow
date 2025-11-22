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
#include "flow/util/thread_lcl.hpp"
#include "flow/util/util.hpp"
#include "flow/async/single_thread_task_loop.hpp"
#include "flow/test/test_logger.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <optional>
#include <memory>
#include <mutex>

namespace flow::util::test
{

namespace
{
using std::optional;
using std::string;
using std::make_shared;
using std::shared_ptr;
using std::mutex;
using std::atomic;
using flow::test::Test_logger;
using Thread_loop = async::Single_thread_task_loop;
template<typename T>
using Tl_reg = Thread_local_state_registry<T>;

static mutex s_events_mtx;
static string s_events;
static atomic<uint32_t> s_id{1};

void events_clear() { Lock_guard<mutex> lock; events_clear(); }
void events_set(const string& str) { Lock_guard<mutex> lock; s_events = str; }
string events() { Lock_guard<mutex> lock; return s_events; }

struct State
{
  string m_stuff{"stuff"};

  std::atomic<bool> m_do_action{false};

  ~State()
  {
    events_set(ostream_op_string(events(), "~State/", s_id++, '\n'));
  }
};
struct State2
{
  const int m_x;

  State2(int x): m_x(x) {}

  ~State2()
  {
    events_set(ostream_op_string(events(), "~State/", s_id++, '\n'));
  }
};

static optional<Tl_reg<State>> s_reg2{std::in_place, nullptr, "testReg2"};

} // Anonymous namespace

TEST(Thread_local_state_registry, Interface)
{
  Test_logger logger;
  FLOW_LOG_SET_CONTEXT(&logger, Flow_log_component::S_UNCAT);

  optional<Tl_reg<State>> reg1;
  reg1.emplace(&logger, "testReg1");

  /* We don't test that this works (maybe we should? @todo), but one can see it work in the console output,
   * and at least this won't crash; that's something.  Plus we declared it `static` to test pre-main() init
   * (.set_logger() exists essentially for that use-case).
   *
   * @todo Should also test set_logger() propagation:
   *   - If T in Thread_local_state_registry<T> has Log_context_mt, set_logger() shall propagate to each thread's
   *     extant T and future such `T`s.
   *   - */
  s_reg2->set_logger(&logger);

  EXPECT_TRUE(events().empty());

  auto s1 = reg1->this_thread_state();
  EXPECT_EQ(s1->m_stuff, "stuff");
  EXPECT_EQ(s1, reg1->this_thread_state());

  auto s2 = s_reg2->this_thread_state();
  s2->m_stuff = "other";
  EXPECT_EQ(s2->m_stuff, "other");
  EXPECT_NE(s1, s2);
  EXPECT_EQ(s2, s_reg2->this_thread_state());

  s_reg2.reset();
  reg1.reset();

  EXPECT_EQ(events(), "~State/1\n~State/2\n");
  events_clear();

  {
    optional<Tl_reg<State>> reg3;
    reg3.emplace(&logger, "testReg3");
    {
      Thread_loop t1{&logger, "thread1"};
      t1.start([&]()
      {
        auto s1 = reg3->this_thread_state();
        EXPECT_EQ(s1->m_stuff, "stuff");
        EXPECT_EQ(s1, reg3->this_thread_state());
      });
      Thread_loop t2{&logger, "thread2"};
      t2.start([&]()
      {
        auto s1 = reg3->this_thread_state();
        EXPECT_EQ(s1->m_stuff, "stuff");
        EXPECT_EQ(s1, reg3->this_thread_state());
      });
      auto s2 = reg3->this_thread_state();
      EXPECT_EQ(s2, reg3->this_thread_state());

      EXPECT_TRUE(events().empty());
      t1.stop();
      EXPECT_EQ(events(), "~State/3\n");
      t2.stop();
      EXPECT_EQ(events(), "~State/3\n~State/4\n");
    }
    EXPECT_EQ(events(), "~State/3\n~State/4\n");
  }
  EXPECT_EQ(events(), "~State/3\n~State/4\n~State/5\n");
  events_clear();

  {
    // Create a couple of `Tl_reg`s of the same type; and of a different type (some internal `static`s exercised).
    optional<Tl_reg<State>> reg3;
    reg3.emplace(&logger, "testReg3");
    optional<Tl_reg<State>> reg4;
    reg4.emplace(&logger, "testReg4");
    optional<Tl_reg<State2>> reg3b;
    reg3b.emplace(&logger, "test3b", []() -> auto { return new State2{3}; });
    optional<Tl_reg<State2>> reg4b;
    reg4b.emplace(&logger, "test4b");
    reg4b->m_create_state_func = []() -> auto { return new State2{4}; };

    Thread_loop t1{&logger, "thread1"};
    Thread_loop t2{&logger, "thread2"};
    t1.start([&]() { reg3->this_thread_state(); reg3b->this_thread_state();
                     reg4->this_thread_state(); reg4b->this_thread_state();
                     EXPECT_EQ(reg3b->this_thread_state()->m_x, 3);
                     EXPECT_EQ(reg4b->this_thread_state()->m_x, 4); });
    t2.start([&]() { reg3->this_thread_state(); reg4b->this_thread_state(); });
    EXPECT_TRUE(events().empty());
    t1.stop();
    EXPECT_EQ(events(), "~State/6\n~State/7\n~State/8\n~State/9\n");
    reg4b.reset();
    EXPECT_EQ(events(), "~State/6\n~State/7\n~State/8\n~State/9\n~State/10\n");
    reg3.reset();
    EXPECT_EQ(events(), "~State/6\n~State/7\n~State/8\n~State/9\n~State/10\n~State/11\n");
    t2.stop();
    EXPECT_EQ(events(), "~State/6\n~State/7\n~State/8\n~State/9\n~State/10\n~State/11\n");
    events_clear();
  }
  EXPECT_TRUE(events().empty());

  {
    using Task = async::Scheduled_task;

    optional<Tl_reg<State>> reg3;
    reg3.emplace(&logger, "testLock");

    Thread_loop t1{&logger, "threadLoop1"};
    auto func1 = make_shared<Task>();
    *func1 = [self = func1, &reg3, &t1](bool)
    {
      bool exp{true};
      if (reg3->this_thread_state()->m_do_action.compare_exchange_strong(exp, false, std::memory_order_relaxed))
      {
        events_set(events() + "didAction\n");
      }
      t1.schedule_from_now(boost::chrono::milliseconds(500), Task{*self});
    };
    Thread_loop t2{&logger, "threadLoop2"};
    auto func2 = make_shared<Task>();
    *func2 = [self = func1, &reg3, &t2](bool)
    {
      bool exp{true};
      if (reg3->this_thread_state()->m_do_action.compare_exchange_strong(exp, false, std::memory_order_relaxed))
      {
        events_set(events() + "didAction\n");
      }
      t2.schedule_from_now(boost::chrono::milliseconds(500), Task{*self});
    };

    t1.start([func = func1]() { (*func)(false); });
    t2.start([func = func2]() { (*func)(false); });

    EXPECT_TRUE(events().empty());
    reg3->while_locked([&](const auto& lock)
    {
      const auto& states = reg3->state_per_thread(lock);
      for (const auto& state_and_mdt : states)
      {
        FLOW_LOG_INFO("Affecting TLS state for thread [" << state_and_mdt.second.m_thread_nickname << "].");
        state_and_mdt.first->m_do_action.store(true, std::memory_order_relaxed);
      }
    });
    this_thread::sleep_for(boost::chrono::seconds(2));
    EXPECT_EQ(events(), "didAction\ndidAction\n");
    this_thread::sleep_for(boost::chrono::seconds(2));
    EXPECT_EQ(events(), "didAction\ndidAction\n");
  }
  EXPECT_EQ(events(), "didAction\ndidAction\n~State/12\n~State/13\n");
} // TEST(Thread_local_state_registry, Interface)

TEST(Thread_local_state_registry, DISABLED_Advanced)
{
  /* @todo I (ygoldfel) am unconvinced (my own) existing testing of T_l_s_registry above checks all code paths
   * of this tricky facility.  I wrote it under major time pressure, alongside developing the facility under
   * major time pressure; so it does check the essential stuff most relevant at the time.  I did not, however,
   * scan all the code paths and subtleties and test them all for sure.
   *
   * This test thus "fails" but is DISABLED to perhaps draw some attention to this to-do.
   *
   * Recommended approach:
   *   - Go over `Interface` test carefully and (1) comment what is being tested and (2) therefore understand
   *     what is being tested.
   *   - Go over the Thread_local_state_registry code in detail and add tests in here for the parts missed
   *     in preceding bullet point. */
  EXPECT_TRUE(false);
}

TEST(Thread_local_state_registry, DISABLED_Polled_shared_state)
{
  /* Flow-IPC (a Flow-using project) does use Polled_shared_state extensively, and those P_s_s-using facilities
   * are tested more black-boxily, so there is some P_s_s unit/functional-testing indirectly, vaguely speaking 
   * in Flow-land (as of this writing Flow-IPC and Flow are generally tested together, at least in open-source
   * official project).  However:
   *
   * @todo Add unit test here for Polled_shared_state itself.
   *
   * This test thus "fails" but is DISABLED to perhaps draw some attention to this to-do. */
  EXPECT_TRUE(false);
}

} // namespace flow::util::test
