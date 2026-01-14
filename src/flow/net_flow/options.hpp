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
#include <boost/program_options.hpp>

namespace flow::net_flow
{
// Types.

/**
 * A set of low-level options affecting a single Peer_socket.  All comments for `struct`
 * Node_options apply equally to Peer_socket_options (except, of course, those pertaining to the
 * nested Peer_socket_options instance).
 *
 * @see `struct` Node_options.
 */
struct Peer_socket_options
{
  // Types.

  /// Short-hand for boost.program_options config options description.  See setup_config_parsing().
  using Options_description = boost::program_options::options_description;

  /**
   * A time duration, with fine precision, that can be positive, zero, or negative (unless a specific option
   * specifies an additional constraint to that effect).  This is used for most (all?) time-based options in
   * Node_options and Peer_socket_options.
   *
   * @internal
   *
   * This used to be an alias from `duration<uint16_t, Fine_duration::period>` to `Fine_duration_non_neg`,
   * meaning it was specifcally intended to be non-negative.  That constraint made sense for all time-based
   * options at the time and probably still does... but there was a problem: In Boost 1.50, the non-negative
   * implementation integer type caused a comparison to -1 somewhere inside their headers, which triggered a
   * (reasonable) gcc-4.2 warning.  Ironically, in Boost 1.48, using a negative-allowing type caused some
   * other comparison triggering a similar warning.  Anyway, as of Boost 1.50, I've decided to bite the bullet
   * and use a negative-allowing type after all (which causes no warning in that version) and enforce any
   * required non-negativeness (or even positiveness, where applicable) explicitly when calling
   * `VALIDATE_CHECK()` at option parse time.  (That was way longer an explanation than this deserved.)
   */
  using Fine_duration = flow::Fine_duration;

  /// The possible choices for congestion control strategy for the socket.
  enum class Congestion_control_strategy_choice
  {
    /// Classic (Reno-style) AIMD congestion control.
    S_CLASSIC = 0,
    /// Classic congestion control but with loss resulting in a window chosen from an outgoing bandwidth estimate.
    S_CLASSIC_BANDWIDTH_ESTIMATED
  };

  // Constructors/destructor.

  /**
   * Constructs a Peer_socket_options with values equal to those used by Node when the Node
   * creator chooses not to supply custom options.
   */
  explicit Peer_socket_options();

  // Methods.

  /**
   * Analogous to Node_options::setup_config_parsing().  See that method.
   *
   * @param opts_desc
   *        The #Options_description object into which to load the help information, defaults, and
   *        mapping to members of `*this`.
   */
  void setup_config_parsing(Options_description* opts_desc);

  // Data.

  /**
   * The size of block that we will strive to (and will, assuming at least that many bytes are
   * available in Send buffer) pack into each outgoing DATA packet.  It is assumed the other side is
   * following the same policiy (any packets that do not -- i.e., exceed this size -- are dropped).
   * This is an important control; the higher it is the better for performance AS LONG AS it's not so
   * high it undergoes IP fragmentation (or does that even happen in UDP? if not, even worse --
   * it'll just be dropped and not sent!).  The performance impact is major; e.g., assuming no
   * fragmentation/dropping, we've seen a ~1500 byte MBS result in 20-30% higher throughput than
   * 1024 byte MBS.
   *
   * Additionally, if using `net_flow` module with no reliability feature -- i.e., if you want to perform FEC
   * or something else outside the Flow protocol -- then it is absolutely essential that EVERY `send*()` call
   * provides a buffer whose size is a multiple of max-block-size.  Otherwise packet boundaries
   * will not be what you expect, and you will get what seems to be corrupted data at the
   * application layer (since our stream-based API has no way of knowing where your message begins
   * or ends).  Alternatively you can encode message terminators or packet sizes, but since in
   * unreliable mode integrity of a given block is guaranteed only if all blocks align with
   * max-block-size boundaries, you'll still probably be screwed.
   *
   * @todo max-block-size should be dynamically determined (but overridable by this option).  That
   * is a complex topic, however, with considerations such as MTU discovery, ICMP errors, and who knows
   * what else.
   */
  size_t m_st_max_block_size;

  /**
   * How often to resend SYN or SYN_ACK while SYN_ACK or SYN_ACK_ACK, respectively, has not been received.
   * In other words, this controls the pause between retries during the connection opening phase, by either side,
   * if the other side is not responding with the appropriate response.
   */
  Fine_duration m_st_connect_retransmit_period;

  /// How long from the first SYN or SYN_ACK to allow for connection handshake before aborting connection.
  Fine_duration m_st_connect_retransmit_timeout;

  /**
   * Maximum number of bytes that the Send buffer can hold.  This determines how many bytes user can
   * `send()` while peer cannot send over network until send() refuses to take any more bytes.  Notew
   * that any value given will be increased, if necessary, to the nearest multiple of
   * max-block-size.  This is important to preserve message boundaries when operating in
   * unreliable mode (guaranteed max-block-size-sized chunks of data will be sent out in their
   * entirety instead of being fragmented).
   */
  size_t m_st_snd_buf_max_size;

  /**
   * Maximum number of bytes that the Receive buffer can hold.  This determines how many bytes
   * can be received in the background by the Node without user doing any receive()s.  It is also
   * rounded up to to the nearest multiple of max-block-size.
   */
  size_t m_st_rcv_buf_max_size;

  /**
   * Whether flow control (a/k/a receive window a/k/a rcv_wnd management) is enabled.  If this is
   * disabled, an infinite rcv_wnd will always be advertised to the sender; so if the Receive buffer
   * is exceeded packets are dropped as normal, but the sender will not know it should stop sending
   * until Receive buffer space is freed.  If this is enabled, we keep the sender informed of how
   * much Receive buffer space is available, so it can suspend the flow as necessary.
   */
  bool m_st_rcv_flow_control_on;

  /**
   * % of rcv-buf-max-size such that if Receive buffer stores up to (100 + this many) % of
   * rcv-buf-max-size bytes, the bytes will still be accepted.  In other words, this allows the max
   * Receive buffer to hold slightly more than rcv-buf-max-size bytes.  However, the current Receive
   * buffer capacity advertised to the other side of the connection will be based on the
   * non-inflated rcv-buf-max-size.  This option provides some protection against the fact that the
   * receive window value sent to the other side will lag behind reality somewhat.
   */
  unsigned int m_st_rcv_buf_max_size_slack_percent;

  /**
   * % of rcv-buf-max-size that has to be freed, since the last receive window advertisement, via
   * user popping data from Receive buffer, before we must send a receive window advertisement.
   * Normally we send rcv_wnd to the other side opportunistically in every ACK; but there can be
   * situations when there is no packet to acknowledge, and hence we must specifically make a
   * packet just to send over rcv_wnd.  Typically we should only need to do this if the buffer was
   * exceeded and is now significantly freed.  This value must be in [1, 100], but anything over 50
   * is probably pushing it.
   */
  unsigned int m_st_rcv_buf_max_size_to_advertise_percent;

  /**
   * The limit on the size of Peer_socket::m_rcv_packets_with_gaps, expressed as what percentage the
   * maximal size of that structure times max-block-size is of the maximal receive buffer size.  For
   * example, if this is 200, then Peer_socket::m_rcv_packets_with_gaps can represent up to roughly
   * 2x as many full-sized blocks as the Receive buffer can.  This should also by far exceed any
   * sender congestion window max size to avoid packet loss.
   *
   * @see Peer_socket::m_rcv_packets_with_gaps for details.
   * @see Peer_socket::m_rcv_syn_rcvd_data_q.
   */
  unsigned int m_st_rcv_max_packets_after_unrecvd_packet_ratio_percent;

  /**
   * The maximum amount of time to delay sending ACK with individual packet's acknowledgment since
   * receiving that individual packet.  If set to zero duration, any given individual acknowledgment
   * is sent within a non-blocking amount of time of its DATA packet being read.  Inspired by RFC
   * 1122-4.2.3.2.
   */
  Fine_duration m_st_delayed_ack_timer_period;

  /**
   * If there are at least this many TIMES max-block-size bytes' worth of individual acknowledgments
   * to be sent, then the delayed ACK timer is to be short-circuited, and the accumulated
   * acknowledgments are to be sent as soon as possible.  Inspired by RFC 5681.
   */
  size_t m_st_max_full_blocks_before_ack_send;

  /**
   * Whether to enable reliability via retransmission.  If `false`, a detected lost packet may have
   * implications on congestion control (speed at which further data are sent) but will not cause
   * that packet to be resent; receiver application code either has to be OK with missing packets or
   * must implement its own reliability (e.g., FEC).  Packets may also be delivered in an order
   * different from the way they were sent.  If `true`, the receiver need not worry about it, as any
   * lost packets will be retransmitted with no participation from the application code on either
   * side, as in TCP.  Also as in TCP, this adds order preservation, so that the stream of bytes sent
   * will be exactly equal to the stream of bytes received.  Retransmission removes the requirement
   * for the very exacting block-based way in which `send()` and friends must be called.
   *
   * This option must have the same value on both sides of the connection, or the server will refuse
   * the connection.
   */
  bool m_st_rexmit_on;

  /**
   * If retransmission is enabled and a given packet is retransmitted this many times and has to be
   * retransmitted yet again, the connection is reset.  Should be positive.
   */
  unsigned int m_st_max_rexmissions_per_packet;

  /**
   * Once socket enters ESTABLISHED state, this is the value for Peer_socket::m_snd_drop_timeout until the first RTT
   * measurement is made (the first valid acknowledgment arrives).
   */
  Fine_duration m_st_init_drop_timeout;

  /**
   * If `true`, when scheduling Drop Timer, schedule it for Drop Timeout relative to the send time of
   * the earliest In-flight packet at the time.  If `false`, also schedule DTO relative to the time of
   * scheduling.  The latter is less aggressive and is recommended by RFC 6298.
   */
  bool m_st_drop_packet_exactly_after_drop_timeout;

  /**
   * If `true`, when the Drop Timer fires, all In-flight packets are to be considered Dropped (and thus the
   * timer is to be disabled).  If `false`, only the earliest In-flight packet is to be considered Dropped (and
   * thus the timer is to restart).  RFC 6298 recommends `false`.  `true` is more aggressive.
   */
  bool m_st_drop_all_on_drop_timeout;

  /**
   * If an In-flight packet is acknowledged, but it is not the earliest In-flight packet (i.e., it's
   * an out-of-order acknowledgment), and this is `true`, the timer is restarted.  Otherwise the timer
   * continues to run.  The former is less aggressive.  RFC 6298 wording is ambiguous on what it
   * recommends (not clear if cumulative ACK only, or if SACK also qualifies).
   */
  bool m_st_out_of_order_ack_restarts_drop_timer;

  /**
   * Enables or disables packet pacing, which attempts to spread out, without sacrificing overall
   * send throughput, outgoing low-level packets to prevent loss.  If disabled, any packet that is
   * allowed by congestion/flow control to be sent over the wire is immediately sent to the UDP
   * net-stack; so for example if 200 packets are ready to send and are allowed to be sent, they're sent
   * at the same time.  If enabled, they will be spread out over a reasonable time period instead.
   * Excessive burstiness can lead to major packet drops, so this can really help.
   */
  bool m_st_snd_pacing_enabled;

  /**
   * When estimating the available send bandwidth, each sample must be compiled over at least this
   * long of a time period, even if the SRTT is lower.  Normally a sample is collected over at
   * least an SRTT, but computing a bandwidth sample over a quite short time period can produce
   * funky results, hence this floor.  Send bandwidth estimation is used at least for some forms of
   * congestion control.
   */
  Fine_duration m_st_snd_bandwidth_est_sample_period_floor;

  /// The congestion control algorithm to use for the connection or connections.
  Congestion_control_strategy_choice m_st_cong_ctl_strategy;

  /**
   * The initial size of the congestion window, given in units of max-block-size-sized blocks.
   * The special value 0 means RFC 5681's automatic max-block-size-based computation should
   * be used instead.
   */
  size_t m_st_cong_ctl_init_cong_wnd_blocks;

  /**
   * The constant that determines the CWND limit in Congestion_control_classic_data::congestion_window_at_limit() and
   * Congestion_control_classic_data::clamp_congestion_window() (in multiples of max-block-size).  When choosing this
   * value, use these constraints:
   *
   *   - This limits total outgoing throughput.  The throughput B will be <= CWND/RTT, where RTT is
   *     roughly the RTT of the connection, and CWND == max-cong-wnd-blocks * max-block-size.
   *     Therefore, choose B and RTT values and set max-cong-wnd-blocks = B * RTT / max-block-size
   *     (B in bytes/second, RTT in seconds).
   *   - Until we implement Receive window, this value should be much (say, 4x) less than the size
   *     of the Receive buffer, to avoid situations where even a well-behaving user (i.e., user that
   *     `receive()`s all data ASAP) cannot keep up with reading data off Receive buffer, forcing
   *     `net_flow` to drop large swaths of incoming traffic.  If CWND is much smaller than Receive
   *     buffer size, then this avoids that problem.
   *
   * @todo Reconsider this value after Receive window feature is implemented.
   */
  size_t m_st_cong_ctl_max_cong_wnd_blocks;

  /// On Drop Timeout, set congestion window to this value times max-block-size.
  size_t m_st_cong_ctl_cong_wnd_on_drop_timeout_blocks;

  /**
   * The multiple of max-block-size by which to increment CWND in congestion avoidance mode after receiving
   * at least a full CWND's worth of clean acknowledgments.  RFC 5681 (classic Reno) mandates this is set to 1,
   * but we allow it to be overridden.  The special value 0 causes the RFC value to be used.
   */
  unsigned int m_st_cong_ctl_cong_avoidance_increment_blocks;

  /**
   * In classic congestion control, RFC 5681 specifies the window should be halved on loss; this
   * option allows one to use a custom percentage instead.  This should be a value in [1, 100] to
   * have the window decay to that percentage of its previous value, or 0 to use the RFC
   * 5681-recommended constant (50).
   */
  unsigned int m_st_cong_ctl_classic_wnd_decay_percent;

  /// Ceiling to impose on the Drop Timeout.
  Fine_duration m_dyn_drop_timeout_ceiling;

  /**
   * Whenever the Drop Timer fires, upon the requisite Dropping of packet(s), the DTO (Drop Timeout)
   * is set to its current value times this factor, and then the timer is rescheduled accordingly.
   * RFC 6298 recommends 2.  Another value might be 1 (disable feature).  The lower the more
   * aggressive.
   */
  unsigned int m_dyn_drop_timeout_backoff_factor;

  /**
   * When the mode triggered by rcv-buf-max-size-to-advertise-percent being exceeded is in effect,
   * to counteract the possibility of ACK loss the receive window is periodically advertised
   * subsequently -- with the period given by this option -- until either some new data arrive or
   * rcv-wnd-recovery-max-period is exceeded.
   */
  Fine_duration m_dyn_rcv_wnd_recovery_timer_period;

  /**
   * Approximate amount of time since the beginning of rcv_wnd recovery due to
   * rcv-buf-max-size-to-advertise-percent until we give up and end that phase.
   */
  Fine_duration m_dyn_rcv_wnd_recovery_max_period;

private:
  // Friends.

  /**
   * The two `struct`s work together (since Node_options contains an instance of us).
   * @see Node_options.
   */
  friend struct Node_options;
  // Friend of Peer_socket_options: For access to our internals.
  friend std::ostream& operator<<(std::ostream& os, const Peer_socket_options& opts);

  // Methods.

  /**
   * Analogous to Node_options::setup_config_parsing_helper().  See that method.
   *
   * @param opts_desc
   *        See Node_options::setup_config_parsing_helper().
   * @param target
   *        See Node_options::setup_config_parsing_helper().
   * @param defaults_source
   *        See Node_options::setup_config_parsing_helper().
   * @param printout_only
   *        See Node_options::setup_config_parsing_helper().
   */
  static void setup_config_parsing_helper(Options_description* opts_desc,
                                          Peer_socket_options* target,
                                          const Peer_socket_options& defaults_source,
                                          bool printout_only);
}; // struct Peer_socket_options

/**
 * A set of low-level options affecting a single Flow Node, including Peer_socket objects and other
 * objects subsequently generated by that Flow Node.  Typically these values can be left at
 * defaults, and thus the `net_flow` user normally need not deal with these objects.  However, if
 * low-level tuning of Flow internals is needed, you can use this `struct`.
 *
 * This is just a simple `struct` with a couple of utilities to serialize (to `ostream`) and deserialize
 * (from a config file or command line) the values in the `struct`; it is not connected to any other
 * object (for example, it can exist with no Node).
 *
 * You may read from and write to members of this `struct` at will, and no checking will be performed;
 * moreover, doing so will have no effect other than the field being read or written.
 *
 * Fields are validated and potentially take effect, however, when you pass a Node_options object
 * to Node.  Node always copies the given Node_options and never saves a reference to it.  All of
 * that applies equally to Peer_socket_options.
 *
 * Node_options contains a Peer_socket_options #m_dyn_sock_opts (a very similar, in spirit, `struct`)
 * within it.  Options stored within that are per-::flow::net_flow::Peer_socket
 * options.  For example, #m_st_low_lvl_max_buf_size
 * applies to the entire Node at all times and is stored directly in `*this` (as there is only one
 * UDP buffer for the entire Node); whereas max-block-size can be different for each
 * individual Peer_socket and is thus stored in `this->m_dyn_sock_opts`.  See Node::listen() and
 * Node::connect() for how per-socket options are distributed to subsequent sockets as connections
 * are made.
 *
 * The `struct`, when default-constructed, contains sane default values as used by Node when not
 * provided with a Node_options object.  Alternatively, you can get a Node's existing set of
 * options via Node::options().
 *
 * After obtaining a Node_options either way, you may assign values to it manually.
 *
 * Alternatively, you can fill it parsing a config file or command line using
 * boost.program_options (which makes it a snap) with the help of
 * Node_options::setup_config_parsing(), which will provide a program_options-suitable
 * #Options_description object to enable this parsing.  You may conversely print #Options_description
 * to an `ostream` (e.g., `cout`) for full help text on the meaning of each option and the defaults.
 *
 * You may also print a filled Node_options itself to an `ostream` for the current settings stored in
 * that object.
 *
 * ### Thready safety ###
 * Same as any `struct` with no locking done therein.
 *
 * @see Node constructor, Node::set_options(), Node::listen(), Node::connect().
 * @see Peer_socket::set_options(), Peer_socket::options().
 * @see Peer_socket_options.
 *
 * @internal
 *
 * If you want to add an option, follow these steps:
 *
 *   1. Decide whether it's a per-socket or per-Node option (usually the former) and add it to
 *      Peer_socket_options or Node_options, respectively, using the below steps.
 *   2. Decide whether the option's value may be changed after the enclosing Node_options or
 *      Peer_socket_options has been accepted by a running Node or Peer_socket, respectively.  Most
 *      options tend to be static (cannot be changed once a Node or Peer_socket has accepted them).
 *      Based on this, the data member must start with `m_st_` (static) or `m_dyn_` (dynamic).  Choose
 *      the rest of the member name as well.
 *   3. Add the data member into the chosen `struct`.  Fully comment the meaning, as for other
 *      present options.
 *   4. Add an ADD_CONFIG_OPTION() line into the proper `struct` setup_config_parsing_helper() by analogy
 *      with other present options.  The description string should usually be a copy of the comment
 *      from step 3.
 *   5. Add the default value (very important) into the proper `struct` constructor.
 *      Explain the choice of default with a comment.
 *   6. Is this a static option (step 2)?  If so, add static validation (by analogy with present
 *      options) to Node::validate_options() or Node::sock_validate_options().
 *   7. Add semantic validation (such as for illegal values, including illegal values due to
 *      inconsistency with other option values) to Node::validate_options() or
 *      Node::sock_validate_options().  For some options all values are legal, in which case you
 *      needn't add anything.
 *   8. Use the option.  EVERY read of the option should be performed through Node::opt() or
 *      Peer_socket::opt(), for thread safety.  The only exception is if you manually lock
 *      Node::m_opt_mutex or Peer_socket::m_opt_mutex, respectively.
 */
struct Node_options
{
  // Types.

  /// Short-hand for boost.program_options config options description.  See setup_config_parsing().
  using Options_description = Peer_socket_options::Options_description;

  /// A time duration, with fine precision.
  using Fine_duration = Peer_socket_options::Fine_duration;

  // Constructors/destructor.

  /**
   * Constructs a Node_options with values equal to those used by Node when the Node
   * creator chooses not to supply custom options.
   */
  explicit Node_options();

  // Methods.

  /**
   * Modifies a boost.program_options options description object to enable subsequent parsing of a
   * command line or config file into the data members of this object, as well printing a help
   * message about these options to an `ostream`.  In particular, after executing this method:
   *
   *   - If you output `*opts_desc` to an `ostream`, the `ostream` will get a detailed description of each
   *     option, its symbolic name (for config file or command line), and its value if the config
   *     file or command line omits this option (the default).  This default will equal to the value
   *     of that particular option in a default-constructed Node_options.
   *
   *   - If you perform something like the following code snippet,
   *     then `*this` will now be filled with values parsed from `ifstream F` (and default values
   *     for any omitted values):
   *
   *   ~~~
   *   namespace opts = boost::program_options;
   *   Node_options flow_opts;
   *   opts::options_description opts_desc;
   *   std::ifstream F(...);
   *   flow_opts.setup_config_parsing(&opts_desc);
   *   opts::variables_map cfg_vars;
   *   opts::store(opts::parse_config_file(F, *opts_desc), cfg_vars);
   *   opts::notify(cfg_vars);
   *   ~~~
   *
   * Note the default values will come from the current values inside `*this`.
   *
   * @param opts_desc
   *        The #Options_description object into which to load the help information, defaults, and
   *        mapping to members of `*this`.
   */
  void setup_config_parsing(Options_description* opts_desc);

  // Data.

  /**
   * If and only if this is `true`, the Node will detect SIGINT and SIGTERM (or your OS's version thereof);
   * upon seeing such a signal, it will fire Node::interrupt_all_waits(), which will interrupt all
   * blocking operations, conceptually similarly to POSIX's `EINTR`.  If this is `true`, the user may register
   * their own signal handler(s) (for any purpose whatsoever) using `boost::asio::signal_set`. However, behavior
   * is undefined if the program registers signal handlers via any other API, such as `sigaction()` or `signal()`.
   * If you need to set up such a non-`signal_set` signal handler, AND you require `EINTR`-like behavior,
   * then (1) set this option to `false`; (2) trap SIGINT and SIGTERM yourself; (3) in your handlers for the
   * latter, simply call Node::interrupt_all_waits().  Similarly, if you want custom behavior regarding
   * Node::interrupt_all_waits(), feel free to call it whenever you want (not necessarily even from a signal
   * handler), and set this to `false`.  However, if a typical, common-sense behavior is what you're after -- and either
   * don't need additional signal handling or are OK with using `signal_set` for it -- then setting this to `true` is a
   * good option.
   */
  bool m_st_capture_interrupt_signals_internally;

  /**
   * The max size to ask the OS to set our UDP socket's receive buffer to in order to minimize loss
   * if we can't process datagrams fast enough.  This should be as high as possible while being
   * "acceptable" in terms of memory.  However, the OS will probably have its own limit
   * and may well pick a limit that is the minimum of that limit and this value.
   */
  size_t m_st_low_lvl_max_buf_size;

  /**
   * A time period such that the boost.asio timer implementation for this platform is able to
   * accurately able to schedule events within this time period or greater.  If you select 0, the
   * code will decide what this value is based on the platform, but its logic for this may or may not
   * be correct (actually it will probably be correct but possibly too conservative [large], causing
   * timer coarseness in mechanisms like the rather critical packet pacing).
   */
  Fine_duration m_st_timer_min_period;

  /**
   * The UDP net-stack may deliver 2 or more datagrams to the Flow Node at the same time.  To lower
   * overhead and increase efficiency, Flow will process all such datagrams -- and any more that
   * may arrive during this processing -- before preparing any resulting outgoing messages, such as
   * acknowledgments or more data packets.  In high-speed conditions this may result in excessive
   * burstiness of outgoing traffic.  This option's value places a limit on the number of datagrams
   * to process before constructing and sending any resulting outgoing messages to prevent this
   * burstiness.  If 0, there is no limit.
   */
  unsigned int m_dyn_max_packets_per_main_loop_iteration;

  /**
   * Any incoming low-level (UDP) packet will be truncated to this size.  This should be well above
   * per-socket max-block-size (# of bytes of user payload per DATA packet).  There will only be one buffer
   * of this size in memory at a time, so no need to be too stingy, but on the other hand certain
   * allocation/deallocation behavior may cause performance drops if this unnecessarily large.
   */
  size_t m_dyn_low_lvl_max_packet_size;

  /**
   * This very inside-baseball setting controls the allocation/copy behavior of the UDP receive-deserialize
   * operation sequence.  When enabled, there is exactly one input buffer large enough to
   * hold any one serialized incoming packet; any deserialized data (including DATA and ACK payloads) are
   * stored in separately allocated per-packet buffers; and and the input buffer is repeatedly reused
   * without reallocation.  When disabled, however, at least some packet types (most notably DATA) will
   * use the zero-copy principle, having the deserializer take ownership of the input buffer and access pieces
   * inside it as post-deserialization values (most notably the DATA payload); in this case the input buffer
   * has to be reallocated between UDP reads.  As of this writing the former behavior seems to be somewhat
   * faster, especially if low-lvl-max-packet-size is unnecessarily large; but arguably the zero-copy behavior
   * may become faster if some implementation details related to this change.  So this switch seemed worth
   * keeping.
   */
  bool m_dyn_guarantee_one_low_lvl_in_buf_per_socket;

  /**
   * The set of per-Peer_socket options in this per-Node set of options.  This represents the
   * per-socket options each subsequent socket generated in the corresponding Node will get, unless
   * user specifies a custom set of such options.
   *
   * Note that, in the context of the per-Node options set, this is dynamic; any per-socket option
   * (even a static one) must be changeable in the global per-Node options set, as otherwise it would just
   * be a regular per-Node data member in Node_options, as it could never change anyway, so no point in having
   * it be per-Peer_socket.
   */
  Peer_socket_options m_dyn_sock_opts;

private:
  // Friends.

  /**
   * The two `struct`s work together (since Peer_socket_options needs add_config_option()).
   * @see Peer_socket_options.
   */
  friend struct Peer_socket_options;
  /**
   * Node needs opt_id_to_str().
   * @see Node.
   */
  friend class Node;
  // Friend of Node_options: For access to our internals.
  friend std::ostream& operator<<(std::ostream& os, const Node_options& opts);

  // Methods.

  /**
   * Helper that, for a given option `m_blah`, takes something like `"m_blah_blah"` and returns the
   * similar more suitable option name the user can use in a config file or command line.  For
   * example, `"m_st_max_block_size"` yields `"max-block-size"`.
   *
   * @param opt_id
   *        A string whose content equals the name (in C++ code) of a data member of Node_options or
   *        Peer_socket_options (presumably generated using the macro hash technique).
   * @return See above.
   */
  static std::string opt_id_to_str(const std::string& opt_id);

  /**
   * A helper that adds a single option to a given #Options_description, for use either in printing
   * out the current state of a Node_options or Peer_socket_options; or for parsing of config into such
   * an object.
   *
   * @tparam Opt_type
   *         The type of a data member of of Node_options or Peer_socket_options.
   * @param opts_desc
   *        The #Options_description object into which to load a single `option_description`.
   * @param opt_id
   *        A string whose content equals the name (in C++ code) of a data member of Node_options or
   *        Peer_socket_options (presumably generated using the macro hash technique).
   * @param target_val
   *        If `!printout_only`, and the user parses via `*opts_desc`, the parsed option value will be
   *        loaded into `*target_val`.  Otherwise ignored.
   * @param default_val
   *        The default value to this option in `*opts_desc` will be this.
   * @param description
   *        If `!printout_only`, the detailed description of the option will be this.  Otherwise ignored.
   * @param printout_only
   *        See above.
   */
  template<typename Opt_type>
  static void add_config_option(Options_description* opts_desc,
                                const std::string& opt_id,
                                Opt_type* target_val, const Opt_type& default_val,
                                const char* description, bool printout_only);

  /**
   * Loads the full set of boost.program_options config options into the given #Options_description,
   * either for the full help message and parsing config into a Node_options, or for purposes
   * of printout of the state of an existing Node_options object (e.g., when printing
   * Node_options to an `ostream`).
   *
   * ### `printout_only` case ###
   * Will generate an #Options_description with "defaults" set to values from a
   * `defaults_source` object (which should be the object you're trying to output to a stream), the
   * names of each option, no detailed option description, and no target for each value should the
   * caller try to actually parse using `*opts_desc` (which he should not).  This is great when
   * printing the current contents of a given Node_options (it just reuses the
   * boost.program_options machinery for this purpose).
   *
   * ### `!printout_only` case ###
   * Will generate an #Options_description with "defaults" set to values from a
   * `defaults_source` object (which should be a default-constructed Node_options), the
   * names of each option, detailed option descriptions, and each option's target inside `*target`,
   * should the caller actually parse using `*opts_desc`.  This is useful for parsing the options set
   * from a config file or command line.
   *
   * This is the main specification for the different possible options.
   *
   * @param opts_desc
   *        The #Options_description object to load.
   * @param target
   *        If `!printout_only`, and the user parses using `*opts_desc`, the parsed values will end up
   *        in members of `*target`.
   * @param defaults_source
   *        The defaults listed (and used, if `!printout_only`, and you choose to parse using
   *        `*opts_desc`) for each option.
   * @param printout_only
   *        See above.
   */
  static void setup_config_parsing_helper(Options_description* opts_desc,
                                          Node_options* target,
                                          const Node_options& defaults_source,
                                          bool printout_only);

}; // struct Node_options

// Free functions: in *_fwd.hpp.

// However the following refer to inner type(s) and hence must be declared here and not _fwd.hpp.

/**
 * Deserializes a Peer_socket_options::Congestion_control_strategy_choice `enum` from
 * a standard input stream.  Reads a single space-delimited token
 * from the given stream.  Maps that token to an aforementioned enumeration value.
 * If the token is not recognized or cannot be read, some reasonable
 * default strategy is chosen.  This enables a few key things to
 * work, including parsing from config file/command line via and conversion from `string` via `boost::lexical_cast`.
 *
 * @relatesalso Peer_socket_options
 *
 * @param is
 *        Stream from which to deserialize.
 * @param strategy_choice
 *        Reference to `enum` value which to set to the mapped strategy.
 * @return `is`.
 *
 * @internal
 * @todo Peer_socket_options::Congestion_control_strategy_choice stream inserter `<<` and `>>` operators should use
 * the flow::util::istream_to_enum() pattern which is much easier than the overwrought old thing in there now involving
 * two `map`s.  Perhaps add a generic `enum_to_strings()` to provide the body for
 * net_flow::Congestion_control_selector::get_ids().
 */
std::istream& operator>>(std::istream& is,
                         Peer_socket_options::Congestion_control_strategy_choice& strategy_choice);

/**
 * Serializes a Peer_socket_options::Congestion_control_strategy_choice `enum` to a standard `ostream` -- the reverse of
 * operator>>(). Writes a space-less token to the given stream based on the given strategy `enum` value.  This
 * enables a few key things to work, including output of defaults and values in the help via
 * Peer_socket_options, and conversion to `string` via `boost::lexical_cast`.
 *
 * @relatesalso Peer_socket_options
 *
 * @param os
 *        Stream to which to serialize.
 * @param strategy_choice
 *        Value to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os,
                         const Peer_socket_options::Congestion_control_strategy_choice& strategy_choice);


} // namespace flow::net_flow
