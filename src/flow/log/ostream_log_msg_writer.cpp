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
#include <array>
#include <chrono>
#include <fmt/chrono.h>
#include <iterator>

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

  const auto since_epoch = std::chrono::duration_cast<std::chrono::microseconds>(metadata.m_called_when.time_since_epoch());

  const auto usec_since_epoch = since_epoch.count();
  const std::chrono::microseconds::rep sec = usec_since_epoch / 1000000;
  const std::chrono::microseconds::rep usec = usec_since_epoch % 1000000;

  // sec.usec, padded with zeroes up to 6 digits.  We've already set 0 as fill character.  setw(6) affects only one <<.
  m_os << sec << '.'
       << setw(6) << usec << ' ';
  // Formatting is essentially back to default now, except '0' fill character which only matters with setw(non-zero).

  log_past_time_stamp(metadata, msg);
} // Ostream_log_msg_writer::do_log_with_epoch_time_stamp()

void Ostream_log_msg_writer::do_log_with_human_friendly_time_stamp(const Msg_metadata& metadata, util::String_view msg)
{
  static std::array<char, 40> cached_humanreadable_timestamp;
  static size_t cached_humanreadable_timestamp_size = 0;
  static std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds> cached_rounded_timestamp{};
  
  const auto rounded_timestamp = std::chrono::time_point_cast<std::chrono::seconds>(metadata.m_called_when);
  if (cached_rounded_timestamp != rounded_timestamp)
  {
    auto end = fmt::format_to(
      std::begin(cached_humanreadable_timestamp), 
      "{0:%Y-%m-%d %H:%M:}{1:%S} {0:%z} ", 
          fmt::localtime(std::chrono::system_clock::to_time_t(metadata.m_called_when)), 
          metadata.m_called_when
    );
    *end = '\0';

    cached_rounded_timestamp = rounded_timestamp;
    cached_humanreadable_timestamp_size = std::distance(std::begin(cached_humanreadable_timestamp), end);
  }

  constexpr size_t seconds_start = 17;
  fmt::format_to(
    std::next(std::begin(cached_humanreadable_timestamp), seconds_start), 
    "{:%S}", metadata.m_called_when
  );

  m_os << std::string_view(cached_humanreadable_timestamp.data(), cached_humanreadable_timestamp_size);

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
