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
#include "flow/net_flow/endpoint.hpp"
#include "flow/net_flow/options.hpp"
#include "flow/net_flow/peer_socket.hpp"
#include "flow/net_flow/server_socket.hpp"
#include "flow/net_flow/error/error.hpp"
#include "flow/test/test_logger.hpp"
#include "flow/util/util.hpp"
#include "flow/common.hpp"
#include <gtest/gtest.h>
#include <vector>

/* Tests for the per-Server_socket accept-backlog limit.  This limit protects a listening server against a SYN-flood
 * scenario (too many connections being half-opened or fully-opened-but-not-yet-accepted, tying up resources).  The
 * actual mechanism is implemented in Node::handle_syn_to_listening_server() and is driven by the option
 * Node_options::m_dyn_accept_backlog_limit. */

namespace flow::net_flow::test
{

namespace
{
using flow::test::Test_logger;
using flow::util::Ip_address_v4;
using flow::util::Udp_endpoint;
namespace chrono = boost::chrono;
} // Anonymous namespace

// A trivial guard against silently huge changes to the default; basically we don't want it to be something massive.
TEST(Net_flow_backlog, default_value)
{
  const Node_options defaults;
  using lim_t = decltype(Node_options::m_dyn_accept_backlog_limit);
  EXPECT_TRUE(flow::util::in_closed_range(lim_t(32), defaults.m_dyn_accept_backlog_limit, lim_t(128)))
    << "Default should be something sane; [" << defaults.m_dyn_accept_backlog_limit << "] is not what "
       "what we had envisioned -- so just look into it, and then fix the test or the default.";

  Test_logger logger;
  try
  {
    Node node{&logger, Udp_endpoint{Ip_address_v4::loopback(), 0}};
    EXPECT_EQ(defaults.m_dyn_accept_backlog_limit, node.options().m_dyn_accept_backlog_limit)
      << "This is pretty paranoid, but we're briefly checking that indeed Node::options() defaults to Node_options{}, "
         "for this setting at least."; // Not a substitute for testing the options submodule of NetFlow.
  }
  catch (const std::exception& exc)
  {
    FAIL() << "Unexpected exception: [" << exc.what() << "].";
  }
}

/* End-to-end exercise of the backlog limit.
 *
 * Strategy: bring up a server Node with a deliberately small backlog (smaller than the default 64, so this test runs
 * fast and is unambiguous).  Never call sync_accept() on it, so every successful handshake accumulates in the
 * server's unaccepted-backlog.  Then, from a single client Node, issue back-to-back sync_connect()s:
 *   - The first `backlog` should each succeed (they fill the backlog).
 *   - Each subsequent attempt should fail quickly with S_CONN_REFUSED: the server answers the SYN with RST without
 *     allocating a Peer_socket.  (Fast = before the first SYN retransmit, which by default is 125ms apart; we allow
 *     generous slack for test-env scheduling.)
 *
 * Single client Node (not N client Nodes): the backlog check is a pure count -- it does not care about source UDP
 * endpoint -- so N Nodes would exercise the same code path as one.
 *
 * @todo If we wanted to test it exhaustively as a black box, as opposed to a pragmatic check, then we could try various
 * combinations of connects from the same Node versus multiple Nodes.
 *
 * @todo This exercises only the `m_unaccepted_socks.size()` side of the backlog count -- i.e., connections that
 * fully handshook and sit unaccepted.  The equally-relevant `m_connecting_socks.size()` side (SYN_RCVD half-opens,
 * which is the real SYN-flood threat model) is not covered.  For a hardening-grade suite, fill the backlog with
 * half-opens via Net_env_simulator: pass a server-side simulator with a deterministic Packet_loss_seq that keeps
 * incoming SYNs and drops incoming SYN_ACK_ACKs (pattern `{false, true, false, true, ..., false}`).  Also raise the
 * server's `m_dyn_sock_opts.m_st_connect_retransmit_period` beyond the test's wall-clock duration so the
 * server does not retransmit SYN_ACK (which would prompt the client to re-send SYN_ACK_ACK past the end of the
 * loss sequence, transitioning the server out of SYN_RCVD and defeating the setup).  The `BACKLOG + 1`st SYN should
 * still be RSTed identically to the current test.  As above, we're going for a pragmatic check here, not an
 * exhaustive blackbox-stressing thing for the time being. */
TEST(Net_flow_backlog, excess_syns_rejected)
{
  using chrono::milliseconds;
  using chrono::seconds;
  using chrono::round;
  using std::vector;

  constexpr unsigned int BACKLOG = 3;
  constexpr unsigned int EXTRA = 3; // Attempts beyond the backlog that we expect to be rejected.
  constexpr flow_port_t LOCAL_FLOW_PORT = 50;
  /* We expect the server-side RST to reach the client within well under the SYN-retransmit period (125ms).  Loopback
   * round-trip should be orders of magnitude below that. */
  constexpr auto REJECT_MAX_PERIOD = seconds{1};
  /* Give handshakes plenty of headroom.  The effective cap is the protocol handshake timeout
   * (m_st_connect_retransmit_timeout, default 3s); we just want sync_connect()'s user timeout to not fire first. */
  constexpr auto CONNECT_USER_TIMEOUT = seconds{10};

  Test_logger logger;

  // Server.
  Node_options srv_opts;
  srv_opts.m_dyn_accept_backlog_limit = BACKLOG;

  /* nullptr/omitted Error_code* => throw.  Nothing should throw until we start getting the RSTs on reaching BACKLOG,
   * and those we handle inline below.  Hence: one outer try/catch with FAIL() in the catch for unexpected throws. */
  try
  {
    Node srv{&logger, Udp_endpoint{Ip_address_v4::loopback(), 0}, nullptr, nullptr, srv_opts};
    ASSERT_TRUE(srv.running());

    Server_socket::Ptr listener = srv.listen(LOCAL_FLOW_PORT);
    ASSERT_TRUE(listener);
    /* Deliberately never sync_accept() on `listener`: successful handshakes sit in the unaccepted-backlog, which is
     * precisely what we want to fill. */

    // Client (one Node, reused).
    Node cli{&logger, Udp_endpoint{Ip_address_v4::loopback(), 0}};
    ASSERT_TRUE(cli.running());

    const Remote_endpoint target{srv.local_low_lvl_endpoint(), LOCAL_FLOW_PORT};

    // Hold onto the good sockets so they don't close (and thus free backlog slots) before we probe the overflow.
    vector<Peer_socket::Ptr> accepted;
    accepted.reserve(BACKLOG);

    for (unsigned int idx = 0; idx != BACKLOG; ++idx)
    {
      SCOPED_TRACE("Connect [" + std::to_string(idx) + "] (within backlog).");
      accepted.emplace_back(cli.sync_connect(target, CONNECT_USER_TIMEOUT));
      ASSERT_TRUE(accepted.back());
    }

    for (unsigned int idx = 0; idx != EXTRA; ++idx)
    {
      SCOPED_TRACE("Connect [" + std::to_string(BACKLOG + idx) + "] (beyond backlog).");
      const auto from_when = Fine_clock::now();
      try
      {
        const auto sock = cli.sync_connect(target, CONNECT_USER_TIMEOUT);
        ADD_FAILURE() << "Beyond-backlog connect unexpectedly succeeded (socket truthy? = "
                         "[" << bool(sock) << "]).";
      }
      catch (const flow::error::Runtime_error& exc)
      {
        EXPECT_EQ(exc.code(), error::Code::S_CONN_REFUSED) << "Threw unexpected error: [" << exc.what() << "].";
      }
      const auto elapsed_period = Fine_clock::now() - from_when;
      EXPECT_LT(round<milliseconds>(elapsed_period), REJECT_MAX_PERIOD)
        << "Rejection took [" << round<milliseconds>(elapsed_period) << "] (expected < [" << REJECT_MAX_PERIOD << "]).";
    }
  } // try
  catch (const std::exception& exc)
  {
    FAIL() << "Unexpected exception: [" << exc.what() << "].";
  }
} // TEST(Net_flow_backlog, excess_syns_rejected)

} // namespace flow::net_flow::test
