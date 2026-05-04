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
#include "flow/net_flow/info.hpp"
#include "flow/net_flow/options.hpp"
#include "flow/net_flow/peer_socket.hpp"
#include "flow/net_flow/server_socket.hpp"
#include "flow/net_flow/net_env_simulator.hpp"
#include "flow/net_flow/error/error.hpp"
#include "flow/log/buffer_logger.hpp"
#include "flow/log/config.hpp"
#include "flow/test/test_common_util.hpp"
#include "flow/test/test_logger.hpp"
#include "flow/util/util.hpp"
#include "flow/common.hpp"
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/regex.hpp>
#include <cstdint>
#include <vector>
#include <iostream>

/* Guardrail for the server-side DATA-in-SYN_RCVD buffer limit.  Background:
 *
 * While the server sits in SYN_RCVD (SYN sent, SYN_ACK_ACK not yet received), DATA packets may
 * already arrive from the peer (re-ordering, or peer-already-ESTABLISHED-while-our-SYN_ACK_ACK-lost).
 * handle_data_to_syn_rcvd() buffers them, subject to
 * Peer_socket_options::m_st_rcv_sync_rcvd_data_q_cumulative_max_size -- a defense against
 * SYN-flood-style resource exhaustion where an attacker parks the server in SYN_RCVD and then keeps
 * feeding DATA.  This test verifies that cap is enforced.
 *
 * Approach: use Net_env_simulator on the server side to drop exactly one SYN_ACK_ACK (the first one
 * the server would receive), which keeps the server in SYN_RCVD while the client -- which has
 * already seen the SYN_ACK and believes itself ESTABLISHED -- happily sync_send()s DATA that then
 * arrives at the server-in-SYN_RCVD.  Once we've delivered enough DATA to overflow the cap, the
 * server's SYN_ACK retransmit (bumped to ~500ms to give us comfortable headroom) prompts a fresh
 * SYN_ACK_ACK from the client; the Net_env_simulator loss-sequence is exhausted by then, so that 2nd one
 * passes, the handshake completes, and the socket becomes acceptable.
 *
 * Post-accept, we read Peer_socket_info::m_rcv.m_total_data_{size|count} off the server socket.
 * DATA dropped by the SYN_RCVD cap never reaches that accounting; DATA that fit in
 * the queue is replayed into the rcv pipeline on the
 * SYN_RCVD=>ESTABLISHED transition and counted once there.  So m_total_data_size measures
 * precisely "bytes that survived the cap" -- must be <= CAP if the cap is working, and > 0 if the
 * SYN_RCVD path was actually exercised.
 *
 * Precondition witness: an INFO log line from handle_data_to_syn_rcvd() ("first time? = [1]")
 * confirms DATA really did arrive in SYN_RCVD -- without this, a too-fast handshake would silently
 * invalidate the whole test.
 *
 * Depends on a specific INFO log line for the precondition check; if that line's format
 * changes, it'll fail and encourage updating the regex.
 *
 * Also depends on the assumption that the 2nd received packet at the
 * server is the SYN_ACK_ACK (i.e., Packet_loss_seq is per-received-packet-index, not per-type). */

namespace flow::net_flow::test
{

namespace
{
using flow::test::Test_logger;
using flow::log::Buffer_logger;
using flow::log::Config;
using flow::log::Sev;
using flow::util::Ip_address_v4;
using flow::util::Udp_endpoint;
namespace chrono = boost::chrono;
using boost::regex;
using boost::regex_search;
using std::vector;
using std::cerr;
using std::flush;
} // namespace (anon)

TEST(Net_flow_syn_rcvd_data, Limit_enforced_on_overflow)
{
  constexpr flow_port_t LOCAL_FLOW_PORT = 50;
  constexpr size_t CAP = 1000; // Server SYN_RCVD-buffer byte cap.
  constexpr size_t CHUNK_SZ = 500; // Per sync_send() -- two fit under CAP, rest overflow.
  constexpr unsigned int N_CHUNKS = 5;
  constexpr auto RETX_PERIOD = chrono::milliseconds{500}; // Server's SYN_ACK retransmit pause.
  constexpr auto CONNECT_TIMEOUT = chrono::seconds{10};
  constexpr auto SEND_TIMEOUT = chrono::seconds{1};
  constexpr auto ACCEPT_TIMEOUT = chrono::seconds{10};

  // INFO-level logging so our "first DATA in SYN_RCVD" precondition-witness line is captured.
  Config log_cfg{Sev::S_INFO};
  log_cfg.init_component_to_union_idx_mapping<Flow_log_component>
    (100, Config::standard_component_payload_enum_sparse_length<Flow_log_component>(), true);
  log_cfg.init_component_names<Flow_log_component>(S_FLOW_LOG_COMPONENT_NAME_MAP, false, "flow-");
  Buffer_logger logger{&log_cfg};

  // Any return (including failed ASSERT()s) will run the thing (very helpful to figure out what happened).
  const auto cleanup = util::setup_auto_cleanup([&]()
  {
    if (::testing::Test::HasFailure())
    {
      cerr << "=== Buffered log follows ===\n" << logger.buffer_str() << '\n' << flush;
    }
  });

  Node_options srv_opts;
  srv_opts.m_dyn_sock_opts.m_st_rcv_sync_rcvd_data_q_cumulative_max_size = CAP;
  // Long enough to send all our DATA before server retransmits SYN_ACK (which would promptly end SYN_RCVD).
  srv_opts.m_dyn_sock_opts.m_st_connect_retransmit_period = RETX_PERIOD;

  /* Server-side simulator: drop exactly the 2nd received packet (the first SYN_ACK_ACK), keep the
   * rest.  Beyond the loss_seq, fallback loss prob=0 => pass.  That second SYN_ACK_ACK (from the server's
   * ~500ms SYN_ACK retransmit prompting the client to re-ACK) is what ultimately closes out the
   * handshake. */
  Net_env_simulator::Packet_loss_seq loss_seq;
  loss_seq.push(false); // SYN.
  loss_seq.push(true); // SYN_ACK_ACK #1 -- dropped.

  // @todo Some unwise design in 2011 by me (ygoldfel).  Make it take unique_ptr&&, or make Net_env_simulator movable.
  const auto srv_sim = new Net_env_simulator{&logger, 0, 0.0, loss_seq};

  size_t n_delivered_bytes = 0;
  size_t n_delivered_pkts = 0;

  try
  {
    {
      Node srv{&logger, Udp_endpoint{Ip_address_v4::loopback(), 0}, srv_sim, nullptr, srv_opts};
      ASSERT_TRUE(srv.running());
      Server_socket::Ptr listener = srv.listen(LOCAL_FLOW_PORT);
      ASSERT_TRUE(listener);

      Node cli{&logger, Udp_endpoint{Ip_address_v4::loopback(), 0}};
      ASSERT_TRUE(cli.running());

      const Remote_endpoint target{srv.local_low_lvl_endpoint(), LOCAL_FLOW_PORT};
      auto cli_sock = cli.sync_connect(target, CONNECT_TIMEOUT);
      ASSERT_TRUE(cli_sock);
      // Client thinks ESTABLISHED; server is in SYN_RCVD (simulator dropped its SYN_ACK_ACK).

      const vector<uint8_t> payload(CHUNK_SZ, uint8_t{0xAB});
      for (unsigned int idx = 0; idx != N_CHUNKS; ++idx)
      {
        FLOW_TEST_TRACE_CTX("Chunk [", idx, "].");
        const size_t sent = cli_sock->sync_send(boost::asio::buffer(payload), SEND_TIMEOUT);
        ASSERT_EQ(sent, payload.size());
      }

      /* Retransmitted SYN_ACK (~RETX_PERIOD later) prompts client's 2nd SYN_ACK_ACK, which the
       * simulator now passes => server -> ESTABLISHED, listener becomes acceptable. */
      auto srv_sock = listener->sync_accept(ACCEPT_TIMEOUT);
      ASSERT_TRUE(srv_sock);

      /* info().m_rcv.m_total_data_{size|count} = bytes/packets that survived the cap (replayed from the
       * queue into the rcv pipeline).  Dropped-by-cap DATA never reaches this accumulator; see top comment
       * comment for why this is what we want. */
      const auto info = srv_sock->info();
      n_delivered_bytes = info.m_rcv.m_total_data_size;
      n_delivered_pkts = info.m_rcv.m_total_data_count;
    } // Both Nodes destroyed; worker threads joined => buffer_str() safe below.
  }
  catch (const std::exception& exc)
  {
    FAIL() << "Unexpected exception: [" << exc.what() << "].";
  }

  // Cap is hard: bytes that survived the cap (= everything the server's rcv pipeline ultimately accepted) must not exceed it.
  EXPECT_LE(n_delivered_bytes, CAP)
    << "Bytes delivered through the server rcv pipeline [" << n_delivered_bytes << "] exceed cap [" << CAP << "]; "
       "cap was not enforced.";
  // Some DATA must have made it through -- otherwise the test didn't actually exercise the SYN_RCVD path.
  EXPECT_GE(n_delivered_bytes, CHUNK_SZ)
    << "No DATA delivered (total = [" << n_delivered_bytes << "]); "
       "server probably was not in SYN_RCVD when DATA arrived.";
  // Not *all* chunks got through -- otherwise cap was not enforced.
  EXPECT_LT(n_delivered_pkts, N_CHUNKS)
    << "All [" << N_CHUNKS << "] chunks were delivered (count = [" << n_delivered_pkts << "]); cap was not enforced.";

  /* Precondition witness: the INFO-level line from handle_data_to_syn_rcvd() fired with
   * `first time? = [1|true]`, confirming DATA really did hit SYN_RCVD (and not, e.g., ESTABLISHED
   * because the handshake completed too quickly).  Permissive regex: specific log fields may change
   * formatting, but these markers should persist. */
  const regex first_data_in_syn_rcvd
    {R"(received \[[^\]]+\] packet while in \[[^\]]*SYN_RCVD[^\]]*\][^\n]*first time\? = \[(1|true)\])"};
  EXPECT_TRUE(regex_search(logger.buffer_str(), first_data_in_syn_rcvd))
    << "Log did not contain the expected 'first DATA in SYN_RCVD' INFO line -- server may not have actually been "
       "in SYN_RCVD when DATAs arrived.  Log format may have changed; update the regex.";


} // TEST(Net_flow_syn_rcvd_data, Limit_enforced_on_overflow)

} // namespace flow::net_flow::test
