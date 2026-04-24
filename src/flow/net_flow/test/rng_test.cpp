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

#include "flow/test/test_logger.hpp"
#include "flow/test/test_common_util.hpp"
#include "flow/net_flow/detail/port_space.hpp"
#include "flow/net_flow/detail/seq_num.hpp"
#include "flow/net_flow/node.hpp"
#include "flow/net_flow/endpoint.hpp"
#include "flow/net_flow/peer_socket.hpp"
#include "flow/net_flow/server_socket.hpp"
#include "flow/net_flow/error/error.hpp"
#include "flow/log/buffer_logger.hpp"
#include "flow/log/config.hpp"
#include "flow/util/util.hpp"
#include "flow/common.hpp"
#include <gtest/gtest.h>
#include <boost/regex.hpp>
#include <set>
#include <string>
#include <vector>

/* Pragmatic seed-diversity tests for NetFlow's three RNG-driven primitives: ephemeral-port reservation (Port_space),
 * initial sequence number generation (Sequence_number::Generator), and connection security token (picked internally
 * during handshake, observable via INFO-level log output).
 *
 * We do *not* attempt to prove randomness quality.  We only check that independent,
 * freshly constructed instances do not produce identical values -- i.e., that nobody accidentally seeded them all
 * from a constant, or from a too-coarse clock, or process-wide-only (where a single-process test would still see
 * collisions if the generator is re-seeded from a fixed constant on each instantiation): N fresh instances, one
 * value apiece, ensure all are distinct.  Collision probability in the random-but-properly-seeded case:
 *   - ephemeral port (~60K-element space, N=10): ~7e-4 for ANY dup.  Non-negligible.  Accommodated by allowing up
 *     to 1 duplicate in that test only (which drops false-positive rate to ~1e-6; still catches the realistic
 *     failure modes of "all identical" or "small-group collisions from coarse seeding").
 *   - ISN (64-bit), security token: collision probability vanishing; strict all-distinct check.
 * So a failure here is an actual seed-diversity bug, not bad luck. */

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
using boost::smatch;
using boost::regex_search;
using std::vector;
using std::string;
using std::set;

/// Number of independent instances to sample per test.  See file-level comment for collision analysis.
constexpr unsigned int N = 10;

/**
 * Shared distinctness check: given N values, EXPECTs that at most `allowed_dups` are duplicates (i.e., at least
 * `vals.size() - allowed_dups` are unique).  Typically `allowed_dups == 0` (strict all-distinct).
 */
template<typename T>
void expect_mostly_distinct(const vector<T>& vals, unsigned int allowed_dups = 0)
{
  /* Use () and not {} for the iterator-pair ctor: though `set<T>{begin, end}` happens to dispatch correctly here
   * (iterators aren't convertible to T), () removes any doubt when initializer_list and iterator-pair overloads
   * could in principle compete. */
  const set<T> uniq(vals.begin(), vals.end());
  EXPECT_GE(uniq.size() + allowed_dups, vals.size())
    << "Expected at least [" << (vals.size() - allowed_dups) << "] unique of [" << vals.size() << "]; got ["
    << uniq.size() << "] unique.  All identical? = [" << (uniq.size() == 1) << "].";
}

} // namespace (anon)

// Fresh Port_space instances should pick different random ephemeral ports.
TEST(Net_flow_rng, Port_space_ephemeral)
{
  Test_logger logger;

  vector<flow_port_t> ports;
  ports.reserve(N);
  for (unsigned int idx = 0; idx != N; ++idx)
  {
    Port_space port_spc{&logger};
    /* A fresh Port_space has ~60K free ports, so no error expected.
     * Per documentation, as an internal API this one expects non-null Error_code; it won't throw. */
    Error_code err_code;
    ports.push_back(port_spc.reserve_ephemeral_port(&err_code));
    ASSERT_FALSE(err_code) << "reserve_ephemeral_port() must succeed; failed: "
                              "[" << err_code << "] [" << err_code.message() << "].";
  }

  // Port space is only 16-bit; tolerate 1 dup in N=10 -- see file-level comment for probability analysis.
  expect_mostly_distinct(ports, 1);
}

// Fresh Sequence_number::Generator instances should pick different initial sequence numbers.
TEST(Net_flow_rng, Isn_generator)
{
  Test_logger logger;

  vector<Sequence_number> isns;
  isns.reserve(N);
  for (unsigned int idx = 0; idx != N; ++idx)
  {
    Sequence_number::Generator gen{&logger};
    isns.push_back(gen.generate_init_seq_num());
  }

  expect_mostly_distinct(isns);
}

/* End-to-end check of the security token and ISN (as used during a real handshake) via log-scraping.
 *
 * Security token is not exposed via any public accessor; it is logged at INFO on the active-connect side
 * on receipt of SYN_ACK.  That same line carries the ISN as seen end-to-end (from the SYN_ACK), so we grab
 * both with one regex, getting seed-diversity coverage for the security-token path and a bonus end-to-end
 * check of the ISN path in one shot.
 *
 * We use one Buffer_logger per session (not a shared one) so the regex only ever sees output from the pair under
 * test -- avoids cross-pair contamination and races on buffer_str() across Node threads (which would keep logging
 * in background... which we could avoid... but it's just easier to not deal with it).
 *
 * @warning This depends on the exact log format, so test could break, if someone changes the line.  Usually that
 * stuff is self-explanatory to fix. */
TEST(Net_flow_rng, Security_token_and_isn_via_logs)
{
  constexpr flow_port_t LOCAL_FLOW_PORT = 50;
  constexpr auto CONNECT_USER_TIMEOUT = chrono::seconds{1};

  Config log_cfg{Sev::S_INFO};
  log_cfg.init_component_to_union_idx_mapping<Flow_log_component>
    (100, Config::standard_component_payload_enum_sparse_length<Flow_log_component>(), true);
  log_cfg.init_component_names<Flow_log_component>(S_FLOW_LOG_COMPONENT_NAME_MAP, false, "flow-");

  /* Match the single active-connect-continuation INFO line, capturing ISN (group 1) and security token (group 2).
   * Anchored on "continuing active-connect" so we don't accidentally match the passive-connect line (which has an
   * ISN but no security token) or any unrelated [...]-bracketed substring. */
  const regex line_re
    {R"(continuing active-connect of [^\n]*?ISN \[([^\]]+)\]; security token \[([^\]]+)\])"};

  vector<string> tokens;
  vector<string> isns;
  tokens.reserve(N);
  isns.reserve(N);

  try
  {
    for (unsigned int idx = 0; idx != N; ++idx)
    {
      FLOW_TEST_TRACE_CTX("Session [", idx, "].");

      Buffer_logger logger{&log_cfg};
      {
        Node srv{&logger, Udp_endpoint{Ip_address_v4::loopback(), 0}};
        ASSERT_TRUE(srv.running());
        Server_socket::Ptr listener = srv.listen(LOCAL_FLOW_PORT);
        ASSERT_TRUE(listener);

        Node cli{&logger, Udp_endpoint{Ip_address_v4::loopback(), 0}};
        ASSERT_TRUE(cli.running());

        /* The active-connect-continuation log line we key on fires from the client's worker thread upon SYN_ACK
         * receipt, synchronously before sync_connect() returns -- so when sync_connect() returns successfully,
         * the line has been appended to the buffer. */
        const Remote_endpoint target{srv.local_low_lvl_endpoint(), LOCAL_FLOW_PORT};
        auto sock = cli.sync_connect(target, CONNECT_USER_TIMEOUT);
        ASSERT_TRUE(sock);
      } // Both Nodes (and the Peer_socket) destroyed here; worker threads joined => no concurrent logging below.

      // Per Buffer_logger docs, buffer_str() is only safe to read when no other thread is logging -- hence the block.
      const auto& log = logger.buffer_str();
      smatch m;
      ASSERT_TRUE(regex_search(log, m, line_re))
        << "Log did not contain the expected active-connect-continuation line with ISN + security token.  "
           "Log format may have changed; update the regex.";
      isns.emplace_back(m[1].str());
      tokens.emplace_back(m[2].str());
    }
  }
  catch (const std::exception& exc)
  {
    FAIL() << "Unexpected exception: [" << exc.what() << "]."; // Connection failures, etc.
  }

  {
    FLOW_TEST_TRACE_CTX("Security tokens (end-to-end).");
    expect_mostly_distinct(tokens);
  }
  {
    FLOW_TEST_TRACE_CTX("ISNs (end-to-end).");
    expect_mostly_distinct(isns);
  }
} // TEST(Net_flow_rng, Security_token_and_isn_via_logs)

} // namespace flow::net_flow::test
