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
#include "flow/log/ostream_log_msg_writer.hpp"
#include "flow/log/config.hpp"

namespace flow::log
{

// Static initializations.

const std::vector<util::String_view> Ostream_log_msg_writer::S_SEV_STRS({ "null", // Never used (sentinel).
                                                                          "fatl", "eror", "warn",
                                                                          "info", "debg", "trce", "data" });
const boost::chrono::system_clock::time_point Ostream_log_msg_writer::S_POSIX_EPOCH
  = boost::chrono::system_clock::from_time_t(0);

// Implementations.

Ostream_log_msg_writer::Ostream_log_msg_writer(const Config& config, std::ostream& os) :
  m_config(config),
  m_do_log_func(m_config.m_use_human_friendly_time_stamps
                  ? (&Ostream_log_msg_writer::do_log_with_human_friendly_time_stamp)
                  : (&Ostream_log_msg_writer::do_log_with_epoch_time_stamp)),
  m_os(os),
  m_clean_os_state(m_os) // Memorize this before any messing with formatting.
{
  using std::setfill;
  using boost::chrono::time_fmt;
  using timezone = boost::chrono::timezone;

  /* Note: The ostream here is totally unrelated to ostream used elsewhere to create msg in the first place.
   * (In fact, this class shouldn't have to know anything about that;
   * it receives a msg that is already put together.  I only mention it here in case you've been reading Logger
   * stuff and get confused.)  In particular, any formatters we use don't affect that other ostream.  So even
   * if we accidentally left std::hex formatter enabled at the end of this method, that would in no way cause hex
   * output of integer values in the Logger's user's message (such as passed to FLOW_LOG_INFO() and friends).
   * On the other hand, leaving std::hex on *would* indeed affect the users of m_os, which would indeed
   * be uncivilized of us; consider m_os might be std::cout for example.  However, we've been assured that no one
   * uses m_os until we're done with it; so we only restore the original formatting in our destructor. */

  // Set some global formatting.  Since we've been promised no cross-use of m_os by outside code, this is enough.
  if (m_config.m_use_human_friendly_time_stamps)
  {
    m_os << time_fmt(timezone::local); // When printing a system_clock::time_point, use local (not UTC) time zone.
  }
  else
  {
    m_os << setfill('0');
  }
} // Ostream_log_msg_writer::Ostream_log_msg_writer()

/* This is here just so we could document this cleanly in .hpp.  As promised m_clean_os_state dtor will auto-restore
 * any formatting changes we'd made to m_os throughout. */
Ostream_log_msg_writer::~Ostream_log_msg_writer() = default;

void Ostream_log_msg_writer::log(const Msg_metadata& metadata, util::String_view msg)
{
  m_do_log_func(this, metadata, msg);
}

void Ostream_log_msg_writer::do_log_with_epoch_time_stamp(const Msg_metadata& metadata, util::String_view msg)
{
  using std::setw;
  using boost::chrono::microseconds;
  using boost::chrono::round;
  using boost::chrono::system_clock;

  /* Get POSIX time stamp, a ubiquitous (if not 100% unambiguous, due to leap seconds) way to time-stamp a log
   * line.  Show it as seconds.microseconds with appropriate padding.  POSIX/UTC time is relative to 1/1/1970 00:00.
   *
   * Note: On at least some systems, we can get even better precision than microseconds; at least empirically in some
   * Linuxes one can see (1) system_clock::duration is nanoseconds; and (2) the last 3 digits are not 000.  However
   * how accurate this is is dubious; it seems safer to round up to microseconds.  Sub-microsecond-precision timing
   * should use Fine_clock (and/or flow::perf::Checkpointing_timer). */
  const auto since_epoch = round<microseconds>(metadata.m_called_when - S_POSIX_EPOCH);

  const microseconds::rep usec_since_epoch = since_epoch.count();
  const microseconds::rep sec = usec_since_epoch / 1000000;
  const microseconds::rep usec = usec_since_epoch % 1000000;

  // sec.usec, padded with zeroes up to 6 digits.  We've already set 0 as fill character.  setw(6) affects only one <<.
  m_os << sec << '.'
       << setw(6) << usec << ' ';
  // Formatting is essentially back to default now, except '0' fill character which only matters with setw(non-zero).

  log_past_time_stamp(metadata, msg);
} // Ostream_log_msg_writer::do_log_with_epoch_time_stamp()

void Ostream_log_msg_writer::do_log_with_human_friendly_time_stamp(const Msg_metadata& metadata, util::String_view msg)
{
  /* In practice either: YYYY-MM-DD HH:MM:SS.UUUUUU zZZZZ
   *                 or: YYYY-MM-DD HH:MM:SS.NNNNNNNNN zZZZZ
   * Example:
   * 2017-03-23 19:02:55.023337 -0800
   *
   * where: the choice of UUUUUU or NNNNNNNNN appears to be based on the capabilities of the hardware/system, and cannot
   * be specified (without unsightly, and unnecessary, hacking -- and even that only in the "lop off digits" direction);
   * `z` is `+` or `-`; and ZZZZ is a time-zone offset from UTC in minutes.  The time zone is the local time zone.
   *
   * Notes:
   *   - The reason we are able to do this is that boost.chrono I/O v2, thank god, `ostream<<`s a
   *     system_clock::time_point in this form.  (v1 wouldn't; prints nanoseconds since 1/1/1970.  So make sure to
   *     #include <boost/chrono/io/time_point_io.hpp> and not chrono/chrono_io.hpp.)  It is a very nice, helpful,
   *     and predictable form.
   *     - Before that, I (ygoldfel) jumped through unspeakable hoops, including boost.locale haxoring, to get
   *       some kind of decent human-friendly output which included microsecond+ precision.  So this is so much easier.
   *       It also gives us predicate zZZZZ output, whereas the best I was able to come up with with boost.locale was
   *       %Z which made unpredictable (though usually nice -- but sometimes super long) results.
   *   - As mentioned, this is in the local time zone.  It is also possible, via boost.chrono time_fmt(timezone::utc),
   *     to select UTC.  (It is not possible to choose any other time zone.) */
  m_os << metadata.m_called_when << ' ';

  log_past_time_stamp(metadata, msg);
} // Ostream_log_msg_writer::do_log_with_human_friendly_time_stamp()

void Ostream_log_msg_writer::log_past_time_stamp(const Msg_metadata& metadata, util::String_view msg)
{
  using std::flush;

  /* time_stamp [<sevr>]: T<thread nickname or ID>: <component>: <file>:<function>(<line>): <msg>
   * Note: The "<component>: " part is optional at the discretion of Config m_config.  For example the component
   * may be null, or it may not have been registered in Config. */

  assert(metadata.m_msg_sev != Sev::S_NONE); // S_NONE can be used only as a sentinel.

  m_os << '[' << S_SEV_STRS[static_cast<size_t>(metadata.m_msg_sev)] << "]: T";
  if (metadata.m_call_thread_nickname.empty())
  {
    m_os << metadata.m_call_thread_id;
  }
  else
  {
    m_os << metadata.m_call_thread_nickname;
  }
  m_os << ": ";

  // As noted, this part may be omitted by Config.
  if (m_config.output_component_to_ostream(&m_os, metadata.m_msg_component))
  {
    m_os << ": ";
  }

  m_os << FLOW_UTIL_WHERE_AM_I_FROM_ARGS(metadata.m_msg_src_file, metadata.m_msg_src_function, metadata.m_msg_src_line)
       << ": "
       << msg << '\n'
       << flush;
} // Ostream_log_msg_writer::log_past_time_stamp()

} // namespace flow::log
