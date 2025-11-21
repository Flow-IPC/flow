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

#include "flow/async/async_fwd.hpp"
#include "flow/log/log.hpp"
#include "flow/util/random.hpp"
#include <vector>

namespace flow::async
{
// Types.

/* See `using Op =` in async_fwd.hpp for a key (in this context) doc header.
 * @todo Maybe this stuff has grown big enough to deserve its own op_fwd.hpp? */

/**
 * Simple, immutable `vector`-like sequence of N opaque async::Op objects, usually corresponding to N worker threads,
 * with typically used operations such as `[index]` and random `Op` selection.
 *
 * ### Thread safety ###
 * The object is immutable after construction; therefore each `*this` is safe for concurrent Op_list method calls.
 *
 * @internal
 *
 * ### Design rationale ###
 * This really only exists, at least originally, in service of Concurrent_task_loop::per_thread_ops() (which returns
 * an Op_list nowadays).  An alternative approach would have been to move these simple facilities directly into
 * Concurrent_task_loop superclass.  Historically that's how it started; there was a `vector<Op>` directly therein.
 * It worked fine but (1) complicated the otherwise pure interface Concurrent_task_loop into part-interface,
 * part-logic-store-for-subclasses; and (2) increased the required cooperation between subclass and superclass (which is
 * cute in finished code but harder to build on/maintain).  Hence, as experienced OO coders learn eventually,
 * composition (where X is a member of Y) is more flexible and no more verbose than inheritance (where Y is a
 * subclass of X); in both case the desired/achieved effect is to let Y leverage X's abilities.  Hence Op_list is a
 * separate little class to be included as a member by whatever subclass of Concurrent_task_loop needs it.
 *
 * It is also a good sign that Op_list stands alone as a clean, reusable concept.  It may find other uses than
 * in Concurrent_task_loop::per_thread_ops().
 */
class Op_list :
  public log::Log_context
{
public:
  // Types.

  /**
   * Short-hand for function that that takes the target `Op`-list index and returns new async::Op whose copy
   * is suitable as the permanent `Op` at that index.
   */
  using Create_op_func = Function<Op (size_t target_op_idx)>;

  // Constructors/destructor.

  /**
   * Populates the async::Op list, which is immutable after this returns.  Caller may permanently
   * store `*this` with a `const` qualifier.
   *
   * @param logger_ptr
   *        Logger to which to log subsequently.
   * @param n_ops
   *        How many `Op`s will we permanently store?  The index range will therefore be
   *        `[0, n_ops)` or `[0, n_ops - 1)`.  Must be at least 1, or `assert()` trips.
   * @param create_op_func
   *        Caller's factory function that will generate the actual `n_ops` async::Op objects to store.
   *        The caller shall populate each `Op` it returns with a (small) value of whatever arbitrary type it wants.
   *        `create_op_func()` shall be called, in order, `n_ops` times synchronously, with index argument
   *        ranging fully through `[0, n_ops)`.  A copy of `Op create_op_func(idx)` is saved, and
   *        subsequently `(*this)[idx]` will return a reference to this permanently stored copy.
   */
  explicit Op_list(log::Logger* logger_ptr, size_t n_ops, const Create_op_func& create_op_func);

  // Methods.

  /**
   * Returns the number of async::Op stored in this Op_list.  The returned value is always the same.
   * @return See above.
   */
  size_t size() const;

  /**
   * Returns reference to immutable async::Op at the given index in `[0, size())`.
   * It is just like the `const` variety of `"vector<Op>::operator[]"`.
   *
   * @param idx
   *        Value in `[0, size())`.  `assert()` trips if out of range.
   * @return See above.  Note the address stored in the returned *reference* is valid until destructor runs;
   *         hence it's not necessary (though cheap) to copy the `Op`.
   */
  const Op& operator[](size_t idx) const;

  /**
   * Returns `(*this)[R]`, where we randomly select R as if by random_idx() and communicate it to the caller via
   * optional out-arg.  Logs a TRACE message.
   *
   * @param chosen_idx
   *        If not null, then a post-condition is that `*chosen_idx` has been set to the index of the returned
   *        `Op`.
   * @return See above.  Note the address stored in the returned *reference* is valid until destructor runs;
   *         hence it's not necessary (though cheap) to copy the `Op`.
   */
  const Op& random_op(size_t* chosen_idx = nullptr) const;

  /**
   * Returns a randomly selected index from range [O, size()).
   * @return See above.
   */
  size_t random_idx() const;

  /**
   * Returns reference to immutable internally stored sequence of async::Op in order consistent with other
   * methods of this class.
   * @return See above.
   */
  const std::vector<Op>& ops_sequence() const;

private:
  /**
   * The payload of async::Op objects.  This is effectively `const` (immutable) after construction.  While `Op`
   * copy is light-weight, the effective `const`ness we can nevertheless save a copy by always taking
   * addresses like &m_ops[i] when possible.
   */
  std::vector<Op> m_ops;

  /**
   * Random number facility for generating random indices in `[0, m_ops.size())`.  Thread-safe sans mutex.
   *
   * ### Implementation detail ###
   * `m_rnd_op_idx()` is the only used operation, and it emits a pseudo-random number.  This can be viewed as
   * as merely accessing an outside resource, conceptually, hence such operations should probably be `const`, as
   * mutexes are for example.  So, as with a mutex, make it `mutable`.
   */
  mutable util::Rnd_gen_uniform_range_mt<size_t> m_rnd_op_idx;
}; // class Op_list

} // namespace flow::async
