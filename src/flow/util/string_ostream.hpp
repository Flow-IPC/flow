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

#include "flow/util/util_fwd.hpp"
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/stream_buffer.hpp>
#include <boost/iostreams/device/back_inserter.hpp>


namespace flow::util
{

/**
 * Similar to `ostringstream` but allows fast read-only access directly into the `std::string` being written;
 * and some limited write access to that string.  Also it can take over an existing `std::string`.
 *
 * `ostringstream` is fine, except for the giant flaw that is the fact that the essentially only way to read the string
 * is to call `ostringstream::str()` which returns a copy, not a reference.
 *
 * @todo Consider using `alt_sstream` in boost.format; it doesn't seem to have public documentation but isn't in
 * a `detail` directory either.  So that's interesting.  It might have better performance than the implementation
 * here (by being more "direct" probably).  Then again it might not.
 *
 * ### Thread safety ###
 * This provides the same level of thread safety as `ostringstream`.  That is, you should use a mutex if planning
 * concurrent read/write access to the same object.
 */
class String_ostream :
  private boost::noncopyable
{
public:
  // Constructors/destructor.

  /**
   * Wraps either the given `std::string` or a newly created empty string if a null pointer is passed.
   *
   * @param target_str
   *        Pointer to the string of which to take ownership; null to use an internal string currently blank.
   *        If non-null is passed, any subsequnt access to `*target_str` except via this class's API results in
   *        undefined behavior.  (It should go without saying, but using `const_cast` or equivalents counts as being
   *        outside the bounds of this class's API.)
   */
  explicit String_ostream(std::string* target_str = nullptr);

  // Methods.

  /**
   * Access to stream that will write to owned string.
   *
   * @return Stream.
   */
  std::ostream& os();

  /**
   * Read-only access to stream that will write to owned string.
   *
   * @return Read-only reference to stream.
   */
  const std::ostream& os() const;

  /**
   * Read-only access to the string being wrapped.
   *
   * The reference's value never changes for `*this` object.  The string's value may change, if one writes to
   * `os()` or does str_clear(), etc.
   *
   * ### Rationale ###
   * Why return `const string&` instead of util::String_view?  Answer: By definition we are backed by an `std::string`,
   * either our own or one user actively passed to ctor.  Hence it's unnecessary obfuscation; plus it can lead to
   * errors, like if the user thinks a returned object's `String_view::size()` will auto-adjust based on what happens
   * to the wrapped `std::string` subsequently.  If they want a `String_view`, they can always just construct one, just
   * as we would inside here anyway.
   *
   * @return Read-only reference to string; the address therein is guaranteed to always be the same given a `*this`.
   */
  const std::string& str() const;

  /**
   * Performs `std::string::clear()` on the object returned by `str()`.  The latter is `const`, so you may not
   * call `clear()` directly.
   *
   * @note `clear()` is a frequently desired operation, which is why access to it is provided as a special case.
   *       It is intentional that no read-write version of str() is provided for arbitrary write operations.
   */
  void str_clear();

private:
  // Types.

  /// Short-hand for an `ostream` writing to which will append to an std::string it is adapting.
  using String_appender_ostream = boost::iostreams::stream<boost::iostreams::back_insert_device<std::string>>;

  // Data.

  /**
   * Underlying string to use if user chooses not to pass in their own in constructor.  Otherwise unused.
   * (Could use `unique_ptr` to avoid even allocating it if not needed; but the memory use of an empty
   * string is negligible, and this is otherwise equally fast or faster and leads to simpler code.)
   */
  std::string m_own_target_str;

  /// Pointer to the target string.  Emptied at construction and in str_clear() *only*.
  std::string* m_target;

  /// Inserter into #m_target.
  boost::iostreams::back_insert_device<std::string> m_target_inserter;

  /// Appender `ostream` into #m_target by way of #m_target_inserter.  Write/`flush` here to write to #m_target.
  String_appender_ostream m_target_appender_ostream;
}; // class String_ostream

} // namespace flow::util
