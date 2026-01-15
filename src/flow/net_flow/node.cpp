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
#include "flow/net_flow/endpoint.hpp"
#include "flow/util/sched_task.hpp"
#include <algorithm>
#include <limits>

namespace flow::net_flow
{
// Static initializations.

const Fine_duration Node::S_REGULAR_INFREQUENT_TASKS_PERIOD = boost::chrono::seconds{1}; // Infrequent enough CPU-wise.

// Note that they're references, not copies.  Otherwise non-deterministic static initialization order would screw us.
const size_t& Node::S_NUM_PORTS = Port_space::S_NUM_PORTS;
const size_t& Node::S_NUM_SERVICE_PORTS = Port_space::S_NUM_SERVICE_PORTS;
const size_t& Node::S_NUM_EPHEMERAL_PORTS = Port_space::S_NUM_EPHEMERAL_PORTS;
const flow_port_t& Node::S_FIRST_SERVICE_PORT = Port_space::S_FIRST_SERVICE_PORT;
const flow_port_t& Node::S_FIRST_EPHEMERAL_PORT = Port_space::S_FIRST_EPHEMERAL_PORT;

// Implementations.  For Node code pertaining to individual Server_sockets and Peer_sockets see their .cpp files.

Node::Node(log::Logger* logger_ptr, const util::Udp_endpoint& low_lvl_endpoint, Net_env_simulator* net_env_sim,
           Error_code* err_code, const Node_options& opts) :
  log::Log_context(this_thread_init_logger_setup("", logger_ptr),
                   Flow_log_component::S_NET_FLOW),
  /* Take the given Node_options set and copy it into our stored global options.  (Note the default
   * is Node_options().)  The default is safe, but if they actually are providing a custom set of
   * options, then we must validate before accepting.  This may result in a validation error.
   * If !err_code, then it'll throw exception right here.  If err_code, then it will set *err_code,
   * so we check for it inside the constructor body and exit. */
  m_opts(validate_options(opts, true, err_code)),
  m_net_env_sim(net_env_sim), // Note this is a smart pointer taking over a raw pointer (we did advertise owning it).
  m_low_lvl_sock(m_task_engine), // Blank (unbound) UDP socket.
  m_packet_data(logger_ptr),
  m_ports(logger_ptr),
  m_seq_num_generator(logger_ptr),
  m_sock_events(Event_set::empty_ev_type_to_socks_map()),
  // Set up future object used to wait for m_event_loop_ready to become success or failure.
  m_event_loop_ready_result(m_event_loop_ready.get_future()),
  /* Set up a signal set object; this is a no-op until we .add() signals to it (which we may or may not do).
   * Whether we do or not is more significant than merely whether whatever handler we'd later register
   * via m_signal_set.async_wait() will be called; if we .add() zero signals, then IF some non-boost.asio
   * signal handler is currently registered (such as the default OS handler; or the user's non-boost.asio handler)
   * (or will be registered in the future) will continue to work undisturbed.  If, however, we .add() one or more
   * signals (or, equivalently, list 1 or more signal numbers right here in the constructor call),
   * then we will REPLACE such non-boost.asio handlers with boost.asio's mechanism.  (This behavior is
   * explicitly documented in boost.asio docs.)  Therefore we must be careful not to mindlessly .add() handler(s),
   * and/or (equivalently) list 1 or more signal numbers in this constructor call here.  The choice will be left to
   * later code which will .add() or not .add() deliberately. */
  m_signal_set(m_task_engine),
  // Spawn new thread W; in it execute this->worker_run(low_lvl_endpoint).  low_lvl_endpoint is copied.
  m_worker(&Node::worker_run, this, low_lvl_endpoint)
{
  using flow::error::Runtime_error;

  // We are in thread U.

  FLOW_LOG_INFO("Starting flow::Node [" << static_cast<void*>(this) << "].");

  // validate_options() may have already detected an error; then it would've thrown if (!err_code); else: Check it.
  if (err_code && (*err_code))
  {
    FLOW_LOG_WARNING("Cannot start Node due to above error.");
    return;
  }

  Error_code our_err_code; // Prepare this if they passed in no Error_code, so we can throw exception.
  err_code || (err_code = &our_err_code);

  FLOW_LOG_INFO("\n\n" << options()); // Log initial option values.  Once per Node, so it's not too verbose.

  FLOW_LOG_INFO("Just launched Flow worker thread [T" << m_worker.get_id() << "].  "
                "Waiting for it to report startup success or failure.");

  // Now wait around (probably not long) until thread W tells us either initialization success or failure.
  m_event_loop_ready_result.wait();
  *err_code = m_event_loop_ready_result.get();
  if (*err_code)
  {
    FLOW_LOG_WARNING("Flow worker thread [T" << m_worker.get_id() << "] reported to me it failed to initialize: "
                     "[" << *err_code << "] [" << err_code->message() << "].");
  }
  else
  {
    FLOW_LOG_INFO("Flow worker thread [T" << m_worker.get_id() << "] reports to me it is ready for work.");
  }
  // m_task_engine.stopped() can now be reliably used to tell whether the Node (i.e., thread W) is running.

  if (our_err_code) // Throw exception if there is an error, and they passed in no Error_code.
  {
    throw Runtime_error{our_err_code, FLOW_UTIL_WHERE_AM_I_STR()};
  }
} // Node::Node()

log::Logger* Node::this_thread_init_logger_setup(const std::string& thread_type, log::Logger* logger_ptr)
{
  using log::Logger;
  using log::beautify_chrono_logger_this_thread;
  using std::string;
  using std::hex;

  if (!logger_ptr)
  {
    logger_ptr = get_logger();
  }

  // Use standard beautified formatting for chrono durations/etc. output (and conversely input).
  beautify_chrono_logger_this_thread(logger_ptr);

  if (!thread_type.empty())
  {
    // Nickname the thread for more convenient logging.
    string thread_nickname; // Write directly into this string.
    util::ostream_op_to_string(&thread_nickname, "nod@", hex, this, '_', thread_type);
    Logger::this_thread_set_logged_nickname(thread_nickname, logger_ptr);
  }

  return logger_ptr;
}

Node::~Node() // Virtual.
{
  FLOW_LOG_INFO("Waiting for Flow worker thread [T" << m_worker.get_id() << "] to finish.");

  m_task_engine.stop(); // Let current running callbacks finish, then exit m_task_engine.run() and the thread.
  m_worker.join(); // Wait for thread to finish.  Closing activities are in worker_run() after run() exits.

  // Aside: m_net_env_sim (unless null anyway) will probably die due to ref-count=0 at the end of this { block }.

  FLOW_LOG_INFO("Node [" << static_cast<void*>(this) << "] shut down.");
} // Node::~Node()

void Node::worker_run(const util::Udp_endpoint low_lvl_endpoint)
{
  using log::Logger;
  using util::schedule_task_from_now;
  using boost::system::system_error;
  using boost::asio::socket_base;
  using boost::asio::post;
  using std::string;
  using std::hex;

  // We're in thread W.

  // Global (for this thread, for this Node) logging setup.
  this_thread_init_logger_setup("wrk");

  FLOW_LOG_INFO("Flow worker thread reporting for duty.  Will bind to [UDP " << low_lvl_endpoint << "].");

  // Set up the UDP socket at the given interface and UDP port.
  try // Performance not a concern during initialization; use exceptions for convenience.
  {
    // BSD socket equivalents: socket(), setsockopt(), setsockopt/ioctl/whatever(), bind(); in that order.
    m_low_lvl_sock.open(low_lvl_endpoint.protocol()); // Pick IPv4 vs. IPv6 based on the bind IP they provided.

    /* A small UDP buffer size (empirically seen to be ~80 1k datagrams in an older Linux system, for example) can
     * cause loss when data are coming in very fast, and thread W's processing can't keep, causing
     * UDP buffer overflow and thus dropped datagrams.  So we set it to a high value to avoid that
     * as much as we can.  Also see related @todo in Node doc header (it has to do with moving
     * UDP processing into a separate thread, so that datagrams are read off UDP as fast as
     * possible and not blocked by other processing). */

    socket_base::receive_buffer_size rcv_buf_opt(opt(m_opts.m_st_low_lvl_max_buf_size));
    m_low_lvl_sock.set_option(rcv_buf_opt);

    // Now read it back and store it for informational purposes.  OS may not respect what we tried to set.
    m_low_lvl_sock.get_option(rcv_buf_opt);
    m_low_lvl_max_buf_size = rcv_buf_opt.value();

    m_low_lvl_sock.non_blocking(true);
    m_low_lvl_sock.bind(low_lvl_endpoint);

    /* Save it for local_low_lvl_endpoint() (for user access).  Why not just have
     * local_low_lvl_endpoint() call m_low_lvl_sock.local_endpoint()?  Answer: we'd have to
     * protect m_low_lvl_sock with a mutex.  Why not save to m_low_lvl_local_endpoint directly from
     * low_lvl_endpoint?  Because if, say, user selected UDP port 0, we want
     * local_low_lvl_endpoint() to return the actual port the OS chose, not the
     * more-useless "0." */
    m_low_lvl_endpoint = m_low_lvl_sock.local_endpoint();
  }
  catch (const system_error& exc)
  {
    const Error_code sys_err_code = exc.code();
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();
    FLOW_LOG_WARNING("Unable to set up low-level socket.  Node cannot initialize.  Thread exiting.");

    // Constructor is waiting to see if we were able to start event loop.  We were not.
    m_task_engine.stop(); // So that Node::running() will return false.
    m_event_loop_ready.set_value(exc.code());

    // Since we never started m_task_engine, Node::running() will return false.
    return;
  }

  // Once run() executes below, this should be the first thing it does (report to constructor thread).
  post(m_task_engine, [this]()
  {
    // We are in thread W.
    m_event_loop_ready.set_value(Error_code{});
  });

  // When a packet is available for reading (or error), call this->low_lvl_recv_and_handle(<error code>).
  async_low_lvl_recv();

  // Also execute some low-priority tasks (such as periodic stat logging) every S_REGULAR_INFREQUENT_TASKS_PERIOD.
  schedule_task_from_now(get_logger(), S_REGULAR_INFREQUENT_TASKS_PERIOD, true, &m_task_engine,
                         [this](bool)
  {
    perform_regular_infrequent_tasks(true);
  });

  /* Go!  Sleep until the next registered event.  Our handlers (like low_lvl_recv_and_handle()) should themselves
   * register more events to wait for thus ensuring run() doesn't run out of work (thus doesn't exit) until
   * something intentionally wants to stop it (stop the Node). */

  FLOW_LOG_INFO("Low-level socket initialized.");

  if (m_opts.m_st_capture_interrupt_signals_internally)
  {
    FLOW_LOG_INFO("Setting up internal wait-interrupting interrupt signal handler.  "
                  "CAUTION!  User program MUST avoid using non-boost::asio::signal_set signal handling!  "
                  "If it does use non-boost.asio, behavior is undefined.");

    // Add the classic EINTR-inducing signal numbers.
    m_signal_set.add(SIGINT);
    m_signal_set.add(SIGTERM);

    /* At this point, receiving those signals will NOT just exit the program (or whatever other non-boost.asio
     * handling was active before the .add() calls).  Before actually reporting successful initialization (reminder:
     * constructor is currently waiting for us to finish initializatio and report it via the promise object), set up
     * the handler that'll be called upon receiving the signals. */

    /* this->interrupt_all_waits_internal_sig_handler(err_code, signal_number) will be called on signal (or error).
     * Note that that function's contract (from its doc comment) is it must execute in thread W.
     * Indeed boost::asio::io_context semantics guarantee it'll run in thread W (not some
     * no-man's-land signal handler thread of execution, as one might fear could be the case) for the same reason
     * the various socket I/O handlers and timer handlers above will run in thread W: because we'll run
     * m_task_engine.run() below from thread W, and all such functions are guaranteed to run "as if"
     * post(m_task_engine)ed.  Anything post(m_task_engine)ed is guaranteed by boost.asio docs to execute form
     * the same thread as m_task_engine.run(). */
    m_signal_set.async_wait([this](const Error_code& sys_err_code, int sig_num)
    {
      interrupt_all_waits_internal_sig_handler(sys_err_code, sig_num);
    });
  } // if (m_opts.m_st_capture_interrupt_signals_internally)
  /* else if (!m_opts.m_st_capture_interrupt_signals_internally)
   * {
   *   Do NOT .add() anything; and don't async_wait() anything.  As noted in comment at m_signal_set construction time,
   *   .add() does more than make it possible to .async_wait().  It also replaces any default OS or user's own
   *   non-boost.asio signal handling machinery with boost.asio's signal_set machinery. That can be quite draconian,
   *   so user must specifically set that option to true.  If it's false (in all Nodes constructed by user in the entire
   *   app), then whatever signal handling machinery the user wants to set up for themselves (or leave at OS's
   *   discretion) will remain undisturbed.  By the way, of course, they can use boost.asio machinery themselves too;
   *   it's just that doing so would still work even if m_st_capture_interrupt_signals_internally were true, so that's
   *   not the dangerous scenario.
   * } */

  FLOW_LOG_INFO("Flow event loop starting now.");

  m_task_engine.run();

  /* Destructor must have stop()ped m_task_engine.  restart() will allow the below poll()s to
   * proceed. */
  m_task_engine.restart();

  // Log final state report before closing down.  Do not schedule to run again.
  perform_regular_infrequent_tasks(false);

  /* We should clean up everything (like close sockets, ensuring user gets errors when trying to
   * send/receive/etc.), but quickly.  We should avoid potentially slow blocking operations like
   * graceful closes here; if the Node is shutting down, shut it down abruptly.  @todo Reconsider.
   *
   * Therefore sending RSTs synchronously to all connected peers and then abruptly closing all
   * sockets should be sufficient.  User threads waiting on Readable, Writable, Acceptable will be
   * woken and informed of the error. */

  try // Performance not a concern during shutdown; use exceptions for convenience.
  {
    /* stop() lets any handlers running at the time finish but then run() exits before running any
     * more handlers.  This means there may have been some handlers queued up to run immediately.
     * Let them run now, as if stop() was called just a few moments later. */
    FLOW_LOG_TRACE("Flow worker thread event loop stopped.  Running queued up handlers.");
    m_task_engine.poll();

    // Send RSTs (synchronously).

    FLOW_LOG_INFO("Worker thread told to stop (probably Node destructor executed).  "
                    "Sending [RST] to all open Flow sockets.");

    // Just in case the RST sending operation blocks for some reason....
    m_low_lvl_sock.non_blocking(false);
    for (const auto& sock_pair : m_socks)
    {
      // May technically block but should not be for long if so (UDP).  Probably OK.
      sync_sock_low_lvl_rst_send(sock_pair.second); // Will log if there's a problem.
    }
    m_low_lvl_sock.non_blocking(true);

    // Now close (synchronously) all open sockets.  This posts no handlers on m_task_engine except canceled timers.
    FLOW_LOG_TRACE("Abruptly closing all Flow peer sockets.");
    while (!m_socks.empty()) // Don't loop over it -- loop body will erase elements!
    {
      Socket_id_to_socket_map::const_iterator sock_it = m_socks.begin();
      close_connection_immediately(sock_it->first, sock_it->second, error::Code::S_NODE_SHUTTING_DOWN, false);
      // ^-- defer_delta_checks == false: no need to optimize during shutdown.

      // Note that the above covers not-fully-open (e.g., awaiting SYN_ACK) sockets as well.
    }

    // Close (synchronously) all server sockets.
    FLOW_LOG_TRACE("Abruptly closing all Flow server sockets.");
    while (!m_servs.empty()) // As above.
    {
      Port_to_server_map::const_iterator serv_it = m_servs.begin();
      close_empty_server_immediately(serv_it->first, serv_it->second, error::Code::S_NODE_SHUTTING_DOWN, false);
      // ^-- defer_delta_checks == false: no need to optimize during shutdown.
    }

    // Close all Event_sets.  This is always synchronous.  As advertised, this may trigger on-event behavior.
    FLOW_LOG_TRACE("Closing all event sets and waking up any on-going waits on those event sets.");
    while (!m_event_sets.empty()) // As above.
    {
      const Event_set::Ptr event_set = *m_event_sets.begin();
      Event_set::Lock_guard lock(event_set->m_mutex); // Pre-condition for event_set_close_worker().
      event_set_close_worker(event_set);
    }

    // Run those canceled timer handlers for cleanliness (and in case that would let go of some resources).
    FLOW_LOG_TRACE("Cleaning up canceled timer tasks.");
    m_task_engine.poll();

    /* Note: to ensure everything is cleaned up by this point, turn on TRACE logging and see that
     * (at least the major classes') destructors for all objects have logged that they are being
     * destroyed.  Be especially careful with any classes that we work with via shared_ptrs
     * (Peer_socket and others). */

    // Let go of low-level net-stack resources.
    FLOW_LOG_TRACE("Closing low-level UDP socket.");

    m_low_lvl_sock.non_blocking(false); // Just in case closing operation blocks for some reason....
    m_low_lvl_sock.close(); // Give socket back to OS (UDP close()).
  } // try
  catch (const system_error& exc)
  {
    const Error_code sys_err_code = exc.code();
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();
    FLOW_LOG_WARNING("Could not cleanly shutdown, but still shut down.");
  }
} // Node::worker_run()

const util::Udp_endpoint& Node::local_low_lvl_endpoint() const
{
  // We are in thread U != W, but that's OK (m_low_lvl_local_endpoint cannot change after constructor).
  return m_low_lvl_endpoint;
}

void Node::perform_accumulated_on_recv_tasks()
{
  // We are in thread W.

  /* We're being executed from the end of low_lvl_recv_and_handle() or
   * async part of async_wait_latency_then_handle_incoming().
   * Thus we must perform tasks, accumulated over the course of that call for efficiency, now. */

  // Handle per-socket tasks.

  // Handle accumulated incoming acknowledgments/rcv_wnd updates.
  for (Peer_socket::Ptr sock : m_socks_with_accumulated_acks)
  {
    handle_accumulated_acks(socket_id(sock), sock);
  }
  // Clean slate for the next receive handler invocation.
  m_socks_with_accumulated_acks.clear();

  // Handle accumulated outgoing acknowledgments.
  for (Peer_socket::Ptr sock : m_socks_with_accumulated_pending_acks)
  {
    handle_accumulated_pending_acks(socket_id(sock), sock);
  }
  // Clean slate for the next receive handler invocation.
  m_socks_with_accumulated_pending_acks.clear();

  /* Have read all available low-level data.  As a result m_sock_events may have elements (various
   * events that have occurred, like a socket becoming Readable).  Therefore a "delta" check of
   * all Event_sets is needed, so that the user is informed of any events that he's been waiting
   * on that have occurred.  (See Event_set::async_wait() for details and general strategy.)
   *
   * Pass "false" to event_set_check_delta(), meaning "you must in fact check the Event_sets
   * instead of deferring until later."  Could we defer here?  Not really.  If events have
   * occurred, we have a "non-blocking" amount of time since then during which we must inform the
   * user of the events that have occurred.  Doing so within one receive loop of detecting the
   * events meets this criterion (since boost.asio is not given a chance to sleep -- we haven't
   * exited the handler).  If we do exit the handler, boost.asio can sleep (until the next timer
   * or more UDP data arrive, for example), which certainly breaks the criterion.  So we must do
   * it no later than now.
   *
   * Do this AFTER the above, because handle_accumulated_*() may have added more events to handle
   * here (but event_set_all_check_delta() cannot add more tasks for the former to perform). */
  event_set_all_check_delta(false);
} // Node::perform_accumulated_on_recv_tasks()

bool Node::running() const
{
  // Simply check if the event loop is waiting for events vs. ran out of work (failed).
  return !m_task_engine.stopped();
}

void Node::handle_incoming(util::Blob* packet_data,
                           const util::Udp_endpoint& low_lvl_remote_endpoint)
{
  using boost::static_pointer_cast;
  using boost::dynamic_pointer_cast;
  using boost::shared_ptr;
  using boost::asio::const_buffer;
  using std::type_index;

  // We are in thread W.

  // Save before *packet_data potentially annihilated.
  const size_t packet_data_size = packet_data->size();

  /* `packet_data` contains packet binary data.  Deserialize it into a structure and handle it.
   *
   * Discussion of 2nd argument (m_dyn_guarantee_one_low_lvl_in_buf_per_socket option):
   * For simplicity, suppose only DATA packets exist (they are the biggest, easily, in practice).
   * Further suppose that m_dyn_low_lvl_max_packet_size = 32k, m_st_max_block_size = 1k.
   *
   * If the arg is set to true, then the low-level UDP input code will allocate a 32k buffer exactly once per Node;
   * read up to 1k (+header overhead) into it.  Then DATA deserializer will copy the sub-sequence within that
   * up-to-1k-ish prefix of the 32k buffer that stores the actual DATA payload (which will be vast majority of
   * that up-to-1k) into a newly allocated buffer for that packet.  (This copy will then be moved around, to the
   * extent possible, eventually ending up [probably copied, ultimately] in the user's data structure of choice,
   * presumably first living in Receive buffer for the appropriate socket.)  Then for the next incoming packet,
   * the same original 32k buffer will be reused to store the deserialized data again; rinse/repeat.
   * So the total comes to: 1 32k allocation and deallocation; N up-to-1k allocations and deallocations; and
   * N up-to-1k copies; where N is # of DATA packets received (could be huge).
   *
   * If the arg is set to false, then the low-level UDP input code will allocate a 32k buffer once per DATA packet;
   * read up to 1k (+header overhead) into it.  Then DATA deserializer will MOVE the entire up-to-32k buffer including
   * up-to-1k-ish prefix within it that stores the actual DATA payload (which will be vast majority of
   * that up-to-1k) into the place where deserialized data are stored.  (This buffer (and information about which part
   * of it the actual DATA payload) will then be moved around, to the extent possible, eventually ending up [probably
   * copied, ultimately] in the user's data structure of choice, presumably first living in Receive buffer for the
   * appropriate socket.)  Then for the next incoming packet, the low-level UDP input code will have to reallocate
   * a new 32k buffer, because the deserializer "took" it for its own purpose (without copying it, thinking that's
   * faster, which it is -- in that limited context), so it cannot be reused; rinse/repeat.
   * So the total comes to: N 32k allocations and deallocations; but no copies; and N (quite cheap) moves; where
   * N is # of DATA packets received (could be huge).
   *
   * What is faster?  In practice (assuming a CPU-limited environment, meaning no loss, no delay, and a constant
   * stream of data being sent and received), at least on a localhost Mac setup, it turns out that the `true` way
   * is a bit faster, in that throughput is consistently higher by ~5%.  (This should be taken with grain of salt:
   * improvements in other areas of the code could make this impact become higher in proportion, etc. etc.)
   * This isn't too surprising; N-bytes alloc/copy/dealloc is bad, but a 32x-bigger alloca/dealloc can certainly
   * be worse.
   *
   * However, suppose we lower knob for "32k" to ~1k.  Then we have, essentially:
   * true => N 1k x alloc/copy/dealloc;
   * false => N 1k alloc/dealloc.
   *
   * In this case `false` might be somewhat better.  The good thing is that, as of this writing, all these values
   * have option knobs.  Of course, all this is subject to change and/or become automated over time.
   *
   * A huge caveat about the above discussion: Not all of the allocs/copies/deallocs occur in the same thread,
   * so in a multi-core environment, throughput might be improved by parallelism vs. the implications of above text.
   * Above could be rewritten to account for this; but at the moment the above should at least elucidate the issues
   * involved sufficiently for the reader to reason about this on their own. */
  Low_lvl_packet::Ptr packet
    = Low_lvl_packet::create_from_raw_data_packet(get_logger(), packet_data,
                                                  opt(m_opts.m_dyn_guarantee_one_low_lvl_in_buf_per_socket));
  // ^-- That logged as much or as little as appropriate (we need not).  null if error. `packet_data` may be blown away.

  if (!packet)
  {
    return; // As advertised, caller is free to do anything they want to *packet_data now (e.g., read another packet).
  }

  const auto& packet_ref = *packet;

  /* Preliminary notes for the rest of this important method:
   * - Invalid fields:  If some field in the packet is simply erroneous, such as
   *   a negative number where a positive number is expected or something, the packet is
   *   simply dropped.  No RST is sent and no error is reported to the user (other than a log
   *   message).  This is similar to what at least the Zaghal/Khan FSM model for TCP describes, and
   *   it seems reasonable (and easy).  This only applies to fields that could never be right in any
   *   situation.
   * - Other errors:  More likely are other error situations, such as receiving a SYN to a socket
   *   that's in SYN_SENT state.  Since such problems can be a result of delayed packets or other
   *   conditions, there are various possibilities for response.  Examples:
   *   - Example: Getting SYN in SYN_SENT: always illegal but could happen due to delayed packets or
   *     freak coincidences; send RST and close connection.
   *   - Example: Getting SYN on a non-listening port: common error situation; send RST.
   *   - Example: Getting SYN_ACK twice: duplicate packet or result of loss; send SYN_ACK_ACK
   *     again and continue normally.
   *   - Example: Getting RST in ESTABLISHED: other side is resetting connection.  Close connection;
   *     don't respond with RST. */

  // Sanity-check basic fields.

  // Sanity-check source and destination Flow ports.
  const flow_port_t flow_local_port = packet->m_packed.m_dst_port;
  const flow_port_t flow_remote_port = packet->m_packed.m_src_port;
  if (flow_remote_port == S_PORT_ANY)
  {
    FLOW_LOG_WARNING("Invalid src_port value [" << S_PORT_ANY << "] from [UDP " << low_lvl_remote_endpoint << "].  "
                     "Dropping packet.");
    return;
  }
  // else
  if (flow_local_port == S_PORT_ANY)
  {
    FLOW_LOG_WARNING("Invalid dst_port value [" << S_PORT_ANY << "] from [UDP " << low_lvl_remote_endpoint << "].  "
                     "Dropping packet.");
    return;
  }

  // Sanity-check data size.  Should be guaranteed during deserialization.
  assert((typeid(packet_ref) != typeid(Data_packet)) ||
         (!(static_pointer_cast<const Data_packet>(packet)->m_data.empty())));

  // Demultiplex to proper destination socket; and handle according to state machine.

  /* Check if destination port is to a peer-to-peer socket.
   * It's important to check for this before checking whether the port matches a listening server
   * socket.  Example: server S listens on port 80 (so m_servs[80] exists); client C at remote
   * endpoint R connects to S:80 (so now m_socks[R, 80] also exists); then client C sends a data
   * packet to S:80.  Should this go to S:[R, 80] or S:80?  Clearly the former.
   *
   * The only caveat is: what if client C1 also wants to now connect S:80?  With this algorithm this
   * will go to S:80, which is good.  However what if C1's address is C1:R?  Then that SYN would
   * erroneously go to the existing peer-to-peer socket S:[R, 80].  However that means somehow 2
   * sockets bound to the same port and thus the same remote endpoint which shouldn't happen. */
  const Socket_id socket_id{ { low_lvl_remote_endpoint, flow_remote_port }, flow_local_port };
  Socket_id_to_socket_map::const_iterator sock_entry = m_socks.find(socket_id);
  if (sock_entry != m_socks.end())
  {
    // Successful demultiplex to a peer socket.
    Peer_socket::Ptr sock = sock_entry->second;

    /* Record packet size, type in stats of the socket, now that we know the latter.  (What if sock unknown?
     * See below.) */
    sock->m_rcv_stats.low_lvl_packet(typeid(packet_ref), packet_data_size);

    // state cannot change unless this thread W changes it, so no need to lock object across the below code.
    const Peer_socket::Int_state state = sock->m_int_state;

    /* A little bit of extra validation.  Could not check against max_block_size before
     * demuxing the socket, as it's a per-socket option. */
    const size_t max_block_size = sock->max_block_size();

    {
      shared_ptr<Data_packet> data;
      shared_ptr<Ack_packet> ack;
      shared_ptr<Syn_packet> syn;
      shared_ptr<Syn_ack_packet> syn_ack;

      /* Perform any tweaks and checks based on the new (for this packet) knowledge that is the identity of `sock`.
       *
       * One tweak in particular that applies to multiple packet types as done below:
       * Every Sequence_number stored therein can be more conveniently logged as relative to the ISN (so 0, 1, etc.
       * instead of some huge 64-bit thing) as well as a multiple of max-block-size (so 2x8192, 3x8192, etc.; as opposed
       * to the actual results of those products).  Now that we know `sock`, we can apply those values from it
       * to the known Sequence_number members, so that they are logged nicely instead of 64-bit monstrosities.
       * Nuance: you'll note (ISN + 1) popping up repeatedly; the `+ 1` is explained in class Sequence_number doc header
       * near *Metadata* discussion. */
      if ((data = dynamic_pointer_cast<Data_packet>(packet)))
      {
        // If it IS a DATA packet, and its contained data field is too large....
        if (data->m_data.size() > max_block_size)
        {
          FLOW_LOG_WARNING("Packet of type [" << packet->m_type_ostream_manip << "] targeted at [" << sock << "] in "
                           "state [" << state << "] has data size [" << data->m_data.size() << "] exceeding "
                           "maximum block size [" << max_block_size << "].  Dropping packet.");
          return;
        }

        data->m_seq_num.set_metadata('R', sock->m_rcv_init_seq_num + 1, max_block_size);
      }
      else if ((ack = dynamic_pointer_cast<Ack_packet>(packet)))
      {
        for (const Ack_packet::Individual_ack::Ptr& individual_ack : ack->m_rcv_acked_packets)
        {
          individual_ack->m_seq_num.set_metadata('L', sock->m_snd_init_seq_num + 1, max_block_size);
        }
      }
      else if ((syn = dynamic_pointer_cast<Syn_packet>(packet)))
      {
        syn->m_init_seq_num.set_metadata('R', syn->m_init_seq_num + 1, max_block_size);
      }
      else if ((syn_ack = dynamic_pointer_cast<Syn_ack_packet>(packet)))
      {
        syn_ack->m_init_seq_num.set_metadata('R', syn_ack->m_init_seq_num + 1, max_block_size);
      }
    }

    // else

    bool reply_with_rst = false;
    Error_code err_code; // Initialize to success.

    switch (state)
    {
    case Peer_socket::Int_state::S_SYN_SENT:
    {
      // We're in SYN_SENT state: awaiting SYN_ACK.
      if (typeid(packet_ref) == typeid(Syn_packet)) // To SYN_SENT state.
      {
        /* Replying to SYN with SYN is not allowed.  We don't support simultaneous active opens.
         * (RFC 793 does, but it's not a mainstream situation.)  In fact one would have to perform
         * Node::connect() while binding to a service port, which we do not allow.
         *
         * Another possibility is that this SYN is from some previous connection with the same
         * socket pair, and we should disregard (no RST) it via sequence number checking.  For now
         * we assume our port reservation scheme on both ends eliminates socket pair reuse in this
         * way and thus don't check for this.
         *
         * So, other side is misbehaving.  Send RST and close connection. */
        err_code =  error::Code::S_CONN_RESET_BAD_PEER_BEHAVIOR;
        reply_with_rst = true;
      }
      else if (typeid(packet_ref) == typeid(Syn_ack_ack_packet)) // To SYN_SENT state.
      {
        // Similar to SYN.  We should be sending SYN_ACK_ACK (later) -- not they.
        err_code = error::Code::S_CONN_RESET_BAD_PEER_BEHAVIOR;
        reply_with_rst = true;
      }
      else if (typeid(packet_ref) == typeid(Data_packet)) // To SYN_SENT state.
      {
        // Similar to SYN.  Should not be getting DATA yet -- must complete handshake first.
        err_code = error::Code::S_CONN_RESET_BAD_PEER_BEHAVIOR;
        reply_with_rst = true;
      }
      else if (typeid(packet_ref) == typeid(Ack_packet)) // To SYN_SENT state.
      {
        // Similar to DATA.
        err_code = error::Code::S_CONN_RESET_BAD_PEER_BEHAVIOR;
        reply_with_rst = true;
      }
      else if (typeid(packet_ref) == typeid(Rst_packet)) // To SYN_SENT state.
      {
        /* Sent SYN; received RST.  Could be common: other side may simply not be listening on the
         * port to which we wanted to connect.  Close connection but don't reply to RST with RST
         * (not useful; could cause endless RSTing). */
        err_code = error::Code::S_CONN_REFUSED; // To SYN_SENT state.
      }
      else if (typeid(packet_ref) == typeid(Syn_ack_packet)) // To SYN_SENT state.
      {
        // Sent SYN; received SYN_ACK.  Great.
        handle_syn_ack_to_syn_sent(socket_id,
                                   sock,
                                   static_pointer_cast<const Syn_ack_packet>(packet));
        break;
      }
      else
      {
        assert(false); // We've eliminated this possibility already above.
      }
      break;
    } // case Peer_socket::Int_state::S_SYN_SENT:

    case Peer_socket::Int_state::S_SYN_RCVD:
    {
      // We're in SYN_RCVD state: awaiting SYN_ACK_ACK.
      if (typeid(packet_ref) == typeid(Syn_packet)) // To SYN_RCVD state.
      {
        /* Replying to SYN_ACK with SYN is not allowed.  However it could be be a duplicated
         * original SYN (ha ha), in which case it's not an error and is dropped silently.  Otherwise
         * it's an error.
         *
         * Another possibility is that this SYN is from some previous connection with the same
         * socket pair, and we should disregard (no RST) it via sequence number checking.  For now
         * we assume our port reservation scheme on both ends eliminates socket pair reuse in this
         * way and thus don't check for this.
         *
         * So, decide if other side is misbehaving.  If so, send RST and close connection.
         * Otherwise ignore. */
        if (static_pointer_cast<const Syn_packet>(packet)->m_init_seq_num == sock->m_rcv_init_seq_num)
        {
          // Should be plenty of context if TRACE logging enabled anyway.
          FLOW_LOG_TRACE("Duplicate valid [" << packet->m_type_ostream_manip << "] packet; ignoring.");
        }
        else
        {
          err_code =  error::Code::S_CONN_RESET_BAD_PEER_BEHAVIOR;
          reply_with_rst = true;
        }
      }
      else if (typeid(packet_ref) == typeid(Syn_ack_packet)) // To SYN_RCVD state.
      {
        // SYN_ACK in response to SYN_ACK is always wrong.
        err_code = error::Code::S_CONN_RESET_BAD_PEER_BEHAVIOR;
        reply_with_rst = true;
        break;
      }
      else if (typeid(packet_ref) == typeid(Data_packet)) // To SYN_RCVD state.
      {
        /* This is legitimate under loss conditions.  Suppose we are SYN_RCVD, send SYN_ACK, they
         * receive SYN_ACK, so they go SYN_SENT -> ESTABLISHED, and they send SYN_ACK_ACK.  Now
         * suppose that SYN_ACK_ACK was dropped.  Now say they send DATA (which they have every
         * right do to: their state is ESTABLISHED).  So we've received DATA, but we're still in
         * SYN_RCVD.  It can also happen if DATA and SYN_ACK_ACK are re-ordered by the network. */
        handle_data_to_syn_rcvd(sock, static_pointer_cast<Data_packet>(packet));
      }
      else if (typeid(packet_ref) == typeid(Ack_packet)) // To SYN_RCVD state.
      {
        /* On the other hand, since we are not yet ESTABLISHED, we couldn't have sent DATA, so we
         * shouldn't be getting any ACKs. */
        err_code = error::Code::S_CONN_RESET_BAD_PEER_BEHAVIOR;
        reply_with_rst = true;
      }
      else if (typeid(packet_ref) == typeid(Rst_packet)) // To SYN_RCVD state.
      {
        /* Received SYN; sent SYN_ACK; received RST.  Shouldn't be common, but they are refusing
         * connection at this late stage for some reason.
         *
         * Close connection but don't reply to RST with RST (not useful; could cause endless
         * RSTing). */
        err_code = error::Code::S_CONN_REFUSED;
      }
      else if (typeid(packet_ref) == typeid(Syn_ack_ack_packet)) // To SYN_RCVD state.
      {
        // Sent SYN_ACK; received SYN_ACK_ACK.  Great.
        handle_syn_ack_ack_to_syn_rcvd(socket_id, sock, static_pointer_cast<const Syn_ack_ack_packet>(packet));
      }
      else
      {
        assert(false); // We've eliminated this possibility already above.
      }
      break;
    } // case Peer_socket::Int_state::S_SYN_RCVD:

    case Peer_socket::Int_state::S_ESTABLISHED:
    {
      /* We're in ESTABLISHED state: will accept DATA and ACKs, etc.  However just because we've
       * ESTABLISHED does not mean they have.  Specifically, suppose we were SYN_SENT, got SYN_ACK,
       * sent SYN_ACK_ACK, so transferred to ESTABLISHED.  Now suppose that SYN_ACK_ACK was dropped.
       * We're ESTABLISHED, but they're going to resend SYN_ACK due to retransmission timer.
       * So we must be ready for this resent SYN_ACK, not just DATA and such. */
      if (typeid(packet_ref) == typeid(Syn_packet)) // To ESTABLISHED state.
      {
        /* SYN while in ESTABLISHED is not allowed.  However it could be a duplicated original SYN
         * (ha ha), in which case it's not an error and is dropped silently.  Otherwise it's an
         * error.
         *
         * Another possibility is that this SYN is from some previous connection with the same
         * socket pair, and we should disregard (no RST) it via sequence number checking.  For now
         * we assume our port reservation scheme on both ends eliminates socket pair reuse in this
         * way and thus don't check for this.
         *
         * So, decide if other side is misbehaving.  If so, send RST and close connection.
         * Otherwise ignore. */
        if (static_pointer_cast<const Syn_packet>(packet)->m_init_seq_num == sock->m_rcv_init_seq_num)
        {
          // Should be plenty of context if TRACE logging enabled anyway.
          FLOW_LOG_TRACE("Duplicate valid [" << packet->m_type_ostream_manip << "] packet; ignoring.");
        }
        else
        {
          err_code = error::Code::S_CONN_RESET_BAD_PEER_BEHAVIOR;
          reply_with_rst = true;
        }
        break;
      } // if (SYN)
      else if (typeid(packet_ref) == typeid(Syn_ack_packet)) // To ESTABLISHED state.
      {
        /* This could be the corner case above (we are original SYNner, but our SYN_ACK_ACK was
         * dropped, so they're resending SYN_ACK, which they think may have been lost).  It could
         * also just be a duplicate SYN_ACK.  Suppose we respond with SYN_ACK_ACK as before.  If
         * they're resending SYN_ACK due to loss, then this is clearly proper; if they get
         * SYN_ACK_ACK this time, they're good to go.  If it's just a duplicate (they're already
         * ESTABLISHED), then they'll similarly just accept SYN_ACK_ACK as a harmless duplicate.
         * So, that's fine.
         *
         * As usual it can also be an invalid packet, in which case we decide the other side is
         * misbehaving and RST/close connection. */

        auto syn_ack = static_pointer_cast<const Syn_ack_packet>(packet);

        if (sock->m_active_connect && (syn_ack->m_init_seq_num == sock->m_rcv_init_seq_num))
        {
          handle_syn_ack_to_established(sock, syn_ack);
        }
        else
        {
          err_code = error::Code::S_CONN_RESET_BAD_PEER_BEHAVIOR;
          reply_with_rst = true;
        }
      }
      else if (typeid(packet_ref) == typeid(Syn_ack_ack_packet)) // To ESTABLISHED state.
      {
        /* We're ESTABLISHED but got SYN_ACK_ACK.  Suppose we were the active connector.  Then
         * SYN_ACK_ACK is simply illegal at all times.  Now suppose we were the passive connectee.
         * To get to ESTABLISHED, we had to have received SYN_ACK_ACK already.  Therefore this
         * must be a duplicate SYN_ACK_ACK (assuming it equals the original SYN_ACK_ACK; otherwise
         * it's just invalid).
         *
         * If it's a duplicate SYN_ACK_ACK, we just drop it as harmless.  Otherwise the other guy
         * is misbehaving, so we RST/close. */
        if ((!sock->m_active_connect) &&
            (static_pointer_cast<const Syn_ack_ack_packet>(packet)->m_packed.m_security_token
               == sock->m_security_token))
        {
          // Should be plenty of context if TRACE logging enabled anyway.
          FLOW_LOG_TRACE("Duplicate valid [" << packet->m_type_ostream_manip << "] packet; ignoring.");
        }
        else
        {
          err_code = error::Code::S_CONN_RESET_BAD_PEER_BEHAVIOR;
          reply_with_rst = true;
        }
      }
      else if (typeid(packet_ref) == typeid(Rst_packet)) // To ESTABLISHED state.
      {
        /* Shouldn't be common, but they are resetting connection for some reason.
         *
         * Close connection but don't reply to RST with RST (not useful; could cause endless
         * RSTing). */
        err_code = error::Code::S_CONN_RESET_BY_OTHER_SIDE;
      }
      else if (typeid(packet_ref) == typeid(Data_packet)) // To ESTABLISHED state.
      {
        // Got DATA!  Great.
        handle_data_to_established(socket_id, sock,
                                   static_pointer_cast<Data_packet>(packet),
                                   false); // false <=> packet received in ESTABLISHED.
      }
      else if (typeid(packet_ref) == typeid(Ack_packet)) // To ESTABLISHED state.
      {
        // Got ACK!  Great.
        handle_ack_to_established(sock, static_pointer_cast<const Ack_packet>(packet));
      }
      else
      {
        assert(false); // We've eliminated this already above.
      }
      break;
    } // case Peer_socket::Int_state::S_ESTABLISHED:
    case Peer_socket::Int_state::S_CLOSED:
      assert(false); // Can't be CLOSED, since it's in m_socks.
    } // switch (state)

    // If above found we need to close this connection and/or send RST, do that.
    if (err_code)
    {
      if (reply_with_rst)
      {
        FLOW_LOG_WARNING("Packet of type [" << packet->m_type_ostream_manip << "] targeted at [" << sock << "] in "
                         "state [" << state << "]; replying with RST.");
        async_sock_low_lvl_rst_send(sock);
      }
      else
      {
        FLOW_LOG_WARNING("Packet of type [" << packet->m_type_ostream_manip << "] targeted at [" << sock << "] in "
                         "state [" << state << "]; dropping without reply.");
      }

      /* Close connection in our structures (inform user if necessary as well).  Pre-conditions
       * assumed by call: sock in m_socks and sock->state() == S_OPEN (yes, since sock is m_socks);
       * err_code contains the reason for the close (yes).  This will empty the Send and Receive
       * buffers (if applicable, i.e., only if state is ESTABLISHED or later).  That is OK,
       * because this is the abrupt type of close (error). */
      close_connection_immediately(socket_id, sock, err_code, true);
      /* ^-- defer_delta_check == true: because the only way to get to this method is from
       * async_low_lvl_recv(), which will perform event_set_all_check_delta(false) at the end of itself,
       * before the boost.asio handler exits.  See Node::m_sock_events doc header for details. */
    } // if (error)

    return;
  } // if (destination port is to a peer socket.)
  // else destination port is not to a peer socket.

  // Check if destination port is to a server socket.
  Port_to_server_map::const_iterator serv_entry = m_servs.find(flow_local_port);
  if (serv_entry != m_servs.end())
  {
    // Successful demultiplex to a server socket.
    Server_socket::Ptr serv = serv_entry->second;

    /* Server socket can be in the following states:
     * - S_LISTENING: Waiting for SYNs.  Anything except a SYN is an error and yields an RST (except
     *   an RST does not yield an RST, as that's not useful and could lead to endless RSTing).
     * - S_CLOSING: No longer listening (in the process of being removed from Node).  Anything
     *   except an RST yields an RST (interepreted by other side as: connection refused [if SYN] or
     *   invalid message).
     *
     * S_CLOSED means it's not in m_serv, so that's not a possible state. */

    // state cannot change unless this thread W changes it, so no need to lock object across the below code.
    const Server_socket::State state = serv->state();
    if ((state == Server_socket::State::S_LISTENING) &&
        (typeid(packet_ref) == typeid(Syn_packet)))
    {
      auto new_sock = handle_syn_to_listening_server(serv,
                                                     static_pointer_cast<const Syn_packet>(packet),
                                                     low_lvl_remote_endpoint);
      if (new_sock)
      {
        /* Record packet size, type in stats of the socket, now that we know the latter.  It's kind of interesting,
         * as new_sock (as name implies) was just created based on the very packet we are registering here.
         * If it's a packet not corresponding to any Peer_socket, it won't be counted against any Peer_socket's stats
         * (but that is an unusual case; though there is a to-do [as of this writing] for counting things Node-wide
         * as well). */
        new_sock->m_rcv_stats.low_lvl_packet(typeid(packet_ref), packet_data_size);
      }
      return;
    }
    // else
    if (typeid(packet_ref) != typeid(Rst_packet))
    {
      FLOW_LOG_WARNING("Packet of type [" << packet->m_type_ostream_manip << "] from "
                       "[UDP " << low_lvl_remote_endpoint << "] targeted at state [" << state << "] "
                       "server port [" << flow_local_port << "]; replying with RST.");
      async_no_sock_low_lvl_rst_send(packet, low_lvl_remote_endpoint);
    }
    else
    {
      FLOW_LOG_WARNING("Packet of type [" << packet->m_type_ostream_manip << "] from "
                       "[UDP " << low_lvl_remote_endpoint << "] targeted at state [" << state << "] "
                       "server port [" << flow_local_port << "]; dropping without reply.");
    }
    return;
  } // if (demuxed to server socket)
  // else destination port is not to a peer-to-peer socket or a server socket.

  // Handle NOT_A_SOCKET state.

  /* Destination socket is not a socket.  In this degenerate state any incoming packets are
   * rejected.  Help the other side by issuing an RST.  In particular they may be trying to SYN a
   * non-listening socket, for example, and the RST will prevent them from waiting around before
   * giving up.
   *
   * Exception: Don't issue an RST to an RST, as that's not useful (and could result in endless
   * RSTing, maybe). */
  if (typeid(packet_ref) != typeid(Rst_packet))
  {
    FLOW_LOG_WARNING("Packet from [UDP " << low_lvl_remote_endpoint << "] specified state [NOT_A_SOCKET] "
                     "port [" << flow_local_port << "]; meaning no such socket known; replying with RST.");
    async_no_sock_low_lvl_rst_send(packet, low_lvl_remote_endpoint);
  }
  else
  {
    FLOW_LOG_WARNING("Packet of type [" << packet->m_type_ostream_manip << "] from "
                     "[UDP " << low_lvl_remote_endpoint << "] specified state [NOT_A_SOCKET] "
                     "port [" << flow_local_port << "]; meaning no such socket known; dropping without reply.");
  }
} // Node::handle_incoming()

/// @cond
/* -^- Doxygen, please ignore the following.  (Don't want docs generated for temp macro; this is more maintainable
 * than specifying the macro name to omit it, in Doxygen-config EXCLUDE_SYMBOLS.) */

/* Normaly I try to avoid macro cleverness, but in this case to get a nice printout we need the
 * # technique, and also this eliminates quite a bit of repetition.  So let's.... */
#define VALIDATE_STATIC_OPTION(ARG_opt) \
  validate_static_option(opts.ARG_opt, m_opts.ARG_opt, #ARG_opt, err_code)
#define VALIDATE_CHECK(ARG_check) \
  validate_option_check(ARG_check, #ARG_check, err_code)

// -v- Doxygen, please stop ignoring.
/// @endcond

const Node_options& Node::validate_options(const Node_options& opts, bool init, Error_code* err_code) const
{
  // Note: Can't use FLOW_ERROR_EXEC_AND_THROW_ON_ERROR() as return is a reference.  @todo Look into solution?
  if (!err_code) // Call the non-null err_code version of ourselves, and throw exception on error.
  {
    Error_code our_err_code;
    const Node_options& result = validate_options(opts, init, &our_err_code);
    if (our_err_code)
    {
      throw flow::error::Runtime_error{our_err_code, FLOW_UTIL_WHERE_AM_I_STR()};
    }
    return result;
  }
  // else

  /* We are to validate the given set of global option values.  If init, then the context is that an
   * already-running Node is being called with set_options(), i.e. user is modifying options for an
   * existing Node.  In that case we must ensure that no static (unchangeable) option's value
   * would be changed by this.
   *
   * If not init, then we're validating the options set given to us in Node constructor.  So in
   * that case that static option check is to be skipped, since there are no existing option values
   * to change.
   *
   * Finally, we must check for individual integrity of the specified values (including consistency
   * with other option values). */

  // We are in thread U != W or in thread W.

  if (!init)
  {
    /* As explained above, they're trying to change an existing Node's option values.  Ensure all
     * the static options' values are the same in opts and m_opts. */

    // Explicitly documented pre-condition is that m_opts is already locked if necessary.  So don't lock.

    const bool static_ok
      = VALIDATE_STATIC_OPTION(m_st_capture_interrupt_signals_internally) &&
        VALIDATE_STATIC_OPTION(m_st_low_lvl_max_buf_size) &&
        VALIDATE_STATIC_OPTION(m_st_timer_min_period);

    if (!static_ok)
    {
      // validate_static_option() has set *err_code.
      return opts;
    }
    // else
  } // if (!init)

  // Now sanity-check the values themselves.  @todo Comment and reconsider these?

  const bool checks_ok
    = VALIDATE_CHECK(opts.m_st_low_lvl_max_buf_size >= 128 * 1024) &&
      VALIDATE_CHECK(opts.m_st_timer_min_period.count() >= 0) &&
      VALIDATE_CHECK(opts.m_dyn_low_lvl_max_packet_size >= 512);

  if (!checks_ok)
  {
    // On error, validate_option_check() has set *err_code.
    return opts;
  }
  // else

  /* The above validated only global options.  Now verify that the per-socket template options (that
   * will be used to generate child Peer_sockets) are also valid. */
  sock_validate_options(opts.m_dyn_sock_opts, nullptr, err_code); // Will not throw.  Will set *err_code if needed.
  // On error, that set *err_code.

  return opts;

#undef VALIDATE_CHECK
#undef VALIDATE_STATIC_OPTION
} // Node::validate_options()

bool Node::set_options(const Node_options& opts, Error_code* err_code)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, set_options, opts, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in thread U != W.

  if (!running())
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_NODE_NOT_RUNNING);
    return Peer_socket::Ptr{}.get();
  }
  // else

  /* We just want to replace m_opts with a copy of opts.  First validate opts (including with
   * respect to m_opts, and also check for invalid values and such), then copy it over. */

  // Log new options values.  A bit computationally expensive so just use TRACE for now.  @todo Reconsider?
  FLOW_LOG_TRACE("\n\n" << opts);

  // Will be writing if all goes well, so must acquire exclusive ownership of m_opts.
  Options_lock lock{m_opts_mutex};

  /* Validate the new option set (including ensuring they're not changing static options' values).
   * Note that an explicit pre-condition of this method is that m_opts_mutex is locked if needed,
   * hence the above locking statement is not below this call. */
  validate_options(opts, false, err_code);
  if (*err_code)
  {
    return false;
  }
  // else

  // Boo-ya.
  m_opts = opts;
  return true;
} // Node::set_options()

bool Node::validate_option_check(bool check, const std::string& check_str,
                                 Error_code* err_code) const
{
  if (!check)
  {
    FLOW_LOG_WARNING("When changing options, check [" << check_str << "] is false.  "
                     "Ignoring entire option set.");
    FLOW_ERROR_EMIT_ERROR(error::Code::S_OPTION_CHECK_FAILED);
    return false;
  }
  // else
  return true;
}

Node_options Node::options() const
{
  return opt(m_opts); // Lock, copy entire struct, unlock, return copy.
}

size_t Node::max_block_size() const
{
  return opt(m_opts.m_dyn_sock_opts.m_st_max_block_size);
}

void Node::perform_regular_infrequent_tasks(bool reschedule)
{
  using util::schedule_task_from_now;

  // We are in thread W.

  // The frequency is low enough to where we can use detailed logging with impunity in this method.

  FLOW_LOG_INFO("[=== Periodic state logging. ===");

  // Just log the stats for each Peer_socket.  @todo Add Server_socket, Event_set when they have stats; global state.

  for (const auto& id_and_sock : m_socks)
  {
    Peer_socket::Ptr sock = id_and_sock.second;
    sock_log_detail(sock);
  }

  FLOW_LOG_INFO("=== Periodic state logging. ===]");

  if (reschedule)
  {
    schedule_task_from_now(get_logger(), S_REGULAR_INFREQUENT_TASKS_PERIOD, true, &m_task_engine,
                           [this](bool)
    {
      perform_regular_infrequent_tasks(true);
    });
  }
} // Node::perform_regular_infrequent_tasks()

size_t Node::Socket_id::hash() const
{
  using boost::hash_combine;

  size_t val = 0;
  hash_combine(val, m_remote_endpoint);
  hash_combine(val, m_local_port);
  return val;
}

bool operator==(const Node::Socket_id& lhs, const Node::Socket_id& rhs)
{
  return (lhs.m_local_port == rhs.m_local_port) && (lhs.m_remote_endpoint == rhs.m_remote_endpoint);
}

size_t hash_value(const Node::Socket_id& socket_id)
{
  return socket_id.hash();
}

} // namespace flow::net_flow
