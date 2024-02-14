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
#include "flow/util/string_view.hpp"
#include "flow/util/fmt.hpp"
#include <chrono>

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
  m_clean_os_state(m_os), // Memorize this before any messing with formatting.
  m_last_human_friendly_time_stamp_str_sz(0)
{
  using std::setfill;

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
  if (!m_config.m_use_human_friendly_time_stamps)
  {
    m_os << setfill('0');
  }
} // Ostream_log_msg_writer::Ostream_log_msg_writer()

/* As promised m_clean_os_state dtor will auto-restore any formatting changes we'd made to m_os throughout.
 * This class uses boost::basic_ios_all_saver() (aka Ostream_state) which is explicitly marked as noexcept(false)
 * in boost 1.84. The fix is to override the noexcept(false) specification to noexcept(true) in this class
 * and let it call std::terminate in case it throws (IMO it shouldn't throw, but I didn't study it to the depth). */
Ostream_log_msg_writer::~Ostream_log_msg_writer() noexcept
{
}

void Ostream_log_msg_writer::log(const Msg_metadata& metadata, util::String_view msg)
{
  m_do_log_func(this, metadata, msg);
}

void Ostream_log_msg_writer::do_log_with_epoch_time_stamp(const Msg_metadata& metadata, util::String_view msg)
{
  using std::setw;
  using std::chrono::duration_cast;
  using std::chrono::microseconds;

  /* Get POSIX time stamp, a ubiquitous (if not 100% unambiguous, due to leap seconds) way to time-stamp a log
   * line.  Show it as seconds.microseconds with appropriate padding.  POSIX/UTC time is relative to 1/1/1970 00:00.
   *
   * Note: On at least some systems, we can get even better precision than microseconds; at least empirically in some
   * Linuxes one can see (1) system_clock::duration is nanoseconds; and (2) the last 3 digits are not 000.
   * To not worry about it showing up as SSSSSS000 just go with microseconds. */
  const auto since_epoch = duration_cast<microseconds>(metadata.m_called_when.time_since_epoch());

  const auto usec_since_epoch = since_epoch.count();
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
  using util::String_view;
  using std::chrono::time_point_cast;
  using std::chrono::seconds;
  using std::chrono::system_clock;

  // See background in class doc header Impl section; then come back here.

  // Keep consistent with S_HUMAN_FRIENDLY_TIME_STAMP_*_SZ_TEMPLATE values.
  constexpr String_view TIME_STAMP_FORMAT("{0:%Y-%m-%d %H:%M:}{1:%S} {0:%z} ");
  // Offset from start of human-friendly time stamp output, where the seconds.subseconds output begins.
  constexpr size_t SECONDS_START = S_HUMAN_FRIENDLY_TIME_STAMP_MIN_SZ_TEMPLATE.size();

  /* We just want to print m_called_when in a certain format, including %S which is seconds.subseconds.
   * However the part up-to (not including) %S rarely changes, yet it is somewhat expensive to compute
   * each time.  So we remember the formatted string printed in the last call to the present method in
   * m_last_human_friendly_time_stamp_str; and in the present call merely splice-in the %S part, saving some cycles.
   *
   * Naturally m_last_human_friendly_time_stamp_str becomes obsolete ~every minute, so we need to re-output it fully,
   * when we detect that m_called_when when m_last_human_friendly_time_stamp_str was last re-output was such that
   * the up-to-%S part would have been different and thus is now obsolete.  So step 1 is detecting whether that's
   * the case.  To do so we save m_cached_rounded_time_stamp each time this re-output occurs; and in step 1
   * compare whether the # of whole seconds in this saved value is different from # of whole seconds in the new
   * m_called_when.  (We could perhaps store/compare minutes, but that's an extra conversion, and anyway ~every second
   * is rare enough.) */
  const auto rounded_time_stamp = time_point_cast<seconds>(metadata.m_called_when);
  if (m_cached_rounded_time_stamp != rounded_time_stamp)
  {
    /* Need to re-output the whole thing.  In the %S part, output the same thing we would in the
     * fast path (the else{} below).
     *
     * Subtlety: Use localtime() to ensure output in the present time zone, not UTC.
     * It cannot be used for %S to get sub-second resolution, as to_time_t() yields 1-second resolution, so
     * just use m_called_when for %S and localtime() for the rest.
     * @todo Maybe there is some way to just force it to print full-precision m_called_when in local time zone?
     * (boost.chrono did it, with a switch that would cause it to use either local time zone or UTC in the output;
     * but we are not using boost.chrono output anymore for perf reasons; plus one cannot specify a format
     * which is a nice feature we can use to expand Ostream_log_writer capabilities in the future.) */
    const auto end = fmt::format_to(m_last_human_friendly_time_stamp_str.begin(),
                                    static_cast<const std::string_view&>(TIME_STAMP_FORMAT),
                                    fmt::localtime(system_clock::to_time_t(metadata.m_called_when)),
                                    metadata.m_called_when);
    *end = '\0'; // Perhaps unnecessary, but it's cheap enough and nice for debugging.

    m_cached_rounded_time_stamp = rounded_time_stamp;
    m_last_human_friendly_time_stamp_str_sz = end - m_last_human_friendly_time_stamp_str.begin();
  }
  else
  {
    // m_last_human_friendly_time_stamp_str_sz is already correct, except the %S part needs to be overwritten:
    fmt::format_to(m_last_human_friendly_time_stamp_str.begin() + SECONDS_START,
                   "{:%S}", metadata.m_called_when);
  }

  m_os << String_view(m_last_human_friendly_time_stamp_str.data(), m_last_human_friendly_time_stamp_str_sz);

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
