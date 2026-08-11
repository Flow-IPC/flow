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
#include "flow/util/thread_lcl_ptr.hpp"
#include "flow/test/test_common_util.hpp"
#include "flow/log/buffer_logger.hpp"
#include <gtest/gtest.h>
#include <boost/thread/thread.hpp>
#include <thread>
#include <type_traits>
#include <ostream>

namespace flow::log::test
{

namespace
{
using std::string;
using std::cout;
using std::flush;

/* For the Near_thread_death test: markers logged from the thread-death window.  (File-scope: the prober is a
 * thread_local; the hook is a plain-function-pointer Thread_local_ptr cleanup; neither can capture.) */
Logger* s_death_test_logger{}; // The test's Buffer_logger.
const char* s_death_marker{}; // Distinguishes the test's legs.

/* Logs during thread_local deinit.  In the test each thread constructs one of these *before* its first
 * normal log call -- hence before the per-thread log-machinery thread_locals -- so by reverse-order
 * destruction this dtor runs *after* those are gone: the logging below deterministically exercises the
 * degraded (fresh-appender) path. */
struct Death_logging_prober
{
  ~Death_logging_prober()
  {
    FLOW_LOG_SET_CONTEXT(s_death_test_logger, Flow_log_component::S_LOG);
    FLOW_LOG_INFO("Dtor-log marker [" << s_death_marker << "].");
  }
};

/* Logs from Thread_local_ptr cleanup: in a std::thread that phase runs even later -- after *all*
 * thread_local deinit; in a boost::thread it runs before deinit (a normal-ish time; included for contrast). */
void death_logging_tlp_cleanup(int* /* unused: static-storage payload */)
{
  FLOW_LOG_SET_CONTEXT(s_death_test_logger, Flow_log_component::S_LOG);
  FLOW_LOG_INFO("Cleanup-log marker [" << s_death_marker << "].");
}

} // Anonymous namespace

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
    ASSERT_TRUE(comp0.empty());

    cout << "Testing type [" << typeid(Log_context_t).name() << "].\n" << flush;

    // @todo Maybe should implement operator==(Component, Component)?  Then use it/test it here?

    /* This isn't a test of Component functionality; it assumes that works; it just checks whether they're
     * equal and ensures this matches what our test is expecting that for those particular c1 and c2. */
    const auto comps_equal = [](Component c1, Component c2)
    {
      if (c1.empty() && c2.empty())
      {
        return; // So equal then.
      }
      // Better both be not-empty.
      EXPECT_EQ(c1.empty(), c2.empty());

      EXPECT_EQ(c1.payload_type(), c2.payload_type());
      EXPECT_EQ(int(c1.payload_enum_raw_value()), int(c2.payload_enum_raw_value()));
    };

    /* The below uses .log_while_locked() merely as an accessor helper; its central use -- actual logging,
     * via FLOW_LOG_*_LOCKED() as prescribed -- is exercised in TEST(Log_context_mt, Locked_logging). */
    const auto get_logger_in = [](const Log_context_t& ctx) -> Logger*
    {
      if constexpr(std::is_same_v<Log_context_t, Log_context_mt>)
      {
        Logger* lgr;
        ctx.log_while_locked([&](auto&& get_logger, auto&&) { lgr = get_logger(); });
        return lgr;
      }
      else // Plain Log_context still exposes get_logger() directly.
      {
        return ctx.get_logger();
      }
    };
    const auto get_log_component_in = [](const Log_context_t& ctx) -> const Component&
    {
      if constexpr(std::is_same_v<Log_context_t, Log_context_mt>)
      {
        const Component* comp;
        ctx.log_while_locked([&](auto&&, auto&& get_log_component) { comp = &(get_log_component()); });
        return *comp;
      }
      else // Plain Log_context still exposes get_log_component() directly.
      {
        return ctx.get_log_component();
      }
    };

    Log_context_t ctx1;
    EXPECT_TRUE(get_log_component_in(ctx1).empty());
    { FLOW_TEST_TRACE(); comps_equal(get_log_component_in(ctx1), comp0); }
    EXPECT_EQ(get_logger_in(ctx1), nullptr);
    EXPECT_EQ(ctx1.set_logger(&logger2), nullptr); // Was null; returns previous.
    EXPECT_EQ(get_logger_in(ctx1), &logger2);
    ctx1 = Log_context_t{&logger1, comp1};
    EXPECT_FALSE(get_log_component_in(ctx1).empty());
    { FLOW_TEST_TRACE(); comps_equal(get_log_component_in(ctx1), comp1); }
    EXPECT_EQ(get_logger_in(ctx1), &logger1);

    Log_context_t ctx2{&logger2};
    EXPECT_TRUE(get_log_component_in(ctx2).empty());
    { FLOW_TEST_TRACE(); comps_equal(get_log_component_in(ctx2), comp0); }
    EXPECT_EQ(get_logger_in(ctx2), &logger2);
    EXPECT_EQ(ctx2.set_logger(&logger1), &logger2); // Was logger2; returns previous.
    EXPECT_EQ(get_logger_in(ctx2), &logger1);
    ctx2 = Log_context_t{&logger2, comp2};
    EXPECT_FALSE(get_log_component_in(ctx2).empty());
    { FLOW_TEST_TRACE(); comps_equal(get_log_component_in(ctx2), comp2); }
    EXPECT_EQ(get_logger_in(ctx2), &logger2);

    swap(ctx1, ctx2);
    { FLOW_TEST_TRACE(); comps_equal(get_log_component_in(ctx1), comp2); }
    EXPECT_EQ(get_logger_in(ctx1), &logger2);
    { FLOW_TEST_TRACE(); comps_equal(get_log_component_in(ctx2), comp1); }
    EXPECT_EQ(get_logger_in(ctx2), &logger1);

    ctx1 = ctx2; // Copy-assign.
    { FLOW_TEST_TRACE(); comps_equal(get_log_component_in(ctx1), comp1); }
    EXPECT_EQ(get_logger_in(ctx1), &logger1);
    { FLOW_TEST_TRACE(); comps_equal(get_log_component_in(ctx2), comp1); }
    EXPECT_EQ(get_logger_in(ctx2), &logger1);
    ctx1 = Log_context_t{&logger2, comp2};

    ctx2 = std::move(ctx1); // Move-assign.
    EXPECT_TRUE(get_log_component_in(ctx1).empty());
    EXPECT_EQ(get_logger_in(ctx1), nullptr);
    { FLOW_TEST_TRACE(); comps_equal(get_log_component_in(ctx2), comp2); }
    EXPECT_EQ(get_logger_in(ctx2), &logger2);

    Log_context_t ctx3{ctx2}; // Copy-ct.
    { FLOW_TEST_TRACE(); comps_equal(get_log_component_in(ctx3), comp2); }
    EXPECT_EQ(get_logger_in(ctx3), &logger2);
    { FLOW_TEST_TRACE(); comps_equal(get_log_component_in(ctx2), comp2); }
    EXPECT_EQ(get_logger_in(ctx2), &logger2);

    Log_context_t ctx4{std::move(ctx3)}; // Move-ct.
    EXPECT_TRUE(get_log_component_in(ctx3).empty());
    EXPECT_EQ(get_logger_in(ctx3), nullptr);
    { FLOW_TEST_TRACE(); comps_equal(get_log_component_in(ctx4), comp2); }
    EXPECT_EQ(get_logger_in(ctx4), &logger2);
  }; // const auto test_type =

  test_type(Log_context{});
  test_type(Log_context_mt{});
} // TEST(Log_context, Interface)

// For the Locked_logging test: the prescribed user-shape #1 -- a class derived from Log_context_mt,
// logging from its member functions via FLOW_LOG_*_LOCKED().
class Locked_logging_widget : public Log_context_mt
{
public:
  explicit Locked_logging_widget(Logger* logger_ptr) :
    Log_context_mt(logger_ptr, Flow_log_component::S_UTIL)
  {
    // Nothing.
  }

  void do_stuff(int val) // Note: FLOW_LOG_*_LOCKED() find our inherited log_while_locked() directly.
  {
    FLOW_LOG_INFO_LOCKED("Widget did stuff [" << val << "].");
    FLOW_LOG_TRACE_LOCKED("Widget triviality [" << val << "] (filtered at default verbosity).");
  }
};

/* log_while_locked() -- via its preferred users FLOW_LOG_*_LOCKED() -- used as prescribed, i.e., to
 * actually log: from a Log_context_mt-derived class's member functions; and from a free context via
 * FLOW_LOG_SET_LOCKED_CONTEXT().  The logger is resolved under the lock at each log call: proven by
 * swapping loggers mid-life and watching the output move between capture streams. */
TEST(Log_context_mt, Locked_logging)
{
  Config cfg; // Default verbosity: INFO (TRACE is filtered -- also asserted below).
  Buffer_logger logger_a{&cfg};
  Buffer_logger logger_b{&cfg};

  { // Prescribed shape #1: derived class, member-function logging.
    Locked_logging_widget widget{&logger_a};
    widget.do_stuff(1);
    EXPECT_NE(logger_a.buffer_str().find("Widget did stuff [1]."), string::npos);
    EXPECT_EQ(logger_a.buffer_str().find("triviality"), string::npos); // TRACE filtered: should_log() honored.

    // The logger is resolved at log time, under the lock: re-aim it; new output lands in the new stream.
    EXPECT_EQ(widget.set_logger(&logger_b), &logger_a);
    widget.do_stuff(2);
    EXPECT_EQ(logger_a.buffer_str().find("Widget did stuff [2]."), string::npos);
    EXPECT_NE(logger_b.buffer_str().find("Widget did stuff [2]."), string::npos);

    // Null logger: locked-logging is a safe no-op (the null-check runs within the locked section).
    widget.set_logger(nullptr);
    widget.do_stuff(3);
    EXPECT_EQ(logger_a.buffer_str().find("[3]"), string::npos);
    EXPECT_EQ(logger_b.buffer_str().find("[3]"), string::npos);
  }

  { // Prescribed shape #2: free/other context, via FLOW_LOG_SET_LOCKED_CONTEXT().
    Log_context_mt ctx{&logger_a, Flow_log_component::S_LOG};
    FLOW_LOG_SET_LOCKED_CONTEXT(&ctx);
    FLOW_LOG_INFO_LOCKED("Free-context locked message [" << 42 << "].");
    EXPECT_NE(logger_a.buffer_str().find("Free-context locked message [42]."), string::npos);
    FLOW_LOG_WARNING_LOCKED("Free-context locked warning [" << 43 << "].");
    EXPECT_NE(logger_a.buffer_str().find("Free-context locked warning [43]."), string::npos);
  }
} // TEST(Log_context_mt, Locked_logging)

/* flow.log's near-thread-death guarantee: logging works from the thread-death window -- during
 * thread_local deinit (from thread_local dtors) and even after it (from thread_specific_ptr-style cleanup,
 * which in a non-boost::thread thread runs post-deinit) -- merely losing the per-thread stream-state
 * carry-over for such messages.  Each leg logs markers from the window, and we assert they *landed* (not
 * merely did-not-crash); moreover the thread nickname -- itself stored window-safely -- must still adorn the
 * marker lines.  (Buffer_logger logs synchronously, so these messages also route through the sync-path
 * Msg_metadata, itself a window-safe thread_local -- all 3 window-proofed flow.log pieces in one shot.) */
TEST(Log, Near_thread_death)
{
  using util::Thread_local_ptr;

  Config cfg; // Default verbosity: INFO.
  Buffer_logger logger{&cfg};
  s_death_test_logger = &logger;

  static int s_dummy = 0; // Static storage: the Tlp cleanup hook is a logging probe, not a deleter.
  Thread_local_ptr<int> tlp{&death_logging_tlp_cleanup};

  const auto thread_body = [&]()
  {
    // Constructed before any logging below (hence destroyed after the log-machinery thread_locals).
    [[maybe_unused]] thread_local Death_logging_prober s_prober{};

    Logger::this_thread_set_logged_nickname("deathNick", nullptr, false);
    tlp.reset(&s_dummy); // Arms the cleanup-phase logging hook.

    FLOW_LOG_SET_CONTEXT(&logger, Flow_log_component::S_LOG);
    FLOW_LOG_INFO("Live-body marker [" << s_death_marker << "]."); // Sanity + constructs log machinery.
  };
  // Does the buffer line containing `marker` carry the thread nickname?
  const auto marker_line_has_nickname = [&](const string& marker) -> bool
  {
    const auto& buf = logger.buffer_str();
    const auto pos = buf.find(marker);
    if (pos == string::npos)
    {
      return false;
    }
    const auto line_start = buf.rfind('\n', pos) + 1; // (npos + 1 == 0: first line works out too.)
    return buf.substr(line_start, pos - line_start).find("deathNick") != string::npos;
  };
  const auto check_leg = [&](const string& leg)
  {
    EXPECT_NE(logger.buffer_str().find("Live-body marker [" + leg), string::npos);
    EXPECT_TRUE(marker_line_has_nickname("Dtor-log marker [" + leg)); // Mid-deinit: landed, nickname intact.
    EXPECT_TRUE(marker_line_has_nickname("Cleanup-log marker [" + leg)); // Cleanup-phase: ditto.
  };

  s_death_marker = "leg1";
  boost::thread{thread_body}.join();
  check_leg("leg1");

  s_death_marker = "leg2"; // The spicier sort: the cleanup-phase marker logs post-thread_local-deinit.
  std::thread{thread_body}.join();
  check_leg("leg2");

  s_death_test_logger = nullptr;
} // TEST(Log, Near_thread_death)

} // namespace flow::log::test
