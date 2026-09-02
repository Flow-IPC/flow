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

#include "flow/util/detail/util.hpp"
// So that this_thread_unique_token() is usable (_fwd.hpp only => incomplete type; but they can #include us at least):
#include "flow/util/uniq_id_holder.hpp"
#include "flow/util/string_ostream.hpp"
#include "flow/util/util_fwd.hpp"
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <type_traits>
#include <utility>

namespace flow::util
{

// Types.

/**
 * An empty interface, consisting of nothing but a default `virtual` destructor, intended as a boiler-plate-reducing
 * base for any other (presumably `virtual`-method-having) class that would otherwise require a default `virtual`
 * destructor.
 *
 * Usually, if you have a base class `C` at the top of a `virtual`-method-having hierarchy, then it needs a `virtual`
 * destructor, even if it is `= default` or `{}`.  Otherwise, trying to delete an object of subclass `C2 : public C`
 * via a `C*` pointer will fail to call destructor `~C2()` -- which may not be empty, causing leaks and so on.
 * Declaring `~C()` and its empty implementation is surprisingly verbose.  So, instead, don't; and `public`ly derive
 * from Null_interface.
 *
 * It is particularly useful for interface classes.
 */
class Null_interface
{
public:
  // Destructor.

  /**
   * Boring `virtual` destructor.
   *
   * Why is it pure? Main reason: Then Null_interface becomes abstract and cannot be itself instantiated, which is good.
   * Otherwise we'd need a `protected` constructor or something to prevent it.
   *
   * I (ygoldfel) actually thought this means a subclass *has* to now define a body (even if merely `= default` or
   * `{}`): but no: the compiler will (and must) generate an empty (and, because of us, `virtual`) destructor for any
   * subclass that doesn't explicitly define one.  A destructor isn't a regular method, so that's how it works.
   * There will not be a linker error.
   */
  virtual ~Null_interface() = 0;
};

/**
 * Useful as a no-unique-address private member to make a type noncopyable while keeping that type an aggregate
 * (can be direct-initialized).
 *
 * So you can do: `[[no_unique_address]] flow::util::Noncopyable m_nc{};`.
 *
 * ### Rationale ###
 * The usual technique of deriving from `boost::noncopyable` disables aggregateness.  In C++20 declaring
 * a `= delete` copy ctor also disables it.  This trick still works though.
 */
struct Noncopyable
{
  // Constructors/destructor.

  /// Makes it possible to instantiate.
  Noncopyable() = default;
  /// Forbid copying.
  Noncopyable(const Noncopyable&) = delete;

  // Methods.

  /// Forbid copying.
  void operator=(const Noncopyable&) = delete;
};

/**
 * A simple RAII-pattern class template that, at construction, sets the specified location in memory to a specified
 * value, memorizing the previous contents; and at destruction restores the value.  E.g.:
 *
 *   ~~~
 *   thread_local int s_this_thread_val;
 *   ...
 *   {
 *     Scoped_setter<int> setter{&s_this_thread_val, 75}; // Set it to 75 and memorize (whatever).
 *     ...
 *     {
 *       Scoped_setter<int> setter{&s_this_thread_val, 125}; // Set it to 125 and memorize 75.
 *       ...
 *     } // Restored from 125 to 75.
 *     ...
 *   } // Restored from (probably) 75 to (whatever).
 *   ~~~
 *
 * The object is movable, not copyable (which is similar to `unique_ptr`) to prevent "double-restore."  Related:
 * one can easily return customized auto-setter/restorers:
 *
 *   ~~~
 *   thread_local Widget s_widget;
 *   auto widget_setter_auto(Widget&& widget_moved)
 *   {
 *     return flow::util::Scoped_setter<Widget>{&s_widget, std::move(widget_moved)};
 *   }
 *   ...
 *     { // Later, some block: Set s_widget.  Code here doesn't even know/care a Scoped_setter is involved.
 *       const auto setter_auto = widget_setter_auto({ ...widget-init... });
 *       ...
 *     } // Restore s_widget.
 *   ~~~
 *
 * ### Thready safety ###
 * This is a simple object: it just performs a few assignments without any added
 * concurrency protection.  If the memory location can be accessed simultaneously by other threads, watch out.
 *
 * In particular it's a good fit for thread-local locations: `&X`, where `X` is declared `thread_local`, or
 * `X == *(P.get())` where `P` is a `boost::thread_specific_ptr` or equivalent.
 *
 * @tparam Value_t
 *         The stored type, which must be move-assignable and move-constructible.
 *         All `Value` writes are performed using exclusively these operations.
 *         Informally: For best performance when `Value` is a heavy-weight type, these operations should be
 *         be written to be light-weight, such as in terms of swapping a few scalars.
 *         In particular this is already the case for all STL-compliant container types.
 *
 * @internal
 * ### Implementation ###
 * An alternative implementation, which could even be reduced to just an alias, would have used `unique_ptr`.
 * However in this case I (ygoldfel) wanted maximum control for perf.  The use case originally driving this was
 * the thread-local verbosity override: log::Config::this_thread_verbosity_override_auto().  flow::log is fairly
 * paranoid about performance, in general, although admittedly this particular call isn't necessarily ubiquitous.
 */
template<typename Value_t>
class Scoped_setter
{
public:
  // Types.

  /// Alias for template parameter.
  using Value = Value_t;

  // Constructors/destructor.

  /**
   * Post-condition: `*target` contains was `val_src_moved` contained at ctor entry; and the destructor invocation shall
   * reverse this, so that `*target` is restored to its value at entry.
   *
   * `*this` cannot be copied, but it can be moved.  As a result, it is guaranteed that the aforementioned destructor
   * will execute exactly once; however it can be move-constructed-from-`*this` other Scope_setter's destructor,
   * while our own dtor therefore is a no-op.
   *
   * @param target
   *        The target object that shall be set to `val_src_moved` now and restored in our, or moved-object's, dtor.
   *        The current value of `*target` is saved internally via assignment of `move(*target)`.
   *        Behavior undefined (assertion may trip) if null.
   * @param val_src_moved
   *        Value to save to `*target` immediately, via assignment of `move(val_src_moved)`.
   */
  explicit Scoped_setter(Value* target, Value&& val_src_moved);

  /**
   * Move constructor:  `*this` acts as `src_moved` would-have, while `src_moved` becomes a no-op object permanently.
   *
   * @param src_moved
   *        Source object.  Its destructor shall do nothing after this returns.
   */
  Scoped_setter(Scoped_setter&& src_moved);

  /// Prohibit copying: for each `explicit` ctor invocation, there shall be exactly 1 non-no-op dtor invocation.
  Scoped_setter(const Scoped_setter&) = delete;

  /**
   * Restores `*target` (from main ctor) to its value at entry to said ctor; or does nothing if `*this` has been
   * moved-from via the move ctor.
   */
  ~Scoped_setter();

  // Methods.

  /// Prohibit copying (see `delete`d copy ctor).
  Scoped_setter& operator=(const Scoped_setter&) = delete;

  /// Prohibit modifying existing `*this`; except that moving-from is enabled via the move ctor.
  Scoped_setter& operator=(Scoped_setter&&) = delete;

private:
  // Data.

  /// Target object location; see ctors; if null then this is a moved-from Scoped_setter that intentionally no-ops.
  Value* m_target_or_null;

  /// If and only if #m_target_or_null is non-null, this saves `*m_target_or_null`.  Otherwise meaningless.
  Value m_saved_value;
}; // class Scoped_setter

/**
 * A simple locking-proxy class template (execute-around-pointer idiom): at construction locks the given mutex and
 * provides `->` access to the given target object; at destruction unlocks it.  In typical use one obtains `*this` as
 * an unnamed temporary in the middle of an expression, so that the mutex is locked for exactly that expression's
 * duration.  E.g., given `Widget w` and a mutex `m` serializing all access to `w` -- suppose some `w_locked()` returns
 * `Locked_proxy{&w, &m}`:
 *
 *   ~~~
 *   const auto n = w_locked()->compute_n(); // Lock m; invoke w.compute_n(); save the result; unlock m.
 *   ~~~
 *
 * This is the *execute-around-pointer idiom*.  The typical motivation: some class `C` internally owns both a
 * mutex-protected object and the mutex, its background/concurrent machinery locking the latter as needed; and
 * wants to let its user safely invoke methods of that object.  Then `C` can supply a `w_locked()`-style accessor
 * as sketched above -- instead of writing a delegating, internally-locking method for every operation of
 * interest.  The set of available operations then maintains itself, as does the correctness of each such call's
 * locking.
 *
 * To grant read-only (`const` methods only) access, simply use a `const`-qualified #Target type: e.g.,
 * `Locked_proxy<const Widget, ...>`.
 *
 * ### Keep it to one expression ###
 * `*this` existing = the mutex being locked.  The intended use is as an unnamed temporary (see above example):
 * then the lock is held for a few microseconds, and it is impossible to leak.  If you *do* save a `*this` to
 * extend the locked section, keep the scope tight: whatever machinery normally locks that mutex is blocked
 * in the meantime.
 *
 * @see `boost::synchronized_value` implements the same idiom but *owns* both the payload and the mutex
 *      ("wrap your data in me").  `*this`, by contrast, is non-owning: it is for objects whose guarding
 *      mutex exists independently -- e.g., one that guards more state than just the target object.
 *
 * @tparam Target_t
 *         Type of the object to which `*this` proxies access; possibly `const`-qualified (see above).
 * @tparam Mutex_t
 *         A `Lockable`-concept mutex type, e.g. util::Mutex_non_recursive.
 */
template<typename Target_t, typename Mutex_t>
class Locked_proxy
{
public:
  // Types.

  /// Alias for template parameter.
  using Target = Target_t;

  /// Alias for template parameter.
  using Mutex = Mutex_t;

  // Constructors/destructor.

  /**
   * Locks `*mutex` -- blocking as-needed until that is possible -- and makes `*this` proxy `*target`.
   * The destructor invocation shall unlock it.
   *
   * `*this` cannot be copied, but it can be moved.  As a result, it is guaranteed that the aforementioned
   * unlocking will occur exactly once; however it can occur via the destructor of another Locked_proxy that was
   * move-constructed from `*this`, our own dtor therefore being a no-op.  (A moved-from `*this` must not be
   * dereferenced.)
   *
   * @param target
   *        The object to which operator->() shall forward.  Behavior undefined (assertion may trip) if null.
   * @param mutex
   *        The mutex to hold for `*this` lifetime: the mutex -- the one and only one -- by which all
   *        access to `*target` is serialized (see class doc header for the typical setup).
   *        Must be non-null; must outlive `*this`.
   */
  explicit Locked_proxy(Target* target, Mutex* mutex);

  /**
   * Move constructor: `*this` acts as `src_moved` would-have (in particular its eventual destruction unlocks the
   * mutex), while `src_moved` becomes a no-op object permanently.
   *
   * @param src_moved
   *        Source object.  Its destructor shall do nothing after this returns; it must not be dereferenced.
   */
  Locked_proxy(Locked_proxy&& src_moved);

  /// Prohibit copying: for each `explicit` ctor invocation, there shall be exactly 1 unlocking dtor invocation.
  Locked_proxy(const Locked_proxy&) = delete;

  // Methods.

  /// Prohibit copying (see `delete`d copy ctor).
  Locked_proxy& operator=(const Locked_proxy&) = delete;

  /// Prohibit modifying existing `*this`; except that moving-from is enabled via the move ctor.
  Locked_proxy& operator=(Locked_proxy&&) = delete;

  /**
   * Returns pointer to the target object, the guarding mutex being locked by `*this` (do not use if moved-from).
   * @return See above.
   */
  Target* operator->() const;

  /**
   * Returns reference to the target object, the guarding mutex being locked by `*this` (do not use if moved-from).
   * Useful, e.g., to print the target (`os << *w_locked()`) or to pass it to a function taking a reference --
   * keeping in mind the mutex is locked only until the end of the full expression (in typical unnamed-temporary
   * use).
   *
   * @return See above.
   */
  Target& operator*() const;

private:
  // Data.

  /// Holds the caller-supplied mutex locked throughout `*this` lifetime (unless moved-from: then holds nothing).
  Lock_guard<Mutex> m_lock;

  /// The target object to which operator->() forwards.  Meaningless if `*this` is moved-from.
  Target* m_target;
}; // class Locked_proxy

// Template implementations.

// Scoped_setter template implementations.

template<typename Value_t>
Scoped_setter<Value_t>::Scoped_setter(Value* target, Value&& val_src_moved) :
  m_target_or_null(target),
  m_saved_value(std::move(*m_target_or_null))
{
  *m_target_or_null = std::move(val_src_moved);
}

template<typename Value_t>
Scoped_setter<Value_t>::Scoped_setter(Scoped_setter&& src_moved) : // =default might work fine but to be clear/certain:
  m_target_or_null(src_moved.m_target_or_null),
  m_saved_value(std::move(src_moved.m_saved_value))
{
  assert(m_target_or_null && "Should not be moving-from a thing that has already been moved-from.");

  src_moved.m_target_or_null = nullptr;
  // As promised: Now src_moved's dtor will no-op.
}

template<typename Value_t>
Scoped_setter<Value_t>::~Scoped_setter()
{
  if (m_target_or_null)
  {
    *m_target_or_null = std::move(m_saved_value);
  }
  // else { `*this` must have been moved-from.  No-op. }
}

// Locked_proxy template implementations.

template<typename Target_t, typename Mutex_t>
Locked_proxy<Target_t, Mutex_t>::Locked_proxy(Target* target, Mutex* mutex) :
  m_lock(*mutex), // Locks it (blocking as-needed).
  m_target(target)
{
  assert(target && "Locked_proxy ctor: target must be non-null.");
}

template<typename Target_t, typename Mutex_t>
Locked_proxy<Target_t, Mutex_t>::Locked_proxy(Locked_proxy&&) = default;

template<typename Target_t, typename Mutex_t>
Target_t* Locked_proxy<Target_t, Mutex_t>::operator->() const
{
  return m_target;
}

template<typename Target_t, typename Mutex_t>
Target_t& Locked_proxy<Target_t, Mutex_t>::operator*() const
{
  return *m_target;
}

// Free function template (and/or constexpr) implementations.

template<typename Time_unit, typename N_items>
double to_mbit_per_sec(N_items items_per_time, size_t bits_per_item)
{
  /* Let there be U/W seconds per Time_unit.  Then the following holds:
   *
   * items_per_time items/Time_units * W/U Time_units/second * bits_per_item bits/item
   *   * 1/(1000*1000) megabits/bits
   *   = items_per_time * W / U * bits_per_item / 1000 / 1000 megabits/second.
   *
   * Notice the units work out.  W and U are conveniently available in Time_unit::period, which is a boost::ratio. */

  return
    /* This zealously converts everything to double ASAP to avoid overflow.  Could probably speed things up a bit
     * by postponing some of those conversions until after some integer multiplications, but then overflows could
     * creep in.  It's best not to assume too much about the values of den and num, as this function is meant to
     * be rather adaptable to various situations.  I did try to avoid unnecessary divisions though in favor of
     * multiplications, sort of guessing the latter are faster.  Or not... *shrug*. */
    double(items_per_time) * double(bits_per_item) * double(Time_unit::period::den)
    / (double(Time_unit::period::num) * double(1000 * 1000));
}

template<typename Integer, typename Integer2>
constexpr Integer ceil_div(Integer dividend, Integer2 divisor)
{
  // ceil(A : B) = (A + B - 1) / B, where : is floating point division, while / is integer division.
  static_assert(std::is_integral_v<Integer>, "ceil_div<T, T2>: T must be an integer type.");
  static_assert(std::is_integral_v<Integer2>, "ceil_div<T, T2>: T2 must be an integer type.");

  // assert(dividend >= 0); // Cannot do that (or throw in C++17, can in C++20; @todo) in constexpr.
  // assert(divisor > 0); // Ditto.

  return (dividend + static_cast<Integer>(divisor) - static_cast<Integer>(1)) / static_cast<Integer>(divisor);
  /* (Could one do further bitwise trickery?  Perhaps but let optimizer do it.  Wouldn't optimizer also just
   * optimize a literal floating-point `ceil(a / b)`?  Well, no.  Probably not.  So we wrote this function.) */
}

template<typename Integer, typename Integer2>
constexpr Integer round_to_multiple(Integer dividend, Integer2 unit)
{
  return static_cast<Integer>(unit) * ceil_div(dividend, unit);
}

template<uint64_t POSITIVE_INT>
constexpr bool is_power_of_two()
{
  static_assert(POSITIVE_INT > 0, "UINT tparam needs to be a positive integer.");
  return (POSITIVE_INT & (POSITIVE_INT - uint64_t(1))) == uint64_t(0);
}

constexpr size_t max_align_sz()
{
  return alignof(std::max_align_t);
}

template<typename Data, size_t ALIGN_SZ>
constexpr size_t aligned_sz_of()
{
  static_assert(is_power_of_two<ALIGN_SZ>(), "ALIGN_SZ must be one of 1, 2, 4, 8, ....");
  return round_to_multiple(sizeof(Data), ALIGN_SZ);
}

template<typename Prefix, typename Data, size_t ALIGN_SZ>
Prefix* aligned_prefix_before(Data* data_ptr)
{
  const auto data_byte_ptr = reinterpret_cast<uintptr_t>(data_ptr);
  assert(((data_byte_ptr % ALIGN_SZ) == 0)
           && "Input ptr is not aligned; output wouldn't be either.");

  return reinterpret_cast<Prefix*>
           (data_byte_ptr - aligned_sz_of<Prefix, ALIGN_SZ>());
}

template<typename Prefix, typename Data, size_t ALIGN_SZ>
const Prefix* aligned_prefix_before(const Data* data_ptr)
{
  return aligned_prefix_before<Prefix, Data, ALIGN_SZ>(const_cast<Data*>(data_ptr));
}

template<typename Data, typename Prefix, size_t ALIGN_SZ>
Data* after_aligned_prefix(Prefix* prefix_ptr)
{
  const auto prefix_byte_ptr = reinterpret_cast<uintptr_t>(prefix_ptr);
  assert(((prefix_byte_ptr % ALIGN_SZ) == 0)
           && "Input ptr is not aligned; output wouldn't be either.");

  return reinterpret_cast<Data*>
           (prefix_byte_ptr + aligned_sz_of<Prefix, ALIGN_SZ>());
}

template<typename Data, typename Prefix, size_t ALIGN_SZ>
const Data* after_aligned_prefix(const Prefix* prefix_ptr)
{
  return after_aligned_prefix<Data, Prefix, ALIGN_SZ>(const_cast<Prefix*>(prefix_ptr));
}

template<typename T>
bool in_closed_range(T const & min_val, T const & val, T const & max_val)
{
  // This writes "(min_val <= val) && (val <= max_val)" by using only <, to support the greatest number of types.
  return ((min_val < val) || (!(val < min_val))) &&
         ((val < max_val) || (!(max_val < val)));
}

template<typename T>
bool in_open_closed_range(T const & min_val, T const & val, T const & max_val)
{
  // This writes "(min_val < val) && (val <= max_val)" by using only <, to support the greatest number of types.
  return (min_val < val) &&
         ((val < max_val) || (!(max_val < val)));
}

template<typename T>
bool in_closed_open_range(T const & min_val, T const & val, T const & max_val)
{
  // This writes "(val < max_val) && (min_val <= val)" by using only <, to support the greatest number of types.
  return (val < max_val) &&
         ((min_val < val) || (!(val < min_val)));
}

template<typename T>
bool in_open_open_range(T const & min_val, T const & val, T const & max_val)
{
  return (min_val < val) && (val < max_val);
}

template<typename Container>
bool key_exists(const Container& container, const typename Container::key_type& key)
{
  return container.find(key) != container.end();
}

template<typename Cleanup_func>
Auto_cleanup setup_auto_cleanup(Cleanup_func&& func)
{
  /* This trick, from shared_ptr or bind Boost docs (if I recall correctly), uses shared_ptr's deleter feature.  The
   * Auto_cleanup gains "ownership" of null pointer, purely for the purpose of running a deleter on it when the object
   * goes out of scope sometime later. */
  return Auto_cleanup{nullptr,
                      [func = std::move(func)](auto) { func(); }};
}

template<typename Minuend, typename Subtrahend>
bool subtract_with_floor(Minuend* minuend, const Subtrahend& subtrahend, const Minuend& floor)
{
  assert(minuend);

  /* Basically just avoid implicit conversions and anything that mind overflow or underflow.
   * The one underflow we allow is the subtraction of `floor`: doc header says keep `floor` small.
   * So it's their problem if it's not. */

  const Minuend converted_subtrahend = Minuend{subtrahend};

  // min - sub <= floor <===> min - floor <= sub.
  if ((*minuend - floor) <= converted_subtrahend)
  {
    *minuend = floor;
    return false;
  }
  // else
  *minuend -= converted_subtrahend;
  return true;
}

template<typename From, typename To>
size_t size_unit_convert(From num_froms)
{
  return ((num_froms * sizeof(From)) + sizeof(To) - 1) / sizeof(To);
}

template<typename... T>
void feed_args_to_ostream(std::ostream* os, T&&... ostream_args)
{
  (*os << ... << std::forward<T>(ostream_args));
}

template<typename... T>
void ostream_op_to_string(std::string* target_str, T&&... ostream_args)
{
  using std::flush;

  /* Pushes characters directly onto an `std::string`, instead of doing so into an `ostringstream` and then getting it
   * by copy via `ostringstream::copy()`.  This is for performance and may make a large difference
   * overall, if this is used in logging for example.  However, Thread_local_string_appender accomplishes
   * better performance still and some other features. */
  String_ostream os{target_str};
  feed_args_to_ostream(&(os.os()), std::forward<T>(ostream_args)...);
  os.os() << flush;
}

template<typename... T>
std::string ostream_op_string(T&&... ostream_args)
{
  using std::string;

  string result;
  ostream_op_to_string(&result, std::forward<T>(ostream_args)...);
  return result;
}

template<typename Map, typename Sequence>
void sequence_to_inverted_lookup_map
       (Sequence const & src_seq, Map* target_map,
        const Function<typename Map::mapped_type (size_t)>& idx_to_map_val_func)
{
  size_t idx = 0;
  for (const auto& src_element : src_seq)
  {
    (*target_map)[src_element] = idx_to_map_val_func(idx);
    ++idx;
  }
}

template<typename Map, typename Sequence>
void sequence_to_inverted_lookup_map(Sequence const & src_seq, Map* target_map)
{
  sequence_to_inverted_lookup_map(src_seq, target_map, [](size_t idx) -> size_t
  {
    return idx;
  });
}

template<typename Const_buffer_sequence>
std::ostream& buffers_to_ostream(std::ostream& os,
                                 const Const_buffer_sequence& data,
                                 const std::string& indentation,
                                 size_t bytes_per_line)
{
  using boost::io::ios_fill_saver;
  using boost::io::ios_flags_saver;
  using boost::io::ios_width_saver;
  using boost::asio::buffers_iterator;
  using std::isprint;

  /* This sweet type will iterate over the buffer sequence (jumping between contained buffers, if ther are > 1).
   * If `Bufs_iter it`, then *it is of type uint8_t. */
  using Bufs_iter = buffers_iterator<Const_buffer_sequence, uint8_t>;

  constexpr size_t BYTES_PER_LINE_DEFAULT = 16;
  bool single_line_mode = false;
  if (bytes_per_line == 0)
  {
    bytes_per_line = BYTES_PER_LINE_DEFAULT;
  }
  else if (bytes_per_line == size_t(-1))
  {
    /* Firstly just pretend exactly the bytes in the whole input = "max" bytes per line.
     * This accomplishes the bulk of what "single-line mode" means. */
    bytes_per_line = buffer_size(data);
    // A bit slow potentially to need to enumerate all scattered buffers.  Eh.  Contract said we should be assumed slow.

    // The rest of what it means is lacking a newline at the end in single-line mode.  So just remember that part.
    single_line_mode = true;
  }

  // Ensure format settings return to their previous values subsequently.
  ios_flags_saver flags_saver{os};
  ios_fill_saver fill_saver{os};
  ios_width_saver width_saver{os};

  /* Set formatting and output numeric value (hex) of first byte.
   * @todo Is there a way to write this with manipulators too? */
  os.setf(std::ios::right | std::ios::hex, std::ios::adjustfield | std::ios::basefield);
  os << std::setfill('0');

  const Bufs_iter end_byte_it = Bufs_iter::end(data);
  for (Bufs_iter cur_byte_it = Bufs_iter::begin(data);
       cur_byte_it != end_byte_it;
       /* Advancing of cur_byte_it occurs within body of loop. */)
  {
    // The for() loop around us guarantees there is at least this first byte.  Print the numeric value.
    os << indentation << '['
       << std::setw(2) << int(*cur_byte_it); // Numeric value in hex.

    // Repeat for remaining bytes left in this line.  Stop at bytes/line limit, or if reached end of buffers.
    size_t n_bytes_printed;
    for ((n_bytes_printed = 1), ++cur_byte_it; // Account for printing that first byte above.
         (n_bytes_printed != bytes_per_line) && (cur_byte_it != end_byte_it);
         ++cur_byte_it, ++n_bytes_printed)
    {
      os << ' ' << std::setw(2) << int(*cur_byte_it); // Numeric value in hex.
    }

    // Spaces as if rest of line still had a few ghost values to print (space + 2 spaces for the hex digits).
    for (size_t n_bytes_printed_including_padding = n_bytes_printed;
         n_bytes_printed_including_padding != bytes_per_line;
         ++n_bytes_printed_including_padding)
    {
      os << "   ";
    }

    // Backtrack and print those same bytes -- this time as printable characters (when printable, else dots).
    cur_byte_it -= n_bytes_printed;

    os << '|';
    for (size_t n_chars_printed = 0;
         n_chars_printed != n_bytes_printed;
         ++cur_byte_it, ++n_chars_printed)
    {
      char c = *cur_byte_it;
      os << (isprint(c) ? c : '.');
    }
    os << ']';
    if (!single_line_mode)
    {
      os << '\n';
    }
  } // for (cur_byte_it)

  return os;

  // The formatting changes will be restored here as the savers exit scope.
} // buffers_to_ostream()

template<typename Const_buffer_sequence>
std::string buffers_dump_string(const Const_buffer_sequence& data, const std::string& indentation,
                                size_t bytes_per_line)
{
  using std::flush;
  using std::string;

  // See comment in ostream_op_to_string() which applies here too (re. perf).

  string target_str;
  String_ostream os{&target_str};
  buffers_to_ostream(os.os(), data, indentation, bytes_per_line);
  os.os() << flush;

  return target_str;
}

template<typename Enum>
Enum istream_to_enum(std::istream* is_ptr, Enum enum_default, Enum enum_sentinel,
                     bool accept_num_encoding, bool case_sensitive,
                     Enum enum_lowest)
{
  using boost::lexical_cast;
  using boost::bad_lexical_cast;
  using boost::algorithm::equals;
  using boost::algorithm::is_iequal;
  using std::locale;
  using std::string;
  using std::isdigit;
  using std::isalnum;
  using Traits = std::char_traits<char>;
  using enum_t = std::underlying_type_t<Enum>;

  // Reminder: There are various assumptions about Enum this takes for granted; behavior undefined otherwise.

  assert(enum_t(enum_lowest) >= 0); // Otherwise we'd have to allow '-' (minus sign), and we'd... just rather not.
  auto& is = *is_ptr;
  const is_iequal i_equal_func{locale::classic()};

  // Read into `token` until (and not including) the first non-alphanumeric/underscore character or stream end.
  string token;
  char ch;
  while (((ch = is.peek()) != Traits::eof()) && (isalnum(ch) || (ch == '_')))
  {
    token += ch;
    is.get();
  }

  Enum val = enum_default;

  if (!token.empty())
  {
    if (accept_num_encoding && isdigit(token.front())) // Hence ostream<< shouldn't serialize a digit-leading value.
    {
      enum_t num_enum;
      try
      {
        num_enum = lexical_cast<enum_t>(token);
        // This assumes a vanilla enum integer value ordering.
        if ((num_enum >= enum_t(enum_sentinel) || (num_enum < enum_t(enum_lowest))))
        {
          num_enum = enum_t(enum_default);
        }
        val = Enum{num_enum};
      }
      catch (const bad_lexical_cast&)
      {
        assert(val == enum_default);
      }
    } // if (accept_num_encoding && isdigit())
    else // if (!(accept_num_encoding && isdigit()))
    {
      enum_t idx;
      // This assumes a vanilla enum integer value ordering within this [closed range].
      for (idx = enum_t(enum_lowest); idx != enum_t(enum_sentinel); ++idx)
      {
        const auto candidate = Enum{idx};
        /* Note -- lexical_cast<string>(Enum) == (operator<<(ostringstream&, Enum)).str() -- the symbolic
         * encoding of Enum (as we promised to accept, case-[in]sensitively), not the numeric encoding.  The numeric
         * encoding is checked-for in the `if (accept_num_encoding...)` branch above using a non-looping technique. */
        if (case_sensitive ? equals(token, lexical_cast<string>(candidate))
                           : equals(token, lexical_cast<string>(candidate), i_equal_func))
        {
          val = candidate;
          break;
        }
      }
      assert((idx != enum_t(enum_sentinel)) || (val == enum_default));
    } // else if (!(accept_num_encoding && isdigit()))
  } // if (!token.empty())

  return val;
} // istream_to_enum()

template<typename T, typename... Ctor_args>
T* construct_at(T* obj, Ctor_args&&... ctor_args)
{
  using Value = T;

  // Use placement-new expression used by C++20's construct_at() per cppreference.com.
  return ::new (const_cast<void*>
                  (static_cast<void const volatile*>
                     (obj)))
           Value(std::forward<Ctor_args>(ctor_args)...);
  /* Careful -^-... Value{} would be preferred in most non-generic code, but generically as here it can mess things
   * up by unintentionally invoking an initializer-list ctor form, if one exists, and the types line up just so
   * (as when Value is, say, vector<size_t>, and one passes-in a single integer).  Use Value() to avoid
   * any such surprises. */
}

template<typename T>
T* default_init_at(T* obj)
{
  using Value = T;

  static_assert(std::is_trivially_default_constructible_v<Value>,
                "default_init_at() exists to begin an object's lifetime while writing nothing; a type whose "
                  "default-ction (possibly) writes defeats that purpose.  "
                  "If you want the ctor to run, use construct_at().");

  // Similar to the above but...
  return ::new (const_cast<void*>
                  (static_cast<void const volatile*>
                     (obj)))
           Value; // ...default-initialize: no `()`.  E.g., a struct of `int`s won't have them zeroed by this.
}

} // namespace flow::util
