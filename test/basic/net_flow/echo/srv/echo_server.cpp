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
#include <fstream>

/* This simple program is a Flow-based echo server.
 *   <executable> <UDP port> [localhost]
 * It will bind to UDP port on either localhost or all interfaces (depending on whether the localhost
 * argument is supplied). It will listen for echo client connections on hard-coded Flow port.
 * Each connection will spawn its own thread, where using blocking calls it will wait for a message, reply to it;
 * and repeat ad infinitum until the connection goes down. Connections will thus be handled simultaneously.
 *
 * This program, and its client counterpart, are a decent example of Flow basics applied to a somewhat
 * realistic small task. Note that servers are often not written using this thread-per-connection, block-each-thread
 * model; this is just an example where it is done that way. Other paradigms are certainly supported. */

/* This program is sligthly more complex than the client, involving a thread per connection, meaning that
 * there are at least two functions involved (main() and the thread body function). In order to Flow-log to
 * `cout` and `cerr` conveniently, the easiest way to go is have a simple class containing the functions
 * involved. Then actual `int main()` just invokes the class's main().
 *
 * We could easily just use `cout` and `cerr` instead, but this
 * Flow stuff will give us time stamps and such for free, so why not? Besides, it's nice to have the overall
 * program driver be written in an OO way, regardless, IMHO. */
class Main : public flow::log::Log_context, private boost::noncopyable
{
public:
  // Boring constructor.
  explicit Main();
  // The program body. Forward `int main()` here.
  int main(int argc, const char** argv);

private:
  // Helper: The thread body for the just-connected Flow socket `sock`.
  void on_client_connected(flow::net_flow::Peer_socket::Ptr sock);

  // The logger for our console output.
  flow::log::Config m_std_log_config;
  flow::log::Simple_ostream_logger m_logger;
};

int main(int argc, const char** argv)
{
  Main prog;
  return prog.main(argc, argv);
}

Main::Main() :
  // FLOW_LOG_...() from this class will go to m_logger, which logs to cout, cerr.
  Log_context(&m_logger, flow::Flow_log_component::S_UNCAT),
  m_logger(&m_std_log_config) // Initialize logger to cout, cerr.
{
  m_std_log_config.init_component_to_union_idx_mapping<flow::Flow_log_component>(1000, 999, true);
  m_std_log_config.init_component_names<flow::Flow_log_component>(flow::S_FLOW_LOG_COMPONENT_NAME_MAP, false, "srv-");

  // Name this main thread, to identify it in each log message logged from it.
  flow::log::Logger::this_thread_set_logged_nickname("echo_srv_main_native", get_logger());
}

int Main::main(int argc, const char** argv)
{
  using flow::net_flow::flow_port_t;
  using flow::log::Simple_ostream_logger;
  using flow::log::Async_file_logger;
  using flow::log::Config;
  using flow::log::Sev;
  using flow::net_flow::Node;
  using flow::net_flow::Node_options;
  using flow::util::Udp_endpoint;
  using flow::Error_code;
  using flow::util::Ip_address_v4;
  using flow::net_flow::Server_socket;
  using flow::net_flow::Peer_socket;
  using flow::net_flow::Remote_endpoint;
  using flow::error::Runtime_error;
  using String_view = flow::util::String_view;
  using boost::asio::io_context;
  using resolver = boost::asio::ip::udp::resolver;
  using boost::thread;
  using std::ofstream;
  using std::ios_base;
  using std::exception;

  const int BAD_EXIT = 1;
  const flow_port_t LOCAL_FLOW_PORT = 50; // Must match what's in client.cpp.
  const String_view LOCALHOST_TOKEN = "localhost";
  const boost::filesystem::path LOG_FILE = "flow_echo_srv.log";

  // Set up logging to file for Flow node. This is separate from our console logging.
  FLOW_LOG_INFO("Opening log file [" << LOG_FILE << "] for Flow logs only.");
  Config log_config = m_std_log_config;
  log_config.configure_default_verbosity(Sev::S_DATA, true);
  /* First arg: could use &m_logger to log-about-logging to console; but it's a bit heavy for such a console-dependent
   * little program.  Just just send it to /dev/null metaphorically speaking. */
  Async_file_logger log_logger(0, &log_config, LOG_FILE, true /* Hook up SIGHUP log rotation for fun. */);

  if ((argc == 1) || ((argc - 1) > 2) || (((argc - 1) == 2) && (argv[2] != LOCALHOST_TOKEN)))
  {
    FLOW_LOG_WARNING("Usage: " << argv[0] << " <port> [" << LOCALHOST_TOKEN << "]");
    return BAD_EXIT;
  }
  // else

  const String_view port_str(argv[1]);
  const bool bind_to_localhost = (argc - 1) == 2;

  // Argument parsing done. Go!

  // For simplicity, choose the exception-throwing error handling from Flow. Do not pass in &Error_code.
  try
  {
    // Resolve the host/port strings (e.g., port can be a number or service string) to a UDP endpoint.
    Udp_endpoint local_udp_endpoint;
    {
      const String_view host_str = bind_to_localhost ? "127.0.0.1" : "0.0.0.0";

      io_context io;
      resolver res(io);
      Error_code ec;
      const auto results = res.resolve(host_str, port_str, ec);
      if (ec)
      {
        throw Runtime_error(ec, FLOW_UTIL_WHERE_AM_I_STR());
      }
      // else

      if (results.empty())
      {
        FLOW_LOG_WARNING("Could not resolve [" << host_str << ':' << port_str << "].");
        return BAD_EXIT;
      }

      // Essentially, this is just a resolved IP address and port number. Note this has nothing to do with Flow per se.
      local_udp_endpoint = *(results.begin());
      FLOW_LOG_INFO("Resolved successfully: [" << host_str << ':' << port_str << "] "
                    "=> [" << local_udp_endpoint << "].");
    }

    // Now put our transport endpoint on selected interfaces, specific UDP port resolved above.
    Node_options opts;
    opts.m_st_capture_interrupt_signals_internally = true; // Be reasonably graceful on SIGTERM (throw exception, etc.).

    Node node(&log_logger, local_udp_endpoint, 0, 0, opts);
    // Within that, listen on hard-coded Flow port.
    Server_socket::Ptr serv = node.listen(LOCAL_FLOW_PORT);

    FLOW_LOG_INFO("Awaiting incoming connections.");

    // Simple block-per-connection-thread model. Each connection gets own, independent thread.
    for ( ; ; )
    {
      Peer_socket::Ptr sock = serv->sync_accept();
      assert(sock);

      thread(&Main::on_client_connected, this, sock); // Run this->on_client_connected(sock) as thread body.
    }
  }
  catch (const Runtime_error& exc)
  {
    FLOW_LOG_WARNING("Caught exception: [" << exc.what() << "].");
    return BAD_EXIT;
  }

  return 0;
}

void Main::on_client_connected(flow::net_flow::Peer_socket::Ptr sock)
{
  // Thread starts here.

  using flow::Error_code;
  using flow::error::Runtime_error;
  using std::string;
  using boost::array;
  using boost::asio::buffer;
  using boost::chrono::seconds;
  using boost::lexical_cast;
  using std::to_string;

  const size_t MAX_MSG_SIZE = 2000;

  Error_code err_code;

  array<char, MAX_MSG_SIZE> buf_rcvd;
  size_t rcvd;
  size_t sent;

  // Name this per-connection thread, to identify it in each log message logged from it.
  flow::log::Logger::this_thread_set_logged_nickname("echo_srv_conn_thread_["
                                                       + lexical_cast<string>(sock->remote_endpoint().m_udp_endpoint)
                                                       + "]_"
                                                       + to_string(sock->remote_endpoint().m_flow_port),
                                                     get_logger());

  do
  {
    // Unlike elsewhere, use error codes instead of exception to finely detect errors and handle them.
    rcvd = sock->sync_receive(buffer(buf_rcvd), seconds(30), &err_code);
    if (!err_code)
    {
      assert(rcvd != 0);
      /* Copy received buffer into this string. We are going to need a string to log it anyway, so might as well use
       * the same string as the reply payload buffer. (Or could use buf_rcvd for that; doesn't matter.) */
      const string rcvd_msg(buf_rcvd.data(), rcvd);
      FLOW_LOG_INFO("On Flow socket [" << sock << "]: received message [" << rcvd_msg << "].");

      sent = sock->sync_send(buffer(rcvd_msg), seconds(5), &err_code);
      if (!err_code)
      {
        assert(sent != 0);
        FLOW_LOG_INFO("Sent it back at them!");
      }
    }
  }
  while ((!err_code) && (sent == rcvd)); // Keep going while all keeps going well.

  // Loop had to exit for one of the reasons in the while() clause just above. Figure out which.
  if (err_code)
  {
    /* Either sync_receive() or sync_send() failed. Log the would-be exception's string encoding.
     * For example, this could be the connection-finished error code. */
    FLOW_LOG_INFO("Done because: [" << Runtime_error(err_code, FLOW_UTIL_WHERE_AM_I_STR()).what() << "].");
  }
  else
  {
    assert(sent != rcvd);
    // Received fine but somehow failed to send all of reply. Shouldn't happen (see discussion in client.cpp).
    FLOW_LOG_WARNING("Done -- could not queue up all [" << rcvd << "] characters in last message reply.");
  }

  // Thread ends here.
}
