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
#include "flow/util/string_ostream.hpp"
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>

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
 * from Null_interface instead.
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
 *     Scoped_setter<int> setter(&s_this_thread_val, 75); // Set it to 75 and memorize (whatever).
 *     ...
 *     {
 *       Scoped_setter<int> setter(&s_this_thread_val, 125); // Set it to 125 and memorize 75.
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
 *     return flow::util::Scoped_setter<Widget>(&s_widget, std::move(widget_moved));
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
 * `X == *(P.get())` where `P` is a `boost::thread_specific_ptr`.
 *
 * @tparam Value
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
template<typename Value>
class Scoped_setter
{
public:
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

// Template implementations.

// Scoped_setter template implementations.

template<typename Value>
Scoped_setter<Value>::Scoped_setter(Value* target, Value&& val_src_moved) :
  m_target_or_null(target),
  m_saved_value(std::move(*m_target_or_null))
{
  *m_target_or_null = std::move(val_src_moved);
}

template<typename Value>
Scoped_setter<Value>::Scoped_setter(Scoped_setter&& src_moved) : // =default might work fine but to be clear/certain:
  m_target_or_null(src_moved.m_target_or_null),
  m_saved_value(std::move(src_moved.m_saved_value))
{
  assert(m_target_or_null && "Should not be moving-from a thing that has already been moved-from.");

  src_moved.m_target_or_null = nullptr;
  // As promised: Now src_moved's dtor will no-op.
}

template<typename Value>
Scoped_setter<Value>::~Scoped_setter()
{
  if (m_target_or_null)
  {
    *m_target_or_null = std::move(m_saved_value);
  }
  // else { `*this` must have been moved-from.  No-op. }
}

// Free function template implementations.

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

template<typename Integer>
Integer ceil_div(Integer dividend, Integer divisor)
{
  // ceil(A : B) = (A + B - 1) / B, where : is floating point division, while / is integer division.
  static_assert(std::is_integral_v<Integer>, "ceil_div<T>: T must be an integer type.");
  assert(dividend >= 0);
  assert(divisor > 0);

  return (dividend + divisor - 1) / divisor;
  /* (Could one do further bitwise trickery?  Perhaps but let optimizer do it.  Wouldn't optimizer also just
   * optimize a literal floating-point `ceil(a / b)`?  Well, no.  Probably not.  So we wrote this function.) */
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
Auto_cleanup setup_auto_cleanup(const Cleanup_func& func)
{
  /* This trick, from shared_ptr or bind Boost docs (if I recall correctly), uses shared_ptr's deleter feature.  The
   * Auto_cleanup gains "ownership" of null pointer, purely for the purpose of running a deleter on it when the object
   * goes out of scope sometime later.  Deleting 0 itself would have been useless no-op, and instead we ignore the null
   * and simply call user's func(), which is what the goal is.
   *
   * Subtlety: shared_ptr docs say the passed-in deleter is saved by copy, so we needn't worry about it disappearing
   * after we return.
   *
   * Subtlety: we need to make a copy (via capture) of func, as there's zero guarantee (and low likelihood in practice)
   * that func is a valid object at the time cleanup is actually needed (sometime after we return). */
  return Auto_cleanup(static_cast<void*>(0),
                      [func](void*) { func(); });
}

template<typename Minuend, typename Subtrahend>
bool subtract_with_floor(Minuend* minuend, const Subtrahend& subtrahend, const Minuend& floor)
{
  assert(minuend);

  /* Basically just avoid implicit conversions and anything that mind overflow or underflow.
   * The one underflow we allow is the subtraction of `floor`: doc header says keep `floor` small.
   * So it's their problem if it's not. */

  const Minuend converted_subtrahend = Minuend(subtrahend);

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

template<typename T1, typename ...T_rest>
void feed_args_to_ostream(std::ostream* os, T1 const & ostream_arg1, T_rest const &... remaining_ostream_args)
{
  // Induction step for variadic template.
  feed_args_to_ostream(os, ostream_arg1);
  feed_args_to_ostream(os, remaining_ostream_args...);
}

template<typename T>
void feed_args_to_ostream(std::ostream* os, T const & only_ostream_arg)
{
  // Induction base.
  *os << only_ostream_arg;
}

template<typename ...T>
void ostream_op_to_string(std::string* target_str, T const &... ostream_args)
{
  using std::flush;

  /* Pushes characters directly onto an `std::string`, instead of doing so into an `ostringstream` and then getting it
   * by copy via `ostringstream::copy()`.  This is for performance and may make a large difference
   * overall, if this is used in logging for example.  However, Thread_local_string_appender accomplishes
   * better performance still and some other features. */
  String_ostream os(target_str);
  feed_args_to_ostream(&(os.os()), ostream_args...);
  os.os() << flush;
}

template<typename ...T>
std::string ostream_op_string(T const &... ostream_args)
{
  using std::string;

  string result;
  ostream_op_to_string(&result, ostream_args...);
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
  ios_flags_saver flags_saver(os);
  ios_fill_saver fill_saver(os);
  ios_width_saver width_saver(os);

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
  String_ostream os(&target_str);
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
  const is_iequal i_equal_func(locale::classic());

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
        val = Enum(num_enum);
      }
      catch (const bad_lexical_cast& exc)
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
        const auto candidate = Enum(idx);
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

} // namespace flow::util
