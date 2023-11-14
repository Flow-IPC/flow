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

/// @file
#include "flow/net_flow/node.hpp"
#include "flow/net_flow/peer_socket.hpp"
#include "flow/net_flow/server_socket.hpp"
#include "flow/async/util.hpp"
#include <boost/algorithm/cxx11/all_of.hpp>
#include <boost/algorithm/cxx11/one_of.hpp>
#include <boost/functional.hpp>

namespace flow::net_flow
{

// Event_set static initializations.

const boost::unordered_map<Event_set::Event_type, Function<bool (const Node*, const boost::any&)>>
        Event_set::S_EV_TYPE_TO_IS_ACTIVE_NODE_MTD
          ({
             { Event_set::Event_type::S_PEER_SOCKET_READABLE, &Node::sock_is_readable },
             { Event_set::Event_type::S_PEER_SOCKET_WRITABLE, &Node::sock_is_writable },
             { Event_set::Event_type::S_SERVER_SOCKET_ACCEPTABLE, &Node::serv_is_acceptable }
           });

// Event_set implementations.

Event_set::Event_set(log::Logger* logger_ptr) :
  log::Log_context(logger_ptr, Flow_log_component::S_NET_FLOW),
  m_state(State::S_CLOSED), // Incorrect; set explicitly.
  m_node(0), // Incorrect; set explicitly.
  m_want(empty_ev_type_to_socks_map()), // Each event type => empty socket set.
  m_can(empty_ev_type_to_socks_map()), // Ditto.
  m_baseline_check_pending(false)
{
  // This can be quite frequent if program uses many sync_*() methods.  Use TRACE.
  FLOW_LOG_TRACE("Event_set [" << this << "] created.");
}

Event_set::~Event_set()
{
  // This can be quite frequent if program uses many sync_*() methods.  Use TRACE.
  FLOW_LOG_TRACE("Event_set [" << this << "] destroyed.");
}

Event_set::State Event_set::state() const
{
  Lock_guard lock(m_mutex);
  return m_state;
}

Node* Event_set::node() const
{
  Lock_guard lock(m_mutex);
  return m_node;
}

bool Event_set::async_wait(const Event_handler& on_event, Error_code* err_code)
{
  namespace bind_ns = util::bind_ns;
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Event_set::async_wait, bind_ns::cref(on_event), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  using boost::algorithm::all_of;

  // We are in user thread U != W.

  /* Caller has loaded *this with desired events (socket + event type pairs); e.g., "I want
   * Peer_socket X to be Readable; I want Server_socket Y to be Acceptabe."  Now she wants to be
   * informed as soon as one or more of those events has occurred (e.g., "Server_socket Y is
   * Acceptable.").  After being so informed, she can check which events did in fact occur and act
   * accordingly (e.g., "Server_socket Y is Acceptable, so I will call Y->accept()").
   *
   * Suppose we lock *this.  There are now several possitibilities.
   *
   *   -1- We are CLOSED.  Bail out: everything is blank in CLOSED state, and no further waits are
   *       possible.
   *   -2- We are WAITING.  Bail out: cannot wait while already WAITING: the on-event behavior
   *       (i.e., on_event) has already been set, and we do not support dislodging it with another
   *       handler.
   *   -3- We are INACTIVE.  The mainstream case.  We should enter WAITING and inform the caller
   *       (via on_event() call) as soon as at least one event has occurred.
   *
   * Obviously -3- is the interesting case.
   *
   * First, some groundwork.  The meaning of "As soon as 1+ event has occurred" is not totally
   * clear-cut.  For example, it could mean "the moment we know that exactly 1 event has occurred."
   * However, a high-performance definition is: "no later than time period E since the moment we
   * know that exactly 1 event has occurred," where E is defined as "the largest amount of time that
   * can be considered 'non-blocking.'"  Informally, this just means that we should detect and
   * accumulate (clump together) as many events as possible -- without extending the wait by a
   * "blocking" time period -- before calling on_event() and thus "giving the results" to the user.
   * This should result in a faster event loop for the user, but we may want to make it tunable.
   *
   * Supposing we want to "clump together" events this way, the problem reduces to 3 conceptual
   * steps.  First, detect that exactly 1 desired event has fired; accumulate it.  Second, perform
   * any immediately pending Node work, and accumulate any events thus fired.  Third, once that's
   * done, call on_event() to inform the user.
   *
   * The latter two steps are interesting but can be left as black boxes for the time being.  What
   * about the former, first, step?  How to detect that first event has occurred in a timely
   * fashion?
   *
   * What we really are looking for is not so much an "event" but rather a CONDITION (e.g., "Socket
   * S is Writable"), and we want to know the earliest moment in the future when this CONDITION is
   * true.  (Time is always passing, so I say "future" instead of "present or future."  Also we can
   * assume, for our purposes, that the CONDITION cannot become false after it is true.)  One
   * guaranteed way to detect this, which is what we'll use, is as follows.  At the soonest moment
   * possible from now, check whether the CONDITION is true. At that point, if it is true, we've
   * found the one "event" that has occurred and can immediately proceed.  If, at that point, it is
   * not true, then we check at every step when the CONDITION *may* become true -- that is, when the
   * underlying logic changes what the CONDITION is about -- and if it is at that point true, we can
   * proceed.  In other words, establish a baseline ("CONDITION is currently false"), and then
   * ignore the whole thing except at time points when CONDITION may deviate from that baseline.
   * This is efficient and works.
   *
   * We call the initial check, right after async_wait() is called, the "baseline" check; while the
   * subsequent checks are called "delta" checks.  (Known terms for this are "level" check and
   * "edge" check, but I didn't know that when originally writing this giant comment.  @todo Change
   * terminology in comments and code.)
   *
   * To be specific, consider the CONDITION "Server_socket S is Acceptable" as an example.  First we
   * check ASAP: is S Acceptable -- i.e., does it have at least one Peer_socket on its accept queue?
   * Yes?  Done.  No?  OK.  <time passes>  Say we get a valid SYN_ACK_ACK on that port.  Since
   * receiving a SYN_ACK_ACK on socket S' belonging to Server_socket S is the only way S can become
   * Acceptable, we check it.  Is it?  Yes.  Done.  At all other points in time we don't concern
   * ourselves with the CONDITION/event "Server_socket S is Acceptable."
   *
   * The above may be seen as obvious, but it's important to be very explicit for the following
   * reasoning to work.  It's pretty clear that the "delta" checking can only occur on thread W;
   * only Node on thread W knows about such things as receiving SYN_ACK_ACKs, for example.  What
   * about the "baseline" (initial) check for the CONDITION in question?  There are two
   * possibilities.  One is we can do it right here, right now, in thread U != W.  The other is we
   * can post() a task onto thread W and have it happen there ASAP.
   *
   * The former (do it here) seems attractive.  It's "faster," in that we can just do it now.  It
   * also lets us short-circuit on_event() and just return something to the user right now, so they
   * can instantly react (in case one of the events is immediately true) instead of having to learn
   * of the event indirectly.  It's also more distributed among threads in some situations (we're
   * loading less work onto one thread W, which may be busy keeping net_flow working).  The negative is
   * we generally prefer to keep work on W when it doesn't seriously hurt performance, to keep the
   * synchronization as simple as possible (avoid bugs); for example that's why Node::listen() and
   * Node::connect() and Event_set::close() work the way they do (using futures).  Another negative
   * is that it introduces a special case to the interface; so the user would need to write code to
   * handle both an instant, non-error return and one via on_event().
   *
   * I thought about this for a while.  I'd go for the faster way, but the real crux is how to set
   * up the synchronization in a way that would not miss events.  I 90% convinced myself that a
   * certain not-hard way would be correct, but it remains difficult to think about; eventually I
   * decided that it would be best to go with the slower solution (do both "baseline" and "delta"
   * checks on W), because of its simplicity and elegance.  I doubt the performance impact is too
   * serious.  On 5-year old server hardware in 2012, assuming an idle thread W and a ready socket event, a
   * return through post(W) + on_event() { boost::promise::set_value() } is in the low hundreds of
   * microseconds.  If on_event() instead sends a message through a Unix domain socket, doing that
   * and detecting and reading that message is in the same ballpark (certainly sub-millisecond).
   * Effectively adding sub-ms latency to some messages does not seem too bad.
   *
   * Conclusion: Lock *this; then filter out CLOSED and WAITING state.  Then, assuming INACTIVE
   * state, proceed to WAITING state.  post() onto thread W the "baseline check" task.  Finally,
   * unlock *this.  Thread W will then proceed with "baseline" and "delta" checks and will fire
   * on_event() when ready.
   *
   * Also note that poll() provides the "instantly check for any of the desired events and don't
   * wait" functionality in isolation (something like a select() with a 0 timeout). */

  // Lock everything.  We're going to be writing to things other user threads and W will be accessing/writing.
  Lock_guard lock(m_mutex);

  // Check for invalid arguments, basically.
  if (all_of(m_want, ev_type_to_socks_map_entry_is_empty))
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_NO_EVENTS);
    return false;
  }
  // else

  if (m_state == State::S_CLOSED)
  {
    // Already CLOSED Event_set -- Node has disowned us.  Mark error in *err_code and log.
    assert(!m_node);

    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_CLOSED);
    return false;
  }
  // else

  if (m_state == State::S_WAITING)
  {
    // Someone already started an async_wait().
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_DOUBLE_WAIT_OR_POLL);
    return false;
  }
  // else the mainstream case.
  assert(m_state == State::S_INACTIVE);

  // Forward to Node, as is the general pattern for Event_set method implementations involving Node state.
  return m_node->event_set_async_wait(shared_from_this(), // Get a Ptr that shares ownership of this.
                                      on_event, err_code);
} // Event_set::async_wait()

bool Event_set::async_wait_finish(Error_code* err_code)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Event_set::async_wait_finish, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in thread U != W.

  /* User wants to move *this to INACTIVE state, so that she can check the results of a wait (in
   * m_can).  If it is already in INACTIVE, then this won't do anything but is allowed (for
   * reasons that will be very soon clear).  If it is CLOSED, then it's just an error like any other
   * operation.
   *
   * If it is WAITING, then there's a race.  We're about to lock *this and try to change to INACTIVE
   * if needed.  If we lose the race -- if Node in thread W is able to, while scanning Event_sets
   * for applicable events that just occurred, lock *this and find some active events in *this and
   * thus change m_state to INACTIVE -- then that's just the NOOP situation for us (INACTIVE ->
   * INACTIVE).  No harm done; *this is still INACTIVE by the time we return, as advertised.  If we
   * win the race -- if we lock and change state to INACTIVE -- then Node will just ignore *this
   * when/if it gets to it, as user is no longer interested in *this's events (which is correct). */

  // Lock everything, as we'll be reading/changing m_state at least.
  Lock_guard lock(m_mutex);

  if (m_state == State::S_CLOSED)
  {
    // Already CLOSED Event_set -- Node has disowned us.  Mark error in *err_code and log.
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_CLOSED);
    return false;
  }
  // else

  if (m_state == State::S_WAITING)
  {
    FLOW_LOG_TRACE("Event_set [" << this << "] wait finish requested; moving from "
                   "[" << Event_set::State::S_WAITING << "] to [" << Event_set::State::S_INACTIVE << "].");

    // As in event_set_fire_if_got_events():
    m_on_event.clear();
    m_state = State::S_INACTIVE;
  }
  else
  {
    assert(m_state == State::S_INACTIVE);
    FLOW_LOG_TRACE("Event_set [" << this << "] wait finish requested, but state was already "
                   "[" << Event_set::State::S_INACTIVE << "].");
  }

  err_code->clear();
  return true;
} // Event_set::async_wait_finish()

bool Event_set::poll(Error_code* err_code)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Event_set::poll, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in thread U != W.

  /* This method can be thought of as sync_wait() with a zero max_wait (which sync_wait() actually
   * disallows, so that poll() is used instead), with the minor difference that the "no events ready 'in time'" result
   * for us means we return `true` (no error) and `!*err_code` (sync_wait() returns a ..._TIMEOUT err_code).
   *
   * It assumes INACTIVE state, goes WAITING, checks
   * for any of the m_want conditions that are true right now (and saves such to m_can), then
   * immediately goes back to INACTIVE.  If you look at the giant comment in async_wait() you'll
   * note that this is basically equivalent to an async_wait() that performs only the initial
   * ("baseline") check and then immediately goes back to INACTIVE regardless of whether the
   * baseline check discovered any active events.
   *
   * The only difference is that we do the baseline check directly in thread U != W, as opposed to
   * post()ing it onto thread W.  It is entirely thread-safe to check each event from thread U, as
   * all the event checking functions (is Readable?  is Writable?  is Acceptable?) explicitly can
   * run from any thread.  Additionally, the argument made (in the async_wait() comment) against
   * performing the baseline check in thread U != W does not apply: we're not interested in
   * performing any blocking, so we can't "miss" any events as long as we simply check immediately.
   * Of course, the result is faster and simpler code as well (compared to post()ing it). */

  // Lock everything, as we'll be reading/changing much state.
  Lock_guard lock(m_mutex);

  if (m_state == State::S_CLOSED)
  {
    // Already CLOSED Event_set -- Node has disowned us.  Mark error in *err_code and log.
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_CLOSED);
    return false;
  }
  // else

  if (m_state == State::S_WAITING)
  {
    // Fairly similar situation to trying to async_wait() while WAITING.
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_DOUBLE_WAIT_OR_POLL);
    return false;
  }
  // else
  assert(m_state == State::S_INACTIVE);

  /* Perform baseline check (that is to say, simply check each event in m_want and if it holds,
   * add it to m_can). */

  // This is a formality, as we'll just go back to INACTIVE in a moment, but event_set_baseline_check() expects it.
  m_state = State::S_WAITING;

  // As in a regular async_wait():
  clear_ev_type_to_socks_map(&m_can);
  m_baseline_check_pending = true;

#ifndef NDEBUG
  const bool baseline_check_ok =
#endif
    m_node->event_set_check_baseline(shared_from_this());
  assert(baseline_check_ok); // There is NO adequate reason for it to fail; we've set m_baseline_check_pending = true.

  // As in a regular async_wait_finish() (but no need to mess with m_on_event, as we didn't set it above):
  m_state = State::S_INACTIVE;

  // Done.  m_can sets are what they are.
  err_code->clear();
  return true;
} // Event_set::poll()

bool Event_set::sync_wait_impl(const Fine_duration& max_wait, Error_code* err_code)
{
  using boost::promise;
  using boost::unique_future;
  using boost::future_status;
  using boost::chrono::milliseconds;
  using boost::chrono::round;
  using boost::algorithm::one_of;

  assert(max_wait.count() > 0);

  // We are in user thread U != W.

  /* This is actually simple.  They want us to block until 1+ of the desired events loaded into
   * m_want has occurred.  Therefore we use async_wait(), and have thread W signal us when this
   * has occurred; then in this thread we wait for that signal.  So how to signal?  We can use
   * Boost future (internally condition_variable probably).
   *
   * Additional optimization: perform a single poll() right away, in case events are ready now.
   * This lets us avoid any thread W work and return immediately. */

  {
    // Lock so that we can safely check result of the poll() without another thread messing with it.
    Lock_guard lock(m_mutex); // OK because m_mutex is recursive (poll() will also momentarily lock).

    if (!poll(err_code)) // May throw.
    {
      return false; // *err_code set (if not null)/logged.
    }
    // else see if already a least one exists for at least one of the event types.
    if (one_of(m_can, boost::not1(ev_type_to_socks_map_entry_is_empty)))
    {
      // Boo-ya, immediately found some active events.
      return true;
    }
    // else need to wait.
  }

  /* We will perform the wait via the future/promise pair. The result of the successful operation is either true or
   * false: interrupted (true) or condition(s) now true (false). */
  promise<bool> intr_promise; // This will be touched by thread W when events ready.
  unique_future<bool> intr_future = intr_promise.get_future(); // We'll use this to wait for that.

  /* The on-event callback is therefore: intr_promise->set_value(interrupted),
   * where interrupted is boolean value async_wait() will pass in, indicating whether the wait was finished
   * due to an explicit interruption (true) vs. the waited-on condition(s) actually becoming true (false).
   * Capture intr_promise by reference, as we will for completion, during which time it'll keep existing. */
  if (!async_wait([&intr_promise](bool interrupted) { intr_promise.set_value(interrupted); },
                  err_code)) // May throw.
  {
    // Was not able to start the asynchronous wait; *err_code set (if not null)/logged.
    return false;
  }
  // else *err_code is success (or untouched if null).

  bool timed_out = false;

  // Block until intr_promise is touched.
  if (max_wait == Fine_duration::max())
  {
    // Infinite wait.
    intr_future.wait();
  }
  else
  {
    // Finite wait.

    if (intr_future.wait_for(max_wait) != future_status::ready)
    {
      /* Timed out.  That's OK: we treat it the same way as if hadn't timed out; we'll simply
       * return 0 active events, not an error.  Therefore we need only finish the wait by going to
       * INACTIVE state using async_wait_finish().  Then if/when Node decides to actually add an
       * active event to *this, *this will already be INACTIVE, so Node will ignore it (which is
       * right).
       *
       * Note that in the tiny period of time between wait_for() exiting with false and us
       * calling async_wait_finish(), Node in thread W may actually be able to detect an event,
       * add it to m_can, and go to INACTIVE.  This would mean that in async_wait_finish() we'll
       * try to go from INACTIVE to INACTIVE.  That is OK; that method specifically supports
       * that.  In that case it won't do anything, still return succes, and we'll happily return 1
       * or more active events inm_can, as if there was no timeout after all. */

      FLOW_LOG_INFO("Synchronous wait on Event_set [" << this << "] timed out; "
                    "timeout = [" << round<milliseconds>(max_wait) << "].");

      if (!async_wait_finish(err_code)) // May throw.
      {
        // Extraordinary situation -- *this was close()d or something.  *err_code set (if not null)/logged.
        return false;
      }
      // else *err_code is still success (or untouched if null).

      timed_out = true;
    } // if (intr_future timed out)
  } // else if (needed to wait until a deadline)

  if (!timed_out)
  {
    /* The wait didn't time out (if that was even possible); but the result may have been that the (successful, from
     * async_wait()'s point of view) wait finished due an interruption as opposed to the waited-on condition(s) becoming
     * true. In this case, as advertised, we return a specific error. This is conceptually similar to POSIX's
     * errno=EINTR semantics. */

    if (intr_future.get())
    {
      FLOW_LOG_INFO("Synchronous wait on Event_set [" << this << "] was interrupted.");

      if (err_code)
      {
        *err_code = error::Code::S_WAIT_INTERRUPTED;
      }

      return false;
    }
    // else if (conditions(s) are actually true)
  }

  // Caller can now examine m_can for fired events (if any).
  assert((!err_code) || (!*err_code)); // Either null or success.
  return true;
} // Event_set::sync_wait_impl()

bool Event_set::sync_wait(Error_code* err_code)
{
  using boost::chrono::microseconds;
  return sync_wait(microseconds(microseconds::max()), err_code); // Wait indefinitely.  May throw.
}

void Event_set::close(Error_code* err_code)
{
  if (flow::error::exec_void_and_throw_on_error([this](Error_code* actual_err_code) { close(actual_err_code); },
                                                err_code, FLOW_UTIL_WHERE_AM_I_STR()))
  {
    return;
  }
  // else

  // We are in user thread U != W.

  Lock_guard lock(m_mutex); // Lock m_node/m_state; also it's a pre-condition for Node::event_set_close().

  if (m_state == State::S_CLOSED)
  {
    // Already CLOSED Event_set -- Node has disowned us.
    assert(!m_node);

    // Mark error in *err_code and log.
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_CLOSED);
    return;
  }

  // Forward to Node, as is the general pattern for Event_set method implementations involving Node state.
  lock.release(); // Let go of the mutex (mutex is still LOCKED).  event_set_close is now in charge of unlocking it.
  m_node->event_set_close(shared_from_this(), // Get a Ptr that shares ownership of this.
                           err_code);
} // Event_set::close()

bool Event_set::swap_wanted_sockets(Sockets* target_set, Event_type ev_type, Error_code* err_code)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Event_set::swap_wanted_sockets,
                               target_set, ev_type, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  /* @todo There are about 4 methods that have a pattern similar to below, differing mostly by log message
   * contents and what happens just before `return true`. Perhaps code reuse? */

  // We are in thread U != W.

  assert(target_set);

  // Accessing m_state, socket sets, etc. which may be written by other threads at any time.  Must lock.
  Lock_guard lock(m_mutex);

  Sockets& want_set = m_want[ev_type];

  FLOW_LOG_TRACE("Wanted set for event type [" << ev_type << "] swapped in Event_set [" << this << "]; pre-swap sizes: "
                 "Event_set [" << want_set.size() << "]; user [" << target_set->size() << "].");

  if (!ok_to_mod_socket_set(err_code)) // Ensure we can modify want_set in this state.
  {
    // *err_code is set.
    return false; // Closed; do not set *target_set (BTW want_set is blank anyway).
  }
  // else *err_code is success.

  // This is an O(1) operation.
  target_set->swap(want_set);

  return true;
} // Event_set::swap_wanted_sockets()

bool Event_set::clear_wanted_sockets(Event_type ev_type, Error_code* err_code)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Event_set::clear_wanted_sockets, ev_type, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in thread U != W.

  // Accessing m_state, the sets, etc. which may be written by other threads at any time.  Must lock.
  Lock_guard lock(m_mutex);

  Sockets& want_set = m_want[ev_type];

  FLOW_LOG_TRACE("Wanted set for event type [" << ev_type << "] cleared in Event_set [" << this << "]; "
                 "size [" << want_set.size() << "].");

  if (!ok_to_mod_socket_set(err_code)) // Ensure we can modify can_set in this state.
  {
    // *err_code is set.
    return false; // Closed; do not clear (it should be cleared anyway, BTW).
  }
  // else *err_code is success.

  want_set.clear();
  return true;
}

bool Event_set::events_wanted(Error_code* err_code) const
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Event_set::events_wanted, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  using boost::algorithm::one_of;

  // We are in thread U != W.

  // Lock everything, as we'll be reading state and other things.
  Lock_guard lock(m_mutex);

  if (m_state == State::S_CLOSED)
  {
    // Already CLOSED Event_set -- Node has disowned us.  Mark error in *err_code and log.
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_CLOSED);
    return false;
  }
  // else

  return one_of(m_want, boost::not1(ev_type_to_socks_map_entry_is_empty));
} // Event_set::events_wanted()

bool Event_set::events_detected(Error_code* err_code) const
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Event_set::events_detected, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  using boost::algorithm::one_of;

  // We are in thread U != qW.

  // Lock everything, as we'll be reading state.
  Lock_guard lock(m_mutex);

  if (m_state == State::S_CLOSED)
  {
    // Already CLOSED Event_set -- Node has disowned us.  Mark error in *err_code and log.
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_CLOSED);
    return false;
  }
  // else

  if (m_state == State::S_WAITING)
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_RESULT_CHECK_WHEN_WAITING);
    return false;
  }
  // else
  assert(m_state == State::S_INACTIVE);

  // Coolio.
  err_code->clear();

  FLOW_LOG_TRACE("Wait result set checked for activity in Event_set [" << this << "]; "
                 "results set sizes = [" << ev_type_to_socks_map_sizes_to_str(m_can) << "].");

  return one_of(m_can, boost::not1(ev_type_to_socks_map_entry_is_empty));
} // Event_set::events_detected()

bool Event_set::emit_result_sockets(Sockets* target_set, Event_type ev_type, Error_code* err_code)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Event_set::emit_result_sockets, target_set, ev_type, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in thread U != W.

  assert(target_set);

  // Accessing m_state, the sets, etc. which may be written by other threads at any time.  Must lock.
  Lock_guard lock(m_mutex);

  Sockets& can_set = m_can[ev_type];

  FLOW_LOG_TRACE("Wait result set for event type [" << ev_type << "] emitted in Event_set [" << this << "]; "
                 "size [" << can_set.size() << "].");

  if (!ok_to_mod_socket_set(err_code)) // Ensure we can modify can_set in this state.
  {
    // *err_code is set.
    return false; // Closed; do not set *target_set (*want_set is blank anyway).
  }
  // else *err_code is success.

  // Efficiently give them the result set (swap() avoids copy).

  // This is an O(1) operation.
  can_set.swap(*target_set);

  /* Get rid of whatever garbage they had in their target set before calling us.
   * This is O(n), where n is # of elements in *target_set before the swap.  Typically n == 0. */
  can_set.clear();

  return true;
} // Event_set::emit_result_sockets()

bool Event_set::clear_result_sockets(Event_type ev_type, Error_code* err_code)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Event_set::clear_result_sockets, ev_type, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in thread U != W.

  // Accessing m_state, the sets, etc. which may be written by other threads at any time.  Must lock.
  Lock_guard lock(m_mutex);

  Sockets& can_set = m_can[ev_type];

  FLOW_LOG_TRACE("Wait result set for event type [" << ev_type << "] cleared in Event_set [" << this << "]; "
                 "size [" << can_set.size() << "].");

  if (!ok_to_mod_socket_set(err_code)) // Ensure we can modify can_set in this state.
  {
    // *err_code is set.
    return false; // Closed; do not clear (it should be cleared anyway).
  }
  // else *err_code is success.

  can_set.clear();
  return true;
}

bool Event_set::ok_to_mod_socket_set(Error_code* err_code) const
{
  // m_mutex should be locked.

  if (m_state == State::S_CLOSED)
  {
    // Already CLOSED Event_set -- Node has disowned us.
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_CLOSED);
    return false;
  }
  // else

  if (m_state == State::S_WAITING)
  {
    /* Event_set is being waited on; in this phase we do not allow changing events for which to
     * wait. We could probably allow it, but the logic involved would become needlessly complex.
     * Also, consider a BSD socket select() or a Linux epoll_wait(); once select() or epoll_wait()
     * is entered, it's probably impossible to add events to the set (or at least it's
     * undocumented).  So let's just not.  Similarly, don't allow retrieving results untilt he
     * wait is finishied. */
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_IMMUTABLE_WHEN_WAITING);
    return false;
  }
  // else
  assert(m_state == State::S_INACTIVE);

  err_code->clear();
  return true;
} // Event_set::ok_to_mod_socket_set()

bool Event_set::clear(Error_code* err_code)
{
  /* Note this can be (and was) written FAR more consiely in terms of other clear_*() methods; but the cost
   * was significant: repeated redundant state checks, recursive mutex locks, logging....  The following
   * is superior. */

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Event_set::clear, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in thread U != W.

  // Accessing m_state, the sets, etc. which may be written by other threads at any time.  Must lock.
  Lock_guard lock(m_mutex);

  FLOW_LOG_TRACE("Clearing sets in Event_set [" << this << "]; pre-clear set sizes: "
                 "wanted [" << ev_type_to_socks_map_sizes_to_str(m_want) << "], "
                 "results [" << ev_type_to_socks_map_sizes_to_str(m_can) << "].");

  if (!ok_to_mod_socket_set(err_code)) // Ensure we can modify *want_set in this state.
  {
    // *err_code is set.
    return false;
  }
  // else *err_code is success.

  clear_ev_type_to_socks_map(&m_can);
  clear_ev_type_to_socks_map(&m_want);

  return true;
}

Event_set::Ev_type_to_socks_map Event_set::empty_ev_type_to_socks_map() // Static.
{
  return Ev_type_to_socks_map
         ({
            // Linked_hash_map order is significant.  Iteration will occur in this canonical order in logs, etc.
            { Event_type::S_PEER_SOCKET_READABLE, Sockets() },
            { Event_type::S_PEER_SOCKET_WRITABLE, Sockets() },
            { Event_type::S_SERVER_SOCKET_ACCEPTABLE, Sockets() }
          });
}

void Event_set::clear_ev_type_to_socks_map(Ev_type_to_socks_map* ev_type_to_socks_map) // Static.
{
  assert(ev_type_to_socks_map);
  for (auto& ev_type_and_socks : *ev_type_to_socks_map)
  {
    ev_type_and_socks.second.clear();
  }
}

bool Event_set::ev_type_to_socks_map_entry_is_empty(const Ev_type_to_socks_map::Value& ev_type_and_socks) // Static.
{
  return ev_type_and_socks.second.empty();
}

std::string Event_set::ev_type_to_socks_map_sizes_to_str(const Ev_type_to_socks_map& ev_type_to_socks_map)
{
  using std::string;
  string out;

  size_t n_left = ev_type_to_socks_map.size();
  for (const auto& ev_type_and_socks : ev_type_to_socks_map)
  {
    util::ostream_op_to_string(&out,
                               ev_type_and_socks.first, ": ", ev_type_and_socks.second.size(),
                               (((--n_left) == 0) ? "" : ", "));
  }

  return out;
}

std::string Event_set::sock_as_any_to_str(const boost::any& sock_as_any) // Static.
{
  using boost::any_cast;
  using std::string;

  string out;

  /* A boost::any can contain an object of almost any type or be empty.  Our boost:any's can only be those
   * from what we load into Sockets sets; currently the following types.  So just find the correct type
   * and then cast to it. */
  const auto& type_id = sock_as_any.type();
  if (type_id == typeid(Peer_socket::Ptr))
  {
    util::ostream_op_to_string(&out, any_cast<Peer_socket::Ptr>(sock_as_any));
  }
  else if (type_id == typeid(Server_socket::Ptr))
  {
    util::ostream_op_to_string(&out, any_cast<Server_socket::Ptr>(sock_as_any));
  }
  else
  {
    assert(sock_as_any.empty());
    out = "none";
  }

  return out;
}

// Event_set::Socket_as_any* implementations.

// Method implementations.

size_t Event_set::Socket_as_any_hash::operator()(const boost::any& sock_as_any) const
{
  using boost::any_cast;
  using boost::hash;

  /* A boost::any can contain an object of almost any type or be empty.  Our boost:any's can only be those
   * from what we load into Sockets sets; currently the following types.  So just find the correct type
   * and then cast to it.  Then just apply the default hash operation to that, as would be done if we
   * were simply storing a set or map with keys of that type.
   *
   * Collisions are allowed anyway -- though best avoided for hash lookup performance -- but in this case
   * hash<> will just use the pointer's raw value anyway, and in practice that should ensure completely
   * disjoins sets of hash values for each of the possible stored types. */
  const auto& type_id = sock_as_any.type();
  if (type_id == typeid(Peer_socket::Ptr))
  {
    return hash<Peer_socket::Ptr>()(any_cast<Peer_socket::Ptr>(sock_as_any));
  }
  // else
  if (type_id == typeid(Server_socket::Ptr))
  {
    return hash<Server_socket::Ptr>()(any_cast<Server_socket::Ptr>(sock_as_any));
  }
  // else

  assert(sock_as_any.empty());
  return 0;
}

bool Event_set::Socket_as_any_equals::operator()(const boost::any& sock_as_any1,
                                                 const boost::any& sock_as_any2) const
{
  using boost::any_cast;

  /* A boost::any can contain an object of almost any type or be empty.  Our boost:any's can only be those
   * from what we load into Sockets sets; currently the following types.  Firstly, if the two anys' types
   * are different, they are obviously different objects.  (Note, also, that in practice we'll be storing
   * only objects of the same type in each Sockets structure; thus that case should rarely, if ever,
   * come into play.)
   *
   * Assuming they are of the same type, just find the correct type and then cast both to it.  Now that type's
   * operator==() can do the job, as it would if we were simply storing a set or map with keys of that type. */

  if (sock_as_any1.type() != sock_as_any2.type())
  {
    return false;
  }
  // else

  const auto& type_id = sock_as_any1.type();

  if (type_id == typeid(Peer_socket::Ptr))
  {
    return any_cast<Peer_socket::Ptr>(sock_as_any1) == any_cast<Peer_socket::Ptr>(sock_as_any2);
  }
  // else
  if (type_id == typeid(Server_socket::Ptr))
  {
    return any_cast<Server_socket::Ptr>(sock_as_any1) == any_cast<Server_socket::Ptr>(sock_as_any2);
  }
  // else

  assert(sock_as_any1.empty()); // Note: type_id == typeid(void), in this case.
  assert(sock_as_any2.empty());

  return true;
}

// Node implementations (dealing with individual Event_sets but also Node state).

// Method implementations.

Event_set::Ptr Node::event_set_create(Error_code* err_code)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(Event_set::Ptr, Node::event_set_create, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  using async::asio_exec_ctx_post;
  using async::Synchronicity;

  // We are in thread U != W.

  if (!running())
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_NODE_NOT_RUNNING);
    return Event_set::Ptr();
  }
  // else

  /* Put the rest of the work into thread W.  For justification, see big comment in listen().
   * Addendum regarding performance: event_set_create() should be pretty rare. */

  // Load this body onto thread W boost.asio work queue.  event_set_promise captured by reference, as we will wait.
  Event_set::Ptr event_set(new Event_set(get_logger()));
  event_set->m_state = Event_set::State::S_INACTIVE;
  event_set->m_node = this;
  event_set->m_baseline_check_pending = false;
  asio_exec_ctx_post(get_logger(), &m_task_engine, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION, [&]()
  {
    // We are in thread W.
    m_event_sets.insert(event_set);
  });
  // If got here, the task has completed in thread W and signaled us to that effect.

  err_code->clear();
  return event_set;
} // Node::event_set_create()

bool Node::event_set_async_wait(Event_set::Ptr event_set, const Event_set::Event_handler& on_event,
                                Error_code* err_code)
{
  using boost::asio::post;
  using boost::any;

  // We are in thread U != W.

  if (!running())
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_NODE_NOT_RUNNING);
    return false;
  }
  // else

  // See giant comment in Event_set::async_wait(), which just called us, before proceeding.

  // event_set is locked (and will be unlocked after we exit).

  // Check explicitly documented pre-condition.
  assert(event_set->m_state == Event_set::State::S_INACTIVE);

  FLOW_LOG_TRACE("Event_set [" << event_set << "] wait requested; moving from "
                 "[" << Event_set::State::S_INACTIVE << "] to [" << Event_set::State::S_WAITING << "].");

  // Cock the gun....
  event_set->m_state = Event_set::State::S_WAITING;
  event_set->m_on_event = on_event;

  // Clear result sets, so when we get back to S_INACTIVE (events fire), only the events that happened are there.
  Event_set::clear_ev_type_to_socks_map(&event_set->m_can);

  /* As described in Event_set::async_wait() giant comment, thread W must check all events (sockets
   * + desired states) of interest (in event_set->m_want) as soon as possible, in case those
   * conditions already hold.  If any hold, it should fire m_on_event() right then.  For example,
   * Peer_socket P in m_want[S_PEER_SOCKET_READABLE] may already be Readable, so this would be immediately detected;
   * if we don't do that, the Readable status would only be detected later (if ever) if, say, more
   * DATA packets arrive. */

  post(m_task_engine, [this, event_set]() { event_set_check_baseline_assuming_state(event_set); });

  /* Thread W will invoke this->event_set_check_baseline_assuming_state(event_set) the next time it
   * gets a free moment.  (Might be right NOW, though it won't be able to lock and will wait until
   * we unlock just below.) */

  /* Corner case: Let P be in m_want[PEER_SOCKET_READABLE].  Assume thread W is executing a handler right now which
   * gets a DATA packet on socket P.  Having saved it into P's Receive buffer, it will check each
   * Event_set in *this Node.  We will unlock shortly, at which point W will note that event_set is
   * S_WAITING, and P is in m_want[P_S_R].  Therefore it will fire event_set->m_on_event() and make
   * event_set S_INACTIVE again.  This will happen before event_set_check_baseline() runs.  When
   * that runs, it will be in INACTIVE state again and thus not do anything.
   *
   * Is this bad?  Not really.  It just so happens that that sole socket P wins the race and
   * triggers m_on_event().  This doesn't violate the goal: to report the moment 1 or more events
   * hold; 1 is indeed "1 or more."  However, let's try to avoid situation anyway (be "greedy" for
   * more ready events at a time).  Set event_set->m_baseline_check_pending = true.  This will be a
   * sign to thread W that, when it next examines event_set for a "delta" check (such as upon
   * receiving the DATA packet in this example), it should instead perform the full baseline check
   * of all events.  Then the queued up event_set_check_baseline() can do nothing if that bool is
   * false by then; or do the work and set it to false if it wins the race and performs the full check,
   * in which case the aforementioned "delta" check would just do the "delta" check after all. */
  event_set->m_baseline_check_pending = true;

  /* That's it.  event_set_check_baseline() will execute soon.  If it detects no events, thread W
   * will keep checking for relevant events as states become Readable/Writable/Acceptable/etc. on various
   * sockets.  Eventually it will fire and change m_state from S_WAITING to S_INACTIVE. */

  err_code->clear();
  return true;
  // Unlock happens right after this returns.
} // Node::event_set_async_wait()

void Node::event_set_check_baseline_assuming_state(Event_set::Ptr event_set)
{
  // We are in thread W.

  // Imperative to lock all of event_set.  Much access possible from user threads.
  Event_set::Lock_guard lock(event_set->m_mutex);

  /* event_set_async_wait() placed us onto thread W.  When it did so, event_set->m_state ==
   * S_WAITING (waiting for 1+ events to hold, so we can inform user).  However that may have
   * changed by the time boost.asio was able to get to us.  E.g., socket was closed due to error, or
   * an event was detected before we executed, so we're S_INACTIVE again.  So check for that. */

  if (event_set->m_state != Event_set::State::S_WAITING)
  {
    // Unlikely but legitimate and kind of interesting.
    FLOW_LOG_TRACE("Event_set [" << event_set << "] baseline check ran, but state is no "
                   "longer [" << Event_set::State::S_WAITING << "] but [" << event_set->m_state << "] instead.");
    return;
  }
  // else state is WAITING: should actually perform the baseline check.

  if (event_set_check_baseline(event_set)) // Assumes m_mutex is already locked.
  {
    // Some fired events may now be recorded inside event_set.  Inform user and go to INACTIVE again if so.
    event_set_fire_if_got_events(event_set); // This method assumes m_mutex is already locked.
  }
} // Node::event_set_check_baseline_assuming_state()

bool Node::event_set_check_baseline(Event_set::Ptr event_set)
{
  using boost::any;
  using boost::any_cast;

  // We are in thread W *or* thread U != W.  CAUTION!!!  Ensure all operations below can be done from thread U!

  // event_set is already locked (pre-condition).

  // Check explicitly documented pre-condition.
  assert(event_set->m_state == Event_set::State::S_WAITING);

  /* Our mission is to simply run through every condition in the event_set->m_want sets and
   * check whether it holds.  For each one that does, save it in the appropriate event_set->m_got
   * set.  Then if any did apply, run event_set->m_on_event() to inform the user, and go back to
   * INACTIVE state.  The background for this is in Event_set::async_wait() and friends.
   *
   * This is a more thorough (and slow) check than that invoked when a condition of interest
   * (e.g., new DATA packet added to Receive buffer) arises. */

  if (!event_set->m_baseline_check_pending)
  {
    /* Check already performed once since event_set became WAITING.  Therefore all subsequent checks
     * are the faster "delta" checks, invoked when individual conditions of interest become true. */
    FLOW_LOG_TRACE("Event_set [" << event_set << "] baseline check ran, but skipping because same check already "
                   "ran since the last time we entered [" << Event_set::State::S_WAITING << "].");
    return false;
  }
  // else if (!event_set->m_baseline_check_pending)

  FLOW_LOG_TRACE("Event_set [" << event_set << "] baseline check started.");
  event_set->m_baseline_check_pending = false;

  /* For each type of event, run through each event we want to know about if it's true, and if
   * it's true, add it to the active events list for that event type. */

  static_assert(util::Container_traits<Event_set::Sockets>::S_CONSTANT_TIME_INSERT,
                "Expecting amortized constant time insertion sockets container."); // Ensure fastness.

  /* To get the set of Event_type, use S_EV_TYPE_TO_IS_ACTIVE_NODE_MTD which has them as the key set;
   * it also stores the functions of the form `bool Node::does_event_hold(const any& sock_as_any)`, where
   * sock_as_any is a boost::any wrapping a socket appropriate to the given Event_type, and the function will
   * return whether the condition of that type holds for a given socket of interest. */
  for (const auto& ev_type_and_is_active_mtd : Event_set::S_EV_TYPE_TO_IS_ACTIVE_NODE_MTD)
  {
    const Event_set::Event_type ev_type = ev_type_and_is_active_mtd.first;
    const auto& does_event_hold = ev_type_and_is_active_mtd.second;
    const Event_set::Sockets& want_set = event_set->m_want[ev_type];
    Event_set::Sockets& can_set = event_set->m_can[ev_type];

    assert(can_set.empty()); // We will now fill it up.
    for (const any& sock_as_any : want_set)
    {
      if (does_event_hold(this, sock_as_any))
      {
        FLOW_LOG_TRACE("Event of type [" << ev_type << "] for "
                       "object [" << Event_set::sock_as_any_to_str(sock_as_any) << "] detected in "
                       "Event_set [" << event_set << "].");

        can_set.insert(sock_as_any);
      }
    }
  }

  return true; // This only means the check ran, not that it found anything.
} // Node::event_set_check_baseline()

void Node::event_set_fire_if_got_events(Event_set::Ptr event_set)
{
  using boost::algorithm::all_of;

  // We are in thread W.

  // event_set state involved should already be locked.

  // Explicitly documented pre-condition.
  assert(event_set->m_state == Event_set::State::S_WAITING);

  if (all_of(event_set->m_can, Event_set::ev_type_to_socks_map_entry_is_empty))
  {
    // Nope, no events ready.  Carry on in WAITING state.
    return;
  }
  // else

  // If editing this, check interrupt_all_waits_worker() also. That's another path to firing m_on_event().

  // Yes!  At least one event is ready, so inform via the m_on_event() callback.
  FLOW_LOG_TRACE("Event_set [" << event_set << "] has ready events; firing and moving from "
                 "[" << Event_set::State::S_WAITING << "] to [" << Event_set::State::S_INACTIVE << "] again.");

  event_set->m_on_event(false);

  // Forget the callback.  They set the callback each time async_wait() is called (as an argument to that method).
  event_set->m_on_event.clear();

  // Gun has fired and is thus no longer cocked.
  event_set->m_state = Event_set::State::S_INACTIVE;

  /* It's important to understand what happens now.  m_on_event() just did whatever the user wants
   * (presumably signal the user thread(s) (not W) that event_set is back to INACTIVE state and
   * currently holds the active events in event_set->m_can).  For example it might set a future
   * that the user thread is waiting on; or it might send a Unix domain socket message or local TCP
   * message, which the user thread is select()ing on and thus wake it up.  The point is, the user
   * thread U may already be awake RIGHT NOW and trying to check event_set->m_can for activity.
   *
   * However, we have not unlocked event_set yet.  We soon will; until then U will sit there waiting
   * for the unlock (any access to event_set must lock event_set mutex; this is enforced by all
   * public Event_set methods).  Once acquired, it can freely check it and perform actions like
   * receive() or accept(). */
} // Node::event_set_fire_if_got_events()

void Node::event_set_all_check_delta(bool defer_delta_check)
{
  // We are in thread W.

  using boost::any;
  using boost::algorithm::all_of;

  /* Short-circuit (avoid logging, unneeded locking, etc.) if no delta events have been noted so
   * far.  (If baseline check has not run yet, it is queued by boost.asio as
   * event_set_check_baseline_assuming_state(), so we need not worry about it here.) */
  if (all_of(m_sock_events, Event_set::ev_type_to_socks_map_entry_is_empty))
  {
    return;
  }

  FLOW_LOG_TRACE("Possible event of interest occurred; "
                 "defer_delta_check = [" << defer_delta_check << "]; "
                 "results set sizes = [" << Event_set::ev_type_to_socks_map_sizes_to_str(m_sock_events) << "].");

  if (defer_delta_check)
  {
    /* Caller believes there's an event_set_all_check_delta(false) call within a "non-blocking"
     * amount of time, so we should let more possible events accumulate before possibly signaling
     * user.  Okay. */
    return;
  }
  // else

  /* Delta-check not deferred.  We should perform it... but there's a possibility that we haven't
   * performed the baseline check yet.  As explained in giant comment inside
   * Event_set::async_wait(), a delta check must be performed after a baseline check (and assuming
   * the latter fired no events); otherwise its results would be incomplete.  How we can get to a delta check
   * before a baseline check is explained in the comment in Node::event_set_async_wait().
   *
   * So, if we haven't, perform the baseline check first.  Basically it is an exhaustive check of
   * all desired events, regardless of what's in the delta-check variables Node::m_sock_events.  See that
   * below.  For purposes of discussion, though, assume we do indeed perform only delta checks.
   * Then:
   *
   * Now we check all WAITING Event_sets.  For each one, find any waited-on conditions that have
   * indeed occurred since the last time we did this; these are accumulated in this->m_sock_events.  Any
   * that did indicate that at least one event of interest has occurred, and thus we should mark all
   * of them down in the appropriate Event_set(s) and signal those waiting on these Event_sets.
   *
   * Note that we check ALL registered Event_sets.  This seems a little crude/inefficient.  How to
   * avoid it?  Basically we'd need to map each (Peer/Server_socket, event type) pair to any
   * containing, waiting Event_set(s), so that we can quickly go from the pair to the Event_set.
   * The problem is synchronization (since sockets are added to Event_sets, and
   * Event_set::async_wait() is called, by user threads, not W).  I thought about this pretty hard
   * and was unable to come up with anything elegant that would avoid possible deadlocks w/r/t each
   * Event_set's individual m_mutex as well.  I thought and thought and thought and eventually
   * realized that simply iterating over all Event_sets and handling one at a time sidesteps all
   * such complexities.  Is this slow?  Firstly there's the iteration over all Event_sets.  How many
   * are there?  Realistically, there's probably one per user thread, of which there would be about
   * as many as CPU cores.  That is not a lot.  So take one Event_set.  For each active socket in
   * this->m_sock_events[...], we just look it up in the Event_set's appropriate m_want[...]; since we store the
   * latter in a hash table, that's a constant time search.  If we did have the socket-to-Event_set
   * map (and figured out a working synchronization scheme), that would also be just one
   * constant-time search.  So without the nice mapping being available, the performance is only
   * slowed down by having to look at each Event_set, and there are not many in practice, and an
   * extra constant-time lookup beyond that.  So it's fine. */

  for (Event_set::Ptr event_set : m_event_sets)
  {
    // As explained above, work on one Event_set at a time.  Lock it.
    Event_set::Lock_guard lock(event_set->m_mutex);

    if (event_set->m_state != Event_set::State::S_WAITING)
    {
      continue;
    }
    // else

    // As explained above, perform the baseline check if necessary.
    if (!event_set_check_baseline(event_set)) // Method assumes m_mutex already locked.
    {
      // Wasn't necessary (mainstream case).  Perform delta check.

      FLOW_LOG_TRACE("Event_set [" << event_set << "] delta check started.");

      /* For each type of event, run through each event we want to know about if it's true, and if
       * it's true (is in Node::m_sock_events), add it to the active events list for that event type.
       *
       * Note that if some event is desired in two Event_sets, and it has occurred, both Event_sets
       * will fire, as advertised.  It also means the user is a crazy mother, but that's not our
       * problem.... */

      for (auto& ev_type_and_socks_from_node : m_sock_events)
      {
        const Event_set::Event_type ev_type = ev_type_and_socks_from_node.first;
        const Event_set::Sockets& all_can_set = ev_type_and_socks_from_node.second;

        Event_set::Sockets& can_set = event_set->m_can[ev_type];
        Event_set::Sockets& want_set = event_set->m_want[ev_type];

        assert(can_set.empty());

        /* all_can_set is the set of all sockets for which the event has occurred, since the last baseline
         * or delta check on event_set.  Our goal is to load can_set (which is a field in
         * event_set) with all sockets for which the event has occurred AND are in want_set (a field in
         * event_set containing the sockets for which the user is interested in this event).  Therefore,
         * what we want is the set intersection of want_set and all_can_set; and we want to put the result
         * into can_set.
         *
         * Both want_set and all_can_set have O(1) search.  Therefore the fastest way to compute the
         * intersection is to pick the smaller set; iterate through each element; and save the given
         * element into can_set if it is present in the other set. */

        // Ensure fastness just below.
        static_assert(util::Container_traits<Event_set::Sockets>::S_CONSTANT_TIME_SEARCH,
                      "Expecting amortized constant time search sockets container.");
        static_assert(util::Container_traits<Event_set::Sockets>::S_CONSTANT_TIME_INSERT,
                      "Expecting amortized constant time insertion sockets container.");

        const bool want_set_smaller = want_set.size() < all_can_set.size();
        const Event_set::Sockets& small_set = want_set_smaller ? want_set : all_can_set;
        const Event_set::Sockets& large_set = want_set_smaller ? all_can_set : want_set;

        for (const any& sock_as_any : small_set)
        {
          if (util::key_exists(large_set, sock_as_any))
          {
            FLOW_LOG_TRACE("Event of type [" << ev_type << "] for "
                           "socket object [" << Event_set::sock_as_any_to_str(sock_as_any) << "] detected in "
                           "Event_set [" << event_set << "].");

            can_set.insert(sock_as_any);
          }
        }
      } // for ((ev_type, all_can_set) : m_sock_events)
    } // if (!event_set_check_baseline(event_set))

    /* Was necessary to do baseline check.  Thus skip delta check.  Shouldn't we perform the delta check?  No.
     * The baseline check is a superset of the delta check: it simply checks each desired event's
     * socket for the desired condition.  Thus performing a delta check would be useless. */

    // If either check detected active events, fire this event_set (and thus move it to INACTIVE).
    event_set_fire_if_got_events(event_set); // Method assumes m_mutex already locked.
  } // for (all Event_sets in m_event_sets)

  // Each Event_set has been handled.  So begin a clean slate for the next delta check.
  Event_set::clear_ev_type_to_socks_map(&m_sock_events);
} // Node::event_set_all_check_delta()

void Node::event_set_close(Event_set::Ptr event_set, Error_code* err_code)
{
  using async::asio_exec_ctx_post;
  using async::Synchronicity;
  using boost::adopt_lock;

  // We are in user thread U != W.

  if (!running())
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_NODE_NOT_RUNNING);
    return;
  }
  // else

  // Pre-condition is that m_mutex is locked already.

  {
    /* WARNING!!!  event_set->m_mutex is locked, but WE must unlock it before returning!  Can't
     * leave that to the caller, because we must unlock at a specific point below, right before
     * post()ing event_set_close_worker() onto thread W.  Use a Lock_guard that adopts an
     * already-locked mutex. */
    Event_set::Lock_guard lock(event_set->m_mutex, adopt_lock);

    /* Put the rest of the work into thread W.  For justification, see big comment in listen().
     * Addendum regarding performance: close_abruptly() is probably called more frequently than
     * listen(), but I doubt the performance impact is serious even so.  send() and receive() might be
     * a different story. */

    // We're done -- must unlock so that thread W can do what it wants to with event_set.
  } // lock

  // Load this bad boy onto thread W boost.asio work queue.  Safe to capture by &reference, as we will wait.
  asio_exec_ctx_post(get_logger(), &m_task_engine, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION, [&]()
  {
    // We are in thread W.  event_set_close() is waiting for us to set close_promise in thread U.

    // Something like async_wait_finish() may be setting event_set->m_state or other things... must lock.
    Event_set::Lock_guard lock(event_set->m_mutex);

    /* Since we were placed onto thread W, another handler may have been executed before boost.asio
     * got to us.  Therefore we may already be S_CLOSED.  Detect this. */

    if (event_set->m_state == Event_set::State::S_CLOSED) // No need to lock: only W can write to this.
    {
      // Yep, already closed.  Done.
      *err_code = error::Code::S_EVENT_SET_CLOSED;
      return;
    }
    // else actually do it.

    // m_mutex locked (pre-condition).
    event_set_close_worker(event_set);
    err_code->clear(); // Success.
  }); // asio_exec_ctx_post()
  // If got here, the task has completed in thread W and signaled us to that effect.

} // Node::event_set_close()

void Node::event_set_close_worker(Event_set::Ptr event_set)
{
  // We are in thread W.

  // Everything already locked (explicit pre-condition).

  // Check explicitly documented pre-condition.
  assert((event_set->m_state == Event_set::State::S_INACTIVE)
         || (event_set->m_state == Event_set::State::S_WAITING));

  /* If INACTIVE:
   *   event_set is not currently being waited on; it's in the phase where the set of desired events
   *   may be being changed, and/or the last set of fired events may be being examined; no on-event
   *   behavior could be triggered in any case.  Therefore we simply go to CLOSED state and remove
   *   event_set from the master list with no potential for controversy.
   *
   * If WAITING:
   *   We advertised in the doc comment, that we will NOT execute any on-event behavior in close().
   *   Is this OK?  There is  a chance of controversy.  There is a wait happening in some thread U
   *   != W.  We are currently (chronologically speaking) in the middle of some thread U' != W
   *   calling event_set->close().  Let U = U' (the typical case).  Then they (in thread U) must
   *   have called event_set->async_wait() and then event_set->close().  In that case, unless
   *   they're dummies, they should know that, since they're closing the thing, the wait() is to be
   *   abandoned and will never fire on-event behavior.  Therefore, as above, we just go straight to
   *   CLOSED state and not invoke any on-event behavior.  Now let U != U' (atypical in general;
   *   normally one would use a given Event_set in only one thread).  Well, one thread executed
   *   async_wait(), while another executed async_close() afterwards.  Could that screw the former?
   *   If the user is unaware that he might do that to himself, yes... but that is not our problem.
   *   Therefore, OK to not perform on-event behavior and just close in this case also. */

  // First, set various state in *event_set.

  // This can be quite frequent if program uses many sync_*() methods.  Use TRACE.
  FLOW_LOG_TRACE("Closing Event_set [" << event_set << "]: changing state "
                 "from [" << event_set->m_state << "] to [" << Event_set::State::S_CLOSED << "].");

  assert(event_set->m_state != Event_set::State::S_CLOSED);
  event_set->m_state = Event_set::State::S_CLOSED;
  event_set->m_node = 0; // Maintain invariant.

  // Free resources.  In particular, after Event_set close, user won't be able to see last set of fired events.
  Event_set::clear_ev_type_to_socks_map(&event_set->m_want);
  Event_set::clear_ev_type_to_socks_map(&event_set->m_can);

  /* Forget the handler callback.  What if there's an outstanding async_wait()?  Well,
   * m_on_event() will NOT be called unless state is WAITING, and it won't be WAITING; it will be
   * CLOSED once this critical section is over.  So that outstanding async_wait() will never
   * "finish."  However, this is explicitly documented on the public close() method.  Node shutdown
   * will avoid this problem by first closing each socket, thus waking up any waits, before
   * performing Event_set::close(). */
  event_set->m_on_event.clear();

  // Then remove it from the master list.
#ifndef NDEBUG
  const bool erased = 1 ==
#endif
    m_event_sets.erase(event_set);
  assert(erased); // If was not CLOSED, yet was not in m_event_sets.  So it is a serious bug somewhere.
} // Node::event_set_close_worker()

void Node::interrupt_all_waits(Error_code* err_code)
{
  using boost::asio::post;

  if (flow::error::exec_void_and_throw_on_error
        ([this](Error_code* actual_err_code) { interrupt_all_waits(actual_err_code); },
         err_code, FLOW_UTIL_WHERE_AM_I_STR()))
  {
    return;
  }
  // else

  // We are in thread U != W.

  if (!running())
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_NODE_NOT_RUNNING);
    return;
  }
  // else

  /* Put the rest of the work into thread W.  For justification, see big comment in listen().
   * Addendum regarding performance: interrupt_all_waits() should be pretty rare. */

  post(m_task_engine, [this]() { interrupt_all_waits_worker(); });

  err_code->clear();
} // Node::interrupt_all_waits()

void Node::interrupt_all_waits_worker()
{
  // We are in thread W.

  FLOW_LOG_INFO("Executing request to interrupt all waiting Event_sets.");

  for (Event_set::Ptr event_set : m_event_sets)
  {
    // Work on one Event_set at a time.  Lock it.
    Event_set::Lock_guard lock(event_set->m_mutex);

    if (event_set->m_state == Event_set::State::S_WAITING)
    {
      /* The code here is based on event_set_fire_if_got_events(). This is another type of an event being "ready";
       * it's just that it's ready because of it being interrupted instead of due to the actual waited-on condition
       * becoming true. Keeping comments light to avoid redundancy/maintenance issues.
       *
       * If editing this, check event_set_fire_if_got_events() also. */

      FLOW_LOG_INFO("Event_set [" << event_set << "] is being interrupted; firing and moving from "
                    "[" << Event_set::State::S_WAITING << "] to [" << Event_set::State::S_INACTIVE << "] again.");

      /* As promised, the interrupted case means these will be empty.  The actual calls may not be necessary, as maybe
       * adding elements to these always immediately (within same m_mutex locking) leads to going to S_INACTIVE state,
       * meaning the present code would not be executing.  However, no need to rely on that; quite possibly we could
       * have code that would load stuff into event_set->m_can and only go to S_WAITING in another callback. */
      Event_set::clear_ev_type_to_socks_map(&event_set->m_can);

      event_set->m_on_event(true); // true means firing due to interruption, not due to waited-on condition being true.
      event_set->m_on_event.clear();
      event_set->m_state = Event_set::State::S_INACTIVE;
    }
  } // for (all Event_sets in m_event_sets)
} // Node::interrupt_all_waits_worker()

void Node::interrupt_all_waits_internal_sig_handler(const Error_code& sys_err_code, int sig_number)
{
  // We are in thread W.  (Yes. This is a contract of this function.)

  if (sys_err_code == boost::asio::error::operation_aborted)
  {
    return; // Stuff is shutting down; just get out.
  }
  // else

  FLOW_LOG_INFO("Internal interrupt signal handler executed with signal number [" << sig_number << "].");

  if (sys_err_code)
  {
    // This is odd, but there's no need to freak out about anything else.  Just log and get out.
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();
    FLOW_LOG_WARNING("Internal signal handler executed with unexpected error indicator.  Strange!  "
                     "Ignoring and continuing other operation.");
  }
  else
  {
    /* To the user, this default signal handler is just supposed to call Node::interrupt_all_waits().
     * It's a convenience thing, in case they don't want to set up their own signal handler that'll do that, and also
     * they don't need any of their own custom signal handling that may have nothing to do with net_flow at all.
     * Actually they can even have the latter -- as long as it uses signal_set also.
     * (If they do need some other handling, they MUST disable this feature via Node_options. Then we wouldn't be
     * called.) */

    // This must be run in thread W, and indeed we are in thread W by contract. It'll log further, so just call it.
    interrupt_all_waits_worker();
  }

  // Wait for it again, or else this will work only the first time signal is sent.
  m_signal_set.async_wait([this](const Error_code& sys_err_code, int sig_num)
  {
    interrupt_all_waits_internal_sig_handler(sys_err_code, sig_num);
  });
} // Node::interrupt_all_waits_internal_sig_handler()

// Free implementations.

/// @cond
/* -^- Doxygen, please ignore the following.  (Don't want docs generated for temp macro; this is more maintainable
 * than specifying the macro name to omit it, in Doxygen-config EXCLUDE_SYMBOLS.) */

// That's right, I did this.  Wanna fight about it?
#define STATE_TO_CASE_STATEMENT(ARG_state) \
  case Event_set::State::S_##ARG_state: \
    return os << #ARG_state

// -v- Doxygen, please stop ignoring.
/// @endcond

std::ostream& operator<<(std::ostream& os, Event_set::State state)
{
  switch (state)
  {
    STATE_TO_CASE_STATEMENT(INACTIVE);
    STATE_TO_CASE_STATEMENT(WAITING);
    STATE_TO_CASE_STATEMENT(CLOSED);
  }
  return os;
#undef STATE_TO_CASE_STATEMENT
}

/// @cond
// -^- Doxygen, please ignore the following.  (Same deal as just above.)

// That's right, I did this.  Wanna fight about it?
#define TYPE_TO_CASE_STATEMENT(ARG_type) \
  case Event_set::Event_type::S_##ARG_type: \
    return os << #ARG_type

// -v- Doxygen, please stop ignoring.
/// @endcond

std::ostream& operator<<(std::ostream& os, Event_set::Event_type ev_type)
{

  switch (ev_type)
  {
    TYPE_TO_CASE_STATEMENT(PEER_SOCKET_READABLE);
    TYPE_TO_CASE_STATEMENT(PEER_SOCKET_WRITABLE);
    TYPE_TO_CASE_STATEMENT(SERVER_SOCKET_ACCEPTABLE);
  }
  return os;
#undef TYPE_TO_CASE_STATEMENT
}

} // namepace flow::net_flow
