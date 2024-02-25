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

#include "flow/util/basic_blob.hpp"

namespace flow::util
{

/**
 * Basic_blob that works in regular heap (and is itself placed in heap or stack) and memorizes a log::Logger,
 * enabling easier logging albeit with a small perf trade-off.  Most users will use the concrete types
 * #Blob/#Sharing_blob which alias to Blob_with_log_context.
 *
 * @see Basic_blob.  It supplies the core behavior of Blob_with_log_context.  Note that while some APIs are shadowed in
 *      this (non-polymorphic) sub-class and thus documented inside the `class { braces }`, a significant number
 *      are simply inherited: namely the ones that do not log.  Typically one would look at Basic_blob
 *      documentation and simply mentally remove any `Logger* logger_ptr` argument from the prototype, knowing
 *      the logging will occur with the log context established at Blob_with_log_context construction.
 *
 * In a vague sense Blob_with_log_context is to `Basic_blob<>` what `string` is to `basic_string`, in that it is a
 * commonly used concrete type rather than a template.  However, unlike that analogy, it also adds behavior and data.
 * Basic_blob accomplishes (optional) logging by means of the user supplying (if desired) a non-null `Logger*`
 * to each API they want to log.  Blob_with_log_context, instead, takes a `Logger*` in any non-move/copy ctor and
 * uses it for all subsequent logging.  Basic_blob uses a little less memory and is a little bit faster, while
 * Blob_with_log_context (#Blob, #Sharing_blob) is more convenient.
 *
 * It is possible, though somewhat exotic, to use any shadowed APIs (that take the `Logger* logger_ptr` optional
 * arg) from Basic_blob (via cast of `this` to `Basic_blob*` perhaps).
 *
 * Lastly, reminder: Basic_blob, and hence Blob_with_log_context, log at TRACE log level or more verbose.  If in any
 * case there is no interest in logging at such a verbose level (checked against log::Logger::should_log() of course)
 * then using #Blob_sans_log_context or #Sharing_blob_sans_log_context is straightforwardly superior.
 *
 * ### Historic note ###
 * Originally `Blob` was the only class in the hierarchy and (1) could not be used with custom allocators (and was not
 * SHM-friendly) unlike Basic_blob; and (2) stored a log context from construction unlike Basic_blob.
 * Then when those 2 changes became required by some use cases, Basic_blob took the vast majority of what used to
 * be `Blob` and added those 2 changes.  Meanwhile `Blob` was rewritten in terms of Basic_blob in a way that
 * exactly preserved its behavior (so that no call-site changes for Blob-using code were needed).
 * Lastly, when `S_SHARING_ALLOWED` template param was added to Basic_blob, `Blob` became a template
 * Blob_with_log_context, while #Blob aliased to `Blob_with_log_context<false>` thus staying functionally
 * exactly the same as before, minus the share() feature.  (`Sharing_blob` was added, aliasing to
 * `Blob_with_log_context<true>`, being exactly idetical to `Blob` before; the rename was possible due to
 * no production code using the sharing feature yet.)
 *
 * @internal
 * ### Impl notes ###
 * It is quite straightforward; we derive from Basic_blob, with `Allocator` set to the default `std::allocator`
 * (heap alloc) and keep all of its behaviors except shadowing all APIs that take a `Logger*` having the shadows
 * no longer take that but pass-through the ctor-supplied `Logger*` to the shadowee.  As typical we use log::Log_context
 * to store the necessary context including said `Logger*`.  In some cases, also, a Basic_blob API is constrained
 * (dtor, assignment) to not be able to even take a `Logger*` and therefore cannot log at all; we shadow such APIs
 * as well and enable logging by judiciously calling something functionally equivalent to the shadowed thing.
 * (See for example ~Blob_with_log_context() and the assignment operators.)
 * @endinternal
 *
 * @tparam S_SHARING_ALLOWED
 *         See Basic_blob.
 */
template<bool S_SHARING_ALLOWED>
class Blob_with_log_context :
  public log::Log_context,
  public Basic_blob<std::allocator<uint8_t>, S_SHARING_ALLOWED>
{
public:
  // Types.

  /// Short-hand for our main base.
  using Base = Basic_blob<std::allocator<uint8_t>, S_SHARING_ALLOWED>;

  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  using value_type = typename Base::value_type;
  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  using size_type = typename Base::size_type;
  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  using difference_type = typename Base::difference_type;
  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  using Iterator = typename Base::Iterator;
  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  using Const_iterator = typename Base::Const_iterator;
  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  using Allocator_raw = typename Base::Allocator_raw;
  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  using pointer = typename Base::pointer;
  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  using const_pointer = typename Base::const_pointer;
  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  using reference = typename Base::reference;
  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  using const_reference = typename Base::const_reference;
  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  using iterator = typename Base::iterator;
  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  using const_iterator = typename Base::const_iterator;

  // Constants.

  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  static constexpr auto S_SHARING = Base::S_SHARING;

  /// Short-hand for base member (needed because base access to a template must be qualified otherwise).
  static constexpr auto S_UNCHANGED = Base::S_UNCHANGED;

  // Constructors/destructor.

  /**
   * On top of the similar 2-arg Basic_blob ctor, memorizes the given log::Logger for all future logging
   * in `*this`.  (Except, technically, one can subsequently override this by using super-class APIs which take
   * `Logger*`.)
   *
   * @param logger_ptr
   *        The Logger implementation to use subsequently.
   */
  Blob_with_log_context(log::Logger* logger_ptr = 0);

  /**
   * On top of the similar 3-arg Basic_blob ctor, memorizes the given log::Logger for all future logging
   * in `*this`.  (Except, technically, one can subsequently override this by using super-class APIs which take
   * `Logger*`.)
   *
   * @param logger_ptr
   *        The Logger implementation to use subsequently.
   * @param size
   *        See super-class API.
   */
  explicit Blob_with_log_context(log::Logger* logger_ptr, size_type size);

  /**
   * On top of the similar Basic_blob move ctor, moves the source object's log::Logger for all future logging
   * in `*this`.  (Except, technically, one can subsequently override this by using super-class APIs which take
   * `Logger*`.)
   *
   * @param moved_src
   *        See super-class API.
   */
  Blob_with_log_context(Blob_with_log_context&& moved_src);

  /**
   * On top of the similar Basic_blob copy ctor, copies the source object's log::Logger for all future logging
   * in `*this`.  (Except, technically, one can subsequently override this by using super-class APIs which take
   * `Logger*`.)
   *
   * @param src
   *        See super-class API.
   */
  explicit Blob_with_log_context(const Blob_with_log_context& src);

  /// On top of the similar Basic_blob dtor, adds some possible TRACE-logging.
  ~Blob_with_log_context();

  // Methods.

  /**
   * On top of the similar Basic_blob method, logs using the stored log context.
   *
   * @note We do not shadow Basic_blob::assign() (move overload), as it would've been identical to the present
   *       operator.  In Blob_with_log_context, unlike Basic_blob, there is no need for an extra `logger_ptr` optional
   *       arg.
   *
   * @param moved_src
   *        See super-class API.
   * @return See super-class API.
   */
  Blob_with_log_context& operator=(Blob_with_log_context&& moved_src);

  /**
   * On top of the similar Basic_blob method, logs using the stored log context.
   *
   * @note We do not shadow Basic_blob::assign() (copy overload), as it would've been identical to the present
   *       operator.  In Blob_with_log_context, unlike Basic_blob, there is no need for an extra `logger_ptr` optional
   *       arg.
   *
   * @param src
   *        See super-class API.
   * @return See super-class API.
   */
  Blob_with_log_context& operator=(const Blob_with_log_context& src);

  /**
   * On top of the similar Basic_blob method, logs using the stored log context.
   *
   * @param other
   *        See super-class API.
   */
  void swap(Blob_with_log_context& other);

  /**
   * On top of the similar Basic_blob method, logs using the stored log context and copies it to the returned
   * object as well.
   *
   * @param other
   *        See super-class API.
   * @return See super-class API.
   */
  Blob_with_log_context share() const;

  /**
   * On top of the similar Basic_blob method, logs using the stored log context and copies it to the returned
   * object as well.
   *
   * @param size
   *        See super-class API.
   * @return See super-class API.
   */
  Blob_with_log_context share_after_split_left(size_type size);

  /**
   * On top of the similar Basic_blob method, logs using the stored log context and copies it to the returned
   * object as well.
   *
   * @param size
   *        See super-class API.
   * @return See super-class API.
   */
  Blob_with_log_context share_after_split_right(size_type size);

  /**
   * On top of the similar Basic_blob method, logs using the stored log context and copies it to the emitted
   * objects as well.
   *
   * @tparam Emit_blob_func
   *         See super-class API.  However in this version the arg type in the signature is `Blob_with_log_context&&`.
   * @param size
   *        See super-class API.
   * @param headless_pool
   *        See super-class API.
   * @param emit_blob_func
   *        See `Emit_blob_func`.
   */
  template<typename Emit_blob_func>
  void share_after_split_equally(size_type size, bool headless_pool, Emit_blob_func&& emit_blob_func);

  /**
   * On top of the similar Basic_blob method, logs using the stored log context and copies it to the emitted
   * objects as well.
   *
   * @tparam Blob_container
   *         See super-class API.  However in this version the container element type must be Blob_with_log_context.
   * @param size
   *        See super-class API.
   * @param headless_pool
   *        See super-class API.
   * @param out_blobs
   *        See super-class API.
   */
  template<typename Blob_container>
  void share_after_split_equally_emit_seq(size_type size, bool headless_pool, Blob_container* out_blobs);

  /**
   * On top of the similar Basic_blob method, logs using the stored log context and copes it to the emitted
   * objects as well.
   *
   * @tparam Blob_ptr_container
   *         See super-class API.  However in this version the container element pointer's type must be
   *         Blob_with_log_context.
   * @param size
   *        See super-class API.
   * @param headless_pool
   *        See super-class API.
   * @param out_blobs
   *        See super-class API.
   */
  template<typename Blob_ptr_container>
  void share_after_split_equally_emit_ptr_seq(size_type size, bool headless_pool, Blob_ptr_container* out_blobs);

  /**
   * On top of the similar Basic_blob method, logs using the stored log context.
   *
   * @param src
   *        See super-class API.
   * @return See super-class API.
   */
  size_type assign_copy(const boost::asio::const_buffer& src);

  /**
   * On top of the similar Basic_blob method, logs using the stored log context.
   *
   * @param dest
   *        See super-class API.
   * @param src
   *        See super-class API.
   * @return See super-class API.
   */
  Iterator emplace_copy(Const_iterator dest, const boost::asio::const_buffer& src);

  /**
   * On top of the similar Basic_blob method, logs using the stored log context.
   *
   * @param src
   *        See super-class API.
   * @param dest
   *        See super-class API.
   * @return See super-class API.
   */
  Const_iterator sub_copy(Const_iterator src, const boost::asio::mutable_buffer& dest) const;

  /**
   * On top of the similar Basic_blob method, logs using the stored log context.
   *
   * @param capacity
   *        See super-class API.
   */
  void reserve(size_type capacity);

  /// On top of the similar Basic_blob method, logs using the stored log context.
  void make_zero();

  /**
   * On top of the similar Basic_blob method, logs using the stored log context.
   *
   * @param size
   *        See super-class API.
   * @param start_or_unchanged
   *        See super-class API.
   */
  void resize(size_type size, size_type start_or_unchanged = S_UNCHANGED);

  // private: There are no added data per se, but there is the added base, log::Log_context, which stores some stuff.
}; // class Blob_with_log_context

// Free functions: in *_fwd.hpp.

// Template implementations.

template<bool S_SHARING_ALLOWED>
Blob_with_log_context<S_SHARING_ALLOWED>::Blob_with_log_context(log::Logger* logger_ptr) :
  log::Log_context(logger_ptr, Base::S_LOG_COMPONENT)
  // And default-ct Base().
{
  // Nothing else.
}

template<bool S_SHARING_ALLOWED>
Blob_with_log_context<S_SHARING_ALLOWED>::Blob_with_log_context(log::Logger* logger_ptr, size_type size) :
  log::Log_context(logger_ptr, Base::S_LOG_COMPONENT),
  Base(size, get_logger())
{
  // Nothing else.
}

template<bool S_SHARING_ALLOWED>
Blob_with_log_context<S_SHARING_ALLOWED>::Blob_with_log_context(Blob_with_log_context&& moved_src) :
  log::Log_context(static_cast<log::Log_context&&>(std::move(moved_src))),
  Base(std::move(moved_src), get_logger())
{
  // Nothing else.
}

template<bool S_SHARING_ALLOWED>
Blob_with_log_context<S_SHARING_ALLOWED>::Blob_with_log_context(const Blob_with_log_context& src) :
  log::Log_context(static_cast<const log::Log_context&>(src)),
  Base(src, get_logger())
{
  // Nothing else.
}

template<bool S_SHARING_ALLOWED>
Blob_with_log_context<S_SHARING_ALLOWED>::~Blob_with_log_context()
{
  /* ~Basic_blob() doesn't log at all -- no way to give a Logger* to it -- but a way to get some potentially
   * useful logging is to make_zero().  It is redundant (which is why ~Basic_blob() does not bother) but in some
   * cases usefully logs. */
  make_zero();
}

template<bool S_SHARING_ALLOWED>
Blob_with_log_context<S_SHARING_ALLOWED>&
  Blob_with_log_context<S_SHARING_ALLOWED>::operator=(const Blob_with_log_context& src)
{
  using log::Log_context;

  Log_context::operator=(static_cast<const Log_context&>(src));
  Base::assign(src, get_logger()); // Yay!  We can leverage this to make operator=() log which Basic_blob can't do.
  return *this;
}

template<bool S_SHARING_ALLOWED>
Blob_with_log_context<S_SHARING_ALLOWED>&
  Blob_with_log_context<S_SHARING_ALLOWED>::operator=(Blob_with_log_context&& moved_src)
{
  using log::Log_context;

  Log_context::operator=(static_cast<Log_context&&>(std::move(moved_src)));
  Base::assign(std::move(moved_src), get_logger()); // Same comment as in copy-assignment above.
  return *this;
}

template<bool S_SHARING_ALLOWED>
void Blob_with_log_context<S_SHARING_ALLOWED>::swap(Blob_with_log_context& other)
{
  using log::Log_context;
  using std::swap;

  if (this != &other)
  {
    swap(*static_cast<Log_context*>(this), static_cast<Log_context&>(other));
    Base::swap(other, get_logger()); // Might as well keep this inside the `if` as a micro-optimization.
  }
}

template<bool S_SHARING_ALLOWED>
void swap(Blob_with_log_context<S_SHARING_ALLOWED>& blob1, Blob_with_log_context<S_SHARING_ALLOWED>& blob2)
{
  return blob1.swap(blob2);
}

template<bool S_SHARING_ALLOWED>
Blob_with_log_context<S_SHARING_ALLOWED> Blob_with_log_context<S_SHARING_ALLOWED>::share() const
{
  Blob_with_log_context blob(get_logger());
  static_cast<Base&>(blob) = Base::share(get_logger());
  return blob;
}

template<bool S_SHARING_ALLOWED>
Blob_with_log_context<S_SHARING_ALLOWED>
  Blob_with_log_context<S_SHARING_ALLOWED>::share_after_split_left(size_type lt_size)
{
  Blob_with_log_context blob(get_logger());
  static_cast<Base&>(blob) = Base::share_after_split_left(lt_size, get_logger());
  return blob;
}

template<bool S_SHARING_ALLOWED>
Blob_with_log_context<S_SHARING_ALLOWED>
  Blob_with_log_context<S_SHARING_ALLOWED>::share_after_split_right(size_type rt_size)
{
  Blob_with_log_context blob(get_logger());
  static_cast<Base&>(blob) = Base::share_after_split_right(rt_size, get_logger());
  return blob;
}

template<bool S_SHARING_ALLOWED>
template<typename Emit_blob_func>
void Blob_with_log_context<S_SHARING_ALLOWED>::share_after_split_equally(size_type size, bool headless_pool,
                                                                         Emit_blob_func&& emit_blob_func)
{
  Base::share_after_split_equally_impl(size, headless_pool, std::move(emit_blob_func), get_logger(),
                                       [this](size_type lt_size, [[maybe_unused]] log::Logger* logger_ptr)
                                         -> Blob_with_log_context
  {
    assert(logger_ptr == get_logger());
    return share_after_split_left(lt_size);
  });
}

template<bool S_SHARING_ALLOWED>
template<typename Blob_container>
void Blob_with_log_context<S_SHARING_ALLOWED>::share_after_split_equally_emit_seq(size_type size, bool headless_pool,
                                                                                  Blob_container* out_blobs_ptr)
{
  // Almost copy-pasted from Basic_blob::<same method>().  It's short, though, so it seems fine.  @todo Revisit.

  assert(out_blobs_ptr);
  share_after_split_equally(size, headless_pool, [&](Blob_with_log_context&& blob_moved)
  {
    out_blobs_ptr->push_back(std::move(blob_moved));
  });
}

template<bool S_SHARING_ALLOWED>
template<typename Blob_ptr_container>
void Blob_with_log_context<S_SHARING_ALLOWED>::share_after_split_equally_emit_ptr_seq(size_type size,
                                                                                      bool headless_pool,
                                                                                      Blob_ptr_container* out_blobs_ptr)
{
  // Almost copy-pasted from Basic_blob::<same method>().  It's short, though, so it seems fine.  @todo Revisit.

  // By documented requirements this should be, like, <...>_ptr<Blob_with_log_context>.
  using Ptr = typename Blob_ptr_container::value_type;

  assert(out_blobs_ptr);

  share_after_split_equally(size, headless_pool, [&](Blob_with_log_context&& blob_moved)
  {
    out_blobs_ptr->push_back(Ptr(new Blob_with_log_context(std::move(blob_moved))));
  });
}

template<bool S_SHARING_ALLOWED>
void Blob_with_log_context<S_SHARING_ALLOWED>::reserve(size_type new_capacity)
{
  Base::reserve(new_capacity, get_logger());
}

template<bool S_SHARING_ALLOWED>
void Blob_with_log_context<S_SHARING_ALLOWED>::resize(size_type new_size, size_type new_start_or_unchanged)
{
  Base::resize(new_size, new_start_or_unchanged, get_logger());
}

template<bool S_SHARING_ALLOWED>
void Blob_with_log_context<S_SHARING_ALLOWED>::make_zero()
{
  Base::make_zero(get_logger());
}

template<bool S_SHARING_ALLOWED>
typename Blob_with_log_context<S_SHARING_ALLOWED>::size_type
  Blob_with_log_context<S_SHARING_ALLOWED>::assign_copy(const boost::asio::const_buffer& src)
{
  return Base::assign_copy(src, get_logger());
}

template<bool S_SHARING_ALLOWED>
typename Blob_with_log_context<S_SHARING_ALLOWED>::Iterator
  Blob_with_log_context<S_SHARING_ALLOWED>::emplace_copy(Const_iterator dest, const boost::asio::const_buffer& src)
{
  return Base::emplace_copy(dest, src, get_logger());
}

template<bool S_SHARING_ALLOWED>
typename Blob_with_log_context<S_SHARING_ALLOWED>::Const_iterator
  Blob_with_log_context<S_SHARING_ALLOWED>::sub_copy(Const_iterator src, const boost::asio::mutable_buffer& dest) const
{
  return Base::sub_copy(src, dest, get_logger());
}

} // namespace flow::util
