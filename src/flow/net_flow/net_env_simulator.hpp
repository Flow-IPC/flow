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
#pragma once

#include "flow/net_flow/net_flow_fwd.hpp"
#include "flow/log/log.hpp"
#include "flow/util/random.hpp"
#include <boost/utility.hpp>
#include <boost/random.hpp>
#include <queue>
#include <utility>

namespace flow::net_flow
{
// Types.

/**
 * Objects of this class can be fed to Node to make it internally simulate network conditions like
 * loss, latency, and data duplication.  At construction one provides parameters controlling those
 * phenomena; then when Node is about to receive or send some actual network traffic it asks this
 * object whether it should first simulate some phenomenon like a lost packet.  For example, if a
 * loss probability is given at construction, bool should_drop_received_packet() will return whether
 * the current received packet should be "dropped" instead of handled, which Node will simulate
 * internally by ignoring or handling the packet.
 *
 * This is not really meant as a replacement for real tools that simulate such phenomena (or
 * obviously the actual phenomena in a real network).  The way it simulates them is probably
 * unrealistic; for example a wide latency range given in the constructor will cause packets to be
 * re-ordered quite a bit, whereas on a real network the packets are more likely to arrive in the
 * right order but with widely varying latencies over time.  Anyway -- the idea of using this is not
 * to benchmark Node's performance under various network conditions; it is to test its correctness.
 * For example: do frequent packet duplications and drops cause Node to enter an unexpected state
 * and crash?  What happens when an ESTABLISHED socket receives a late re-ordered SYN_ACK after
 * already getting some DATA?  Etc. etc.
 *
 * @todo One thing we should probably add for a bit of realism is the following.  If a loss range
 * [A, B] is specified, an individual packet's simulated latency will be a uniformly random number
 * in that range.  Because of this there will be frequent re-ordering of packets: if range
 * is [50, 100] ms, and we physically get packet X at time 0 and packet Y at time 5 ms, and the
 * simulated latencies are 80, 60 ms, respectively, then Y will "arrive" 80 - 60 - 5 = 15 ms BEFORE
 * X despite being sent earlier (assuming perfect actual network conditions under the simulation).
 * In reality, packets aren't re-ordered all that often.  So if we wanted to simulate a high latency
 * without re-ordering, we'd have to set the range to [A, A] for some A.  To avoid having to do
 * that, we can add some internal memory into Net_env_simulator that would put a floor on a randomly
 * generated latency. In the previous example, the range for packet Y would be chosen as [75, 100]
 * (instead of [50, 100]) to guarantee that packet Y arrives at least a bit later than packet X.  Of
 * course that might skew the latency range in bursty traffic, but that might be an OK behavior.  We
 * could add a knob for how often to ignore this new logic and allow a re-ordering, so that that is
 * also simulated.
 */
class Net_env_simulator :
  public log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand for the random seed integer type.
  using seed_type_t = uint32_t;

  /// Short-hand for floating point between 0 and 1 used to express probability.
  using prob_type_t = double;

  /// Short-hand for list of packet loss outcomes.
  using Packet_loss_seq = std::queue<bool>;

  /// Short-hand for list of packet duplication outcomes.
  using Packet_dup_seq = std::queue<bool>;

  /// Short-hand for list of packet latencies.
  using Latency_seq = std::queue<Fine_duration>;

  /// Short-hand for latency range [low, high].
  using Latency_range = std::pair<Fine_duration, Fine_duration>;

  // Constructors/destructor.

  /**
   * Constructs simulator.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param random_seed
   *        Random seed to use for the internal random number generator.  Assuming all else is equal
   *        (which can often be the case if all tested components are kept on the same
   *        machine/loopback), using the same seed twice to run the same operations should result in
   *        the same results.  If 0 is provided, seed is chosen based on the current time.
   * @param recv_packet_loss_prob
   *        Probability that the given received packet will get dropped instead of actually being
   *        received (0 is never; 1 is always; and anywhere in between).  This probability is
   *        applied only once elements of recv_packet_loss_seq run out.
   * @param recv_packet_loss_seq
   *        List of packet loss outcomes for received packets.  When packet `i` (in chronological
   *        order) is received, it is dropped instead of actually being received if the `i`-th element
   *        in `recv_packet_loss_seq` is true; otherwise it is not so dropped.  Once `i >=
   *        recv_packet_loss_seq.size()`, `recv_packet_loss_prob` is used instead.
   * @param recv_latency_range
   *        The range to use for the randomly generated latencies for received packets.  The pair
   *        specifies the [min, max] (inclusive) latency range in its first and second elements
   *        respectively.  The latency is chosen uniformly along that range.  This range is applied
   *        only once elements of `recv_latency_seq` run out.
   * @param recv_latency_seq
   *        List of latency outcomes for received packets.  When packet `i` (in chronological order)
   *        is received, it is lagged by N, where N is the `i`-th element in `recv_latency_seq`.  Once `i
   *        >= recv_latency_seq.size()`, `recv_latency_range is used instead.
   * @param recv_packet_dup_prob
   *        Probability that the given received packet will get duplicated in addition to actually
   *        being received (0 is never; 1 is always; and anywhere in between).  This probability is
   *        applied only once elements of `recv_packet_dup_seq` run out.
   * @param recv_packet_dup_seq
   *        List of packet loss outcomes for received packets.  When packet `i` (in chronological
   *        order) is received, it is duplicated in addition to actually being received if
   *        the `i`-th element in `recv_packet_dup_seq` is true; otherwise it is not so duplicated. Once
   *        `i >= recv_packet_dup_seq.size()`, `recv_packet_dup_prob` is used instead.
   */
  explicit Net_env_simulator(log::Logger* logger_ptr,
                             seed_type_t random_seed = 0,
                             prob_type_t recv_packet_loss_prob = 0,
                             const Packet_loss_seq& recv_packet_loss_seq = Packet_loss_seq{},
                             const Latency_range& recv_latency_range = Latency_range{},
                             const Latency_seq& recv_latency_seq = Latency_seq{},
                             prob_type_t recv_packet_dup_prob = 0,
                             const Packet_dup_seq& recv_packet_dup_seq = Packet_dup_seq{});

  // Methods.

  /**
   * Low-level packet was received, but should we simulate that it was dropped before receipt?
   * Probability and/or prescribed packet loss are set at construction.
   *
   * @return `true` if dropped; `false` otherwise.
   */
  bool should_drop_received_packet();

  /**
   * Low-level packet was received and not dropped, but should we simulate that it was lagged by an
   * additional amount of time, before actually getting received, and if so how much?  Latency range
   * and/or prescribed latencies are set at construction.
   *
   * @return How long the next packet should be lagged.  May be zero.
   */
  Fine_duration received_packet_latency();

  /**
   * Low-level packet was received, but should we simulate that it was duplicated?
   * Probability and/or prescribed packet duplication decisions are set at construction.
   *
   * @return `true` if duplicated; `false` otherwise.
   */
  bool should_duplicate_received_packet();

private:
  // Types.

  /// Random number generator.
  using Random_generator = util::Rnd_gen_uniform_range_base::Random_generator;

  // Data.

  /// Random seed for #m_rnd_generator (saved for reproducibility of "random" events).
  seed_type_t m_seed;

  /// Random number generator for various random events like packet loss.
  Random_generator m_rnd_generator;

  /**
   * At a given point in time, the `i`-th from now (in chronological order) received packet will be
   * dropped if and only if the `i`-th element of #m_recv_packet_loss_seq is true.  For example, the
   * next packet will be dropped if and only if `m_recv_packet_loss_seq.front()` is `true`.
   */
  Packet_loss_seq m_recv_packet_loss_seq;

  /// Returns `true` or `false` according to some probability set at construction; used for received packet loss.
  boost::random::bernoulli_distribution<prob_type_t> m_recv_packet_loss_distribution;

  /**
   * At a given point in time, the `i`-th from now (in chronological order) received packet will be
   * lagged by value in `i`-th element of #m_recv_latency_seq.  For example, the next packet will lagged by
   * `m_recv_latency_seq.front()`.
   */
  Latency_seq m_recv_latency_seq;

  /**
   * Returns random latencies (in units of `Fine_duration`) within the improper [low, high] range
   * given at construction; used for received packet latency.
   */
  boost::random::uniform_int_distribution<Fine_duration::rep> m_recv_latency_distribution_msec;

  /**
   * At a given point in time, the `i`-th from now (in chronological order) received packet will be
   * duplicated if and only if the `i`-th element of #m_recv_packet_dup_seq is `true`.  For example, the
   * next packet will be duplicated if and only if `m_recv_packet_dup_seq.front()` is `true`.
   */
  Packet_dup_seq m_recv_packet_dup_seq;

  /// Returns `true` or `false` according to some probability set at construction; used for received packet duplication.
  boost::random::bernoulli_distribution<prob_type_t> m_recv_packet_dup_distribution;
}; // class Net_env_simulator

} // namespace flow::net_flow
