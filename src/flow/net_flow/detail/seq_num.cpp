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
#include "flow/net_flow/detail/seq_num.hpp"
#include "flow/util/random.hpp"
#include <boost/functional/hash/hash.hpp>
#include <limits>
#include <cmath>

namespace flow::net_flow
{

// Static initializations.

/* Pick max ISN to be in the first half of the numeric range of sequence numbers (NOTE: not
 * half the bits, but half the numeric range); i.e., about (2^64) / 2.  This is a huge range of
 * possible ISN, and if the ISN = <this value>, then at gigabit speeds and 1 sequence number per
 * byte it would take many centuries for the sequence numbers to overflow seq_num_t. */
const Sequence_number::seq_num_t Sequence_number::Generator::S_MAX_INIT_SEQ_NUM
  = std::numeric_limits<seq_num_t>::max() / 2;
const Fine_duration Sequence_number::Generator::S_TIME_PER_SEQ_NUM = boost::chrono::microseconds{4}; // From RFC 793.
const Fine_duration Sequence_number::Generator::S_MIN_DELAY_BETWEEN_ISN
  = boost::chrono::milliseconds{500}; // From TCP/IP Illustrated Vol. 2: The Implementation (BSD Net/3).

// Implementations.

Sequence_number::Generator::Generator(log::Logger* logger_ptr) :
  log::Log_context(logger_ptr, Flow_log_component::S_NET_FLOW)
{
  // Nothing.
}

Sequence_number Sequence_number::Generator::generate_init_seq_num()
{
  using std::abs;
  using util::Rnd_gen_uniform_range;

  const Fine_time_pt& now = Fine_clock::now();

  if (m_last_init_seq_num.m_num == 0)
  {
    /* First call to this.  Just pick a random-ish ISN from the entire allowed range.  Seed on current time.
     * We only generate one random number ever (for this `this`), so it's fine to just seed it here and use once.
     * @todo Could use a `static` data member RNG.  Good randomness across multiple `this`s isn't required, so the
     * various considerations this would introduce -- multi-threadedness, for instance -- might be too much to worry
     * about given our modest, non-cryptographic needs here. */

    Rnd_gen_uniform_range<seq_num_t> rnd_single_use{1, S_MAX_INIT_SEQ_NUM}; // 0 is a reserved number; do not use.
    m_last_init_seq_num.m_num = rnd_single_use();
  }
  else
  {
    // All subsequent calls.

    /* For now basically follow RFC 793 (original TCP spec): new sequence number every N
     * microseconds, with N defined by the RFC.  Additionally, add a large constant M, as if another
     * 0.5 seconds had passed (as BSD did at least in 1995, as documented in TCP/IP Illustrated:
     * Vol. 2).  abs() should not be necessary with Fine_clock, but just in case.... */

    m_last_init_seq_num.m_num +=
      seq_num_t((now - m_last_isn_generation + S_MIN_DELAY_BETWEEN_ISN) / S_TIME_PER_SEQ_NUM);

    /* It's incredibly unlikely that overflowed seq_num_t given the times involved, but even if it
     * did, so be it.  In that case pretty much any random ISN is still OK.  So just assume no
     * overflow.... */

    // Wrap ISN if needed.  (It's perfectly possible, e.g., if original ISN was right near the end of allowed range.)
    if (m_last_init_seq_num.m_num > S_MAX_INIT_SEQ_NUM)
    {
      // 0 is a reserved number; do not use.
      m_last_init_seq_num.m_num = m_last_init_seq_num.m_num - S_MAX_INIT_SEQ_NUM;
    }
  }

  FLOW_LOG_TRACE("Generated ISN [" << m_last_init_seq_num << "].");

  m_last_isn_generation = now;
  return m_last_init_seq_num;
} // Sequence_number::Generator::generate_init_seq_num()

Sequence_number::Sequence_number() :
  m_num(0), m_num_line_id(0), m_zero_point_num(0), m_multiple_size(0)
{
  // Nothing.
}

Sequence_number::Sequence_number(const Sequence_number& source) = default;

Sequence_number& Sequence_number::operator=(const Sequence_number& source) = default;

bool Sequence_number::valid() const
{
  return m_num != 0;
}

Sequence_number::seq_num_delta_t Sequence_number::operator-(const Sequence_number& rhs) const
{
  /* This can technically overflow (since seq_num_delta_t is a signed int64_t, while the raw #s are
   * unsigned).  This is advertised in the method comment and should not usually be a concern. */
  return seq_num_delta_t(m_num - rhs.m_num);
}

Sequence_number& Sequence_number::operator+=(seq_num_delta_t delta)
{
  m_num += delta; // Can be negative.
  return *this;
}

Sequence_number& Sequence_number::operator-=(seq_num_delta_t delta)
{
  return operator+=(-delta);
}

Sequence_number Sequence_number::operator+(seq_num_delta_t delta) const
{
  return Sequence_number{*this} += delta;
}

Sequence_number Sequence_number::operator-(seq_num_delta_t delta) const
{
  return operator+(-delta);
}

bool Sequence_number::operator==(const Sequence_number& rhs) const
{
  return m_num == rhs.m_num;
}

bool Sequence_number::operator!=(const Sequence_number& rhs) const
{
  return !(*this == rhs);
}

bool Sequence_number::operator<(const Sequence_number& rhs) const
{
  return m_num < rhs.m_num;
}

bool Sequence_number::operator>(const Sequence_number& rhs) const
{
  return rhs < *this;
}

bool Sequence_number::operator>=(const Sequence_number& rhs) const
{
  return !(*this < rhs);
}

bool Sequence_number::operator<=(const Sequence_number& rhs) const
{
  return rhs >= *this;
}

std::ostream& operator<<(std::ostream& os, const Sequence_number& seq_num)
{
  // First the number line ID if any; 0 means unknown so just omit it.
  if (seq_num.m_num_line_id != 0)
  {
    os << seq_num.m_num_line_id;
  }

  const bool use_multiple = seq_num.m_multiple_size != 0; // Multiples (segmentation) is in use.
  const bool non_zero_offset = seq_num.m_zero_point_num != 0; // Magnitude is non-zero.

  /* First assume the magnitude is non-zero.  Then we show the sign, - or +.  However, if the zero point is itself 0
   * then any non-zero magnitude is positive; so simply omit the sign.  Next, show the magnitude (m_num offset by
   * m_zero_point).  If `use_multiple`, then much as we omitted the zero point value itself and just showed the sign
   * in its place, show the 'x' multiplication sign here; then show the multiple count, then "%R"; where R is the
   * remainder beyond the multiples, omitted if R = 0.  Finally, if `use_multiple`, then simply show the magnitude
   * (can think it of it as a multiple of 1, for which R = 0 always, and the 'x' is omitted).  Examples:
   * -x3 (3B to left of zero), +x2%5 (2B+5 to right of zero), x454324123 (454324123B absolute),
   * 454324123123 (454324123 absolute).
   *
   * Finally, what if magnitude IS zero?  For convenience to help interpret other numbers on the same line we use
   * this opportunity to print out the metadata set; and a simple indication that THIS value has magnitude 0.
   * Think of the format as a non-zero-magnitude value, with the "implied" (omitted) values in the previous paragraph
   * explicitly written out.  So, first is the sign, written as the zero point followed by the sign.
   * Next is the multiple, as the multiple followed by 'x'.  The coup de grace, is, simply '0', since that's what this
   * value is.  Finally, each of the two segments mentioned is omitted in the degenerate cases (zero point = 0,
   * no multiple used). Examples: 0 (!use_multiple, !non_zero_offset), 342342342+1024x0 (use_multiple, non_zero_offset),
   * 342342342+0 (!use_multiple, !non_zero_offset).
   *
   * The above is hair/formal, but try to correlate the examples in the 2 paragraphs to see how the practical output
   * makes some sense. */

  Sequence_number::seq_num_t magnitude = seq_num.m_num;
  if (magnitude == seq_num.m_zero_point_num)
  {
    // No sign => show reference values and the constant 0.
    if (non_zero_offset)
    {
      os << seq_num.m_zero_point_num << '+';
    }
    if (use_multiple)
    {
      os << seq_num.m_multiple_size << 'x';
    }
    os << '0';
  }
  else
  {
    // Yes sign => show NO reference values; show the offset vs. zero pt., possibly as a count of multiples + remainder.
    if (non_zero_offset)
    {
      // (Subtlety: avoid any underflow situation by only performing (A - B) when A > B.)
      if (magnitude > seq_num.m_zero_point_num)
      {
        os << '+';
        magnitude -= seq_num.m_zero_point_num;
      }
      else
      {
        assert(magnitude < seq_num.m_zero_point_num);
        os << '-';
        magnitude = seq_num.m_zero_point_num - magnitude;
      }
    }

    if (use_multiple)
    {
      os << 'x' << (magnitude / seq_num.m_multiple_size);
      const auto rem = magnitude % seq_num.m_multiple_size;
      if (rem != 0)
      {
        os << '%' << rem;
      }
    }
    else
    {
      os << magnitude;
    }
  }

  return os;
}

size_t Sequence_number::hash() const
{
  using boost::hash_value;
  return hash_value(m_num);
}

const Sequence_number::seq_num_t& Sequence_number::raw_num_ref() const
{
  return m_num;
}

void Sequence_number::set_raw_num(seq_num_t num)
{
  m_num = num;
}

void Sequence_number::set_metadata(char num_line_id, const Sequence_number& zero_point, seq_num_delta_t multiple_size)
{
  m_num_line_id = num_line_id;
  m_zero_point_num = zero_point.m_num;
  m_multiple_size = multiple_size;
}

size_t hash_value(const Sequence_number& seq_num)
{
  return seq_num.hash();
}

} // namespace flow::net_flow
