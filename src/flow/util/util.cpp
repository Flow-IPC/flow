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
#include "flow/util/util.hpp"

namespace flow::util
{

// Types.

/**
 * Internally used class that enables some of the activities of beautify_chrono_ostream() API.  It's a locale facet,
 * whose `virtual` API can be used to imbue onto an `ostream`;
 * but even within our own internal code the only public API used is beautified_locale(), which
 * returns a reference to a ready-made such `std::locale`.
 *
 * This facet enables the default `chrono` output behavior, except the "micro" short-form prefix
 * is just "u" instead of the default Unicode Greek letter mu.
 *
 * ### Implementation notes ###
 * How it does this is fairly self-explanatory, but finding out how to do it was annoyingly tricky due to the
 * inconsistent/outdated boost.chrono docs which only partially explain boost.chrono I/O v2.  Ultimately I had to
 * find the actual example `french.cpp` (not its outdated v1 version directly inside the main doc text, in Boost 1.76);
 * then cross-reference it with the Reference section that described the `duration_units_default` class.
 */
class Duration_units_beautified : public boost::chrono::duration_units_default<char>
{
public:
  // Methods.

  /**
   * Returns a reference to a locale that -- if imbued onto an `ostream` --
   * equals `std::locale::classic` except beautified by a Duration_units_beautified facet (as described in
   * the class doc header).
   *
   * ### Thread safety and perf ###
   * This (`static`) method is safe to call concurrently with itself.  Internally it achieves this possibly by using a
   * simple mutex.  Since it is not expected one would call this frequently, let alone frequently enough for any
   * contention to occur, we consider this acceptable.
   *
   * @return See above.
   */
  static const std::locale& beautified_locale();

  /**
   * Returns the prefix string for micro-units; namely the default thing for long-form format ("micro") but
   * the non-default "u" for the short-form.  Hence microseconds are either "microsecond[s]" or "us."
   *
   * ### Rationale ###
   * This is `public` only so as to work as a `virtual` facet interface element invoked by STL `ostream` machinery upon
   * imbuing `*this` onto a `locale`.  Since Duration_units_beautified is not exposed as a flow::util public API,
   * leaving this method `public` (to our internal code in this .cpp file only) is acceptable.
   *
   * @param style
   *        Either `name_format` or `symbol_format` (the latter being the short-form choice).
   * @param period_tag
   *        Tag indicating this method is for the micro prefix.
   * @return The prefix string.
   */
  typename boost::chrono::duration_units<char_type>::string_type
    do_get_ratio_prefix(boost::chrono::duration_style style, boost::micro period_tag) const override;

private:
  // Types.

  /// Short-hand for superclass.
  using Duration_units_default = boost::chrono::duration_units_default<char>;

  // Constructors/destructor.

  /// Constructs.
  explicit Duration_units_beautified();

  // Data.

  /**
   * Pointer returned by beautified_locale(), pointing to an object allocated exactly once; or null if that has not yet
   * been called.  The allocated object lives until the process exits.
   */
  static std::locale const * s_beautified_locale_or_null;

  /// Helper to initialize #s_beautified_locale_or_null at most once.
  static boost::once_flag s_beautified_locale_init_flag;
}; // class Duration_units_beautified

// Static initializers.

std::locale const * Duration_units_beautified::s_beautified_locale_or_null{nullptr};
boost::once_flag Duration_units_beautified::s_beautified_locale_init_flag;

// Implementations.

// Local class implementations.

Duration_units_beautified::Duration_units_beautified() :
  Duration_units_default(0) // Just use `refs == 0`.  See boost::chrono docs, if you care to know more re. this detail.
{
  // That's all.
}

const std::locale& Duration_units_beautified::beautified_locale() // Static.
{
  using boost::call_once;
  using std::locale;

  // As advertised create (thread-safely) a locale that survives until the process exits.
  call_once(s_beautified_locale_init_flag,
            [&]() { s_beautified_locale_or_null = new locale(locale::classic(), new Duration_units_beautified); });

  assert(s_beautified_locale_or_null);
  return *s_beautified_locale_or_null;
}

boost::chrono::duration_units<char>::string_type Duration_units_beautified::do_get_ratio_prefix
                                                   (boost::chrono::duration_style style,
                                                    boost::micro period_tag) const // Virtual.
{
  // See Implementation in our class doc header for background.

  if (style == boost::chrono::duration_style::symbol)
  {
    return "u"; // Normally this would be the Unicode string for the Greek letter mu, but we hate that stuff.
  }
  // else
  return Duration_units_default::do_get_ratio_prefix(style, period_tag);
}

// Free function implementations.

Null_interface::~Null_interface() = default;

boost::chrono::microseconds time_since_posix_epoch()
{
  /* Get the # of microseconds since the advertised Epoch time point, which is explicitly what microsec_clock
   * gives us, the latter being officially the most precise documented clock in boost.date_time.  Then just
   * make a boost.chrono duration<> out of that. */
  using boost::posix_time::microsec_clock;
  using boost::posix_time::ptime;
  using boost::chrono::microseconds;
  using boost::gregorian::date;
  return microseconds{(microsec_clock::universal_time() - ptime{date{1970, 1, 1}})
                      .total_microseconds()};

  /* I am using boost.date_time machinery to get the UTC time and then the # of usec since the
   * exact epoch time point.  Beyond dealing with calendars and the like, I find boost.date_time
   * suspect and would always prefer boost.chrono, but my attempt to entirely use that for this
   * failed. chrono::system_clock::now() actually returns the current time point from the correct
   * clock, with the best possible resolution.  That's great, but then the docs say that the epoch
   * against which this is computed is not defined and to use system_clock::to_time_t() to
   * convert now() to a count since the actual POSIX Epoch.  Again, that's great, except that
   * time_t is in seconds, which means the juicy as-high-res-as-possible time point is rounded
   * to seconds, destroying information irretrievably.  We need more resolution than seconds, so
   * we've no choice but to rely on the crusty boost.date_time wall clock time.
   *
   * It doesn't matter that much... it's just style in this one little function. */
}

void beautify_chrono_ostream(std::ostream* os_ptr)
{
  using boost::chrono::symbol_format;

  auto& os = *os_ptr;

  /* We prefer all output of durations to be like "ms" and not "milliseconds."
   * Also we hate the Unicode Greek mu prefix for micro, so we use custom facet to have something nicer. */

  os.imbue(Duration_units_beautified::beautified_locale());
  os << symbol_format;
}

size_t deep_size(const std::string& val)
{
  // We're following the loose pattern explained at the end of Async_file_logger::mem_cost().

#if (!defined(__GNUC__)) || (!defined(__x86_64__))
  static_assert(false, "An estimation trick below has only been checked with x64 gcc and clang.  "
                         "Revisit code for other envs.");
#endif

  /* If it is long enough to not fit inside the std::string object itself
   * (common optimization in STL: Small String Optimization), then it'll allocate a buffer in heap.
   * We could even determine whether it actually happened here at runtime, but that wastes cycles
   * (original use case is in log::Async_file_logger specifically where every cycle counts).
   * Instead we've established experimentally that with default STL and clangs 4-17 and gccs 5-13
   * SSO is active for .capacity() <= 15.  @todo Check LLVM libc++ too.  Prob same thing... SSO is well established. */
  const auto sz = val.capacity();
  return (sz <= 15) ? 0 : sz;
}

} // namespace flow::util
