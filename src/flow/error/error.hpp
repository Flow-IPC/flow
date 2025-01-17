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

#include "flow/error/error_fwd.hpp"
#include "flow/log/log.hpp"
#include "flow/util/detail/util.hpp"
#include <boost/system/system_error.hpp>
#include <stdexcept>

namespace flow::error
{
// Types.

/**
 * An `std::runtime_error` (which is an `std::exception`) that stores an #Error_code.  We derive from boost.system's
 * `system_error` which already does that nicely.  This polymorphic subclass merely improves the `what()`
 * message a little bit -- specially handling the case when there is no #Error_code, or it is falsy -- but is
 * otherwise identical.
 *
 * ### Rationale ###
 * It is questionable whether Runtime_error is really necessary.  One can equally well simply throw
 * `boost::system::system_error(err_code, context)` when `bool(err_code) == true`, and
 * `std::runtime_error(context)` otherwise.  Indeed the user should feel 100% free to do that if desired.
 * flow::error::Runtime_error is mere syntactic sugar for code brevity, when one indeed has an `err_code` that may
 * or may not be falsy: they can just construct+throw a Runtime_error and not worry about the bifurcation.
 * In the end one likely just catches `std::exception exc` and logs/prints `exc.what()`: how precisely it was thrown
 * is of low import, typically.  Runtime_error provides a concise way (with some arguable niceties like the use
 * of `String_view`) to throw it when desired.
 */
class Runtime_error :
  public boost::system::system_error
{
public:
  // Constructors/destructor.

  /**
   * Constructs Runtime_error.
   *
   * @param err_code_or_success
   *        The #Error_code describing the error if available; or the success value (`Error_code()`)
   *        if an error code is unavailable or inapplicable to this error.
   *        In the latter case what() will omit anything to do with error codes and feature only `context`.
   * @param context
   *        String describing the context, i.e., where/in what circumstances the error occurred/what happened.
   *        For example: "Peer_socket::receive()" or "Server_socket::accept() while checking
   *        state," or a string with line number and file info.  Try to keep it brief but informative.
   *        FLOW_UTIL_WHERE_AM_I_STR() may be helpful.
   */
  explicit Runtime_error(const Error_code& err_code_or_success, util::String_view context = "");

  /**
   * Constructs Runtime_error, when one only has a context string and no applicable/known error code.
   * Formally it's equivalent to `Runtime_error(Error_code(), context)`: it is syntactic sugar only.
   *
   * @param context
   *        See the other ctor.
   */
  explicit Runtime_error(util::String_view context);

  // Methods.

  /**
   * Returns a message describing the exception.  Namely:
   *   - If no/success #Error_code passed to ctor: Message includes `context` only.
   *   - If non-success #Error_code passed to ctor: Message includes: numeric value of `Error_code`, a brief
   *     string representing the code category to which it belongs, the system message corresponding to the
   *     `Error_code`, and `context`.
   *
   * @return See above.
   */
  const char* what() const noexcept override;

private:
  // Data.

  /**
   * This is a copy of `context` from ctor if `!err_code_or_success`; or unused otherwise.
   *
   * ### Rationale ###
   * We want what() to act as follows:
   *   - `!err_code_or_success`: Error codes are inapplicable; return `context` only.
   *   - Else: Return a string with `context`; plus all details of the error code.
   *
   * The latter occurs in our superclass `what()` already, if we pass up `context` to the super-ctor.
   * Now suppose `!err_code_or_success`.  No matter which super-ctor we use, it will memorize an `Error_code` --
   * if we pass `Error_code()` it'll remember that; if use a ctor that does not take an `Error_code()` it will
   * memorize its own `Error_code()`.  Therefore we must override `what()` behavior in our own what() in that case.
   *
   * Therefore this algorithm works:
   *   - `!err_code_or_success`: Memorize `context` in #m_context_if_no_code.  what() just returns the latter.
   *     Do not pass up `context`: we shall never use superclass's `what()` which is the only thing that would
   *     access such a thing.
   *   - Else: Do pass `context` up to super-constructor.  Superclass `what()` shall do the right thing.
   *     Do not save to `m_context_if_no_code`: our what() just forwards to superclass `what()`, so the lines
   *     that would ever access #m_context_if_no_code never execute.
   *
   * Note: Past versions of boost.system worked differently; it was possible to get by without this.
   *
   * ### Performance ###
   * Using the above algorithm `context` is copied exactly once either way: either into #m_context_if_no_code or
   * up into superclass's structures.  The other guy then just gets a blank string.
   */
  const std::string m_context_if_no_code;
}; // class Runtime_error

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Func, typename Ret>
bool exec_and_throw_on_error(const Func& func, Ret* ret,
                             Error_code* err_code, util::String_view context)
{
  /* To really "get" what's happening, just pick an example invoker of the macro which invokes us.
   * The background is in the FLOW_ERROR_EXEC_AND_THROW_ON_ERROR() doc header; but to really picture the whole thing,
   * just pick an example and work through how it all fits together.
   *
   * Note: This could also be done by simply stuffing all of the below into the macro and eliminating the
   * present function.  Well, we don't roll that way: we like debugger-friendliness, etc., etc.
   * We use the preprocessor ONLY when what it provides cannot be done without it. */

  if (err_code)
  {
    /* All good: the caller can assume non-null err_code; can perform func() equivalent and set *err_code on error.
     * Our Runtime_error-throwing wrapping services are not required. */
    return false;
  }

  /* err_code is null, so we have to make our own Error_code and throw an error if the wrapped operation actually
   * sets it (indicating an error occurred). */
  Error_code our_err_code;

  /* Side note: It is assumed (but we don't enforce) that func() is the same as what would execute above in the
   * `return false;` case.  That's why typically the caller (usually an API; say, F(a, b, ..., err_code)) would
   * use lambda/bind() to maka a functor F(e_c) that executes F(a, b, ..., e_c).
   * So here we pass our_err_code as "e_c." */
  *ret = func(&our_err_code);

  if (our_err_code)
  {
    /* Error was detected: do our duty.  Pass through the context info from caller.
     * Note that passing in, say, FLOW_UTIL_WHERE_AM_I() would be less useful, since the present location
     * is not helpful to the log reader in determining where the actual error first occurred. */
    throw Runtime_error(our_err_code, context);
  }

  return true;
} // exec_and_throw_on_error()

template<typename Func>
bool exec_void_and_throw_on_error(const Func& func, Error_code* err_code, util::String_view context)
{
  // See exec_and_throw_on_error().  This is just a simplified version where func() returns void.

  if (err_code)
  {
    return false;
  }

  Error_code our_err_code;
  func(&our_err_code);

  if (our_err_code)
  {
    throw Runtime_error(our_err_code, context);
  }

  return true;
} // exec_void_and_throw_on_error()

} // namespace flow::error

// Macros.

/**
 * Sets `*err_code` to `ARG_val` and logs a warning about the error using FLOW_LOG_WARNING().
 * An `err_code` variable of type that is pointer to flow::Error_code must be declared at the point where the macro is
 * invoked.
 *
 * @param ARG_val
 *        Value convertible to flow::Error_code.  Reminder: `Error_code` is trivially/implicitly convertible from
 *        any error code set (such as `errno`s and boost.asio network error code set) that has been
 *        boost.system-enabled.
 */
#define FLOW_ERROR_EMIT_ERROR(ARG_val) \
  FLOW_UTIL_SEMICOLON_SAFE \
  ( \
    ::flow::Error_code FLOW_ERROR_EMIT_ERR_val(ARG_val); \
    FLOW_LOG_WARNING("Error code emitted: [" << FLOW_ERROR_EMIT_ERR_val << "] " \
                     "[" << FLOW_ERROR_EMIT_ERR_val.message() << "]."); \
    *err_code = FLOW_ERROR_EMIT_ERR_val; \
  )

/**
 * Identical to FLOW_ERROR_EMIT_ERROR(), but the message logged has flow::log::Sev::S_INFO severity instead of
 * `S_WARNING`.
 *
 * @param ARG_val
 *        See FLOW_ERROR_EMIT_ERROR().
 */
#define FLOW_ERROR_EMIT_ERROR_LOG_INFO(ARG_val) \
  FLOW_UTIL_SEMICOLON_SAFE \
  ( \
    ::flow::Error_code FLOW_ERROR_EMIT_ERR_LOG_val(ARG_val); \
    FLOW_LOG_INFO("Error code emitted: [" << FLOW_ERROR_EMIT_ERR_LOG_val << "] " \
                  "[" << FLOW_ERROR_EMIT_ERR_LOG_val.message() << "]."); \
    *err_code = FLOW_ERROR_EMIT_ERR_LOG_val; \
  )

/**
 * Logs a warning about the given error code using FLOW_LOG_WARNING().
 *
 * @param ARG_val
 *        See FLOW_ERROR_EMIT_ERROR().
 */
#define FLOW_ERROR_LOG_ERROR(ARG_val) \
  FLOW_UTIL_SEMICOLON_SAFE \
  ( \
    ::flow::Error_code FLOW_ERROR_LOG_ERR_val(ARG_val); \
    FLOW_LOG_WARNING("Error occurred: [" << FLOW_ERROR_LOG_ERR_val << "] " \
                     "[" << FLOW_ERROR_LOG_ERR_val.message() << "]."); \
  )

/**
 * Logs a warning about the (often `errno`-based or from a library) error code in `sys_err_code`.
 * `sys_err_code` must be an object of type flow::Error_code in the context of the macro's invocation.
 * See also FLOW_ERROR_SYS_ERROR_LOG_FATAL().
 *
 * Note this implies a convention wherein system (especially `errno`-based or from a libary) error codes are to
 * be saved into a stack variable or parameter `flow::Error_code sys_err_code`.  Of course if you don't like this
 * convention and/or this error message, it is trivial to log something manually.
 *
 * It's a functional macro despite taking no arguments to convey that it mimics a `void` free function.
 *
 * The recommended (but in no way enforced or mandatory) pattern is something like:
 *
 *   ~~~
 *   Error_code sys_err_code;
 *
 *   // ...
 *
 *   // sys_err_code has been set to truthy value.  Decision has therefore been made to log about it but otherwise
 *   // recover and continue algorithm.
 *
 *   FLOW_LOG_WARNING("(...Explain what went wrong; output values of interest; but not `sys_err_code` since....)  "
 *                    "Details follow.");
 *   FLOW_ERROR_SYS_ERROR_LOG_WARNING();
 *
 *   // Continue operating.  No assert(false); no std::abort()....
 *   ~~~
 */
#define FLOW_ERROR_SYS_ERROR_LOG_WARNING() \
  FLOW_LOG_WARNING("System error occurred: [" << sys_err_code << "] [" << sys_err_code.message() << "].")

/**
 * Logs a log::Sev::S_FATAL message about the (often `errno`-based or from a library) error code in `sys_err_code`,
 * usually just before aborting the process or otherwise entering undefined-behavior land such as via `assert(false)`.
 * `sys_err_code` must be an object of type flow::Error_code in the context of the macro's invocation.
 *
 * This is identical to FLOW_ERROR_SYS_ERROR_LOG_WARNING(), except the message logged has FATAL severity instead
 * of a mere WARNING.  Notes in that macro's doc header generally apply.  However the use case is different.
 *
 * The recommended (but in no way enforced or mandatory) pattern is something like:
 *
 *   ~~~
 *   Error_code sys_err_code;
 *
 *   // ...
 *
 *   // sys_err_code has been set to truthy value that is completely unexpected or so unpalatable as to make
 *   // any attempt at recovery not worth the effort.  Decision has therefore been made to abort and/or
 *   // enter undefined-behavior land.
 *
 *   FLOW_LOG_FATAL("(...Explain what went wrong, and why it is very shocking; output values of interest; but not "
 *                  "`sys_err_code` since....)  Details follow.");
 *   FLOW_ERROR_SYS_ERROR_LOG_FATAL();
 *
 *   // Enter undefined-behavior land.
 *   // Different orgs/projects do different things; but a decent approach might be:
 *   assert(false && "(...Re-explain what went wrong, so it shows up in the assert-trip message on some stderr.");
 *   // Possibly really abort program, even if assert()s are disabled via NDEBUG.
 *   std::abort();
 *   ~~~
 */
#define FLOW_ERROR_SYS_ERROR_LOG_FATAL() \
  FLOW_LOG_FATAL("System error occurred: [" << sys_err_code << "] [" << sys_err_code.message() << "].")

/**
 * Narrow-use macro that implements the error code/exception semantics expected of most public-facing Flow (and
 * Flow-like) class method APIs.  The semantics it helps implement are explained in flow::Error_code doc header.
 * More formally, here is how to use it:
 *
 * First, please read flow::Error_code doc header.  Next read on:
 *
 * Suppose you have an API `f()` in some class `C` that returns type `T` and promises, in its doc header,
 * to implement the error reporting semantics listed in the aforementioned flow::Error_code doc header.  That is, if
 * user passes in null `err_code`, error would cause `Run_time error(e_c)` to be thrown; if non-null, then
 * `*err_code = e_c` would be set sans exception -- `e_c` being the error code explaining what went wrong,
 * such as flow::net_flow::error::Code::S_WAIT_INTERRUPTED.  Then here's how to use the macro:
 *
 *   ~~~
 *   // Example API.  In this one, there's a 4th argument that happens to follow the standard Error_code* one.
 *   // arg2 is a (const) reference, as opposed to a pointer or scalar, and is most efficiently handled by adding
 *   // cref() to avoid copying it.
 *   T f(AT1 arg1, const AT2& arg2, Error_code* err_code = 0, AT3 arg3 = 0)
 *   {
 *     FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(T, // Provide the return type of the API.
 *                                           // Forward all the args into the macro, but replace `err_code` => `_1`.
 *                                        arg1, arg2, _1, arg3);
 *     // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.
 *
 *     // ...Bulk of f() goes here!  You can now set *err_code to anything without fear....
 *   }
 *   ~~~
 *
 * @see flow::Error_code for typical error reporting semantics this macro helps implement.
 * @see exec_void_and_throw_on_error() which you can use directly when `ARG_ret_type` would be void.
 *      The present macro does not work in that case; but at that point using the function directly is concise enough.
 * @see exec_and_throw_on_error() which you can use directly when `T` is a reference type.
 *      The present macro does not work in that case.
 *
 * @param ARG_ret_type
 *        The return type of the invoking function/method.  It cannot be a reference type.
 *        In practice this would be, for example,
 *        `size_t` when wrapping a `receive()` (which returns # of bytes received or 0).
 * @param ARG_function_name
 *        Suppose you wanted to, via infinite recursion, call the function -- from where you use this macro --
 *        and would therefore write a statement of the form `return F(A1, A2, ..., err_code, ...);`.
 *        `ARG_function_name` shall be the `F` part of such a hypothetical statement.
 *        Tip: Even in non-`static` member functions (methods), just the method name -- without the class name `::`
 *        part -- is almost always fine.
 *        Tip: However template args typically do have to be forwarded (e.g., in `template<typename T> X::f() {}`
 *        you'd supply `f<T>` as `ARG_function_name`.
 *        Example: in a `bool X::listen(int x, Error_code* err_code, float y) {}` you'd have
 *        `{ return listen(x, err_code, y); }` and thus `ARG_function_name` shall be just `listen`.
 * @param ...
 *        First see the premise in the doc header for `ARG_function_name` just above.
 *        Then the `...` arg list shall be as follows:
 *        `A1, A2, ..., _1, ...`.  In other words it shall be the arg list for the invoking function/method as-if
 *        recursively calling it, but with the `err_code` arg replaced by the special identifier `_1`.
 *        Example: in a `bool X::listen(int x, Error_code* err_code, float y) {}` you'd have
 *        `{ return listen(x, err_code, y); }` and thus `...` arg list shall be: `x, _1, y`.
 */
#define FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(ARG_ret_type, ARG_function_name, ...) \
  FLOW_UTIL_SEMICOLON_SAFE \
  ( \
    /* We need both the result of the operation (if applicable) and whether it actually ran. */ \
    /* So we must introduce this local variable (note it's within a { block } so should minimally interfere with */ \
    /* the invoker's code).  We can't use a sentinel value to combine the two, since the operation's result may */ \
    /* require ARG_ret_type's entire range. */ \
    ARG_ret_type result; \
    /* We provide the function: f(Error_code*), where f(e_c) == ARG_method_name(..., e_c, ...). */ \
    /* Also supply context info of this macro's invocation spot. */ \
    /* Note that, if f() is executed, it may throw Runtime_error which is the point of its existence. */ \
    if (::flow::error::exec_and_throw_on_error \
          ([&](::flow::Error_code* _1) -> ARG_ret_type \
             { return ARG_function_name(__VA_ARGS__); }, \
           &result, err_code, FLOW_UTIL_WHERE_AM_I_LITERAL(ARG_function_name))) \
    { \
      /* Aforementioned f() WAS executed; did NOT throw (no error); and return value was placed into `result`. */ \
      return result; \
    } \
    /* else: */ \
    /* f() did not run, because err_code is non-null.  So now macro invoker should do its thing assuming that fact. */ \
    /* Recall that the idea is that f() is just recursively calling the method invoking this macro with the same */ \
    /* arguments except for the Error_code* arg, where they supply specifically `_1` by our contract. */ \
  )

/* Now that we're out of that macro's body with all the backslashes...
 * Discussion of the FLOW_UTIL_WHERE_AM_I_LITERAL(ARG_function_name) snippet above:
 *
 * Considering the potential frequency that FLOW_ERROR_EXEC_AND_THROW_ON_ERROR() is invoked in
 * an error-reporting API (such as flow::net_flow) -- even *without* an error actually being emitted --
 * it is important we do not add undue computation.  That macro does not take a context string, so it must
 * compute it itself; as usual we use file/function/line for this.  With the technique used above
 * FLOW_UTIL_WHERE_AM_I_LITERAL() is replaced by a *string literal* -- like:
 *   "/cool/path/to/file.cpp" ":" "Class::someFunc" "(" "332" ")"
 * which is as compile-time as it gets.  So perf-wise that's fantastic; as good as it gets.
 *
 * Are there weaknesses?  Yes; there is one: Per its doc header FLOW_UTIL_WHERE_AM_I_LITERAL(), due to having
 * to be replaced by a literal, cannot cut out "/cool/path/to/" from __FILE__, even though in flow.log we do so
 * for readability of logs.  What if we wanted to get rid of this weakness?  Then we cannot have a literal there;
 * we can use other, not-fully-compile-time-computed FLOW_UTIL_WHERE_AM_I*(); that means extra computation
 * at every call-site.  That, too, can be worked-around: One can add an exec_and_throw_on_error() overload
 * that would take 3 context args instead of 1: strings for file and function, int for line (supplied via
 * __FILE__, #ARG_function, __LINE__); and it would only actually build a context string to pass to
 * Runtime_error *if* (1) an error actually occurred; *and* (2) user in fact used err_code=null (meaning throw on
 * error as opposed to return an Error_code); so lazy-evaluation.
 *
 * That would have been a viable approach but:
 *   - still slower (instead of a single compile-time-known pointer, at least 2 ptrs + 1 ints are passed around
 *     the call stack) at each call-site;
 *   - much slower on exception-throwing error (albeit this being relatively rare, typically; not at each call-site);
 *   - much, much more impl code.
 *
 * So it's cool; just don't massage __FILE__ in a totally flow.log-consistent way. */
