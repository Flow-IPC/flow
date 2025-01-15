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

#include "flow/net_flow/node.hpp"
#include "flow/log/simple_ostream_logger.hpp"
#include "flow/log/async_file_logger.hpp"
#include "flow/util/string_view.hpp"
#include <boost/array.hpp>

/* This simple program is a Flow-based echo client.
 *   <executable> <times to send a message and expect reply> <host> <UDP port> <message contents>
 * It will connect to the given host/port (Flow port within that node is hard-coded and must match same
 * on server). It will send the message, wait for reply, sleep a few seconds, and repeate this the specified
 * number of times.
 *
 * This program, and its server counterpart, are a decent example of Flow basics applied to a somewhat
 * realistic small task. */
int main([[maybe_unused]] int argc, [[maybe_unused]] const char** argv)
{
  using flow::net_flow::flow_port_t;
  using flow::log::Simple_ostream_logger;
  using flow::log::Async_file_logger;
  using flow::log::Config;
  using flow::log::Sev;
  using flow::Flow_log_component;
  using flow::net_flow::Node;
  using flow::util::Udp_endpoint;
  using flow::Error_code;
  using flow::util::Ip_address_v4;
  using flow::net_flow::Peer_socket;
  using flow::net_flow::Remote_endpoint;
  using flow::error::Runtime_error;
  using String_view = flow::util::String_view;
  using boost::conversion::try_lexical_convert;
  using boost::asio::io_context;
  using resolver = boost::asio::ip::udp::resolver;
  using boost::chrono::seconds;
  using boost::asio::buffer;
  using boost::array;
  using std::ios_base;
  using std::exception;

  const int BAD_EXIT = 1;
  const size_t MAX_MSG_SIZE = 2000;
  const flow_port_t REMOTE_FLOW_PORT = 50;
  const boost::filesystem::path LOG_FILE = "flow_echo_cli.log";

  /* Set up logging within this function.  We could easily just use `cout` and `cerr` instead, but this
   * Flow stuff will give us time stamps and such for free, so why not?  Normally, one derives from
   * Log_context to do this very trivially, but we just have the one function, main(), so far so: */
  Config std_log_config;
  std_log_config.init_component_to_union_idx_mapping<Flow_log_component>(1000, 999);
  std_log_config.init_component_names<Flow_log_component>(flow::S_FLOW_LOG_COMPONENT_NAME_MAP, false, "cli-");

  Simple_ostream_logger std_logger(&std_log_config);
  FLOW_LOG_SET_CONTEXT(&std_logger, Flow_log_component::S_UNCAT);

  // This is separate: the Flow node's logging will go into this file. Just pass log_logger to flow::net_flow::Node.
  FLOW_LOG_INFO("Opening log file [" << LOG_FILE << "] for Flow logs only.");
  Config log_config = std_log_config;
  log_config.configure_default_verbosity(Sev::S_DATA, true);
  /* First arg: could use &std_logger to log-about-logging to console; but it's a bit heavy for such a console-dependent
   * little program.  Just just send it to /dev/null metaphorically speaking. */
  Async_file_logger log_logger(0, &log_config, LOG_FILE, false /* No rotation; we're no serious business. */);
  unsigned int n_times;

  if (((argc - 1) != 4) || String_view(argv[4]).empty() ||
      (!try_lexical_convert(argv[1], n_times)) || (n_times == 0))
  {
    FLOW_LOG_WARNING("Usage: " << argv[0] << " <times to send >= 1> <host> <port> <message to send>");
    return BAD_EXIT;
  }
  // else

  const String_view message(argv[4]);
  const size_t msg_size = message.size();
  if (msg_size >= MAX_MSG_SIZE)
  {
    FLOW_LOG_WARNING("Please keep message length at [" << msg_size << "] or lower.");
    return BAD_EXIT;
  }

  const String_view host_str(argv[2]);
  const String_view port_str(argv[3]);

  // Arguments parsed. Go.

  // For simplicity, choose the exception-throwing error handling from Flow. Do not pass in &Error_code.
  try
  {
    // Resolve the host/port strings (e.g., port can be a number or service string) to a UDP endpoint.
    Udp_endpoint remote_udp_endpoint;
    {
      io_context io;
      resolver res(io);
      Error_code ec;
      const auto result_it = res.resolve(host_str, port_str, ec);
      if (ec)
      {
        throw Runtime_error(ec, FLOW_UTIL_WHERE_AM_I_STR());
      }
      // else

      if (result_it.empty())
      {
        FLOW_LOG_WARNING("Could not resolve [" + host_str + ":" + port_str + "].");
        return BAD_EXIT;
      }

      // Essentially, this is just a resolved IP address and port number. Note this has nothing to do with Flow per se.
      remote_udp_endpoint = *result_it;
      FLOW_LOG_INFO("Resolved successfully: [" + host_str + ":" + port_str + "] => [" << remote_udp_endpoint << "].");
    }

    // Now put our transport endpoint on the IPADDR_ANY address (all interfaces), random ephemeral UDP port.
    Node node(&log_logger, Udp_endpoint(Ip_address_v4(), 0));

    /* Connect to the above-resolved UDP endpoint (host/port); within that, hard-coded Flow port REMOTE_FLOW_PORT.
     * Time out if unsuccessful after a while (throw exception; note that our user timeout may exceed lower-layer
     * Flow connection timeout, so it may throw earlier than our timeout). */
    Peer_socket::Ptr sock = node.sync_connect(Remote_endpoint{ remote_udp_endpoint, REMOTE_FLOW_PORT },
                                              seconds(45));

    // Send message/receive reply/sleep n_times times (except the final sleep).
    for (unsigned int n = 0; n != n_times; ++n)
    {
      // Note that timeout will not throw exception but will instead return 0.
      const size_t sent = sock->sync_send(buffer(message), seconds(5)); // Timeout (among others) will throw.
      assert(sent != 0);

      if (sent != msg_size)
      {
        /* Subtlety: To avoid this, MAX_MSG_SIZE must not exceed the send buffer's max size. This is typically
         * megabytes, though, so should be fine. But if it were to occur, that's still fine; a more resilient
         * program would keep trying to queue up bytes while progress was being made. This is a simple echo client,
         * though, so we just give up, as it really is rather odd for it to occur in this setting. */
        FLOW_LOG_WARNING("Send operation only processed [" << sent << "] of [" << msg_size << " bytes; "
                         "something must be wrong.");
        return BAD_EXIT;
      }

      FLOW_LOG_INFO("Send operation succeeded; sent all [" << sent << "] characters.");

      // Wait for reply.
      array<char, MAX_MSG_SIZE> buf_reply;
      const size_t rcvd = sock->sync_receive(buffer(buf_reply), seconds(5)); // Timeout (among others) will throw.
      assert(rcvd != 0);

      if (message.compare(0, message.size(), buf_reply.data(), rcvd) != 0)
      {
        FLOW_LOG_WARNING("Reply received, but it does not equal what we\'d sent; reply is "
                         "[" << String_view(buf_reply.data(), rcvd) << "].");
        return BAD_EXIT;
      }
      // else
      FLOW_LOG_INFO("Reply received; it equals what we\'d sent as expected.  Done!");

      if (n + 1 != n_times)
      {
        FLOW_LOG_INFO("Sleeping for a bit and going again.");
        flow::util::this_thread::sleep_for(seconds(4));
      }
    } // for (n in [0, n_times))
  }
  catch (const Runtime_error& exc)
  {
    FLOW_LOG_WARNING("Caught exception: [" << exc.what() << "].");
    return BAD_EXIT;
  }

  return 0;
} // main()
