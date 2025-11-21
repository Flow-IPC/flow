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
#include "flow/net_flow/options.hpp"
#include "flow/net_flow/detail/cong_ctl.hpp"
#include <boost/algorithm/string.hpp>

// Internal macros (#undef at the end of file).

/// @cond
/* -^- Doxygen, please ignore the following.  (Don't want docs generated for temp macro; this is more maintainable
 * than specifying the macro name to omit it, in Doxygen-config EXCLUDE_SYMBOLS.) */

#define ADD_CONFIG_OPTION(ARG_opt, ARG_desc) \
  Node_options::add_config_option(opts_desc, #ARG_opt, &target->ARG_opt, defaults_source.ARG_opt, ARG_desc, \
                                  printout_only)

// -v- Doxygen, please stop ignoring.
/// @endcond

namespace flow::net_flow
{

// Implementations.

Node_options::Node_options() :
  // They definitely need to opt into this.
  m_st_capture_interrupt_signals_internally(false),
  /* OS is under no obligation to take this literally and will probably impose its own limit which
   * may be smaller than this.  In Linux 2.6.x I've observed the limit can't go higher than somewhere
   * between 1MB and 2MB.  Anyway, we pick a reasonable value and hope it has some effect. */
  m_st_low_lvl_max_buf_size(3 * 1024 * 1024),
  // Let the code pick something reasonable based on platform, hopefully.
  m_st_timer_min_period(0),
  // Allow unlimited # of packets per loop iteration for now.
  m_dyn_max_packets_per_main_loop_iteration(0),
  // default max_block_size + a SMALL overhead.
  m_dyn_low_lvl_max_packet_size(1124),
  // This default is explained in the option description (as of this writing): it's faster.
  m_dyn_guarantee_one_low_lvl_in_buf_per_socket(true)
{
  // Nothing.
}

template<typename Opt_type>
void Node_options::add_config_option(Options_description* opts_desc,
                                     const std::string& opt_id,
                                     Opt_type* target_val, const Opt_type& default_val,
                                     const char* description, bool printout_only) // Static.
{
  using boost::program_options::value;
  if (printout_only)
  {
    opts_desc->add_options()
      (opt_id_to_str(opt_id).c_str(), value<Opt_type>()->default_value(default_val));
  }
  else
  {
    opts_desc->add_options()
      (opt_id_to_str(opt_id).c_str(), value<Opt_type>(target_val)->default_value(default_val),
       description);
  }
}

void Node_options::setup_config_parsing_helper(Options_description* opts_desc,
                                               Node_options* target,
                                               const Node_options& defaults_source,
                                               bool printout_only) // Static.
{
  ADD_CONFIG_OPTION
    (m_st_capture_interrupt_signals_internally,
     "If and only if this is true, the Node will detect SIGINT and SIGTERM (or your OS's version thereof); "
       "upon seeing such a signal, it will fire Node::interrupt_all_waits(), which will interrupt all "
       "blocking operations, conceptually similarly to POSIX's EINTR. If this is true, the user may register "
       "their own signal handler(s) (for any purpose whatsoever) using boost::asio::signal_set. However, behavior "
       "is undefined if the program registers signal handlers via any other API, such as sigaction() or signal().  "
       "If you need to set up such a non-signal_set signal handler, AND you require EINTR-like behavior, "
       "then (1) set this option to false; (2) trap SIGINT and SIGTERM yourself; (3) in your handlers for the "
       "latter, simply call Node::interrupt_all_waits(). Similarly, if you want custom behavior regarding "
       "Node::interrupt_all_waits(), feel free to call it whenever you want (not necessarily even from a signal "
       "handler), and set this to false. However, if a typical, common-sense behavior is what you're after -- and "
       "either don't need additional signal handling or are OK with using signal_set for it -- then setting this to "
       "true is a good option.");
  ADD_CONFIG_OPTION
    (m_st_low_lvl_max_buf_size,
     "The max size to ask the OS to set our UDP socket's receive buffer to in order to minimize loss "
       "if we can't process datagrams fast enough.  This should be as high as possible while being "
       "\"acceptable\" in terms of memory.  However, the OS will probably have its own limit and may well "
       "pick a limit that is the minimum of that limit and this value.");
  ADD_CONFIG_OPTION
    (m_st_timer_min_period,
     "A time period such that the boost.asio timer implementation for this platform is able to "
       "accurately able to schedule events within this time period or greater.  If you select 0, the "
       "code will decide what this value based on the platform, but its logic for this may or may not "
       "be correct (actually it will probably be correct but possibly too conservative [large], causing "
       "timer coarseness in mechanisms like packet pacing).");
  ADD_CONFIG_OPTION
    (m_dyn_max_packets_per_main_loop_iteration,
     "The UDP net-stack may deliver 2 or more datagrams to the Flow Node at the same time.  To lower overhead "
       "and increase efficiency, Flow will process all such datagrams -- and any more that may arrive during this "
       "processing -- before preparing any resulting outgoing messages, such as acknowledgments or more data packets.  "
       "In high-speed conditions this may result in excessive burstiness of outgoing traffic.  This option's value "
       "places a limit on the number of datagrams to process before constructing and sending any resulting outgoing "
       "messages to prevent this burstiness.  If 0, there is no limit.");
  ADD_CONFIG_OPTION
    (m_dyn_low_lvl_max_packet_size,
     "Any incoming low-level (UDP) packet will be truncated to this size.  This should be well above "
       "per-socket max-block-size (# of bytes of user payload per DATA packet).  There will only be one buffer "
       "of this size in memory at a time, so no need to be too stingy, but on the other hand certain "
       "allocation/deallocation behavior may cause performance drops if this unnecessarily large.");
  ADD_CONFIG_OPTION
    (m_dyn_guarantee_one_low_lvl_in_buf_per_socket,
     "This very inside-baseball setting controls the allocation/copy behavior of the UDP receive-deserialize "
       "operation sequence.  When enabled, there is exactly one input buffer large enough to "
       "hold any one serialized incoming packet; any deserialized data (including DATA and ACK payloads) are "
       "stored in separately allocated per-packet buffers; and and the input buffer is repeatedly reused "
       "without reallocation.  When disabled, however, at least some packet types (most notably DATA) will "
       "use the zero-copy principle, having the deserializer take ownership of the input buffer and access pieces "
       "inside it as post-deserialization values (most notably the DATA payload); in this case the input buffer "
       "has to be reallocated between UDP reads.  As of this writing the former behavior seems to be somewhat "
       "faster, especially if low-lvl-max-packet-size is unnecessarily large; but arguably the zero-copy behavior "
       "may become faster if some implementation details related to this change.  So this switch seemed worth "
       "keeping.");

  Peer_socket_options::setup_config_parsing_helper(opts_desc,
                                                   &target->m_dyn_sock_opts,
                                                   defaults_source.m_dyn_sock_opts,
                                                   printout_only);
} // Node_options::setup_config_parsing_helper()

void Node_options::setup_config_parsing(Options_description* opts_desc)
{
  // Set up *opts_desc to parse into *this when the caller chooses to.  Take defaults from *this.
  setup_config_parsing_helper(opts_desc, this, *this, false);
} // Node_options::setup_config_parsing()

std::ostream& operator<<(std::ostream& os, const Node_options& opts)
{
  Node_options sink;
  Node_options::Options_description opts_desc{"Per-net_flow::Node option values"};
  Node_options::setup_config_parsing_helper(&opts_desc, &sink, opts, true);
  return os << opts_desc;
}

std::string Node_options::opt_id_to_str(const std::string& opt_id) // Static.
{
  using boost::algorithm::starts_with;
  using boost::algorithm::replace_all;
  using std::string;

  const string STATIC_PREFIX = "m_st_";
  const string DYNAMIC_PREFIX = "m_dyn_";

  string str = opt_id;
  if (starts_with(opt_id, STATIC_PREFIX))
  {
    str.erase(0, STATIC_PREFIX.size());
  }
  else if (starts_with(opt_id, DYNAMIC_PREFIX))
  {
    str.erase(0, DYNAMIC_PREFIX.size());
  }

  replace_all(str, "_", "-");

  return str;
}

Peer_socket_options::Peer_socket_options() :
  /* Should be a safe UDP datagram size.  @todo Not a totally solid statement... plus it's too low;
   * increasing to ~1500 increases throughput hugely.  Choosing it dynamically is a @todo elsewhere
   * in the code. */
  m_st_max_block_size(1024),
  // Initial value recommended by RFC 6298 is 1 sec but seems too conservative.  @todo Decide.
  m_st_connect_retransmit_period(boost::chrono::milliseconds{125}),
  // @todo.
  m_st_connect_retransmit_timeout(boost::chrono::seconds{3}),
  /* @todo Reconsider.  See also to-do in class Node doc header.
   * WARNING!  If you change this, ensure s_st_max_cong_wnd_blocks is still sufficiently small. */
  m_st_snd_buf_max_size(6 * 1024 * 1024),
  // @todo Ditto.
  m_st_rcv_buf_max_size(m_st_snd_buf_max_size),
  // Disabling flow control is an emergency measure only.
  m_st_rcv_flow_control_on(true),
  // Seems reasonable.  Should be a few hundred KB typically.
  m_st_rcv_buf_max_size_slack_percent(10),
  /* The value 50% is taken from BSD implementation (Stevens/Wright, TCP/IP Illustrated Vol. 2,
   * 1995), although it's barely discussed there.  It does make sense though; it should give the
   * sender a large window to work with but also leave some time to allow our application layer to
   * read off more bytes from Receive buffer after advertising the window at this mark. */
  m_st_rcv_buf_max_size_to_advertise_percent(50),
  /* This allows for about 2.2 Received_packet struct per maximally large packet in
   * m_rcv_packets_with_gaps.  So it's something like 10-20 bytes per 1k of DATA, or about 2-4%
   * overhead.  So it's conservative in terms of memory use.  For any reasonably large max Receive
   * buffer size, it's also at least 100 packets.  If 100 packets arrive numbering higher than
   * unreceived packet P, it's a VERY good bet P will never arrive.  Thus it's also conservative in
   * terms of loss.  Finally, since this formula allows enough space to hold an entire
   * m_rcv_syn_rcvd_data_q's worth of queued up packets, it won't throw away any queued up
   * packets in SYN_RCVD state. */
  m_st_rcv_max_packets_after_unrecvd_packet_ratio_percent(220),
  // Satisfies RFC 5681 (500 ms max); taken from BSD implementation (Stevens/Wright, TCP/IP Illustrated Vol. 2, 1995).
  m_st_delayed_ack_timer_period(boost::chrono::milliseconds{200}),
  // Per RFC 5681.
  m_st_max_full_blocks_before_ack_send(2),
  m_st_rexmit_on(true),
  // @todo Experiment and look at RFCs.
  m_st_max_rexmissions_per_packet(15),
  // @todo Experiment.  RFC 6298 recommends this value.
  m_st_init_drop_timeout(boost::chrono::seconds{1}),
  // @todo Experiment.  Choosing less aggressive values for now, except for m_st_drop_all_on_drop_timeout.
  m_st_drop_packet_exactly_after_drop_timeout(false),
  // Consistent with RFC 4341, but see discussion where it's used.
  m_st_drop_all_on_drop_timeout(true),
  // @todo Experiment.  Choosing less aggressive values for now, except for m_st_drop_all_on_drop_timeout.
  m_st_out_of_order_ack_restarts_drop_timer(true),
  // @todo Experiment.
  m_st_snd_pacing_enabled(true),
  /* Value taken from Linux's westwood.c which was written by the creators of Westwood+ bandwidth
   * estimation algorithm themselves. 50 msec seems like a reasonable line in the sand between
   * "small RTT" and "medium RTT." */
  m_st_snd_bandwidth_est_sample_period_floor(boost::chrono::milliseconds{50}),
  // Pass in a non-existent strategy ID, which will cause our operator<<() to choose what it considers the default.
  m_st_cong_ctl_strategy(boost::lexical_cast<Congestion_control_strategy_choice>("none")),
  // Let code choose initial CWND using RFC 5681 method.
  m_st_cong_ctl_init_cong_wnd_blocks(0),
  /* See the constraints in the doc header for this constant.  That said, here are the calculations I
   * used to get 640:
   *
   * this option = B * RTT / max-block-size = 100 Mbits/sec * 50 msec * 1/1024 blocks/byte = 100
   * Mbits/sec * 50 msec * 1/1000 sec/msec * 1/8 Mbytes/Mbits * 1024*1024 bytes/Mbytes / 1024 bytes
   * = (100*50/1000/8*1024*1024/1024) blocks = 640 blocks.  (Note the units cancel out properly.)
   *
   * Receive buffer is currently 6 Mbytes.  6 Mbytes / 640 blocks = 6 * 1024 * 1024 / (640 * 1024),
   * which is a Receive buffer-to-CWND ratio of 9.6s.
   *
   * WARNING!  If you change this, ensure m_st_max_rcv_buf_size is still sufficiently large. */
  m_st_cong_ctl_max_cong_wnd_blocks(640),
  // Per RFC 5681-3.1.
  m_st_cong_ctl_cong_wnd_on_drop_timeout_blocks(1),
  // Use RFC 5681 default (0 is special value).
  m_st_cong_ctl_cong_avoidance_increment_blocks(0),
  // Use RFC 5681 default (0 is special value).
  m_st_cong_ctl_classic_wnd_decay_percent(0),
  // The minimal allowed ceiling by RFC 6298.
  m_dyn_drop_timeout_ceiling(boost::chrono::seconds{60}),
  // RFC 6298 recommends this value.
  m_dyn_drop_timeout_backoff_factor(2),
  // This shouldn't be too bad.  @todo Though it should probably be based off DTO or something....
  m_dyn_rcv_wnd_recovery_timer_period(boost::chrono::seconds{1}),
  // Seems OK.  After a minute it's probably a lost cause.
  m_dyn_rcv_wnd_recovery_max_period(boost::chrono::minutes{1})
{
  // Nothing.
}

void Peer_socket_options::setup_config_parsing_helper(Options_description* opts_desc,
                                                      Peer_socket_options* target,
                                                      const Peer_socket_options& defaults_source,
                                                      bool printout_only) // Static.
{
  using std::vector;
  using std::string;
  using boost::algorithm::join;

  // Build the comma-separated list of possible string values for enum m_st_cong_ctl_strategy, for help output.
  vector<string> cong_strategy_ids;
  Congestion_control_selector::get_ids(&cong_strategy_ids);
  const string str_cong_strategy_ids = join(cong_strategy_ids, ", ");

  ADD_CONFIG_OPTION
    (m_st_max_block_size,
     "The size of block that we will strive to (and will, assuming at least that many bytes are "
       "available in Send buffer) pack into each outgoing DATA packet.  It is assumed the other side is "
       "following the same policy (any packets that do not -- i.e., exceed this size -- are dropped). "
       "This is an important control; the higher it is the better for performance AS LONG AS it's not so "
       "high it undergoes IP fragmentation (or does that even happen in UDP? if not, even worse -- "
       "it'll just be dropped and not sent!).  The performance impact is major; e.g., assuming no "
       "fragmentation/dropping, we've seen a ~1500 byte MBS result in 20-30% higher throughput than "
       "1024 byte MBS.  "
       "Additionally, if using net_flow module with no reliability feature -- i.e., if you want to perform FEC "
       "or something else outside the Flow protocol -- then it is absolutely essential that EVERY send*() call "
       "provides a buffer whose size is a multiple of max-block-size.  Otherwise packet boundaries "
       "will not be what you expect, and you will get what seems to be corrupted data at the "
       "application layer (since our stream-based API has no way of knowing where your message begins "
       "or ends).  Alternatively you can encode message terminators or packet sizes, but since in "
       "unreliable mode integrity of a given block is guaranteed only if all blocks align with "
       "max-block-size boundaries, you'll still probably be screwed.");
  ADD_CONFIG_OPTION
    (m_st_connect_retransmit_period,
     "How often to resend SYN or SYN_ACK while SYN_ACK or SYN_ACK_ACK, respectively, has not been received.  "
       "In other words, this controls the pause between retries during the connection opening phase, by either side, "
       "if the other side is not responding with the appropriate response.  "
       "Examples: \"250 ms\", \"1 s\".");
  ADD_CONFIG_OPTION
    (m_st_connect_retransmit_timeout,
     "How long from the first SYN or SYN_ACK to allow for connection handshake before aborting connection.  "
       "Examples: \"5500 ms\", \"60 seconds\".");
  ADD_CONFIG_OPTION
    (m_st_snd_buf_max_size,
     "Maximum number of bytes that the Send buffer can hold.  This determines how many bytes user can "
       "send() while peer cannot send over network until send() refuses to take any more bytes.  Note "
       "that any value given will be increased, if necessary, to the nearest multiple of "
       "max-block-size.  This is important to preserve message boundaries when operating in "
       "unreliable mode (guaranteed max-block-size-sized chunks of data will be sent out in their "
       "entirety instead of being fragmented).");
  ADD_CONFIG_OPTION
    (m_st_rcv_buf_max_size,
     "Maximum number of bytes that the Receive buffer can hold.  This determines how many bytes "
       "can be received in the background by the Node without user doing any receive()s.  "
       "It is also rounded up to to the nearest multiple of max-block-size.");
  ADD_CONFIG_OPTION
    (m_st_rcv_flow_control_on,
     "Whether flow control (a/k/a receive window a/k/a rcv_wnd management) is enabled.  If this is "
       "disabled, an infinite rcv_wnd will always be advertised to the sender; so if the Receive buffer "
       "is exceeded packets are dropped as normal, but the sender will not know it should stop sending "
       "until Receive buffer space is freed.  If this is enabled, we keep the sender informed of how "
       "much Receive buffer space is available, so it can suspend the flow as necessary.");
  ADD_CONFIG_OPTION
    (m_st_rcv_buf_max_size_slack_percent,
     "% of rcv-buf-max-size such that if Receive buffer stores up to (100 + this many) % of "
       "rcv-buf-max-size bytes, the bytes will still be accepted.  In other words, this allows the max "
       "Receive buffer to hold slightly more than rcv-buf-max-size bytes.  However, the current Receive "
       "buffer capacity advertised to the other side of the connection will be based on the "
       "non-inflated rcv-buf-max-size.  This option provides some protection against the fact that the "
       "receive window value sent to the other side will lag behind reality somewhat.");
  ADD_CONFIG_OPTION
    (m_st_rcv_buf_max_size_to_advertise_percent,
     "% of rcv-buf-max-size that has to be freed, since the last receive window advertisement, via "
       "user popping data from Receive buffer, before we must send a receive window advertisement.  "
       "Normally we send rcv_wnd to the other side opportunistically in every ACK; but there can be "
       "situations when there is no packets to acknowledge, and hence we must specifically make a "
       "packet just to send over rcv_wnd.  Typically we should only need to do this if the buffer was "
       "exceeded and is now significantly freed.  This value must be in [1, 100], but anything over 50 "
       "is probably pushing it.");
  ADD_CONFIG_OPTION
    (m_st_rcv_max_packets_after_unrecvd_packet_ratio_percent,
     "The limit on the size of Peer_socket::m_rcv_packets_with_gaps, expressed as what percentage the "
       "maximal size of that structure times max-block-size is of the maximal Receive buffer size.  For "
       "example, if this is 200, then Peer_socket::m_rcv_packets_with_gaps can represent up to roughly "
       "2x as many full-sized blocks as the Receive buffer can.  This should also by far exceed any "
       "sender congestion window max size to avoid packet loss.  m_rcv_packets_with_gaps consists of all packets "
       "such that at least one packet sequentially preceding them has not yet been received.");
  ADD_CONFIG_OPTION
    (m_st_delayed_ack_timer_period,
     "The maximum amount of time to delay sending ACK with individual packet's acknowledgment since "
       "receiving that individual packet.  If set to zero duration, any given individual acknowledgment "
       "is sent within a non-blocking amount of time of its DATA packet being read.  Inspired by RFC "
       "1122-4.2.3.2.  Examples: \"200 ms\", \"550200 microseconds\".");
  ADD_CONFIG_OPTION
    (m_st_max_full_blocks_before_ack_send,
     "If there are at least this many TIMES max-block-size bytes' worth of individual acknowledgments "
       "to be sent, then the delayed ACK timer is to be short-circuited, and the accumulated "
       "acknowledgments are to be sent as soon as possible.  Inspired by RFC 5681.");
  ADD_CONFIG_OPTION
    (m_st_rexmit_on,
     "Whether to enable reliability via retransmission.  If false, a detected lost packet may have "
       "implications on congestion control (speed at which further data are sent) but will not cause "
       "that packet to be resent; receiver application code either has to be OK with missing packets or "
       "must implement its own reliability (e.g., FEC).  Packets may also be delivered in an order "
       "different from the way they were sent.  If true, the receiver need not worry about it, as any "
       "lost packets will be retransmitted with no participation from the application code on either "
       "side, as in TCP.  Also as in TCP, this adds order preservation, so that the stream of bytes sent "
       "will be exactly equal to the stream of bytes received. Retransmission removes the requirement "
       "for the very exacting block-based way in which send() and friends must be called.  "
       "This option must have the same value on both sides of the connection, or the server will refuse  "
       "the connection.");
  ADD_CONFIG_OPTION
    (m_st_max_rexmissions_per_packet,
     "If retransmission is enabled and a given packet is retransmitted this many times and has to be "
       "retransmitted yet again, the connection is reset.  Should be positive.");
  ADD_CONFIG_OPTION
    (m_st_init_drop_timeout,
     "Once socket enters ESTABLISHED state, this is the value for m_snd_drop_timeout before the first RTT "
       "measurement is made (the first valid acknowledgment arrives).  Example: \"2 seconds\".");
  ADD_CONFIG_OPTION
    (m_st_drop_packet_exactly_after_drop_timeout,
     "If true, when scheduling Drop Timer, schedule it for Drop Timeout relative to the send time of "
       "the earliest In-flight packet at the time.  If false, also schedule DTO relative to the time of "
       "scheduling.  The latter is less aggressive and is recommended by RFC 6298.");
  ADD_CONFIG_OPTION
    (m_st_drop_all_on_drop_timeout,
     "If true, when the Drop Timer fires, all In-flight packets are to be considered Dropped (and "
       "thus the timer is to be disabled).  If false, only the earliest In-flight packet is to be "
       "considered Dropped (and thus the timer is to restart).  RFC 6298 recommends false.  true is more aggressive.");
  ADD_CONFIG_OPTION
    (m_st_out_of_order_ack_restarts_drop_timer,
     "If an In-flight packet is acknowledged, but it is not the earliest In-flight packet (i.e., it's "
       "an out-of-order acknowledgment), and this is true, the timer is restarted.  Otherwise the timer "
       "continues to run.  The former is less aggressive.  RFC 6298 wording is ambiguous on what it "
       "recommends (not clear if cumulative ACK only, or if SACK also qualifies).");
  ADD_CONFIG_OPTION
    (m_st_snd_bandwidth_est_sample_period_floor,
     "When estimating the available send bandwidth, each sample must be compiled over at least this long "
       "of a time period, even if the SRTT is lower.  Normally a sample is collected over at least an SRTT, but "
       "computing a bandwidth sample over a quite short time period can produce funky results, hence this floor.  "
       "Send bandwidth estimation is used at least for some forms of congestion control.");
  ADD_CONFIG_OPTION
    (m_st_snd_pacing_enabled,
     "Enables or disables packet pacing, which attempts to spread out, without sacrificing overall "
       "send throughput, outgoing low-level packets to prevent loss.  If disabled, any packet that is "
       "allowed by congestion/flow control to be sent over the wire is immediately sent to the UDP "
       "net-stack; so for example if 200 packets are ready to send and are allowed to be sent, they're sent "
       "at the same time.  If enabled, they will be spread out over a reasonable time period instead.  "
       "Excessive burstiness can lead to major packet drops, so this can really help.");
  ADD_CONFIG_OPTION
    (m_st_cong_ctl_strategy,
     (string("The congestion control algorithm to use for the connection or connections.  The choices are: [") +
      str_cong_strategy_ids + "].").c_str());
  ADD_CONFIG_OPTION
    (m_st_cong_ctl_init_cong_wnd_blocks,
     "The initial size of the congestion window, given in units of max-block-size-sized blocks.  "
       "The special value 0 means RFC 5681's automatic max-block-size-based computation should "
       "be used instead.");
  ADD_CONFIG_OPTION
    (m_st_cong_ctl_max_cong_wnd_blocks,
     "The constant that determines the CWND limit in congestion_window_at_limit() and "
       "clamp_congestion_window() (in multiple of max-block-size).  When choosing this value, use these "
       "constraints: "
       "(1) This limits total outgoing throughput.  The throughput B will be <= CWND/RTT, where RTT is "
       "roughly the RTT of the connection, and CWND == S_MAX_CONG_WND_BLOCKS * max-block-size.  "
       "Therefore, choose B and RTT values and set S_MAX_CONG_WND_BLOCKS = B * RTT / max-block-size "
       "(B in bytes/second, RTT in seconds).  "
       "(2) Until we implement Receive window, this value should be much (say, 4x) less than the size "
       "of the Receive buffer, to avoid situations where even a well-behaving user (i.e., user that "
       "receive()s all data ASAP) cannot keep up with reading data off Receive buffer, forcing "
       "net_flow to drop large swaths of incoming traffic.  If CWND is much smaller than Receive "
       "buffer size, then this avoids that problem.");
  ADD_CONFIG_OPTION
    (m_st_cong_ctl_cong_wnd_on_drop_timeout_blocks,
     "On Drop Timeout, set congestion window to this value times max-block-size.");
  ADD_CONFIG_OPTION
    (m_st_cong_ctl_cong_avoidance_increment_blocks,
     "The multiple of max-block-size by which to increment CWND in congestion avoidance mode after receiving "
       "at least a full CWND's worth of clean acknowledgments.  RFC 5681 (classic Reno) mandates this is set to 1, "
       "but we allow it to be overridden.  The special value 0 causes the RFC value to be used.");
  ADD_CONFIG_OPTION
    (m_st_cong_ctl_classic_wnd_decay_percent,
     "In classic congestion control, RFC 5681 specified the window should be halved on loss; this "
       "option allows one to use a customer percentage instead.  This should be a value in [1, "
       "100] to have the window decay to that percentage of its previous value, or 0 to use the RFC "
       "5681-recommended constant (50).");
  ADD_CONFIG_OPTION
    (m_dyn_drop_timeout_backoff_factor,
     "Whenever the Drop Timer fires, upon the requisite Dropping of packet(s), the DTO (Drop Timeout) "
       "is set to its current value times this factor, and then the timer is rescheduled accordingly.  "
       "RFC 6298 recommends 2.  Another value might be 1 (disable feature).  The lower the more "
       "aggressive.");
  ADD_CONFIG_OPTION
    (m_dyn_rcv_wnd_recovery_timer_period,
     "When the mode triggered by rcv-buf-max-size-to-advertise-percent being exceeded is in effect, "
       "to counteract the possibility of ACK loss the receive window is periodically advertised "
       "subsequently -- with the period given by this option -- until either some new data arrive or "
       "rcv-wnd-recovery-max-period is exceeded.  Example: \"5 s\".");
  ADD_CONFIG_OPTION
    (m_dyn_rcv_wnd_recovery_max_period,
     "Approximate amount of time since the beginning of rcv_wnd recovery due to "
       "rcv-buf-max-size-to-advertise-percent until we give up and end that phase.  Example: \"30 s\".");
  ADD_CONFIG_OPTION
    (m_dyn_drop_timeout_ceiling,
     "Ceiling to impose on the Drop Timeout.  Example: \"120 s\".");
} // Peer_socket_options::setup_config_parsing_helper()

void Peer_socket_options::setup_config_parsing(Options_description* opts_desc)
{
  // Set up *opts_desc to parse into *this when the caller chooses to.  Take defaults from *this.
  setup_config_parsing_helper(opts_desc, this, *this, false);
} // Node_options::setup_config_parsing()

std::ostream& operator<<(std::ostream& os, const Peer_socket_options& opts)
{
  Peer_socket_options sink;
  Peer_socket_options::Options_description opts_desc("Per-net_flow::Peer_socket option values");
  Peer_socket_options::setup_config_parsing_helper(&opts_desc, &sink, opts, true);
  return os << opts_desc;
}

#undef ADD_CONFIG_OPTION // For cleanliness.

} // namespace flow::net_flow
