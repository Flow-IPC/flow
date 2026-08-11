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
#include "flow/util/thread_lcl_obj.hpp"
#include "flow/util/thread_lcl_ptr.hpp"
#include "flow/util/util.hpp"
#include "flow/async/single_thread_task_loop.hpp"
#include "flow/perf/checkpt_timer.hpp"
#include "flow/test/test_logger.hpp"
#include <boost/range/algorithm.hpp>
#include <gtest/gtest.h>
#include <boost/thread/thread.hpp>
#include <boost/thread/future.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <set>
#include <thread>
#include <optional>

namespace flow::util::test
{

namespace
{
using boost::promise;
using std::optional;
using std::string;
using std::cout;
using std::cerr;
using std::flush;
using std::atomic;
using std::vector;
using flow::test::Test_logger;
using Thread_loop = async::Single_thread_task_loop;
template<typename T>
using Tl_reg = Thread_local_state_registry<T>;

// Runs task() in the given loop's thread and returns once it has completed: every worker-thread action in
// the registry tests below is sequenced this way (deterministic; no sleeps).
template<typename Task>
void post_wait(Thread_loop* loop, Task&& task)
{
  loop->post(std::forward<Task>(task), async::Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
}

// Bumped by every specimen dtor; observable from any thread.  Each TEST that counts these resets it first.
atomic<int> s_n_state_dtors{0};

// The all-purpose per-thread state: some data; a Poll_flag (for the Polled_shared_state-pattern tests).
struct State
{
  string m_stuff{"stuff"};
  int m_cookie{0}; // Custom create-funcs set this, where used.
  Poll_flag m_flag;

  ~State()
  {
    /* Polling one's own flag from the dtor is within contract (see Polled_shared_state doc header,
     * Lifetime/safety section: the flag is our member, alive as long as we are).  Exercise that on every
     * specimen death, in every test; the result is moot. */
    m_flag.poll_armed();
    ++s_n_state_dtors;
  }
};

/* For the Logger_propagation test: a Thread_local_state deriving log::Log_context_mt, which changes the
 * registry's default-create-func and set_logger() behaviors (see S_TL_STATE_HAS_MT_LOG_CONTEXT). */
struct Logging_state : public log::Log_context_mt
{
  explicit Logging_state(log::Logger* logger_ptr) :
    Log_context_mt(logger_ptr)
  {
    // Yep.
  }

  ~Logging_state()
  {
    ++s_n_state_dtors;
  }

  log::Logger* observed_logger() const
  {
    log::Logger* logger;
    log_while_locked([&](auto&& get_logger, auto&&) { logger = get_logger(); });
    return logger;
  }
};

// Static-storage registry: ctor runs pre-main(), necessarily Logger-less; set_logger() exists ~for this pattern.
optional<Tl_reg<State>> s_static_reg{std::in_place, nullptr, "staticReg"};

// For the Ctor_runs_locked test: a state whose ctor parks at a gate, holding the locked section wide-open.
atomic<bool> s_ctor_entered{false};
atomic<bool> s_ctor_release{false};
atomic<bool> s_ctor_finished{false};

struct Gated_ctor_state
{
  Gated_ctor_state()
  {
    s_ctor_entered = true;
    while (!s_ctor_release)
    {
      this_thread::yield();
    }
    s_ctor_finished = true;
  }
};

// For the Dtor_runs_unlocked test: a state whose dtor calls while_locked() on its own registry.
bool s_dtor_while_locked_ran = false;
bool s_dtor_saw_self_listed = true; // Must be actively cleared by the dtor's observation.

struct Locking_dtor_state
{
  Tl_reg<Locking_dtor_state>* m_reg{}; // Set right after creation.  Null => dtor skips the exercise.

  ~Locking_dtor_state()
  {
    if (!m_reg)
    {
      return;
    }
    /* Per the lifecycle guarantee (thread_lcl.hpp class doc header): in thread-exit cleanup our dtor runs
     * *outside* the registry-locked section, after un-listing.  So this must not deadlock; and we must be
     * absent from the listing. */
    m_reg->while_locked([&](const auto& state_per_thread)
    {
      s_dtor_while_locked_ran = true;
      s_dtor_saw_self_listed = (state_per_thread.count(this) != 0);
    });
  }
};

/* For the Rebirth_during_cleanup test: states whose dtors create successors via this_thread_state() -- the
 * class doc header's corollary (full birth-life-death cycles nested within the cleanup phase, whose
 * repeat-until-empty algorithm sees each successor through).  Two distinct state types, since the cleanup
 * phase spans *all* registries of *all* types; the chain, all within one exiting thread's cleanup phase:
 *   Reborn_state gen 1 -> Reborn_kin_state (cross-type hop) -> Reborn_state gen 2 (cross-type hop back,
 *   re-arming a slot the cleanup loop already processed) -> end (generation guard).
 * (The remaining variation -- a dtor directly re-arming its *own* slot -- is covered one layer down, by
 * TEST(Thread_local_ptr, Cleanup_window)'s self-re-arming specimen.)
 * The dtors are out-of-line: each needs the other type complete. */
Tl_reg<struct Reborn_state>* s_rebirth_reg{};
Tl_reg<struct Reborn_kin_state>* s_rebirth_kin_reg{};
atomic<int> s_n_rebirth_ctors{0};
atomic<int> s_n_rebirth_dtors{0};
atomic<int> s_n_rebirth_kin_ctors{0};
atomic<int> s_n_rebirth_kin_dtors{0};

struct Reborn_state
{
  const int m_generation{++s_n_rebirth_ctors};

  ~Reborn_state();
};

struct Reborn_kin_state
{
  Reborn_kin_state()
  {
    ++s_n_rebirth_kin_ctors;
  }

  ~Reborn_kin_state();
};

Reborn_state::~Reborn_state()
{
  ++s_n_rebirth_dtors;
  if (m_generation == 1)
  {
    s_rebirth_kin_reg->this_thread_state(); // The cross-type successor; its own dtor continues the chain.
  }
}

Reborn_kin_state::~Reborn_kin_state()
{
  ++s_n_rebirth_kin_dtors;
  s_rebirth_reg->this_thread_state(); // Cross back: Reborn_state generation 2 (whose death ends the chain).
}

/* For the Thread_local_obj_deinit_safe tests: a payload with a non-trivial dtor (the class's entire reason
 * to exist) including a heap component (so a use-after-destruction is ASAN-visible, not merely formal). */
atomic<int> s_n_tlods_dtors{0};

struct Tlods_payload
{
  int m_val{42};
  vector<int> m_heapy{1, 2, 3};

  ~Tlods_payload()
  {
    ++s_n_tlods_dtors;
  }
};

/* Deinit-window probers: plain thread_locals whose dtors access the facility during thread_local deinit.
 * Their observations land in the file-scope slots below (read by the main thread post-join).
 * Construction order in the test body is what makes each verdict deterministic (reverse-order destruction):
 * the before-prober is constructed *before* the facility's first access => destroyed after the payload =>
 * must see null (twice: once-null-always-null); the after-prober, the mirror image => must see the live
 * payload, values intact. */
struct Tlods_window_tag; // The probers and the test use this facility instance (isolated from Interface's).
using Tlods_window_facility = Thread_local_obj_deinit_safe<Tlods_payload, Tlods_window_tag>;

bool s_tlods_before_prober_ran = false;
Tlods_payload* s_tlods_seen_by_before_prober{};
Tlods_payload* s_tlods_seen_by_before_prober_2nd{};
bool s_tlods_after_prober_ran = false;
int s_tlods_val_seen_by_after_prober = 0;

struct Tlods_before_prober
{
  ~Tlods_before_prober()
  {
    s_tlods_before_prober_ran = true;
    s_tlods_seen_by_before_prober = Tlods_window_facility::this_thread_obj_or_null();
    s_tlods_seen_by_before_prober_2nd = Tlods_window_facility::this_thread_obj_or_null();
  }
};

struct Tlods_after_prober
{
  ~Tlods_after_prober()
  {
    s_tlods_after_prober_ran = true;
    const auto obj = Tlods_window_facility::this_thread_obj_or_null();
    s_tlods_val_seen_by_after_prober = obj ? obj->m_val : -1;
  }
};

/* First-access-near-death specimens (the facility doc header's "Corner case" section, cases 1 and 2).
 * Case 1: the first-ever facility access occurs *during* thread_local deinit -- from this prober's dtor. */
struct Tlods_case1_tag;
using Tlods_case1_facility = Thread_local_obj_deinit_safe<Tlods_payload, Tlods_case1_tag>;

bool s_tlods_case1_prober_ran = false;
bool s_tlods_case1_saw_obj = false;
bool s_tlods_case1_value_ok = false;

struct Tlods_case1_prober
{
  ~Tlods_case1_prober()
  {
    s_tlods_case1_prober_ran = true;
    const auto obj = Tlods_case1_facility::this_thread_obj_or_null(); // First-ever access: mid-deinit birth.
    if (obj)
    {
      s_tlods_case1_saw_obj = true;
      obj->m_val = 55;
      const auto obj_again = Tlods_case1_facility::this_thread_obj_or_null();
      s_tlods_case1_value_ok = (obj_again == obj) && (obj_again->m_val == 55);
    }
  }
};

/* Case 2: the first-ever access occurs *after* deinit -- from Thread_local_ptr cleanup in a std::thread
 * (which runs post-deinit; see the Thread_local_ptr TESTs below).  This payload is deliberately heap-free:
 * per the doc'd case-2 behavior its dtor never runs -- the documented leak -- so a heapy payload would trip
 * LSAN; the dtor-never-ran assert is the point, and TLS storage does not read as a leak. */
atomic<int> s_n_tlods_case2_ctors{0};
atomic<int> s_n_tlods_case2_dtors{0};

struct Tlods_case2_payload
{
  int m_val{42};

  Tlods_case2_payload()
  {
    ++s_n_tlods_case2_ctors;
  }

  ~Tlods_case2_payload() // User-provided => non-trivial, satisfying the facility's static_assert().
  {
    ++s_n_tlods_case2_dtors;
  }
};

struct Tlods_case2_tag;
using Tlods_case2_facility = Thread_local_obj_deinit_safe<Tlods_case2_payload, Tlods_case2_tag>;

bool s_tlods_case2_hook_ran = false;
bool s_tlods_case2_saw_obj = false;
bool s_tlods_case2_value_ok = false;

void tlods_case2_tlp_cleanup(int* /* unused: static-storage payload */)
{
  s_tlods_case2_hook_ran = true;
  const auto obj = Tlods_case2_facility::this_thread_obj_or_null(); // First-ever access: post-deinit birth.
  if (obj)
  {
    s_tlods_case2_saw_obj = true;
    obj->m_val = 77;
    const auto obj_again = Tlods_case2_facility::this_thread_obj_or_null();
    s_tlods_case2_value_ok = (obj_again == obj) && (obj_again->m_val == 77);
  }
}

} // Anonymous namespace

/* Thread_local_state_registry: the core contract -- on-demand per-thread creation (identity; or_null form);
 * default and custom create-funcs (ctor-supplied and member-assigned); cleanup at thread exit or registry
 * destruction, whichever comes first for a given state.  (The trickier corners get their own TESTs below.) */
TEST(Thread_local_state_registry, Interface)
{
  Test_logger logger;

  s_n_state_dtors = 0;

  // The static registry has existed since pre-main(), Logger-less; supply the Logger now.
  s_static_reg->set_logger(&logger);

  optional<Tl_reg<State>> reg{std::in_place, &logger, "ifaceReg"};

  // On-demand creation; identity; the or_null form.
  EXPECT_FALSE(reg->this_thread_state_or_null()); // Not yet activated in this thread.
  State* const s_main = reg->this_thread_state();
  ASSERT_TRUE(s_main);
  EXPECT_EQ(s_main->m_stuff, "stuff"); // Default create-func: `new State`.
  EXPECT_EQ(reg->this_thread_state(), s_main); // Same answer every time...
  EXPECT_EQ(reg->this_thread_state_or_null(), s_main); // ...in both forms.

  // Registries do not share states, same Thread_local_state type or not; the static one acts like any other.
  State* const s_static = s_static_reg->this_thread_state();
  EXPECT_NE(s_static, s_main);
  s_static->m_stuff = "other";
  EXPECT_EQ(s_static_reg->this_thread_state()->m_stuff, "other");
  EXPECT_EQ(s_main->m_stuff, "stuff");

  // Custom create-funcs, both forms: ctor-supplied; assigned to the public member post-ctor.
  optional<Tl_reg<State>> reg_c{std::in_place, &logger, "ctorFuncReg",
                                []() -> auto { return new State{"custom", 3, {}}; }};
  optional<Tl_reg<State>> reg_a{std::in_place, &logger, "assignFuncReg"};
  reg_a->m_create_state_func = []() -> auto { return new State{"custom", 4, {}}; };

  {
    Thread_loop t1{&logger, "iface1"};
    Thread_loop t2{&logger, "iface2"};
    t1.start();
    t2.start();

    // Thread 1 activates all 4 registries; thread 2 only 2 of them.
    post_wait(&t1, [&]()
    {
      EXPECT_EQ(reg_c->this_thread_state()->m_cookie, 3);
      EXPECT_EQ(reg_a->this_thread_state()->m_cookie, 4);
      reg->this_thread_state();
      s_static_reg->this_thread_state();
    });
    post_wait(&t2, [&]()
    {
      reg->this_thread_state();
      reg_a->this_thread_state();
    });
    EXPECT_EQ(s_n_state_dtors.load(), 0);

    // Thread exit cleans exactly that thread's states.
    t1.stop();
    EXPECT_EQ(s_n_state_dtors.load(), 4);

    // Registry destruction cleans its remaining states -- here t2's and main's...
    reg.reset();
    EXPECT_EQ(s_n_state_dtors.load(), 6);
    // ...while a state whose registry died first is *not* cleaned again at its owner thread's exit:
    t2.stop();
    EXPECT_EQ(s_n_state_dtors.load(), 7); // Just t2's reg_a state; its `reg` state died with `reg` above.
  }

  reg_c.reset(); // Nothing left inside (t1, its only user, exited long ago).
  reg_a.reset(); // Ditto (main never activated it).
  EXPECT_EQ(s_n_state_dtors.load(), 7);

  s_static_reg.reset(); // Main's state is still in there though.
  EXPECT_EQ(s_n_state_dtors.load(), 8);
} // TEST(Thread_local_state_registry, Interface)

/* The states' two possible deleters and their hand-off (whichever fires first does the deed; the other must
 * detect that and no-op): thread-exit-first is all over Interface; here the *registry dtor* goes first, while
 * its activated threads live on -- the class doc header's "Stability note" scenario.  The dtor must delete
 * every state (other threads' included); the threads' own eventual exits must then find nothing to clean --
 * and not crash trying -- in either thread sort (boost::thread and std::thread cleanup phases run on opposite
 * sides of thread_local deinit; see the Thread_local_ptr saga below).  Should this test ever misbehave: the
 * Stability note and the Thread_local_ptr doc header's "A corner case" section document a latent danger in
 * exactly this scenario -- in which case this test has justified its existence as the canary. */
TEST(Thread_local_state_registry, Cleanup_choreography)
{
  using boost::shared_future;

  Test_logger logger;

  s_n_state_dtors = 0;

  optional<Tl_reg<State>> reg{std::in_place, &logger, "choreoReg"};

  promise<void> activated_b;
  promise<void> activated_s;
  promise<void> unblock;
  shared_future<void> unblock_future{unblock.get_future().share()};

  const auto thread_body = [&](promise<void>* activated)
  {
    reg->this_thread_state();
    activated->set_value();
    unblock_future.wait();
    /* The registry is gone by now; we may not touch it (nor our state -- deleted).  Just exit: this thread's
     * thread-local cleanup must detect the dead registry and no-op. */
  };
  boost::thread thread_b{[&]() { thread_body(&activated_b); }};
  std::thread thread_s{[&]() { thread_body(&activated_s); }};
  activated_b.get_future().wait();
  activated_s.get_future().wait();

  reg->this_thread_state(); // And the main thread makes 3.
  EXPECT_EQ(s_n_state_dtors.load(), 0);

  reg.reset(); // The registry dtor deletes all 3 -- 2 of them out from under still-running threads.
  EXPECT_EQ(s_n_state_dtors.load(), 3);

  unblock.set_value();
  thread_b.join();
  thread_s.join();
  EXPECT_EQ(s_n_state_dtors.load(), 3); // Their exits cleaned nothing further (and did not crash trying).
} // TEST(Thread_local_state_registry, Cleanup_choreography)

/* The birth half of the lifecycle locking guarantee (class doc header, lifecycle section): a state's ctor
 * runs *inside* the registry-locked section -- the thread-caching-of-central-state pattern depends on it.
 * Technique: the ctor parks at a gate mid-flight; a helper thread then attempts while_locked(), whose task
 * asserts the ctor has finished by the time it runs -- red if the ctor body executes outside the lock. */
TEST(Thread_local_state_registry, Ctor_runs_locked)
{
  Test_logger logger;

  s_ctor_entered = false;
  s_ctor_release = false;
  s_ctor_finished = false;

  Tl_reg<Gated_ctor_state> reg{&logger, "ctorLockReg"};

  boost::thread creator{[&]() { reg.this_thread_state(); }};
  while (!s_ctor_entered)
  {
    this_thread::yield();
  }
  // The ctor is now parked -- with the registry locked, per contract.

  bool helper_saw_ctor_finished = false;
  boost::thread helper{[&]()
  {
    reg.while_locked([&](const auto&) { helper_saw_ctor_finished = s_ctor_finished; });
  }};
  /* Give the helper a beat to reach the mutex -- the suite's only sleep; one cannot portably observe
   * "blocked on a mutex."  It errs one-sidedly: a correct impl passes regardless; a broken one (ctor body
   * outside the locked section) gets caught, since then while_locked() can run during the park, while
   * s_ctor_finished is still false. */
  this_thread::sleep_for(boost::chrono::milliseconds(250));

  s_ctor_release = true;
  creator.join();
  helper.join();

  EXPECT_TRUE(helper_saw_ctor_finished); // while_locked() could not interleave with the parked ctor.
} // TEST(Thread_local_state_registry, Ctor_runs_locked)

/* The death half of the same guarantee: at thread-exit cleanup, a state is un-listed under the registry lock
 * but destroyed *after unlocking* -- so `~Thread_local_state()` may itself take locks, while_locked() on the
 * very same registry included.  (SHM-jemalloc's Thread_cache dtor, for one, banks on this.)  The specimen's
 * dtor does exactly that: the test completing at all proves the no-deadlock half; the dtor's observation that
 * it is no longer listed proves the un-list-before-destroy ordering.  Both thread sorts, as usual. */
TEST(Thread_local_state_registry, Dtor_runs_unlocked)
{
  Test_logger logger;

  Tl_reg<Locking_dtor_state> reg{&logger, "dtorLockReg"};

  const auto thread_body = [&]()
  {
    reg.this_thread_state()->m_reg = &reg;
  }; // Thread exit: cleanup un-lists the state, unlocks, then destroys it; the dtor locks freely.

  s_dtor_while_locked_ran = false;
  s_dtor_saw_self_listed = true;
  boost::thread{thread_body}.join();
  EXPECT_TRUE(s_dtor_while_locked_ran);
  EXPECT_FALSE(s_dtor_saw_self_listed);

  s_dtor_while_locked_ran = false;
  s_dtor_saw_self_listed = true;
  std::thread{thread_body}.join();
  EXPECT_TRUE(s_dtor_while_locked_ran);
  EXPECT_FALSE(s_dtor_saw_self_listed);
} // TEST(Thread_local_state_registry, Dtor_runs_unlocked)

/* We document that a state's dtor -- running during the thread-exit cleanup phase -- can create a successor
 * state via this_thread_state(), even one of a different type in a different registry; the cleanup phase's
 * repeat-until-empty algorithm (spanning all registries) sees each successor through its own full lifecycle,
 * death included.  The specimens' chain (see their doc header): Reborn_state gen 1 -> Reborn_kin_state ->
 * Reborn_state gen 2 -> end; so three cleanup-loop iterations, crossing types both ways, the last one via a
 * slot the loop had already processed once.  Assertable: 2+1 ctors and 2+1 dtors by join time; and both
 * registries list nothing afterward (each successor was properly un-listed, not leaked).  Both thread
 * sorts -- in the std::thread one the nested births happen in the post-thread_local-deinit window, the
 * spicier variant (see comment at that leg). */
TEST(Thread_local_state_registry, Rebirth_during_cleanup)
{
  Test_logger logger;

  Tl_reg<Reborn_state> reg{&logger, "rebirthReg"};
  Tl_reg<Reborn_kin_state> kin_reg{&logger, "rebirthKinReg"};
  s_rebirth_reg = &reg;
  s_rebirth_kin_reg = &kin_reg;

  // (No initial zeroing needed: the 4 counters are file-scope-initialized to 0 and used by this TEST only.)
  const auto run_leg_checks = [&]()
  {
    EXPECT_EQ(s_n_rebirth_ctors.load(), 2);
    EXPECT_EQ(s_n_rebirth_dtors.load(), 2);
    EXPECT_EQ(s_n_rebirth_kin_ctors.load(), 1);
    EXPECT_EQ(s_n_rebirth_kin_dtors.load(), 1);
    s_n_rebirth_ctors = 0;
    s_n_rebirth_dtors = 0;
    s_n_rebirth_kin_ctors = 0;
    s_n_rebirth_kin_dtors = 0;
  };

  boost::thread{[&]() { reg.this_thread_state(); }}.join();
  run_leg_checks();

  /* The std::thread sort: rebirth in the *post*-thread_local-deinit cleanup phase.  There the
   * this_thread_state() slow path leans on flow.log's near-thread-death guarantees: thread-info capture
   * works off immortal (trivially-destructible thread_local) storage; and logging works even with the
   * per-thread appender machinery gone (a fresh appender is used per call).  This leg is those guarantees'
   * regression test as much as this class's. */
  std::thread{[&]() { reg.this_thread_state(); }}.join();
  run_leg_checks();

  reg.while_locked([](const auto& state_per_thread) { EXPECT_TRUE(state_per_thread.empty()); });
  kin_reg.while_locked([](const auto& state_per_thread) { EXPECT_TRUE(state_per_thread.empty()); });

  s_rebirth_reg = nullptr;
  s_rebirth_kin_reg = nullptr;
} // TEST(Thread_local_state_registry, Rebirth_during_cleanup)

/* while_locked(): the registry's main reason for existing.  Checks: the map lists exactly the extant states; Metadata
 * matches each state's owner thread (unique-token cross-check); writes to individual states from under the
 * lock are seen by owner threads (the doc'd cross-thread signaling idiom -- here via the states' embedded
 * `Poll_flag`s); callable from any thread; callable on a `const` registry. */
TEST(Thread_local_state_registry, While_locked)
{
  Test_logger logger;
  FLOW_LOG_SET_CONTEXT(&logger, Flow_log_component::S_UNCAT);

  s_n_state_dtors = 0;

  Tl_reg<State> reg{&logger, "wlReg"};

  Thread_loop t1{&logger, "wl1"};
  Thread_loop t2{&logger, "wl2"};
  t1.start();
  t2.start();

  // Who owns what, according to each owner itself -- cross-checked against Metadata below.
  boost::unordered_map<State*, Thread_token> owner_by_state;
  State* s1{};
  State* s2{};
  post_wait(&t1, [&]() { s1 = reg.this_thread_state(); owner_by_state[s1] = this_thread_unique_token(); });
  post_wait(&t2, [&]() { s2 = reg.this_thread_state(); owner_by_state[s2] = this_thread_unique_token(); });
  State* const s_main = reg.this_thread_state();
  owner_by_state[s_main] = this_thread_unique_token();

  const auto& const_reg = reg;
  const_reg.while_locked([&](const auto& state_per_thread) // (On a const registry, note.)
  {
    EXPECT_EQ(state_per_thread.size(), 3u);
    for (const auto& [state, mdt] : state_per_thread)
    {
      const auto it = owner_by_state.find(state);
      ASSERT_NE(it, owner_by_state.end());
      EXPECT_EQ(mdt.m_thread_token, it->second);
      FLOW_LOG_INFO("Listed per-thread state of thread [" << mdt.m_thread_nickname << "]; "
                    "unique-token matches the owner's own reading.");
    }
    // Write to the pointees from under the lock (the doc'd signaling idiom): arm every state's flag.
    for (const auto& [state, nil] : state_per_thread)
    {
      state->m_flag.arm_next_poll();
    }
  });

  // Each owner observes its arm.
  post_wait(&t1, [&]() { EXPECT_TRUE(reg.this_thread_state()->m_flag.poll_armed()); });
  post_wait(&t2, [&]() { EXPECT_TRUE(s2->m_flag.poll_armed()); }); // (Via saved pointer: equally kosher.)
  EXPECT_TRUE(s_main->m_flag.poll_armed());

  // while_locked() is callable from any thread, not only the main/registry-creating one.
  post_wait(&t1, [&]() { reg.while_locked([](const auto& state_per_thread)
                                            { EXPECT_EQ(state_per_thread.size(), 3u); }); });

  // The listing tracks thread exits, naturally.
  t1.stop();
  t2.stop();
  reg.while_locked([&](const auto& state_per_thread)
  {
    ASSERT_EQ(state_per_thread.size(), 1u);
    EXPECT_EQ(state_per_thread.begin()->first, s_main);
  });
  EXPECT_EQ(s_n_state_dtors.load(), 2);
} // TEST(Thread_local_state_registry, While_locked)

/* The log::Log_context_mt integration (S_TL_STATE_HAS_MT_LOG_CONTEXT): with Thread_local_state deriving
 * Log_context_mt, the default create-func becomes `new State{L}` (L = the registry's current Logger); and
 * set_logger() pushes the new Logger to every extant state (on top of re-aiming the registry's own logging). */
TEST(Thread_local_state_registry, Logger_propagation)
{
  static_assert(!Tl_reg<State>::S_TL_STATE_HAS_MT_LOG_CONTEXT);
  static_assert(Tl_reg<Logging_state>::S_TL_STATE_HAS_MT_LOG_CONTEXT);

  Test_logger logger1;
  Test_logger logger2;

  s_n_state_dtors = 0;
  {
    Tl_reg<Logging_state> reg{&logger1, "logPropReg"}; // No create-func: the Log_context_mt default form.

    Thread_loop t1{&logger1, "logProp1"};
    t1.start();
    Logging_state* worker_state{};
    post_wait(&t1, [&]() { worker_state = reg.this_thread_state(); });
    EXPECT_EQ(worker_state->observed_logger(), &logger1); // The default create-func fed it the ctor-time Logger.

    reg.set_logger(&logger2); // Propagates to the registry itself and to every extant state...
    EXPECT_EQ(worker_state->observed_logger(), &logger2);
    EXPECT_EQ(reg.this_thread_state()->observed_logger(), &logger2); // ...and to any created later, of course.

    reg.set_logger(nullptr); // Null Logger is a first-class value (the pre-main()/post-main() reality).
    EXPECT_EQ(worker_state->observed_logger(), nullptr);

    t1.stop();
    EXPECT_EQ(s_n_state_dtors.load(), 1);
  }
  EXPECT_EQ(s_n_state_dtors.load(), 2);
} // TEST(Thread_local_state_registry, Logger_propagation)

/* Poll_flag alone: disarmed at birth; arm_next_poll() yields exactly one `true` poll_armed() (then `false`
 * again); arming is idempotent (a flag, not a counter); re-armable indefinitely; cross-thread arming is
 * visible (join-synced here; the lock-free-pattern-level synchronization is exercised by the
 * Polled_shared_state TESTs). */
TEST(Poll_flag, Interface)
{
  Poll_flag flag;
  EXPECT_FALSE(flag.poll_armed()); // Disarmed at birth...
  EXPECT_FALSE(flag.poll_armed()); // ...and polling-while-disarmed does not somehow arm.

  flag.arm_next_poll();
  EXPECT_TRUE(flag.poll_armed()); // The one.
  EXPECT_FALSE(flag.poll_armed()); // Consumed.

  flag.arm_next_poll();
  flag.arm_next_poll(); // Arming twice: still just the one `true`.
  EXPECT_TRUE(flag.poll_armed());
  EXPECT_FALSE(flag.poll_armed());

  boost::thread{[&]() { flag.arm_next_poll(); }}.join();
  EXPECT_TRUE(flag.poll_armed());
  EXPECT_FALSE(flag.poll_armed());
} // TEST(Poll_flag, Interface)

/* Polled_shared_state: the pattern end-to-end, closely following the class doc header's worked example
 * (missile launches): an outside thread loads the shared state and then -- only then -- arms each per-thread
 * flag, all within registry.while_locked(); each worker opportunistically polls (fast-path: a lone atomic op)
 * and, on the rare armed poll, consumes its shared-state entry under pss.while_locked(); whichever worker
 * consumes last reports completion.  Plus: ctor-arg forwarding to the Shared_state. */
TEST(Polled_shared_state, Pattern)
{
  using std::deque;
  using std::set;

  Test_logger logger;

  s_n_state_dtors = 0;

  { // Ctor-arg forwarding to the stored Shared_state.
    Polled_shared_state<vector<int>> pss{size_t(3), 42};
    pss.while_locked([](vector<int>* v) { EXPECT_EQ(*v, (vector<int>{42, 42, 42})); });
  }

  Tl_reg<State> reg{&logger, "pssReg"};
  Polled_shared_state<set<State*>> pss;

  deque<Thread_loop> loops;
  for (size_t idx = 0; idx != 3; ++idx)
  {
    auto& loop = loops.emplace_back(&logger, ostream_op_string("pss", idx));
    loop.start();
    post_wait(&loop, [&]() { reg.this_thread_state(); }); // Activate (creates the state and hence its flag).
  }

  int n_launches = 0; // Guarded by the pss lock.
  bool success_reported = false; // Ditto; stands in for the doc example's report_success().

  // Fast-path check: disarmed => the poll says no, and that is the entire cost of asking.
  for (auto& loop : loops)
  {
    post_wait(&loop, [&]() { EXPECT_FALSE(reg.this_thread_state()->m_flag.poll_armed()); });
  }

  // The arming side, per the doc: load the shared state first, arm after, all within registry.while_locked().
  reg.while_locked([&](const auto& state_per_thread)
  {
    EXPECT_EQ(state_per_thread.size(), 3u);
    pss.while_locked([&](set<State*>* threads_to_launch)
    {
      for (const auto& [state, nil] : state_per_thread)
      {
        threads_to_launch->insert(state);
      }
    });
    for (const auto& [state, nil] : state_per_thread) // *After* -- never before -- loading the shared state.
    {
      state->m_flag.arm_next_poll();
    }
  });

  // The consuming side, also per the doc: poll; if armed do our share; the last one out reports.
  const auto opportunistically_launch = [&]()
  {
    State* const self = reg.this_thread_state();
    if (!self->m_flag.poll_armed())
    {
      return; // Fast-path (the 2nd, post-consumption poll per worker takes it; the 1st never does).
    }
    pss.while_locked([&](set<State*>* threads_to_launch)
    {
      if (threads_to_launch->erase(self) == 0)
      {
        return;
      }
      ++n_launches;
      if (threads_to_launch->empty())
      {
        success_reported = true;
      }
    });
  };
  for (auto& loop : loops)
  {
    post_wait(&loop, opportunistically_launch);
    post_wait(&loop, opportunistically_launch); // The 2nd poll: consumed => fast-path (no double launch).
  }

  EXPECT_EQ(n_launches, 3);
  EXPECT_TRUE(success_reported);
  pss.while_locked([](set<State*>* threads_to_launch) { EXPECT_TRUE(threads_to_launch->empty()); });

  for (auto& loop : loops)
  {
    loop.stop();
  }
  EXPECT_EQ(s_n_state_dtors.load(), 3);
} // TEST(Polled_shared_state, Pattern)

/* The pattern's essential teardown guarantee (Polled_shared_state doc header, Lifetime/safety section):
 * arming within registry.while_locked() is safe even as target threads concurrently exit -- a state listed in
 * the map is alive, flag included, at least until the lock is released, since an exiting thread's cleanup
 * contends for that same lock.  Here an armer thread hammers exactly the doc'd arming loop while waves of
 * short-lived threads (boost and std sorts both) activate, wait to observe their own flag armed, and exit --
 * so every wave exercises the arm-vs-exit window.  The assertable part: every thread observes its arm (the
 * handshake), and every state is cleaned exactly once; the sharper part is implicit: no use-after-free in
 * that window (ASAN/TSAN runs make it explicit). */
TEST(Polled_shared_state, Arming_vs_thread_exit)
{
  Test_logger logger;

  s_n_state_dtors = 0;

  optional<Tl_reg<State>> reg{std::in_place, &logger, "armExitReg"};
  atomic<bool> stop{false};
  atomic<int> n_arms_observed{0};

  boost::thread armer{[&]()
  {
    while (!stop.load(std::memory_order_relaxed))
    {
      reg->while_locked([](const auto& state_per_thread)
      {
        for (const auto& [state, nil] : state_per_thread)
        {
          state->m_flag.arm_next_poll();
        }
      });
      this_thread::yield();
    }
  }};

  const auto worker_body = [&]()
  {
    State* const self = reg->this_thread_state();
    while (!self->m_flag.poll_armed()) // The armer will get to us: we are in the map.
    {
      this_thread::yield();
    }
    ++n_arms_observed;
    // Exit forthwith: our cleanup (which contends for the registry lock) races the armer's next pass.
  };

  for (unsigned int wave = 0; wave != 8; ++wave)
  {
    boost::thread b1{worker_body};
    boost::thread b2{worker_body};
    std::thread s1{worker_body};
    std::thread s2{worker_body};
    b1.join();
    b2.join();
    s1.join();
    s2.join();
  }

  stop = true;
  armer.join();

  EXPECT_EQ(n_arms_observed.load(), 32); // (= 8 waves x 4 threads.)
  EXPECT_EQ(s_n_state_dtors.load(), 32);
  reg.reset();
  EXPECT_EQ(s_n_state_dtors.load(), 32); // (Neither the armer nor main ever activated a state.)
} // TEST(Polled_shared_state, Arming_vs_thread_exit)

/* Thread_local_obj_deinit_safe: the live-thread contract -- lazy per-thread creation; identity across calls;
 * per-thread distinctness; <Obj, Tag> identity (same Obj + different Tag => independent storage); dtor runs
 * at thread exit. */
TEST(Thread_local_obj_deinit_safe, Interface)
{
  using Facility = Thread_local_obj_deinit_safe<Tlods_payload>; // Tag defaulted: *the* per-thread Tlods_payload.
  using Facility_b = Thread_local_obj_deinit_safe<Tlods_payload, struct Iface_b_tag>; // Same Obj, own storage.

  s_n_tlods_dtors = 0;

  Tlods_payload* const obj = Facility::this_thread_obj_or_null();
  ASSERT_TRUE(obj);
  EXPECT_EQ(obj->m_val, 42); // Default-constructed on first access.
  obj->m_val = 7;
  EXPECT_EQ(Facility::this_thread_obj_or_null(), obj); // Identity across calls.
  EXPECT_EQ(Facility::this_thread_obj_or_null()->m_val, 7);

  // Distinct Tag => distinct object (same Obj type, same thread).
  Tlods_payload* const obj_b = Facility_b::this_thread_obj_or_null();
  ASSERT_TRUE(obj_b);
  EXPECT_NE(obj_b, obj);
  EXPECT_EQ(obj_b->m_val, 42); // Fresh, unaffected by obj->m_val = 7.

  // Distinct thread => distinct object; and thread exit destroys exactly that thread's object.
  boost::thread{[&]()
  {
    Tlods_payload* const worker_obj = Facility::this_thread_obj_or_null();
    ASSERT_TRUE(worker_obj);
    EXPECT_NE(worker_obj, obj);
    EXPECT_EQ(worker_obj->m_val, 42);
    worker_obj->m_val = 9;
  }}.join();
  EXPECT_EQ(s_n_tlods_dtors.load(), 1); // The worker's object died with it...
  EXPECT_EQ(Facility::this_thread_obj_or_null()->m_val, 7); // ...ours being none of its business.

  /* (Main thread's 2 objects die at main-thread/process exit; no dtor-count assert is possible for them here.
   * The Deinit_window TEST covers the death-side semantics deterministically.) */
} // TEST(Thread_local_obj_deinit_safe, Interface)

/* Thread_local_obj_deinit_safe: its main reason for existing -- access during thread_local deinit.  Deterministic by
 * construction order (reverse-order destruction): a prober thread_local constructed *before* the facility's
 * first access is destroyed *after* the payload -- its dtor must observe null (twice: once null, always
 * null); a prober constructed *after* is destroyed *before* -- its dtor must observe the live payload with
 * values intact.  Both thread sorts: the mechanism is pure thread_local, but the cheap paranoia does not
 * hurt. */
TEST(Thread_local_obj_deinit_safe, Deinit_window)
{
  const auto thread_body = []()
  {
    [[maybe_unused]] thread_local Tlods_before_prober s_before{}; // Constructed 1st => destroyed last.

    Tlods_payload* const obj = Tlods_window_facility::this_thread_obj_or_null(); // Payload born 2nd.
    ASSERT_TRUE(obj);
    obj->m_val = 55;

    [[maybe_unused]] thread_local Tlods_after_prober s_after{}; // Constructed 3rd => destroyed first.
  };
  const auto check_and_rewind = [&]()
  {
    EXPECT_TRUE(s_tlods_after_prober_ran);
    EXPECT_EQ(s_tlods_val_seen_by_after_prober, 55); // Payload was alive and intact for the after-prober.
    EXPECT_TRUE(s_tlods_before_prober_ran);
    EXPECT_FALSE(s_tlods_seen_by_before_prober); // Dead by then; null -- not a crash, not a stale pointer.
    EXPECT_FALSE(s_tlods_seen_by_before_prober_2nd); // And once null, always null.

    s_tlods_before_prober_ran = false;
    s_tlods_seen_by_before_prober = nullptr;
    s_tlods_seen_by_before_prober_2nd = nullptr;
    s_tlods_after_prober_ran = false;
    s_tlods_val_seen_by_after_prober = 0;
  };

  s_n_tlods_dtors = 0;

  boost::thread{thread_body}.join();
  check_and_rewind();

  std::thread{thread_body}.join();
  check_and_rewind();

  EXPECT_EQ(s_n_tlods_dtors.load(), 2); // One payload per thread; each died exactly once.
} // TEST(Thread_local_obj_deinit_safe, Deinit_window)

/* The facility doc header's "Corner case," part 1: the first-ever access in a thread occurring *during*
 * thread_local deinit.  Promised: a full lifecycle compressed into the deinit phase -- the object is born
 * usable, and the ongoing deinit pass destroys it too (registrations made mid-pass are processed by that
 * same pass, in the Itanium-ABI/glibc regime).  The dtor-count assert is the key one: on a hypothetical
 * platform whose deinit pass ignored mid-pass registrations it would go red -- making this TEST the runtime
 * canary complementing the header's port-gate static_assert(). */
TEST(Thread_local_obj_deinit_safe, First_access_during_deinit)
{
  const auto thread_body = []()
  {
    [[maybe_unused]] thread_local Tlods_case1_prober s_prober{};
    // Note: not touching Tlods_case1_facility here: its first access must be the one in the prober's dtor.
  };
  const auto check_and_rewind = [&]()
  {
    EXPECT_TRUE(s_tlods_case1_prober_ran);
    EXPECT_TRUE(s_tlods_case1_saw_obj); // Born mid-deinit...
    EXPECT_TRUE(s_tlods_case1_value_ok); // ...usable and identity-stable...
    EXPECT_EQ(s_n_tlods_dtors.load(), 1); // ...and the same deinit pass destroyed it (the canary assert).
    s_tlods_case1_prober_ran = false;
    s_tlods_case1_saw_obj = false;
    s_tlods_case1_value_ok = false;
    s_n_tlods_dtors = 0;
  };

  s_n_tlods_dtors = 0;
  boost::thread{thread_body}.join();
  check_and_rewind();
  std::thread{thread_body}.join();
  check_and_rewind();
} // TEST(Thread_local_obj_deinit_safe, First_access_during_deinit)

/* Part 2: the first-ever access occurring *after* deinit -- via Thread_local_ptr cleanup in a std::thread,
 * which runs post-deinit (necessarily a std::thread: a boost::thread's tsp-style cleanup precedes deinit, so
 * there the same first access would just be a normal one).  Promised: the object is born fine and fully
 * usable for the remaining teardown; but its dtor -- though registered -- never runs (the deinit pass is
 * over): the documented once-per-dying-thread leak. */
TEST(Thread_local_obj_deinit_safe, First_access_post_deinit)
{
  s_tlods_case2_hook_ran = false;
  s_tlods_case2_saw_obj = false;
  s_tlods_case2_value_ok = false;
  s_n_tlods_case2_ctors = 0;
  s_n_tlods_case2_dtors = 0;

  static int s_dummy = 0; // Static storage: the Tlp cleanup hook is a probe, not a deleter.
  Thread_local_ptr<int> tlp{&tlods_case2_tlp_cleanup};

  std::thread{[&]() { tlp.reset(&s_dummy); }}.join(); // Thread exit fires the hook, post-deinit.

  EXPECT_TRUE(s_tlods_case2_hook_ran);
  EXPECT_TRUE(s_tlods_case2_saw_obj); // Born post-deinit...
  EXPECT_TRUE(s_tlods_case2_value_ok); // ...usable and identity-stable...
  EXPECT_EQ(s_n_tlods_case2_ctors.load(), 1);
  EXPECT_EQ(s_n_tlods_case2_dtors.load(), 0); // ...and never destroyed: the documented leak, verified.
} // TEST(Thread_local_obj_deinit_safe, First_access_post_deinit)

/* For the Thread_local_ptr tests: its cleanup_func_t is a plain function pointer, so the observation
 * points must be file-scope state + free functions, not capturing lambdas. */
namespace tlp_test
{

int s_cleanup_count = 0; // How many times count_cleanup() ran...
int* s_last_cleaned = nullptr; // ...and its argument the last time.

void count_cleanup(int* val)
{
  ++s_cleanup_count;
  s_last_cleaned = val;
}

// For the payload-deletion (default-ctor Thread_local_ptr) check:
struct Dtor_counting_payload
{
  int* m_dtor_count_ptr;
  ~Dtor_counting_payload() { ++*m_dtor_count_ptr; }
};

// For the cleanup-window (money) test:
Thread_local_ptr<int>* s_tlp_a = nullptr; // The *other* Tlp, accessed from inside b_window_cleanup().
bool s_b_cleanup_ran = false;
int* s_a_seen_by_b_cleanup = nullptr;
Thread_local_ptr<int>* s_tlp_c = nullptr; // The self-re-arming one.
int s_c_cleanup_count = 0;
int s_c_rearm_payload = 0;

void b_window_cleanup(int* /* unused: static-storage payload */)
{
  s_b_cleanup_ran = true;
  /* The point of the whole exercise: during a std::thread's exit, cleanup runs *after* thread_local deinit --
   * squarely in the window where the fast-path cache is inoperative -- and get() must still work by falling
   * back to the canonical thread_specific_ptr.  (Cleanup order among Tlps is unspecified: the result may be
   * the still-set value or null-because-already-cleaned; both fine; crashing/garbage is what's tested against.) */
  s_a_seen_by_b_cleanup = s_tlp_a->get();
}

void c_window_cleanup(int* /* unused: static-storage payload */)
{
  /* reset()-to-non-null from inside one's own cleanup: per contract (inherited from thread_specific_ptr)
   * this re-arms the slot, and the cleanup pass loops until nothing remains -- so this very function runs
   * once more, for the re-armed value. */
  if (++s_c_cleanup_count == 1)
  {
    s_tlp_c->reset(&s_c_rearm_payload);
  }
}

// For the Thread_local_deinit_window test:
Thread_local_ptr<int>* s_tlp_d = nullptr;
bool s_tl_dtor_ran = false;
int* s_d_seen_by_tl_dtor = nullptr;

// A plain user thread_local whose dtor probes a Thread_local_ptr during thread_local-deinit itself.
struct Tl_dtor_prober
{
  ~Tl_dtor_prober()
  {
    if (s_tlp_d)
    {
      s_tl_dtor_ran = true;
      s_d_seen_by_tl_dtor = s_tlp_d->get();
    }
  }
};

// For the Exit_skips_cleanup death test.  Values distinguish the slots (plain-fn-ptr constraint, as above).
void exit_probe_cleanup(int* val)
{
  if (*val == 7) // The joined thread's value: its exit-time cleanup is the positive control.
  {
    std::fprintf(stderr, "thread-value-cleanup-ran ");
    return;
  }
  // else: the main thread's value (9): per contract this must never run during exit(); scream via exit code.
  std::_Exit(42);
}

} // namespace tlp_test

/* Thread_local_ptr: the thread_specific_ptr-equivalent contract, single-threaded portion.
 * (Thread-locality and exit-cleanup: next TEST.  Perf: see Benchmark below.) */
TEST(Thread_local_ptr, Interface)
{
  using tlp_test::count_cleanup;
  using tlp_test::Dtor_counting_payload;

  tlp_test::s_cleanup_count = 0;
  tlp_test::s_last_cleaned = nullptr;

  int a = 1;
  int b = 2;

  { // The custom-cleanup-function form.
    Thread_local_ptr<int> tlp{&count_cleanup};
    EXPECT_EQ(tlp.get(), nullptr); // Fresh: null in this thread.

    tlp.reset(&a);
    EXPECT_EQ(tlp.get(), &a);
    EXPECT_EQ(*tlp, 1); // operator*.
    EXPECT_EQ(tlp.operator->(), &a); // operator-> (contractually just get()).
    EXPECT_EQ(tlp_test::s_cleanup_count, 0); // No cleanup: nothing was displaced.

    tlp.reset(&a); // Same value: no-op, in particular no cleanup.
    EXPECT_EQ(tlp_test::s_cleanup_count, 0);

    tlp.reset(&b); // Different value: displaced value is cleaned.
    EXPECT_EQ(tlp.get(), &b);
    EXPECT_EQ(tlp_test::s_cleanup_count, 1);
    EXPECT_EQ(tlp_test::s_last_cleaned, &a);

    EXPECT_EQ(tlp.release(), &b); // Forgets -- but does NOT clean.
    EXPECT_EQ(tlp.get(), nullptr);
    EXPECT_EQ(tlp_test::s_cleanup_count, 1);

    tlp.reset(&a);
    tlp.reset(); // reset-to-null: cleans.
    EXPECT_EQ(tlp.get(), nullptr);
    EXPECT_EQ(tlp_test::s_cleanup_count, 2);
    EXPECT_EQ(tlp_test::s_last_cleaned, &a);

    // Two instances: entirely independent slots (they share a per-thread map inside; must not interfere).
    Thread_local_ptr<int> tlp2{&count_cleanup};
    tlp.reset(&a);
    tlp2.reset(&b);
    EXPECT_EQ(tlp.get(), &a);
    EXPECT_EQ(tlp2.get(), &b);
    tlp2.reset();
    EXPECT_EQ(tlp.get(), &a); // Unbothered by the neighbor's reset...
    EXPECT_EQ(tlp_test::s_cleanup_count, 3);

    // Scope exit: dtor cleans this thread's value (tlp holds &a; tlp2 holds nothing).
  }
  EXPECT_EQ(tlp_test::s_cleanup_count, 4);
  EXPECT_EQ(tlp_test::s_last_cleaned, &a);

  // The default-ctor form: cleanup = `delete`.
  int n_dtors = 0;
  {
    Thread_local_ptr<Dtor_counting_payload> tlp; // (Declared after n_dtors: payload dtors can outlive it not.)
    tlp.reset(new Dtor_counting_payload{&n_dtors});
    EXPECT_EQ(n_dtors, 0);
    tlp.reset(new Dtor_counting_payload{&n_dtors}); // Displaced one is deleted...
    EXPECT_EQ(n_dtors, 1);
  } // ...and the dtor deletes the current one:
  EXPECT_EQ(n_dtors, 2);
} // TEST(Thread_local_ptr, Interface)

/* Thread_local_ptr: the multi-threaded portion.  Values are per-thread; thread exit runs cleanup for that
 * thread's value -- in both boost::thread and std::thread, whose cleanup phases run on opposite sides of
 * thread_local deinit (see class doc header: that duality is the class's central battlefield). */
TEST(Thread_local_ptr, Thread_locality)
{
  using tlp_test::count_cleanup;

  tlp_test::s_cleanup_count = 0;

  int main_val = 100;
  int boost_val = 200;
  int std_val = 300;

  Thread_local_ptr<int> tlp{&count_cleanup};
  tlp.reset(&main_val);

  boost::thread{[&]()
  {
    EXPECT_EQ(tlp.get(), nullptr); // Sibling threads' values are invisible here.
    tlp.reset(&boost_val);
    EXPECT_EQ(tlp.get(), &boost_val);
  }}.join();
  EXPECT_EQ(tlp_test::s_cleanup_count, 1); // The boost::thread's exit cleaned its value...
  EXPECT_EQ(tlp_test::s_last_cleaned, &boost_val);
  EXPECT_EQ(tlp.get(), &main_val); // ...ours being none of its business.

  std::thread{[&]()
  {
    EXPECT_EQ(tlp.get(), nullptr);
    tlp.reset(&std_val);
    EXPECT_EQ(tlp.get(), &std_val);
  }}.join();
  EXPECT_EQ(tlp_test::s_cleanup_count, 2); // Ditto the std::thread (the post-thread_local-deinit sort).
  EXPECT_EQ(tlp_test::s_last_cleaned, &std_val);
  EXPECT_EQ(tlp.get(), &main_val);

  tlp.reset(); // Clean up shop (before dtor, for determinism of the count check).
  EXPECT_EQ(tlp_test::s_cleanup_count, 3);
} // TEST(Thread_local_ptr, Thread_locality)

/* Thread_local_ptr: the cleanup-window corner.  During a std::thread's exit, cleanup functions run *after*
 * thread_local deinit -- the fast-path cache is gone, and the class must transparently fall back to the
 * canonical thread_specific_ptr.  A cleanup function here (1) get()s a *different* Tlp and (2) re-arms its
 * own slot via reset()-to-non-null (which the cleanup pass must then clean once more).  Mostly this test's
 * assertion is implicit: no crash/no UAF in that window (ASAN/TSAN CI runs make it explicit). */
TEST(Thread_local_ptr, Cleanup_window)
{
  namespace tt = tlp_test;

  tt::s_b_cleanup_ran = false;
  tt::s_a_seen_by_b_cleanup = nullptr;
  tt::s_c_cleanup_count = 0;

  int a_val = 1;
  int b_val = 2;
  int c_val = 3;

  Thread_local_ptr<int> tlp_a{nullptr}; // Null cleanup: it is only get()-bait for tlp_b's cleanup.
  Thread_local_ptr<int> tlp_b{&tt::b_window_cleanup};
  Thread_local_ptr<int> tlp_c{&tt::c_window_cleanup};
  tt::s_tlp_a = &tlp_a;
  tt::s_tlp_c = &tlp_c;

  const auto set_all = [&]()
  {
    tlp_a.reset(&a_val);
    tlp_b.reset(&b_val);
    tlp_c.reset(&c_val);
  };
  const auto check_and_rewind = [&]()
  {
    EXPECT_TRUE(tt::s_b_cleanup_ran);
    /* Cleanup order among the slots is unspecified: b's cleanup saw a's value either still-set or
     * already-cleaned-hence-null; anything else means the window machinery mis-tracked. */
    EXPECT_TRUE((tt::s_a_seen_by_b_cleanup == &a_val) || (tt::s_a_seen_by_b_cleanup == nullptr));
    EXPECT_EQ(tt::s_c_cleanup_count, 2); // Once for c_val; once more for the re-armed payload.
    tt::s_b_cleanup_ran = false;
    tt::s_a_seen_by_b_cleanup = nullptr;
    tt::s_c_cleanup_count = 0;
  };

  // The std::thread ordering: cleanup runs *after* thread_local deinit (cache dead; dtor-set flag governs).
  std::thread{set_all}.join();
  check_and_rewind();

  /* The boost::thread ordering: cleanup runs *before* thread_local deinit (cache technically alive, but the
   * at_thread_exit()-set flag has already forbidden it -- the other branch of the impl's analysis). */
  boost::thread{set_all}.join();
  check_and_rewind();

  tt::s_tlp_a = nullptr;
  tt::s_tlp_c = nullptr;
} // TEST(Thread_local_ptr, Cleanup_window)

/* Thread_local_ptr: verifies the documented exit()-time behavior (see dtor + ctor doc headers): when the
 * process exits -- as opposed to a non-main thread exiting -- the thread_specific_ptr cleanups (and
 * at_thread_exit() functions) do NOT run for the exiting thread; the value simply leaks into process
 * teardown.  Encoding: the main-value's cleanup, were it to wrongly run, changes the exit code (42); the
 * joined thread's cleanup is the positive control proving the cleanup plumbing as such is live. */
TEST(Thread_local_ptr, Exit_skips_cleanup)
{
  EXPECT_EXIT
  ({
     static int s_thread_val = 7;
     static int s_main_val = 9;
     Thread_local_ptr<int> tlp{&tlp_test::exit_probe_cleanup};
     boost::thread{[&]() { tlp.reset(&s_thread_val); }}.join(); // Positive control fires at this thread's exit.
     tlp.reset(&s_main_val);
     std::fprintf(stderr, "about-to-exit");
     std::exit(0);
   }, ::testing::ExitedWithCode(0), "thread-value-cleanup-ran.*about-to-exit");
} // TEST(Thread_local_ptr, Exit_skips_cleanup)

/* Thread_local_ptr: the *other* exit-time window -- a plain user `thread_local` whose dtor calls get()
 * during the thread_local-deinit phase itself, *after* the internal fast-path cache has been destroyed
 * (but before the thread_specific_ptr cleanup phase).  Deterministic by construction order: the prober
 * thread_local is constructed *first* and the Tlp's internal thread_local state *after* (by the first
 * reset()), so reverse-order destruction guarantees the internal state dies first; the prober's dtor then
 * lands exactly in the window where only the dtor-set inoperative flag prevents a read of the destroyed
 * map.  The value must still be readable -- via the canonical fallback, whose own cleanup phase is later
 * still. */
TEST(Thread_local_ptr, Thread_local_deinit_window)
{
  namespace tt = tlp_test;

  tt::s_tl_dtor_ran = false;
  tt::s_d_seen_by_tl_dtor = nullptr;

  int d_val = 4;
  Thread_local_ptr<int> tlp_d{nullptr}; // Null cleanup: the probing is the point, not cleanup.
  tt::s_tlp_d = &tlp_d;

  std::thread{[&]()
  {
    [[maybe_unused]] static thread_local tt::Tl_dtor_prober s_prober{}; // Constructed first => destroyed last.
    tlp_d.reset(&d_val); // Constructs the internal thread_local state now => destroyed before s_prober.
  }}.join();

  EXPECT_TRUE(tt::s_tl_dtor_ran);
  EXPECT_EQ(tt::s_d_seen_by_tl_dtor, &d_val); // Read through the fallback: exact, not merely non-crashing.

  tt::s_tlp_d = nullptr;
} // TEST(Thread_local_ptr, Thread_local_deinit_window)

/* The rudimentary benchmark Thread_local_ptr's doc header promises a reason to exist: get() faster than
 * boost::thread_specific_ptr's, at the cost of a slower reset().  No asserts on timings (never flake a
 * suite over machine noise): the numbers are logged for the eyeball.  A raw `thread_local` read is included
 * as the floor reference (caveat: unlike the other two -- whose accesses are opaque out-of-TU calls -- the
 * compiler may partially hoist it; take that number as an optimistic bound). */
TEST(Thread_local_ptr, Benchmark)
{
  /* The entire benchmark runs in a freshly spawned thread.  Rationale: both contestants keep one map
   * per thread, shared across all instances -- and on the main thread, by this point, those maps carry
   * leftovers of the preceding tests plus boost-internal TLS entries.  A virgin thread makes the
   * "1-entry"/"16-entry" labels true by construction.  (The other TESTs above need no such isolation:
   * their asserts are all keyed to their own instances' slots, to which neighboring map entries are
   * invisible.) */
  boost::thread{[]()
  {
    using perf::Checkpointing_timer;
    using perf::Clock_type;
    using boost::thread_specific_ptr;
    using boost::min_element;
    using boost::nano;
    using boost::chrono::round;
    using boost::chrono::microseconds;
    using std::deque;
    using Nanosec = boost::chrono::duration<double, nano>;
    using Nano_vec = vector<Nanosec>;

    constexpr unsigned int N_GETS = 10 * 1000 * 1000;
    constexpr unsigned int N_RESETS = 1000 * 1000;

    // The contestants.  (Null cleanup functions: irrelevant to what is measured; nothing to clean anyway.)
    Thread_local_ptr<uint64_t> ours{nullptr};
    thread_specific_ptr<uint64_t> boosts{nullptr};
    static thread_local uint64_t* s_raw_tl;

    uint64_t payload_a = 1;
    uint64_t payload_b = 2;
    ours.reset(&payload_a);
    boosts.reset(&payload_a);
    s_raw_tl = &payload_a;

    uint64_t sink = 0; // Accumulate dereferenced values, so the get()-loops cannot be optimized out.

    const auto bench = [&](std::string&& name, unsigned int n, auto&& op) -> Nanosec
    {
      const string name_copy{name}; // (The timer eats the original.)
      Checkpointing_timer sum_timer{nullptr, std::move(name), Checkpointing_timer::real_clock_types(), 1};
      for (unsigned int idx = 0; idx != n; ++idx)
      {
        op();
      }
      const auto total = sum_timer.checkpoint("total").m_since_last.m_values[size_t(Clock_type::S_REAL_HI_RES)];
      const auto result = Nanosec{total} / n;
      cout << '[' << name_copy << "]: total = [" << round<microseconds>(total) << "] over [" << n << "] ops"
              " => [" << result << "/op].\n" << flush;
      return result;
    };

    const auto us1 = bench("get: Thread_local_ptr (1-entry map)", N_GETS, [&]() { sink += *ours.get(); });
    const auto them1 = bench("get: boost::thread_specific_ptr (1-entry map)", N_GETS, [&]() { sink += *boosts.get(); });
    const auto floor = bench("get: raw thread_local (floor)", N_GETS, [&]() { sink += *s_raw_tl; });

    /* Now the same head-to-head but with realistically-populated per-thread maps: both impls keep ONE map
     * per thread shared across all instances, so instance-count = map occupancy.  A near-empty map is
     * std::map's (boost's) best case; several-to-dozens of instances (cf. Flow-IPC alone) is the realistic
     * regime and the flat-map design's home turf. */
    constexpr size_t N_INSTANCES = 16;
    deque<Thread_local_ptr<uint64_t>> ours_extra;
    deque<thread_specific_ptr<uint64_t>> boosts_extra;
    for (size_t idx = 0; idx != N_INSTANCES; ++idx)
    {
      ours_extra.emplace_back(nullptr).reset(&payload_b);
      boosts_extra.emplace_back(nullptr).reset(&payload_b);
    }
    const auto us16 = bench("get: Thread_local_ptr (16-entry map)", N_GETS, [&]() { sink += *ours.get(); });
    const auto them16 = bench("get: boost::thread_specific_ptr (16-entry map)", N_GETS,
                              [&]() { sink += *boosts.get(); });

    const auto us_rst = bench("reset: Thread_local_ptr", N_RESETS,
                              [&, flip = false]() mutable
                                { ours.reset(flip ? &payload_a : &payload_b); flip = !flip; });
    const auto them_rst = bench("reset: boost::thread_specific_ptr", N_RESETS,
                                [&, flip = false]() mutable
                                  { boosts.reset(flip ? &payload_a : &payload_b); flip = !flip; });

    EXPECT_NE(sink, 0u); // (Also keeps `sink` -- hence the loops -- honest.)

    /* Attn!  If the benchmark checks prove difficult to get a handle on in some environments, FAIL_*_OK -- if used
     * responsibly (*not* to avoid having to deal with potential real T_l_p perf flaws) -- are at our disposal.
     * There is also flow::test::Test_config::get_singleton().m_do_not_fail_benchmarks, but at this time we are
     * consciously ignoring it in favor of this classification.  It can be added for FAIL_IS_FAIL case (do not
     * fail test if a condition under FAIL_IS_FAIL does indeed fail), but it should be done only if we discover
     * a key environment, such as the open-source CI pipeline *potentially*, where ambient conditions are so variable
     * that they can knock otherwise behaving-as-expected benchmarks out of whack.  Or something. */
    enum check_t { FAIL_IS_FAIL, FAIL_MAYBE_OK, FAIL_IS_OK };

    const auto check = [&](bool condition, auto cond_str, check_t check_type)
    {
      if (condition)
      {
        cout << "Benchmark check: PASS: [" << cond_str << "].\n" << flush;
        return;
      }
      // else
      const bool ok = check_type == FAIL_IS_OK;
      (ok ? cout : cerr) << "Benchmark check: FAIL: [" << cond_str << "].\n";
      if (ok)
      {
        cout << "  [FAIL_IS_OK] This benchmark failure is known to be somewhat sporadic and acceptable.\n" << flush;
      }
      else if (check_type == FAIL_MAYBE_OK)
      {
        cerr << "  [FAIL_MAYBE_OK] This benchmark check is *suspected* of being unreliable; maybe look into it and\n"
                "                  potentially reclassify as FAIL_IS_OK or FAIL_IS_FAIL; or fix feature impl.\n"
                "                  Also: Probably disregard in Debug/`-O0`/... builds.\n" << flush;
      }
      else
      {
        cerr << "  [FAIL_IS_FAIL] This benchmark failure is fully unexpected; test shall fail; look into it.\n"
             << flush;
        EXPECT_TRUE(false) << "See preceding [FAIL_IS_FAIL] console message.";
      }
    }; // const auto check =

    check(floor < *(min_element(Nano_vec{us1, them1, us16, them16, us_rst, them_rst})),
          "floor < all others [floor = single thread_local access]", FAIL_IS_FAIL);
    // This one tends to be true almost by a factor of 2x (which conceptually makes sense; 2 inserts instead of 1.
    check(us_rst > them_rst, "reset(): T_l_p > t_s_p [T_l_p does the same work + more]", FAIL_IS_FAIL);
    // This one is the closest, but so far we've seen it be true by ~15%... but sensitive to ambient stuff going on.
    check(us1 < them1, "1-entry get(): T_l_p < t_s_p [T_l_p is optimized, even with tiny map]", FAIL_IS_OK);
    /* This one, from what we've seen, is emphatically true.  Update: not so much; it can be emphatic but not
     * reliably.  Changing from FAIL_IS_FAIL to FAIL_MAYBE_OK. */
    check(us16 < them16, "16-entry get(): T_l_p < t_s_p [T_l_p is optimized, esp for bigger map]", FAIL_MAYBE_OK);
    // The preceding two are the "money shots"; get() is what we're optimizing.  The others are sanity checks.
  }}.join();
} // TEST(Thread_local_ptr, Benchmark)

} // namespace flow::util::test
