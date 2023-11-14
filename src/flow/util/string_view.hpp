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

#include <string>
#include <string_view>

namespace flow::util
{

// Types.

/* Normally Basic_string_view would be forward-declared in a _fwd.hpp and defined here; instead it is defined here,
 * and util_fwd.hpp simply `#include`s us.  This is in accordance with the list of known reasonable exceptions to
 * the _fwd.hpp pattern; namely since our String_view et al = glorified alias for std::string_view et al. */

/**
 * Essentially alias for a C++17-conforming string-view class template, which is a very lightweight `std::string`-like
 * representation of a character sequence already in memory.  As of March 2022 the alias is formally to
 * `std::basic_string_view` (unlike in the past, when that was not available -- until C++17).  However the alias remains
 * to, at least, avoid breaking user code -- by providing the #String_view related alias.
 * In addition this API-only subclass adds starts_with() and ends_with()
 * which implement the common-sense C++20 `basic_string_view` operations `starts_with()` and `ends_width()`.
 * These existed in the Boost `basic_string_view` that, pre-C++17, used to be the
 * `String_view` alias target (when `Ch` is `char`).  The downgrade due to their lack of inclusion in C++17 (which
 * inaugurated `*string_view` in C++ STL) is unfortunate; but these replacements are fine.
 *
 * @see #String_view, which is to `std::string_view` what the present template is to `std::basic_string_view`.
 *      I.e., when working with `char` string sequences, which is typical, use #String_view.
 *
 * ### Rationale ###
 * Why is this even a thing?  Why not use `std::basic_string_view` and `std::string_view` all over?
 * Answer: It is mostly historic.  Flow was C++14 until ~3/2022.
 * `std::string_view` was added in C++17.  Before then, #String_view aliased to `boost::string_view`
 * (another possibility would have been `boost::string_ref` -- same API -- long story), because it was available for
 * C++14.  Now that we're on C++17, it's better to use the `std` thing, especially since many STL (and Boost, nowadays)
 * APIs take `std::string_view` directly.  Hence the alias changed as planned all along.
 *
 * However the type alias remains.  Pragmatically: much code uses #String_view, even outside Flow, and there's no harm
 * in an alias, particularly since we add the nice starts_with() and ends_with().  The alias was actively useful before;
 * now removing it is a breaking change.
 *
 * ### Implementation/API note ###
 * It would have been ideal to define the various constructors by simply inheriting all of `std::basic_string_view`
 * ctors via the single statement `using Base::Base`.  However, at least with our gcc, it appears to cause some issue
 * in propagating `constexpr`ness of these constructors (which is effectively used, at least, by the flow::log macros
 * to make certain optimizations possible): compile errors.  To avoid this and various conversion issues, a number
 * of constructors are explicitly (re)defined.
 */
template<typename Ch, typename Traits = std::char_traits<Ch>>
class Basic_string_view :
  public std::basic_string_view<Ch, Traits>
{
public:
  // Types.

  /// Short-hand for the base.  We add no data of our own in this subclass, just a handful of APIs.
  using Base = std::basic_string_view<Ch, Traits>;

  // Ctors/destructor.

  /// Constructs null view: identical to #Base API.
  constexpr Basic_string_view() noexcept;

  /**
   * Constructs view to string at given start location and length: identical to #Base API.
   * @param s
   *        See #Base API.
   * @param count
   *        See #Base API.
   */
  constexpr Basic_string_view(Ch const * s, size_t count);

  /**
   * Constructs view to given NUL-terminated string: identical to #Base API.
   * @param s
   *        See #Base API.
   */
  constexpr Basic_string_view(Ch const * s);

  /**
   * Boring copy constructor: identical to #Base API.
   * @param s
   *        See #Base API.
   */
  constexpr Basic_string_view(const Basic_string_view& s) noexcept;

  /**
   * Identical to copy constructor but converts from a vanilla #Base.
   * @param s
   *        See copy constructor.
   */
  constexpr Basic_string_view(Base s);

  /**
   * Constructs view directly into a `std::basic_string` (e.g., `std::string`).  `basic_string` has a conversion
   * operator to `basic_string_view` but technically not to our slight embellishment thereof, Basic_string_view.
   * @param s
   *        Self-explanatory.
   */
  Basic_string_view(const std::basic_string<Ch, Traits>& s);

  // Methods.

  /**
   * Boring copy assignment: identical to #Base API.
   * @param s
   *        See #Base API.
   * @return See #Base API.
   */
  constexpr Basic_string_view& operator=(const Basic_string_view& s) noexcept;

  /**
   * Equivalent to C++20 `basic_string_view::starts_with()` which is lacking in C++17 but present in C++20 and
   * previously-used-in-Flow Boost equivalent `string_view`.
   *
   * @param needle
   *        Possible prefix within `*this`.
   * @return Whether `*this` contains `needle` as prefix.
   */
  bool starts_with(Basic_string_view needle) const;

  /**
   * Equivalent to C++20 `basic_string_view::starts_with()` which is lacking in C++17 but present in C++20 and
   * previously-used-in-Flow Boost equivalent `string_view`.
   *
   * @param needle
   *        Possible prefix within `*this`.
   * @return Whether `*this` contains `needle` as prefix.
   */
  bool starts_with(Ch const * needle) const;

  /**
   * Equivalent to C++20 `basic_string_view::starts_with()` which is lacking in C++17 but present in C++20 and
   * previously-used-in-Flow Boost equivalent `string_view`.
   *
   * @param needle
   *        Possible prefix within `*this`.
   * @return Whether `*this` contains `needle` as prefix.
   */
  bool starts_with(Ch needle) const;

  /**
   * Equivalent to C++20 `basic_string_view::ends_with()` which is lacking in C++17 but present in C++20 and
   * previously-used-in-Flow Boost equivalent `string_view`.
   *
   * @param needle
   *        Possible postfix within `*this`.
   * @return Whether `*this` contains `needle` as postfix.
   */
  bool ends_with(Basic_string_view needle) const;

  /**
   * Equivalent to C++20 `basic_string_view::ends_with()` which is lacking in C++17 but present in C++20 and
   * previously-used-in-Flow Boost equivalent `string_view`.
   *
   * @param needle
   *        Possible postfix within `*this`.
   * @return Whether `*this` contains `needle` as postfix.
   */
  bool ends_with(Ch const * needle) const;

  /**
   * Equivalent to C++20 `basic_string_view::ends_with()` which is lacking in C++17 but present in C++20 and
   * previously-used-in-Flow Boost equivalent `string_view`.
   *
   * @param needle
   *        Possible postfix within `*this`.
   * @return Whether `*this` contains `needle` as postfix.
   */
  bool ends_with(Ch needle) const;
}; // class Basic_string_view

/// Commonly used `char`-based Basic_string_view.  See its doc header.
using String_view = Basic_string_view<char>;

// Template implementations.

template<typename Ch, typename Traits>
constexpr Basic_string_view<Ch, Traits>::Basic_string_view(Ch const * s, size_t count) :
  Base(s, count)
{
  // That's it.
}

template<typename Ch, typename Traits>
constexpr Basic_string_view<Ch, Traits>::Basic_string_view(Ch const * s) :
  Base(s)
{
  // That's it.
}

template<typename Ch, typename Traits>
constexpr Basic_string_view<Ch, Traits>::Basic_string_view(Base s) :
  Base(s)
{
  // That's it.
}
template<typename Ch, typename Traits>
Basic_string_view<Ch, Traits>::Basic_string_view(const std::basic_string<Ch, Traits>& s) :
  Basic_string_view(s.data(), s.size())
{
  // That's it.
}

template<typename Ch, typename Traits>
constexpr Basic_string_view<Ch, Traits>::Basic_string_view() noexcept = default;
template<typename Ch, typename Traits>
constexpr Basic_string_view<Ch, Traits>::Basic_string_view(const Basic_string_view&) noexcept = default;
template<typename Ch, typename Traits>
constexpr Basic_string_view<Ch, Traits>& Basic_string_view<Ch, Traits>::operator=(const Basic_string_view& s) noexcept
  = default;

template<typename Ch, typename Traits>
bool Basic_string_view<Ch, Traits>::starts_with(Basic_string_view needle) const
{
  const auto needle_sz = needle.size();
  if (this->size() < needle.size())
  {
    return false;
  }
  // else
  for (size_t idx = 0; idx != needle_sz; ++idx) // needle_sz == 0 => no problem.
  {
    if (!Traits::eq((*this)[idx], needle[idx]))
    {
      return false;
    }
  }

  return true;
}

template<typename Ch, typename Traits>
bool Basic_string_view<Ch, Traits>::starts_with(Ch const * needle) const
{
  /* Conceivably hand-writing it to avoid a Traits::length() search for NUL (in constructing the 2nd
   * arg to starts_with() below) could improve perf; but on the other
   * hand it could even slow it down in the haystack-too-small case; plus length() is likely to be
   * assembly-optimized nicely.  Let's just not worry too much about it. */
  return starts_with(Basic_string_view(needle));
}

template<typename Ch, typename Traits>
bool Basic_string_view<Ch, Traits>::starts_with(Ch needle) const
{
  return (!this->empty()) && (this->front() == needle);
}

template<typename Ch, typename Traits>
bool Basic_string_view<Ch, Traits>::ends_with(Basic_string_view needle) const
{
  const auto needle_sz = needle.size();
  if (this->size() < needle.size())
  {
    return false;
  }
  // else
  const auto base_haystack_idx = this->size() - needle_sz;
  for (size_t idx = 0; idx != needle_sz; ++idx) // needle_sz == 0 => no problem.
  {
    if (!Traits::eq((*this)[base_haystack_idx + idx], needle[idx]))
    {
      return false;
    }
  }

  return true;
}

template<typename Ch, typename Traits>
bool Basic_string_view<Ch, Traits>::ends_with(Ch const * needle) const
{
  // Same comment as in starts_with().
  return ends_with(Basic_string_view(needle));
}

template<typename Ch, typename Traits>
bool Basic_string_view<Ch, Traits>::ends_with(Ch needle) const
{
  return (!this->empty()) && (this->back() == needle);
}

} // namespace flow::util
