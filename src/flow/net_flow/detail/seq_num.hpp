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

#include "flow/net_flow/detail/net_flow_fwd.hpp"
#include "flow/log/log.hpp"

namespace flow::net_flow
{

/**
 * An internal `net_flow` sequence number identifying a piece of data.  While it would not be
 * hard to just deal with raw sequence numbers everywhere, it seemed prudent to wrap the raw number
 * in a light-weight object in order to make the interface abstract and be able to change the
 * implementation later.  For example, we may change the size of the number and how wrapping works
 * (and even whether there is wrapping), at which point we'd rather only change this class's
 * internals as opposed to code all over the place.  For example, operator+() may change from not
 * worrying about wrapping to worrying about it (and similarly for operator-()).
 *
 * The companion nested class Generator is used to generate the initial sequence number (ISN) for a
 * connection.
 *
 * What does one Sequence_number represent?  A byte or a packet?  This class is intentionally
 * ignorant of this.  It just deals with a number on the number line.  The user of the class is
 * responsible for such matters.
 *
 * ### Metadata ###
 *
 * Sequence numbers being used for a given purpose always have certain features to make them useful.
 * The following is a (n arguably overfly formal and detailed) discussion of those features and how
 * we support them via "metadata."
 *
 *   - Each sequence number line has a "zero" point Z, which is a sequence number such that it is more
 *     useful to specify a given absolute sequence number as (S - Z) with the corresponding explicit sign.
 *     So S = Z is written as =0, S > Z (most typical) is +M, S < Z (typically invalid)
 *     is -M, where M means magnitude, with M > 0.
 *   - Sometimes, in particular when individual sequence numbers represent individual sequential bytes in
 *     non-stop traffic (i.e., where user has more bytes than bandwidth can pass at any time, so there are
 *     always more bytes), and when the traffic is packetized into blocks of size B, it is convenient to
 *     specify the magnitude M as a multiple of B. This can be written as xN%R -- or just xN when R = 0 --
 *     where N = M / B, R = M % B.  If one chooses zero point Z just right (so that S = 0 represents the very
 *     first byte in the entire sequence), and there is so much traffic that there are no incomplete
 *     packets (i.e., of length < B) formed, then edge sequence numbers and packet (and multi-packet) ranges
 *     become extremely simple, small, convenient.
 *   - Consider Flow DATA/ACK sequence numbers in particular. The ISN is the value Si chosen for SYN message;
 *     therefore the very first byte has absolute number (Si + 1).  So let's set Z = (Si + 1). If only full
 *     packets are ever sent (which would be true if the Send buffer is always kept full by user, i.e.,
 *     pipe < user data supply, quite typical in test applications), then every single packet will have
 *     sequence number range [+x`N`, +x`(N + 1)`) (where `N` >= 0); and every single contiguous-packet range
 *     of I = 1 or more packets will thus have form [+x`N`, +x`(N + I)`).  Of course, B = max-block-size.
 *     - If one were to use absolute numbers instead, they would be incredibly far from zero and also multiples
 *       of many bytes (often that number, B, being something annoying like 8192 or even some odd number).
 *       That's a far cry from literally 0, 1, 2, ... (with %remainder kicking in, if an incomplete packet
 *       of DATA is generated for some reason).
 *     - CAUTION!  It's tempting -- for this particular application (or in TCP sequence numbers, as well) --
 *       to set Z = ISN.  However, the ISN, by convention, equals *1 less* than the first actual DATA
 *       byte's sequence number.  Always remember to set Z = ISN + 1 -- *even inside the ISN itself*!
 *   - Every sequence number line has some qualitative purpose; e.g., "Remote DATA-received sequence numbers" or
 *     "Local ACK-received sequence numbers."  This can be identified by a single letter prefix places in front
 *     of a number on the line.  E.g., a full number might look like: L-x23%1, which means S = Z - (23 x B + 1).
 *     The prefix is called "number line ID."
 *     - Note that Z and B themselves are hidden.  If computing S is ever necessary, one might have to hunt for
 *       it (or Z, B) in prior logs.  This is a cost that would be paid for the conciseness of L+x2 (for example)
 *       instead of 1285561384998534.
 *
 * Sequence_number has optional support for the above conventions.  While #m_num stores the basic absolute value S,
 * we also store the following, called *metadata*:
 *
 *   - Zero point, Z (#m_zero_point_num).
 *     - Z = 0 = default, is allowed.
 *       - However, by convention sign will be omitted from `ostream`-serialization -- since raw number is unsigned,
 *         and no non-zero numbers less than 0 exist.
 *       - Recall, also, that S = 0 itself is reserved/illegal by convention.
 *   - Multiple size, B (#m_multiple_size)
 *     - 0 = default, "none chosen".
 *       - `ostream`-serialization will avoid mentioning multiples/remainders.
 *   - Number line ID (#m_num_line_id).
 *     - `char(0)` = default, "none chosen".
 *       - `ostream`-serialization will simply feature no prefix.
 *
 * Use set_metadata() to set the full set of metadata.  For maximum effectiveness, consider these tips -- especially
 * vis-a-vis DATA/ACK-pertinent sequence numbers.
 *
 *   - Consider not calling set_metadata() on an incoming packet's various sequence number members until it has been
 *     logged (if appropriate) once.  Thus the raw number is available *if* necessary (hopefully never).
 *   - Remember that, by Flow-protocol (and, arguably, TCP) convention, Z = ISN + 1.  Z =/= ISN.  See above for details.
 *     - Even for the Sequence_number storing the ISN; actually, *especially* that one (see below re. copying)!
 *   - Importantly, every copy (`operator=()`, copy constructor),
 *     including many implicit invocations thereof (`operator+()`, associative storage, etc.), will always copy
 *     the metadata too.  Be sure to rely on this strongly!  If you set an ISN with its Z = ISN + 1 and B = socket's
 *     max-block-size; and then all other Sequence_number objects are copies (direct or otherwise) thereof, then
 *     they should have the proper metadata and will be beautifully logged as a result.  So just set the induction
 *     base and let the machinery work.
 *   - Metadata are not the actual data (#m_num = actual data) and absolutely do not participate in any
 *     comparison, arithmetic, or hash operations (which means they're also 100% ignored in associative lookups).
 *     So algorithms will work even if there's some bug with metadata.
 *
 * ### Thread safety ###
 * Equally thread-safe as built-in types (not safe to read/write or write/write one
 * object simultaneously).
 *
 * Implementation notes
 * --------------------
 *
 * Generator and Sequence_number work based on the same assumptions (like the
 * structure of the sequence number space), and thus their implementations may be related.  So if
 * one is changed the other may also need to be changed (internally).
 */
class Sequence_number
{
public:
  // Types.

  // See below.
  class Generator;

  /**
   * Raw sequence number type.  64 bits used because sequence number wrap-around would take many
   * centuries at high network speeds to occur if the initial sequence number is picked to be
   * sufficiently small.  See also Generator::S_MAX_INIT_SEQ_NUM.
   */
  using seq_num_t = uint64_t;

  /**
   * Integer type used to express differences (distances) between Sequence_numbers.
   *
   * @note This type can overflow for extremely large sequence number differences (those spanning
   *       over half of the range).  This should not usually be a concern in practice.
   *
   * @todo Allowing overflow by using this definition of #seq_num_delta_t is somewhat unlike the rest
   * of the implementation of Sequence_number which avoids overflow like the plague.  Should we do something
   * like use `double` or a big-integer type from boost.multiprecision?
   */
  using seq_num_delta_t = int64_t;

  // Constructors/destructor.

  /// Constructs sequence number that is zero (not a valid sequence number; less than all others).
  explicit Sequence_number();

  /**
   * Copy constructor.  Identical to calling: `operator=(source);`.
   *
   * Implementation detail: Default implementation used.  This is basically here for documentation purposes.
   *
   * @param source
   *        Source object.
   */
  Sequence_number(const Sequence_number& source);

  // Methods.

  /**
   * Copy operator.  Copies both the data (the essential absolute value) and all the metadata.
   *
   * Implementation detail: Default implementation used.  This is basically here for documentation purposes.
   *
   * @param source
   *        Source object.  `*this` is allowed but pointless.
   * @return `*this`.
   */
  Sequence_number& operator=(const Sequence_number& source);

  /**
   * Returns true if and only if `*this != Sequence_number{}` (i.e., is non-zero).
   *
   * @return Ditto.
   */
  bool valid() const;

  /**
   * Returns the distance from `*this` to `rhs`.  This may be negative, meaning `rhs > *this`.
   *
   * @note This will overflow if the difference is extremely large (over half of the sequence number
   *       range).  This should not usually be a concern in practice... but see to-do on #seq_num_delta_t.
   * @param rhs
   *        Object to compare.
   * @return See above.
   */
  seq_num_delta_t operator-(const Sequence_number& rhs) const;

  /**
   * Returns new sequence number that is `*this` advanced (or reversed) by the given distance.
   *
   * @param delta
   *        How much to advance (or reverse, if negative) the result.
   * @return See above.
   */
  Sequence_number operator+(seq_num_delta_t delta) const;

  /**
   * Equivalent to `operator+(-delta)`.
   *
   * @param delta
   *        How much to reverse (or advance, if negative) the result.
   * @return See above.
   */
  Sequence_number operator-(seq_num_delta_t delta) const;

  /**
   * Advances (or reverses) this sequence number by the given distance.
   *
   * @param delta
   *        How much to advance (or reverse, if negative) `*this`.
   * @return `*this`.
   */
  Sequence_number& operator+=(seq_num_delta_t delta);

  /**
   * Equivalent to: `operator+=(-delta)`.
   *
   * @param delta
   *        How much to reverse (or advance, if negative) *this.
   * @return *this.
   */
  Sequence_number& operator-=(seq_num_delta_t delta);

  /**
   * Whether `*this` is the same sequence number as `rhs`.
   *
   * @param rhs
   *        Object to compare.
   * @return See above.
   */
  bool operator==(const Sequence_number& rhs) const;

  /**
   * Return `!(*this == rhs)`.
   *
   * @param rhs
   *        Object to compare.
   * @return See above.
   */
  bool operator!=(const Sequence_number& rhs) const;

  /**
   * Whether `*this` is less than `rhs`.
   *
   * @param rhs
   *        Object to compare.
   * @return See above.
   */
  bool operator<(const Sequence_number& rhs) const;

  /**
   * Return `rhs < *this`.
   *
   * @param rhs
   *        Object to compare.
   * @return See above.
   */
  bool operator>(const Sequence_number& rhs) const;

  /**
   * Return `!(*this > rhs)`.
   *
   * @param rhs
   *        Object to compare.
   * @return See above.
   */
  bool operator<=(const Sequence_number& rhs) const;

  /**
   * Return `rhs <= *this`.
   *
   * @param rhs
   *        Object to compare.
   * @return See above.
   */
  bool operator>=(const Sequence_number& rhs) const;

  /**
   * Hash value of this Sequence_number for `unordered_*<>`.
   *
   * @return Ditto.
   */
  size_t hash() const;

  /**
   * Provides the raw sequence number.  This method is primarily intended for serialization.
   *
   * @return The raw sequence number.
   */
  const seq_num_t& raw_num_ref() const;

  /**
   * Sets the raw sequence number. This method is primarily intended for deserialization.
   *
   * @param num
   *        The raw sequence number.
   */
  void set_raw_num(seq_num_t num);

  /**
   * Updates the full set of metadata (used at least for convenient convention-based logging but not actual algorihtms)
   * for this number.
   *
   * See class Sequence_number doc header for important tips and even more detail before using this
   * surprisingly powerful utility.  It's all quite intuitive, but there are various nuances one might not expect.
   *
   * @note Metadata propagate via copy (copy construction, `operator=()`, and many implicit calls thereto), so it
   *       is important to call this method with final values before the propagating begins.  See class Sequence_number
   *       doc header for discussion.
   * @param num_line_id
   *        A 1-character descriptive identifier (conventionally, an upper-case letter such as L for Local) for
   *        the number line of which this Sequence_number is a part.  `char(0)` means "none" or "unknown" and is
   *        default at construction.
   * @param zero_point
   *        The Sequence_number `Z` such that the relative sequence number is expressed depending on
   *        `this->m_num` versus `Z.m_num`; its sign is +, -, or = if the latter is less than, greater than, or equal
   *        to the former, respectively; its magnitude is the absolute value of their difference.
   *        0 is legal, but see class Sequence_number for nuance(s).  0 is default at construction.
   * @param multiple_size
   *        The block size, or multiple size, which is a positive value given when data are generally expected to be
   *        segmented in whole multiples of this many contiguous Sequence_numbers.  0 means "data are not expected to
   *        be such multiples" or "unknown"; this is the default at construction.
   */
  void set_metadata(char num_line_id = 0,
                    const Sequence_number& zero_point = Sequence_number{},
                    seq_num_delta_t multiple_size = 0);

private:
  // Friends.

  // Friend of Sequence_number: For access to our internals.
  friend std::ostream& operator<<(std::ostream& os, const Sequence_number& seq_num);

  // Data.

  /// The raw sequence number.  This is the only datum used in algorithms.  The others are only for logging and similar.
  seq_num_t m_num;

  /// Identifies the owner number line; `char(0)` = unknown/none.  See set_metadata().
  char m_num_line_id;

  /// Value for `m_num` such that the magnitude is zero.  See set_metadata().
  seq_num_t m_zero_point_num;

  /// Expected size of a full contiguous "block" of these numbers; 0 = unknown/blocks not expected.  See set_metadata().
  seq_num_delta_t m_multiple_size;
}; // class Sequence_number

/**
 * An object of this type generates a series of initial sequence numbers (ISN) that are meant to be
 * sufficiently secure to protect against ISN attacks and confusion among different connections
 * between the same endpoints occurring in series.
 *
 * The basic use case is to construct a Generator at the beginning of a Node's operation to
 * initialize its state and then call generate_init_seq_num() whenever an ISN is needed.
 *
 * ### Thread safety ###
 * Not safe to read/write or write/write one object simultaneously.
 */
class Sequence_number::Generator :
  public log::Log_context,
  private boost::noncopyable
{
public:
  // Constructors/destructor.

  /**
   * Constructs Generator.
   *
   * @param logger
   *        Logger to use subsequently.
   */
  explicit Generator(log::Logger* logger);

  // Methods.

  /**
   * Returns an initial sequence number (ISN) for use in a new connection.
   *
   * @return Ditto.
   */
  Sequence_number generate_init_seq_num();

private:
  // Constants.

  /**
   * The maximum allowed value for the initial sequence number (ISN) for a given Flow-protocol connection
   * (Peer_socket).  This should be high enough to allow a huge range of possible initial sequence
   * numbers (to guard against ISN attacks, for example); but low enough to ensure that sequence
   * numbers following the ISN within the given connection would take an extremely huge amount of
   * time to wrap (i.e., overflow Sequence_numer::seq_num_t).  If such a value is chosen, the ISN can be secure and
   * remove the need to worry about wrapping as well.
   */
  static const seq_num_t S_MAX_INIT_SEQ_NUM;

  /// The ISN given out at a given time should increment every N; this is the value of  N.
  static const Fine_duration S_TIME_PER_SEQ_NUM;

  /**
   * In addition to the actual time passed between two ISN generations, pretend this much additional
   * time has also passed.
   */
  static const Fine_duration S_MIN_DELAY_BETWEEN_ISN;

  // Data.

  /// The last initial sequence number returned by generate_init_seq_num() (or zero if never called).
  Sequence_number m_last_init_seq_num;

  /// #Fine_clock time of the last invocation of generate_init_seq_num() (or default if never called).
  Fine_time_pt m_last_isn_generation;
}; // class Sequence_number::Generator

// Free functions: in *_fwd.hpp.

} // namespace flow::net_flow
