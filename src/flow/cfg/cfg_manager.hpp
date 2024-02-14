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

#include "flow/cfg/cfg_fwd.hpp"
#include "flow/cfg/cfg_manager_fwd.hpp"
#include "flow/cfg/option_set.hpp"
#include "flow/util/util.hpp"
#include "flow/util/shared_ptr_alias_holder.hpp"
#include <boost/array.hpp>

namespace flow::cfg
{
// Types.

/**
 * Utility/traits type to concisely work with final-validation functions when calling methods like
 * Config_manager::apply_static().
 *
 * @tparam Value_set
 *         See Option_set.
 */
template<typename Value_set>
struct Final_validator_func
{
  // Types.

  /**
   * Short-hand for a function that takes a parsed config set (meaning all values are individually OK) and returns
   * what `Config_manager::apply*()` should do to the candidate-for-canonical-state `Value_set` maintained
   * by Config_manager.  Informally its logic should be:
   *   - (Optional) Does `Value_set` indicate that the file that was parsed should be skipped wholesale?  If so
   *     yield Final_validator_outcome::S_SKIP.  Else:
   *   - If there are no further inter-option problems (given that individually each option value is okay)
   *     yield Final_validator_outcome::S_ACCEPT.  Else yield Final_validator_outcome::S_FAIL.
   */
  using Type = Function<Final_validator_outcome (const Value_set&)>;

  // Methods.

  /**
   * Returns a final-validator function that simply always returns Final_validator_outcome::S_ACCEPT.
   * @return See above.
   */
  static Type null();
}; // struct Final_validator_func

/**
 * Empty `struct` suitable as a `*_value_set` template arg for Config_manager, when a slot requires a `Value_set`, but
 * you have no config to actually parse there.  In particular, if you have a static `Value_set` but no corresponding
 * dynamic one, you should provide this as the template arg.
 *
 * @see null_declare_opts_func() and null_final_validator_func().
 */
struct Null_value_set :
  public util::Shared_ptr_alias_holder<boost::shared_ptr<Null_value_set>>
{
};

/**
 * Manages a config setup, intended for a single daemon process, by maintaining
 * 1 or more set(s) of static config and dynamic config, each, via that number of `Option_set<>`-ready raw value
 * `struct` types supplied by the user as template arguments.  There is an even number of template args (or it will
 * not compile); each successive pair contains, in order, a *static* `Value_set` followed by a *dynamic* one.
 * It is possible, for each given pair, to use static config alone or dynamic config alone; in that case pass in
 * Null_value_set for the unused one:
 *
 *   ~~~
 *   // First config pair is static-only; second config pair -- for a separate application module perhaps --
 *   // has both a static and dynamic parts.
 *   flow::cfg::Config_manager<Static_config_general, Null_value_set, Static_socket_opts, Dynamic_socket_opts> ...;
 *   ~~~
 *
 * @see Static_config_manager for the probably-common use case when you have only static config and only one `struct`
 *      at that.  Static_config_manager adapts Config_manager with a very simple API, avoiding parameter packs
 *      and any mention of various dynamic-config complexities.
 *
 * @todo Dynamic config support lacks crash rejection; though these features should be an incremental improvement
 * around the existing code.  By crash rejection I mean something like: config file
 * X comes in; we rename it to X.unparsed; we try to parse it; program crashes -- versus it's fine, so we rename it
 * X.parsed, and next time X.parsed is what we presumably-safely parse after any restart.  Similarly invalid config
 * may not cause a crash but still shouldn't be repeatedly re-parsed (etc.).  Exact design TBD and will be informed
 * by previous work on other projects (by echan and ygoldfel at least).
 *
 * ### When (and how) to use this ###
 * First see the namespace flow::cfg doc header for a brief overview.  If you have chosen to use flow::cfg for (at least
 * some of) your config needs, what is mandatory to use is the class template Option_set.  The choice is to use it
 * directly (writing your own facilities around it) or to use this Config_manager to maintain a couple of `Option_set`s
 * for you in a straightforward way.  Config_manager supports that one straightforward way, and for daemon programs
 * it may be a good choice.  Otherwise, the idea behind Option_set is to be able to flexibly use them as needed
 * (Config_manager providing one such way).
 *
 * Firstly, you may create a `const` (immutable) Config_manager via its constructor and then just use it to output
 * a help message (log_help() or help_to_ostream()).  This could be used with your program's `--help` option or similar,
 * and that's it (no parsing takes place).
 *
 *   ~~~
 *   // Example of a static-config-only Config_manager created just to log the help, and that's it.
 *   flow::cfg::Config_manager<Static_value_set, Null_value_set>
 *     (get_logger(), "cfg", &static_cfg_declare_opts, flow::cfg::null_declare_opts_func())
 *     .log_help();
 *   ~~~
 *
 * Orthogonally, of course, you may want to use it to parse things.  In which case:
 *
 * Config_manager assumes the following setup:
 *   - 1 or more config sets, each applicable perhaps to a different module of your application/library/etc.
 *     For *each* config set, have 1 or both of the following.  (Having neither is pointless; so do not.  Officially
 *     behavior is undefined if you do this.)
 *     - One set of values, such that it is read from config source(s) once; then no value therein ever changes again.
 *       Call this type `S_value_set` (S for static).
 *       `S_value_set` must be a type suitable for Option_set as its `Value_set` template arg.  See Option_set doc
 *       header.  Spoiler alert: It essentially needs to be a `struct` of reasonably-copyable,
 *       reasonably-equality-comparable, stream-parseable, stream-printable scalars (possibly nested); it needs to
 *       declare its default values in a no-arg ctor; and it needs an option-declaring function that calls
 *       FLOW_CFG_OPTION_SET_DECLARE_OPTION() for each data member therein.  The latter function must be passed to
 *       Config_manager ctor.
 *       - After ctor, static config stored herein will be at its default values.  Call apply_static() (or
 *         apply_static_and_dynamic(); see below) to read the static values (from a file, etc.).
 *       - This `S_value_set` is accessible by reference-to-immutable accessor static_values() or by
 *         all_static_values().
 *         - static_values() and all_static_values() are essentially thread-safe, at least after apply_static(), in that
 *           static_values() will always return a reference to the same `S_value_set` and all_static_values() will
 *           always emit the same pointers, and the values within the `S_value_set`(s) shall never change.
 *     - A 2nd set of values, such that it is read from config source(s) at least once; but then potentially
 *       more times, and each time 0 or more of the values may be changed from the original setting.
 *       `D_value_set`, again, must be a type suitable for Option_set as its `Value_set`.
 *       - Initialize the first set of dynamic values by calling either apply_static_and_dynamic() (if your setup allows
 *         for initial dynamic values to be supplied in the same config source(s) (e.g., same file) as static ones)
 *         *and* then apply_dynamic(); or *only* the latter.
 *       - After that initial `apply*_dynamic()`: dynamic_values() returns a ref-counted pointer to the heap-allocated
 *         `D_value_set` canonical at that time.  dynamic_values() may return a different pointer each time (and
 *         will, if a dynamic change is detected); but the values *at* a given return pointer will *never* change.
 *         all_dynamic_values() is an alternate approach but essentially the same idea.
 *       - Now, whenever you have reason to believe the dynamic config may have changed, call apply_dynamic().
 *         - apply_dynamic() (after the first one) and dynamic_values() are mutually thread-safe: they can be safely
 *           called concurrently with each other and/or themselves.  Internally, an atomic pointer to `D_value_set`
 *           is stored; `apply_dynamic()` makes a single update to it -- after a potentially lengthy successful parse
 *           of config source(s) -- while dynamic_values() makes an atomic read followed by a pointer copy (and
 *           returns the copy).  The ref-counted-ptr handle ensures the returned `D_value_set` survives as long
 *           as the user needs it, even if it's immediately replaced by new config within the Config_manager.
 *         - Since dynamic config can change at any time, it is up to the config user how they want to operate on the
 *           ref-counted handle obtained from dynamic_values().  If a potentially new value each time is fine, it's fine
 *           to simply do `cm.dynamic_values()->m_some_opt`.  If one needs a consistent set of 2 or more values, or
 *           if one needs values to not change over time, then one can simply save `auto dyn_cfg = cm.dynamic_values()`
 *           and then access values through `dyn_cfg->` as long as consistency is desired.  (Again: `*dyn_cfg` can never
 *           change, once `dyn_cfg` is returned through dynamic_values().)
 *       - You may also have hooks executed when dynamic config changes.  This is a simple system: before
 *         any relevant `apply*_dynamic()` calls, call register_dynamic_change_listener() for each module that
 *         wants to be informed.  Each callback so registered will be *synchronously* executed from `apply*_dynamic()`,
 *         when a relevant change is detected.
 *
 * To summarize: This is the assumed order of API calls on a given `*this`; other orders may lead to undefined behavior:
 *   - Config_manager ctor.
 *   - apply_static() or (if using dynamic config, but even so still optional) apply_static_and_dynamic().
 *   - If using dynamic config: apply_dynamic().
 *   - Simultaneously/at various times (thread-safe w/r/t each other):
 *     - static_values() (always returns a reference to the same immutable `S_value_set`).
 *     - all_static_values() (always emits the same pointers to immutable `S_value_set`s).
 *     - If using dynamic config: dynamic_values() (if returned `x`, `*x` is immutable; but may return different `x`es
 *       over time).  Alternatively: all_dynamic_values().
 *     - apply_dynamic() (changes ptr returned by dynamic_values() and all_dynamic_values()).
 *   - Config-using modules' destructors/deinitialization code.
 *     - For each such module: Obviously it must run after the last `*ic_values()` call.
 *     - For each such module: It *must* run before Config_manager dtor.
 *   - Config_manager dtor.
 *
 * In addition, between Config_manager ctor and dtor:
 *   - (Optional) If using dynamic config: register_dynamic_change_listener() (for each module interested in it);
 *     not thread-safe against concurrent apply_dynamic(), apply_static_and_dynamic(), or
 *     unregister_dynamic_change_listener().
 *     - Functions `register_dynamic_change_listener()`ed earlier may execute synchronously from within
 *       apply_dynamic() and/or apply_static_and_dynamic().
 *       - Such user functions may load async work elsewhere.
 *       - Ensure that thing onto which they load async work exists!  See register_dynamic_change_listener() for
 *         useful patterns to ensure this.
 *   - (Optional) If a callback has been registered: unregister_dynamic_change_listener(); not thread-safe against
 *     concurrent apply_dynamic(), apply_static_and_dynamic(), or unregister_dynamic_change_listener().
 *     - Any callback which accesses something which can be invalidated SHOULD be unregistered before that thing becomes
 *       invalid.  If there is a chance that apply_dynamic() or apply_static_and_dynamic() might be called after it
 *       has become invalid, then the callback MUST be unregistered (otherwise, undefined behavior could be encountered
 *       when the invalid thing is accessed).
 *
 * ### Advanced feature: multi-source parsing and source skipping ###
 * By default this feature is not in play, as the `bool commit` arg to apply_static_and_dynamic(), apply_static(),
 * and apply_dynamic() defaults to `true`.  The multi-source feature is enabled by the user setting it to `false`
 * for some calls to `apply_*()`.  To explain how it works consider one particular `Value_set` and an attempt
 * to execute one of `apply_*()` methods w/r/t that `Value_set` (among potentially others to which the same
 * discussion applies equally).  For purposes of discussion assume it is a dynamic `Value_set`, and the operation
 * in question is apply_dynamic(); the same points apply to static ones and the other two `apply_*()` calls
 * except where noted below.
 *
 * Suppose the canonical (as returned by dynamic_values() or all_dynamic_values()) `Value_set` is in state B,
 * the baseline state.  (If `Value_set` is static, then B is the default state from default-cted `Value_set`, and
 * the canonical-state accessors are static_values() and all_static_values().)
 * Normally a single *update* consists of just one call to `apply_dynamic(commit = true)`:
 *   -# `R = apply_dynamic(P, F, true)`, where `P` is a file path, and `F` is your Final_validator_func::Type function.
 *
 * If `F() == Final_validator_outcome::S_FAIL`, then `R == false`; state stays at B.
 * If it's `F() == S_ACCEPT` (and the individual options were all fine), then `R == true`; state becomes B';
 * and if B does not equal B' (a change was detected), then the dynamic changes listener(s)
 * (from register_dynamic_change_listener()) are synchronously called before apply_dynamic() returns.
 * (That listener stuff does not apply to static `Value_set`s in apply_static() and apply_static_and_dynamic().)
 *
 * That's the default behavior without engaging this feature.  To engage the feature, one has to
 * call `apply_dynamic(commit = false)` at least once.  A single *update* now consists of 2+ calls
 * to `apply_dynamic()`:
 *   -# `R = apply_dynamic(P1, F, false)`; if `!R`, update failed/exit algorithm; else:
 *   -# `R = apply_dynamic(P2, F, false)`; if `!R`, update failed/exit algorithm; else:
 *   -# ...
 *   -# `R = apply_dynamic(Pn, F, true)`; if `!R`, update failed/exit algorithm; else done.
 *       The `true` arg indicates the final call.
 *
 * (`F` can be different each time, though informally we suspect that would be unorthodox.  It is allowed formally.)
 *
 * Each apply_dynamic() call builds up a *candidate* `Value_set C` which is created (at the top of call 1)
 * to equal baseline/initial state B and then potentially incrementally morphed by each apply_dynamic() call.
 * Assuming no error (from individual validator or an `F() == S_FAIL`) at any stage, the effect on the *candidate*
 * `Value_set C` is as follows:
 *   -# If `F(Cnext) == S_ACCEPT`: Modify the *candidate* by overwriting it: `C = Cnext;`.
 *   -# If `F(Cnext) == S_SKIP`: Keep the *candidate* `Value_set C` unchanged from its current value at entry to that
 *      apply_dynamic() call.  `Cnext` is discarded (so the incremental update to the candidate is *skipped*).
 *
 * However, and this is the key:
 *   - `apply_dynamic(..., false)` does *not* modify the canonical (as returned by all_dynamic_values(), etc.)
 *     state.  It remains equal to B.  Hence no dynamic change listeners are invoked.  However:
 *   - the final call -- `apply_dynamic(..., true)` -- makes the canonical state equal the *candidate* `Value_set C`
 *     that has been built up.  Accordingly, before `apply_dynamic(..., true)` returns, if the canonical
 *     `Value_set B` differs from the candidate `Value_set C` that overwrites it, dynamic change listeners are
 *     invoked.
 *
 * (Hence the one-call update scenario (the vanilla one) is merely a degenerate case of the multi-call scenario;
 * as then the last call is the only call and thus canonicalizes the candidate and calls dynamic change listeners if
 * appropriate.)
 *
 * If at any stage `F(Cnext)` yields Final_validator_outcome::S_FAIL, then apply_dynamic() returns `false`.  This means
 * the entire update has failed; the *candidate* is abandoned, and you should not make the subsequent
 * apply_dynamic() calls that may be remaining in the update.  The next such call will be taken to be the 1st
 * of another update, with a fresh candidate C created to equal B.
 *
 * Why use this?  Why use 2+ files for a given update?  Indeed you shouldn't, unless there is specifically a good
 * reason.  What might that be?  The precipitating use case, and the basic one immediately obvious to the authors,
 * is where exactly for 1 of the files the final-validator shall return Final_validator_outcome::S_ACCEPT, while
 * for *all* others it will return Final_validator_outcome::S_SKIP.  For example, if your distributed network
 * consists of 3 sections, and each machine knows to which section it belongs, and each file is aimed at exactly 1
 * of the 3 and thus specifies it as some option `if-section-is=` (possible values 1 or 2 or 3), then the
 * final-validator func would:
 *   -# Check `if-section-is` option and return SKIP if the machine's actual section does not match it.  Else:
 *   -# Ensure internal consistency of candidate `Value_set` (as usual).
 *   -# Return FAIL or ACCEPT depending on the answer to that (as usual).
 *
 * Now you've enabled conditional config in a distributed deployment.
 *
 * @warning In this *conditional config* use case, it is likely best to declare any given conditional option
 *          using a `_NO_ACC` variation of your `FLOW_CFG_OPTION_SET_DECLARE_OPTION*()` option of choice;
 *          that is `FLOW_CFG_OPTION_SET_DECLARE_OPTION_NO_ACC()` or similarly-postfixed variation.
 *          You probably do not want such an option's value to persist from an earlier file in the update to a later
 *          one; instead it should probably reset to its default "at the top" of each file; `_NO_ACC` variants
 *          will accomplish this.
 *
 * That said the use of SKIP (for all but 1) is not mandatory in such a multi-file-update setup.  You can never-SKIP,
 * or you can ACCEPT (not-SKIP) 2+ files; it is conceivable those setups have use cases as well.  Informally though we
 * reason to you as follows:
 *   - If you never SKIP, then why complicate matters by having updates consist of 2+ files in the first place?
 *     Isn't it simpler (for config deployment and other aspects of the system) to just keep it to 1?
 *   - If you ACCEPT (don't-SKIP) 2+ files, then merely looking at a given file will no longer easily show the resulting
 *     final config, as each individual update is now incremental in nature; a preceding file in the same update
 *     may have changed (versus baseline) some setting not mentioned in that particular file.  This may prove to be
 *     confusing.
 *     - If you ACCEPT (don't-SKIP) exactly 1 file, then this no longer the case, and things are simple enough again.
 *     - If you ACCEPT (don't-SKIP) 2+ files, then to mitigate the problem where the final config is not quite clear
 *       just by looking at the input files, it's simple to use state_to_ostream() or log_state() to direct the
 *       result to a file or the logs, for human viewing.
 *
 * The above are informal recommendations for maximum simplicity or clarity, but it is entirely conceivable a more
 * complex situation arises and requires they not be heeded.
 *
 * ### Relationship with the rest of your program/threads ###
 * At this time Config_manager is deliberately simple in that it doesn't start any threads of its own; it doesn't watch
 * file modifications to detect dynamic changes; and its methods can be called from any thread and always act
 * entirely synchronously.  (The only methods affected by thread considerations are apply_dynamic() and
 * dynamic_values() + all_dynamic_values(); they are intentionally thread-safe w/r/t each other.)
 *
 * To the extent that it uses callbacks -- again, only for dynamic config needs and only optionally -- they, too, are
 * executed synchronously.  However, these callbacks can (of course) `.post()` work onto boost.asio util::Task_engine
 * or async::Concurrent_task_loop or async::Single_thread_task_loop.
 *
 * Informally:
 * It is, however, possible and perhaps probable that one should build more facilities to detect cfg changes of various
 * kinds; for this thread(s) may be useful.  We suspect it is better to build class(es) that do that and make use of
 * Config_manager -- not expand Config_manager to do such things.  Its simplicity and relative light weight and
 * flexibility are good properties to conserve and not saddle with baggage (even optional baggage).
 *
 * ### Thread safety ###
 * This is discussed in bits and pieces above and in various doc headers.  Here is the overall picture however:
 *   - Separate Config_manager objects are independent of each other, so there are no thread safety issues.
 *   - For a given `*this`:
 *     - By default, no non-`const` method is safe to call concurrently with any other method.  Except:
 *     - dynamic_values() / all_dynamic_values() may be called concurrently with itself/each other or any other method
 *       including `apply_dynamic(commit = *)`, as long as either `apply_dynamic(commit = true)` or
 *       `apply_static_and_dynamic(commit = true)` has succeeded at least once prior.
 *
 * @tparam S_d_value_set
 *         An even number (at least 2) of settings `struct`s -- see Option_set doc header for requirements for each --
 *         in order static, dynamic, static, dynamic, ....  Use Null_value_set as needed; but do not use Null_value_set
 *         for both the static and dynamic parameter in a given pair (e.g., `<Null_value_set, Null_value_set, ...>`
 *         is both useless and formally disallowed).
 *
 * @internal
 * ### Implementation notes ###
 * I (ygoldfel) wrote this, originally, in 2 stages.  At first it only supported exactly 2 template args: one static
 * and one dynamic `Value_set`.  In the next stage I turned it into a parameter pack, so that 4, 6, 8, ... args
 * can be provided if desired -- to support multiple configurable modules in the application.  Before this I had done
 * some basic parameter-pack coding, but undoubtedly Config_manager is my first foray into this level of
 * sophistication.  Certainly I welcome changes to improve clarity of the implementation and other suggestions.
 * Generally speaking the code does strike me as complex, though there's a good case to be made to the effect of:
 * That is just the nature of the beast.  It's meta-programming: having the compiler perform loops, of a sort,
 * at *compile time*.  Because `S_d_value_set...` is not homogenous -- within each adjacent pair of tparams the first
 * and the second are *not* always treated equivalently -- the code has to be tricky at times.
 *
 * That said the usability of the API should not be affected by these details much.  At times one has to supply
 * additional `<template args>` -- apply_static() and apply_dynamic() come to mind in particular -- to help the
 * compiler; but the compiler will complain about being unable to deduce the tparams, and then the next action for the
 * user is straightforward.
 *
 * ### A particular trick explained ###
 * In C++17 fold expressions are available (https://en.cppreference.com/w/cpp/language/fold), and we use them
 * to create compile-time loops without the appalling coding/maintenance overhead of the meta-recursive pattern.
 * (This replaces a much uglier trick involving a dummy `int[]` initializer, though that was still superior to
 * doing the meta-recursive thing all over the place.)
 *
 * Suppose, say, you want to call `f<Value_set>(idx, dyn_else_st)`,
 * where `idx` runs through [0, #S_N_VALUE_SETS), while `dyn_else_st` alternates between `false` (static) and
 * `true` (dynamic).  E.g., `f()` might parse the `idx`th slot of #m_s_d_opt_sets and do something extra if it's
 * an odd (dynamic) slot.  Note `f()` is a template parameterized on `Value_set`.  Then:
 *
 *   ~~~
 *   size_t idx = 0;
 *   bool dyn_else_st = false;
 *   (
 *     ..., // This indicates the entire (..., expr) is a fold-expression, where...
 *     (
 *       // ...the `S_d_value_set` in the following expression is the kernel of the param-pack expansion;
 *       // due to the fact the class template is: template<typename... S_d_value_set> class Config_manager
 *       f<S_d_value_set>(idx, dyn_else_st),
 *       ++idx,
 *       dyn_else_st = !dyn_else_st)
 *     )
 *   );
 *   ~~~
 *
 * `(..., expr)` expands to, ultimately, `(expr1, expr2, ..., expr3)`, where each `expr...i...` is `expr` with
 * the i-th `S_d_value_set`.  In our case `expr...i...` is itself a comma-expression with 3 sub-expressions.
 * Comma-expression evaluates left-to-right, so at runtime the side effects of each
 * `expr...i...` are occurring at runtime; in this case this includes incrementing `idx` and repeatedly flipping
 * `dyn_else_st`.
 *
 * In the actual code, often we don't (just) call some helper `f<>()` but rather actually perform some steps in-line
 * there instead -- similarly to how this example does a couple of simple expressions after calling `f<>()`.
 * The good thing is the reader need not jump to another function (and all that maintenance overhead).  The bad thing
 * is the language constructions within an expression in C++ (not a functional language) are limited.  Loops aren't
 * possible; `if ()`s have to be replaced by `?:` ternaries or `&&` or `||` expressions; and so on.
 * It's not always the most readable thing, if one isn't used to it, but the author feels it's doable to get used to it,
 * and it's worth it for the brevity.
 */
template<typename... S_d_value_set>
class Config_manager :
  public log::Log_context
{
public:
  // Types.

  /**
   * Tag type: indicates an `apply_*()` method must *allow* invalid defaults and only complain if the config source
   * does not explicitly supply a valid value.  Otherwise the defaults themselves are also stringently checked
   * regardless of whether they are overridden.  This setting applies only to individual-option-validators.
   * Final_validator_func validation is orthogonal to this.
   */
  enum allow_invalid_defaults_tag_t
  {
    S_ALLOW_INVALID_DEFAULTS ///< Sole value for tag type #allow_invalid_defaults_tag_t.
  };

  /// Short-hand for a callback to execute on dynamic config change.
  using On_dynamic_change_func = Function<void ()>;

  struct On_dynamic_change_func_handle;

  // Constants.

  /// The number of template params in this Config_manager instantiation.  It must be even and positive.
  static constexpr size_t S_N_VALUE_SETS = sizeof...(S_d_value_set);

  static_assert((S_N_VALUE_SETS % 2) == 0,
                "Must have even number of template args to Config_manager; use a trailing Null_value_set if needed.");
  /// The number of static value sets (including any `Null_value_set`s).
  static constexpr size_t S_N_S_VALUE_SETS = sizeof...(S_d_value_set) / 2;
  /// The number of dynamic value sets (including any `Null_value_set`s).
  static constexpr size_t S_N_D_VALUE_SETS = sizeof...(S_d_value_set) / 2;

  // Constructors/destructor.

  /**
   * Constructs a Config_manager ready to read initial config via `apply_*()` and other setup methods; and further
   * capable of both static and dynamic config.  See class doc header for class life cycle instructions.
   *
   * ### Logging assumption ###
   * `*logger_ptr` is a standard logging arg.  Note, though, that the class will assume that log verbosity may not have
   * yet been configured -- since this Config_manager may be the thing configuring it.  Informal recommendations:
   *   - You should let through INFO and WARNING messages in `*logger_ptr`.
   *   - If you plan to use `*this` only for log_help() (such as in your `--help` implementation), you should
   *     *not* let through TRACE-or-more-verbose.
   *   - Once (and if) you engage any actual parsing (apply_static(), etc.), TRACE may be helpful
   *     in debugging as usual.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param nickname
   *        Brief string used for logging subsequently.
   * @param declare_opts_func_moved
   *        For each `S_d_value_set`, in order, the declare-options callback as required by
   *        `Option_set<S_d_value_set>` constructor; see its doc header for instructions.
   *        For each Null_value_set: use the function returned by `null_declare_opts_func()`.
   */
  explicit Config_manager(log::Logger* logger_ptr, util::String_view nickname,
                          typename Option_set<S_d_value_set>::Declare_options_func&&... declare_opts_func_moved);

  // Methods.

  /**
   * Invoke this after construction to load the permanent set of static config from config sources including
   * a static config file; if you are also using dynamic config, see apply_static_and_dynamic() as a *potential*
   * alternative.  See also apply_static() overload with #allow_invalid_defaults_tag_t tag.
   *
   * After this runs and succeeds, assuming `commit == true`, you may use static_values() and/or
   * all_static_values() to access the loaded values.  After this runs and succeeds, but `commit == false`,
   * you should call apply_static() again 1+ times, with the last such call having `commit == true` and thus
   * canonicalizing each progressively-built candidate `Value_set`, so that it becomes available via static_values()
   * and all_static_values().
   *
   * On failure returns `false`; else returns `true`.  In the former case the canonical state remains unchanged,
   * and any candidate built-up via preceding `commit == false` calls (if any) is discarded.  The next
   * `apply_*()` call begins a fresh update.
   *
   * Tip: On failure you may want to exit program with error; or you can continue knowing that
   * static_values() will return a reference to default values (and all_static_values() will emit pointers to
   * `Value_set`s with default values) according to `Value_set()` no-arg ctor (for each `Value_set`).
   * WARNING(s) logged given failure.
   *
   * apply_static() will *not* be tolerant of unknown option names appearing in the config source.  The reasoning
   * (and we might change our minds over time): It should be possible to deliver the proper set of static config
   * along with the binary that supports it.  However apply_static_and_dynamic() is tolerant of unknown option names.
   *
   * You must supply exactly #S_N_S_VALUE_SETS `final_validator_func`s.  The compiler will likely require you to
   * explicitly specify the `struct` types as explicit template args; e.g:
   *
   *   ~~~
   *   Config_manager<S_config1, D_config1, S_config2, D_config2, Null_value_set, D_config3> cfg_mgr(...);
   *   // Explicitly specify the static config struct types.  Provide that number of final-validate functions.
   *   // Use null_final_validator_func() for the dummy Null_value_set in particular.
   *   cfg_mgr.apply_static<S_config1, S_config2, Null_value_set>
   *     (file_path, &s_decl_opts1, &s_decl_opts2, null_final_validator_func());
   *   ~~~
   *
   * For context w/r/t `commit` arg: Please read the section of the class doc header regarding multi-source
   * updates.  Corner case note: Formally: invoking `apply_Y()` immediately after `apply_X(commit = false) == true`,
   * without reject_candidates() between them, where Y and X differ => `apply_Y()` shall log an INFO message
   * and invoke reject_candidates() itself; then proceed.  In this case `apply_Y()` is apply_static().
   * Informally we discourage doing this; it is stylistically better to invoke reject_candidates()
   * explicitly in that scenario which cancels an in-progress update which is unusual though conceivably useful.
   *
   * @note Each `final_validator_func()` can be made quite brief by using convenience macro
   *       FLOW_CFG_OPT_CHECK_ASSERT().  This will take care of most logging in most cases.
   * @note For each Null_value_set: use `final_validator_func = null_final_validator_func()`.
   * @note A validator function will not be run if there is a failure to parse any preceding `Value_set` (including a
   *       failure of its associated validator function).
   * @note A validator function may rely on all_static_values_candidates() and static_values_candidate() providing the
   *       parsed (and validated) values candidate for any preceding static value set.
   *
   * @todo Add support for command line as a config source in addition to file(s), for static config in
   * cfg::Config_manager and cfg::Static_config_manager.
   *
   * @tparam Value_set
   *         These must be `S_d_value_set` template param pack args 0, 2, ..., numbering
   *         #S_N_S_VALUE_SETS in order.
   * @param static_cfg_path
   *        File to read.
   * @param final_validator_func
   *        For each arg:
   *        If parsing and individual-option-validation succeed, the method shall return success if
   *        `final_validator_func(V)` returns Final_validator_outcome::S_ACCEPT or Final_validator_outcome::S_SKIP,
   *        where V is the parsed `Value_set`.  (If `apply_static(commit = false) == true` call(s) preceded this
   *        one, V is the cumulative candidate from such call(s) so far, plus values parsed from
   *        `static_cfg_path` on top of that.)
   *        Informally: Please place individual-option validation
   *        into FLOW_CFG_OPTION_SET_DECLARE_OPTION() invocations; only use `final_validator_func()` for
   *        internal consistency checks (if any) and skip conditions (if any).
   * @param commit
   *        `true` means that this call being successful (returning `true`) shall cause the promotion of
   *        each candidate `Value_set` built-up so far (via this call and all preceding successful calls with
   *        `commit == false`) to canonical state (accessed via static_values() or all_static_values()).
   *        `false` means that this call being successful shall merely create-and-set (if first such call)
   *        or incrementally update (if 2nd, 3rd, ... such call) each candidate `Value_set`.
   * @return `true` if and only if successfully parsed config source and validated all settings including
   *         `final_validator_func() != S_FAIL` for *all* (static) config sets; and defaults were also all individually
   *         valid.  However, if `true` but `commit == false`, then the canonical values
   *         (accessed via static_values() or all_static_values()) have not been updated.
   *         If `false` with `commit == false`, you should not call apply_static() for the planned
   *         subsequent config sources: this update has failed, and any built-up candidate `Value_set`s are
   *         discarded.
   */
  template<typename... Value_set>
  bool apply_static(const fs::path& static_cfg_path,
                    const typename Final_validator_func<Value_set>::Type&... final_validator_func,
                    bool commit = true);

  /**
   * Identical to apply_static() overload without #allow_invalid_defaults_tag_t tag; but skips the stringent
   * check on individual defaults' validity.
   *
   * @see #allow_invalid_defaults_tag_t doc header and/or return-value doc just below.
   *
   * @tparam Value_set
   *         See other apply_static().
   * @param static_cfg_path
   *        See other apply_static().
   * @param final_validator_func
   *        See other apply_static().
   * @param commit
   *        See other apply_static().
   * @return See other apply_static().  The difference between the other `apply_static()` and this overload is:
   *         The other overload will return `false` given an invalid default, even if
   *         file `static_cfg_path` explicitly sets it to a valid value.  This tagged overload will not fail (return
   *         `false` for that reason) and will let parsing continue instead.
   */
  template<typename... Value_set>
  bool apply_static(allow_invalid_defaults_tag_t,
                    const fs::path& static_cfg_path,
                    const typename Final_validator_func<Value_set>::Type&... final_validator_func,
                    bool commit = true);

  /**
   * If you use dynamic config, *and* you allow for initial values for dynamic options to be read from the same file
   * as the static config values, then invoke this instead of apply_static().  See also apply_static_and_dynamic()
   * overload with #allow_invalid_defaults_tag_t tag.
   *
   * With static config-only use case:
   *   - Just apply_static().
   *   - Then static_values() / all_static_values() whenever needed.
   *
   * With dynamic-and-static config use case, *with* allowing baseline dynamic values to be set in static config
   * file:
   *   - apply_static_and_dynamic(). <-- ATTN
   *   - Initial apply_dynamic().  (Typically it is allowed that there is no dynamic config file present yet though.)
   *   - Then static_values() / all_static_values(), dynamic_values() / all_dynamic_values() whenever needed.
   *     - Further apply_dynamic(), if dynamic config source(s) may have changed.
   *
   * With dynamic-and-static config use case, *without* allowing baseline dynamic values to be set in static config
   * file:
   *   - apply_static().
   *   - Initial apply_dynamic().
   *   - Then static_values() / all_static_values(), dynamic_values() / all_dynamic_values() whenever needed.
   *     - Further apply_dynamic(), if dynamic config source(s) may have changed.
   *
   * On failure returns `false`; else returns `true`.  In the former case the canonical state remains unchanged,
   * and any candidate built-up via preceding `commit == false` calls (if any) is discarded.  WARNING(s) logged
   * given failure.
   *
   * For context w/r/t `commit` arg: Please read the section of the class doc header regarding multi-source
   * updates.  Corner case note: Formally: invoking `apply_Y()` immediately after `apply_X(commit = false) == true`,
   * without reject_candidates() between them, where Y and X differ => `apply_Y()` shall log an INFO message
   * and invoke reject_candidates() itself; then proceed.  In this case `apply_Y()` is apply_static_and_dynamic().
   * Informally we discourage doing this; it is stylistically better to invoke reject_candidates()
   * explicitly in that scenario which cancels an in-progress update which is unusual though conceivably useful.
   *
   * @note By definition this will not compile unless `final_validator_func` count equals #S_N_VALUE_SETS.
   *       However, unlike apply_static() or apply_dynamic(), there are no template args to explicitly supply.
   *
   * @param cfg_path
   *        File to read for both static and dynamic config.
   * @param final_validator_func
   *        See apply_static(); particularly the notes about how a validator function will not be run if there is a
   *        preceding failure and that all_static_values_candidates() and static_values_candidate() can be relied upon.
   * @param commit
   *        `true` means that this call being successful (returning `true`) shall cause the promotion of
   *        each candidate `Value_set` built-up so far (via this and all preceding successful calls with
   *        `commit == false`) to canonical state (accessed via `*ic_values()` or `all_*ic_values()`);
   *        and all dynamic change listeners are synchronously called.
   *        `false` means that this call being successful shall merely create-and-set (if first such call)
   *        or incrementally update (if 2nd, 3rd, ... such call) each candidate `Value_set`; no dynamic change
   *        listeners are invoked.
   * @return `true` if and only if successfully parsed config source(s) and validated all settings including
   *         `final_validator_func() != S_FAIL` for *all* (static and dynamic) config sets; and defaults were also all
   *         individually valid.  However, if `true` but `commit == false`, then the canonical values
   *         (accessed via `*ic_values()` or `*ic_values()`) have not been updated.
   *         If `false` with `commit == false`, you should not call apply_static_and_dynamic() for the planned
   *         subsequent config sources: this update has failed, and any built-up candidate `Value_set`s are
   *         discarded.
   */
  bool apply_static_and_dynamic(const fs::path& cfg_path,
                                const typename Final_validator_func<S_d_value_set>::Type&... final_validator_func,
                                bool commit = true);

  /**
   * Identical to apply_static_and_dynamic() overload without #allow_invalid_defaults_tag_t tag; but skips the stringent
   * check on individual defaults' validity.
   *
   * @see #allow_invalid_defaults_tag_t doc header and/or return-value doc just below.
   *
   * @param cfg_path
   *        See other apply_static_and_dynamic().
   * @param final_validator_func
   *        See other apply_static_and_dynamic().
   * @param commit
   *        See other apply_static_and_dynamic().
   * @return See other apply_static_and_dynamic().  The difference between the other `apply_static_and_dynamic()` and
   *         this overload is:
   *         The other overload will return `false` given an invalid default, even if
   *         file `cfg_path` explicitly sets it to a valid value.  This tagged overload will not fail (return
   *         `false` for that reason) and will let parsing continue instead.
   */
  bool apply_static_and_dynamic(allow_invalid_defaults_tag_t,
                                const fs::path& cfg_path,
                                const typename Final_validator_func<S_d_value_set>::Type&... final_validator_func,
                                bool commit = true);

  /**
   * Load the first or subsequent set of dynamic config from config source including a dynamic config file.
   * If you use dynamic config: You must invoke this, or apply_static_and_dynamic(), once before access via
   * dynamic_values() / all_dynamic_values().  After that, optionally invoke it whenever there is good reason
   * to believe new dynamic config is available in the dynamic config source(s).
   *
   * See also apply_static_and_dynamic().  If not using apply_static_and_dynamic(), *and* this is the initial
   * apply_dynamic() invoked, then see also apply_dynamic() overload with #allow_invalid_defaults_tag_t tag.
   *
   * After the initial apply_dynamic(): It is safe to call apply_dynamic() concurrently with any number of
   * `[all_]dynamic_values()`.  However behavior is undefined if one calls apply_dynamic() concurrently with itself
   * or apply_static_and_dynamic() (on the same `*this`).
   *
   * On failure returns `false`; else returns `true`.  In the former case the canonical state remains unchanged,
   * and any candidate built-up via preceding `commit == false` calls (if any) is discarded.  WARNING(s) logged
   * given failure.
   *
   * apply_dynamic() will be tolerant of unknown option names appearing in the config source, though it will
   * log about them.  The reasoning: Dynamic config must grapple with backward- and forward-compatibility.
   *
   * For context w/r/t `commit` arg: Please read the section of the class doc header regarding multi-source
   * updates.  Corner case note: Formally: invoking `apply_Y()` immediately after `apply_X(commit = false) == true`,
   * without reject_candidates() between them, where Y and X differ => `apply_Y()` shall log an INFO message
   * and invoke reject_candidates() itself; then proceed.  In this case `apply_Y()` is apply_dynamic().
   * Informally we discourage doing this; it is stylistically better to invoke reject_candidates()
   * explicitly in that scenario which cancels an in-progress update which is unusual though conceivably useful.
   *
   * ### Performance ###
   * dynamic_values() + all_dynamic_values() locking performance is no worse than: lock mutex,
   * assign #S_N_D_VALUE_SETS `shared_ptr`s, unlock mutex.  The rest of the parse/validate/etc. code is outside any such
   * critical section.  See also Performance section of dynamic_values() doc header.
   *
   * Outside that locking critical section, apply_dynamic() may be expensive, in that it performs file input and some
   * internal copying of option value sets and maps, plus detailed logging.  It is, however, not typically called
   * frequently.  Just be aware it will block the calling thread, albeit still only for a split second in normal
   * conditions.
   *
   * @tparam Value_set
   *         These must be `S_d_value_set` template param pack args 1, 3, ..., numbering
   *         #S_N_D_VALUE_SETS in order.
   * @param dynamic_cfg_path
   *        File to read.
   * @param final_validator_func
   *        See apply_static_and_dynamic().
   * @param commit
   *        `true` means that this call being successful (returning `true`) shall cause the promotion of
   *        each candidate `Value_set` built-up so far (via this and all preceding successful calls with
   *        `commit == false`) to canonical state (accessed via dynamic_values() or all_dynamic_values());
   *        and all relevant dynamic change listeners are synchronously called for each `Value_set` for which
   *        the cumulative candidate differs from the canonical state.
   *        `false` means that this call being successful shall merely create-and-set (if first such call)
   *        or incrementally update (if 2nd, 3rd, ... such call) each candidate `Value_set`; no dynamic change
   *        listeners are invoked.
   * @return `true` if and only if successfully parsed config source(s) and validated all settings including
   *         `final_validator_func() != S_FAIL` for *all* (dynamic) config sets; and defaults were also all
   *         individually valid, in the case of initial apply_dynamic().  However, if `true` but `commit == false`,
   *         then the canonical values (accessed via dynamic_values() or all_dynamic_values()) have not been updated.
   */
  template<typename... Value_set>
  bool apply_dynamic(const fs::path& dynamic_cfg_path,
                     const typename Final_validator_func<Value_set>::Type&... final_validator_func,
                     bool commit = true);

  /**
   * Identical to apply_dynamic() overload without #allow_invalid_defaults_tag_t tag; except that -- applicably only to
   * the initial apply_dynamic() without a preceding apply_static_and_dynamic() -- skips the stringent
   * check on individual defaults' validity.
   *
   * @see #allow_invalid_defaults_tag_t doc header and/or return-value doc just below.
   *
   * @tparam Value_set
   *         See other apply_dynamic().
   * @param dynamic_cfg_path
   *        See other apply_dynamic().
   * @param final_validator_func
   *        See other apply_dynamic().
   * @param commit
   *        See other apply_dynamic().
   * @return See other apply_dynamic().  However -- assuming apply_static_and_dynamic() was not used, and
   *         this is the initial apply_dynamic() -- the other apply_dynamic() will return `false` if a default is
   *         invalid, even if file `dynamic_cfg_path` explicitly sets it to a valid value.  This tagged overload will
   *         not and let parsing continue.  If `apply_static_and_dynamic(commit = true) == true` was used, or
   *         if `apply_dynamic(commit = true) == true` has been called before, then the overloads behave
   *         identically: defaults are not checked; it must occur just before the initial dynamic load or never.
   */
  template<typename... Value_set>
  bool apply_dynamic(allow_invalid_defaults_tag_t,
                     const fs::path& dynamic_cfg_path,
                     const typename Final_validator_func<Value_set>::Type&... final_validator_func,
                     bool commit = true);

  /**
   * Cancel a not-yet-canonicalized (incomplete) multi-source update, if one is in progress.  If one is not
   * in-progress, INFO-log but otherwise no-op.
   *
   * This method only has effect following apply_static(), apply_static_and_dynamic(), or apply_dynamic() that
   *   - returned `true`; and
   *   - had arg `commit == false`.
   *
   * That is, if there are pending *candidate* `Value_set`s that have not yet been upgraded to canonical status
   * via a `commit == true` `apply_*()` call, this call discards them, making it as-if no such call(s) were
   * made in the first place.
   *
   * Informally we do not expect this to be commonly used; typically `apply_*()` will automatically do this
   * upon encountering an error (in individual option validation or due to Final_validator_outcome::S_FAIL).
   * However if there is some outside reason to abort an ongoing, so-far-successful multi-source update
   * this method will similarly do it.
   */
  void reject_candidates();

  /**
   * Emit a pointer to each permanently set static config value set; the same pointers are emitted throughout
   * for each of the #S_N_S_VALUE_SETS static config slots.  Tip: It should be sufficient to pass around only `const`
   * refs (from the pointers obtained here) all around the app -- no `Value_set` copying should be needed.
   *
   * @tparam Value_set
   *         These must be `S_d_value_set` template param pack args 0, 2, ..., numbering
   *         #S_N_S_VALUE_SETS in order.  The compiler should be able to deduce them automatically from
   *         each `value_set_or_null` type; though when passing in null it may be then necessary to cast `nullptr`
   *         (or `0`) to the appropriate pointer type.
   * @param value_set_or_null
   *        For each element in the param pack:
   *        `*value_set_or_null` is set to point to the immutable static config set in that slot;
   *        or that slot is ignored if `value_set_or_null` is null.
   */
  template<typename... Value_set>
  void all_static_values(const Value_set**... value_set_or_null) const;

  /**
   * Similar to all_static_values(), but if called from within a validator function passed to apply_static() or
   * apply_static_and_dynamic(), then for any static value set for which values have been parsed and validated (but not
   * yet applied) so far, a pointer to the parsed values *candidate* will be emitted instead.  If called from elsewhere,
   * the behavior is equivalent to all_static_values().  A values candidate consists of what has been parsed and
   * validated (and not skipped via Final_validator_outcome::S_SKIP) for the value set, but has not yet been applied
   * such that it would be available through all_static_values().
   *
   * This could be useful to provide static values to a final validator function for another value set which depends on
   * them.  During the execution of apply_static() or apply_static_and_dynamic(), the validator function for each value
   * set will be executed before any are canonicalized.  This method can be used by a validator function to obtain the
   * candidate values which have been parsed and validated (but have not yet been canonicalized) for a preceding static
   * value set.
   *
   * @see all_static_values().
   *
   * @tparam Value_set
   *         See all_static_values().
   * @param value_set_or_null
   *        See all_static_values().
   */
  template<typename... Value_set>
  void all_static_values_candidates(const Value_set**... value_set_or_null) const;

  /**
   * Similar to all_static_values(), but obtains the static config in *one* specified slot as opposed to all of them.
   *
   * @tparam Value_set
   *         This must be `S_d_value_set` template param pack args in position `2 * s_value_set_idx`
   *         (so one of [0, 2, ..., `(2 * S_N_S_VALUE_SETS) - 2`]).
   *         You will need to explicitly specify this `tparam`.  E.g.:
   *         `const auto cfg_ptr = cfg_mgr.static_values<S_cfg2>(1)`.
   * @param s_value_set_idx
   *        The static config slot index; so one of [0, 1, ..., #S_N_S_VALUE_SETS).
   * @return The immutable, permanently set static config value set in the `s_value_set_idx` slot.
   */
  template<typename Value_set>
  const Value_set& static_values(size_t s_value_set_idx) const;

  /**
   * Similar to static_values(), but if called from within a validator function passed to apply_static() or
   * apply_static_and_dynamic(), then the parsed values *candidate* will be returned, instead, if values have been
   * parsed and validated for the static value set but have not yet been applied.  Otherwise, the behavior is equivalent
   * to static_values().
   *
   * @see all_static_values_candidates().
   *
   * @tparam Value_set
   *         See static_values().
   * @param s_value_set_idx
   *        See static_values().
   * @return See above and static_values().
   */
  template<typename Value_set>
  const Value_set& static_values_candidate(size_t s_value_set_idx) const;

  /**
   * Obtain ref-counted pointers to each currently-canonical set of dynamic config; each pointed-at `struct` is set
   * permanently; while another call may return a different pointer if config is changed dynamically in the meantime
   * (for that slot).  For each slot: If you require a consistent set of dynamic config over some period of time or over
   * some code path, save a copy of the returned ref-counted pointer to the `Value_set` and keep accessing values in
   * that immutable structure through that ref-counted pointer.
   *
   * After the initial apply_dynamic(): It is safe to call all_dynamic_values() concurrently with any number of itself
   * or of dynamic_values() and/or a call to `apply_dynamic(commit = *)`.
   *
   * @note Have each `Value_set` derive from util::Shared_ptr_alias_holder, so that it is endowed with
   *       `Ptr` and `Const_ptr` ref-counted pointer type aliases, for your convenience and as required by
   *       Config_manager and Option_set.
   *
   * ### Performance ###
   * dynamic_values() performance is no worse than: lock mutex,
   * copy #S_N_D_VALUE_SETS `shared_ptr`s, unlock mutex.  See also Performance section of
   * apply_dynamic() doc header.
   *
   * @tparam Value_set
   *         These must be `S_d_value_set` template param pack args 1, 3, ..., numbering
   *         #S_N_D_VALUE_SETS in order.  Likely you will need to explicitly specify them, as the compiler will
   *         probably not deduce them.  E.g.:
   *         `cfg_mgr.all_dynamic_values<D_cfg1, D_cfg2, Null_value_set>(&d1, &d2, 0)`.
   * @param value_set_or_null
   *        For each element in the param pack:
   *        `*value_set_or_null` is set to point to the immutable dynamic config set currently canonical in that slot;
   *        or that slot is ignored if `value_set_or_null` is null.
   *        Remember: `**value_set_or_null` is immutable; but another call may yield a different
   *        `*value_set_or_null`.
   */
  template<typename... Value_set>
  void all_dynamic_values(typename Value_set::Const_ptr*... value_set_or_null) const;

  /**
   * Similar to all_dynamic_values() but obtains the dynamic config in *one* specified slot as opposed to all of them.
   *
   * @tparam Value_set
   *         This must be `S_d_value_set` template param pack args in position `1 + 2 * d_value_set_idx`
   *         (so one of [1, 3, ..., `(2 * S_N_D_VALUE_SETS) - 1`]).
   *         You will need to explicitly specify this `tparam`.  E.g.:
   *         `const auto cfg_ptr = cfg_mgr.dynamic_values<D_cfg2>(1)`.
   * @param d_value_set_idx
   *        The dynamic config slot index; so one of [0, 1, ..., #S_N_D_VALUE_SETS).
   * @return Ref-counted pointer to the immutable dynamic config set currently canonical in that slot.
   *         Remember: if `p` returned, then `*p` is immutable; but another dynamic_values() call may return
   *         a different `p2` not equal to `p`.
   */
  template<typename Value_set>
  typename Value_set::Const_ptr dynamic_values(size_t d_value_set_idx) const;

  /**
   * Saves the given callback; next time `apply_dynamic(commit = true)` or
   * `apply_static_and_dynamic(commit = true)` detects at least one changed (or initially set) option value
   * in the specified slot, it will execute this and any other previously registered such callbacks
   * synchronously.  The callbacks will be called after the pointers to be returned by all_dynamic_values() have all
   * been updated.
   *
   * This is not thread-safe against concurrent calls to itself; nor against concurrent
   * `apply_dynamic(commit = true)` (against `apply_dynamic(commit = false)` is okay),
   * `apply_static_and_dynamic(commit = true)` (against `apply_static_and_dynamic(commit = false)` is okay), or
   * unregister_dynamic_change_listener().
   *
   * @note The callback may optionally be unregistered by using unregister_dynamic_change_listener().  This *should* be
   *       done before anything that the callback accesses is invalidated.  If there is a chance that `apply*_dynamic()`
   *       might be called later, then the callback *must* be unregistered before anything that it accesses is
   *       invalidated, otherwise there could be undefined behavior when the callback accesses something which is
   *       invalid.
   *
   * @tparam Value_set
   *         See dynamic_values().
   * @param d_value_set_idx
   *        See dynamic_values().
   * @param on_dynamic_change_func_moved
   *        Function to call synchronously from the next `apply*_dynamic()` that detects a change.
   * @return A handle which can be used to unregister the callback.
   */
  template<typename Value_set>
  On_dynamic_change_func_handle register_dynamic_change_listener(size_t d_value_set_idx,
                                                                 On_dynamic_change_func&& on_dynamic_change_func_moved);

  /**
   * Remove a previously registered dynamic change callback.  See register_dynamic_change_listener().
   *
   * This is not thread-safe against concurrent calls to itself; nor against concurrent
   * `apply_dynamic(commit = true)` (against `apply_dynamic(commit = false)` is okay),
   * `apply_static_and_dynamic(commit = true)` (against `apply_static_and_dynamic(commit = false)` is okay), or
   * register_dynamic_change_listener().
   *
   * @param handle
   *        The handle which was returned by register_dynamic_change_listener() when the callback was registered.
   */
  void unregister_dynamic_change_listener (const On_dynamic_change_func_handle& handle);

  /**
   * Prints a human-targeted long-form summary of our contents, doubling as a usage message and a dump of current
   * values where applicable.  This is not thread-safe against several non-`const` methods.
   *
   * @param os
   *        Stream to which to serialize.
   */
  void state_to_ostream(std::ostream& os) const;

  /**
   * Logs what state_to_ostream() would print.  This is not thread-safe against several non-`const` methods.
   *
   * @param sev
   *        Severity to use for the log message(s).
   */
  void log_state(log::Sev sev = log::Sev::S_INFO) const;

  /**
   * Prints a human-targeted long-form usage message that includes all options with their descriptions and defaults.
   * This is thread-safe against all concurrent methods on `*this` and can be invoked anytime after ctor.
   *
   * @param os
   *        Stream to which to serialize.
   */
  void help_to_ostream(std::ostream& os) const;

  /**
   * Logs what help_to_ostream() would print.
   *
   * @param sev
   *        Severity to use for the log message(s).
   */
  void log_help(log::Sev sev = log::Sev::S_INFO) const;

  // Data.

  /// See `nickname` ctor arg.
  const std::string m_nickname;

private:
  // Types.

  /// Short-hand for `shared_ptr`-to-`void` type used to store variable-type values in internal containers.
  using Void_ptr = boost::shared_ptr<void>;

  /**
   * Useful at least in the context of multi-source (`commit == false`) `apply_*()` methods, this distinguishes
   * distinct possible public `apply_*()` methods.
   */
  enum class Update_type
  {
    /// Indicates no `apply_*()` operation.
    S_NONE,
    /// Indicates apply_static().
    S_STATIC,
    /// Indicates apply_static_and_dynamic().
    S_STATIC_AND_DYNAMIC,
    /// Indicates apply_dynamic().
    S_DYNAMIC
  }; // enum class Update_type

  // Methods.

  /**
   * Helper that obtains the Option_set in the slot `m_s_d_opt_sets[value_set_idx]`, which
   * stores an Option_set<Value_set>.
   *
   * @tparam Value_set
   *         `S_d_value_set` in slot `value_set_idx`.
   * @param value_set_idx
   *        One of [0, 1, ..., #S_N_VALUE_SETS).
   * @return Pointer to the Option_set.
   */
  template<typename Value_set>
  Option_set<Value_set>* opt_set(size_t value_set_idx);

  /**
   * Helper that obtains the baseline dynamic `Value_set` in the slot `m_d_baseline_value_sets[value_set_idx]`, which
   * stores a `Value_set`.
   *
   * @tparam Value_set
   *         `S_d_value_set` in dynamic slot `d_value_set_idx`.
   * @param d_value_set_idx
   *        One of [0, 1, ..., #S_N_D_VALUE_SETS).
   * @return Pointer to the `Value_set`.
   */
  template<typename Value_set>
  Value_set* d_baseline_value_set(size_t d_value_set_idx);

  /**
   * `const` overload of the other opt_set() helper.
   * @tparam Value_set
   *         See other opt_set().
   * @param value_set_idx
   *        See other opt_set().
   * @return See other opt_set().
   */
  template<typename Value_set>
  const Option_set<Value_set>* opt_set(size_t value_set_idx) const;

  /**
   * Helper that executes `opt_set->canonicalize_candidate()` or
   * `opt_set->reject_candidate()`.  Used in meta-programming.  Apply when parsing `*opt_set`,
   * assuming `commit == true`.
   *
   * @tparam Value_set
   *         See opt_set().
   * @param opt_set
   *        An opt_set() result.
   * @param canonicalize_else_reject
   *        Specifies whether to... well, you know.
   * @param changed_or_null
   *        If not null and canonicalizing (not rejecting), `*changed_or_null` is set to whether
   *        the `canonicalize_candidate()` detected at least one option's value changed from the previous canonical
   *        state.
   */
  template<typename Value_set>
  void option_set_canonicalize_or_reject(Option_set<Value_set>* opt_set,
                                         bool canonicalize_else_reject, bool* changed_or_null);

  /**
   * Implements all apply_static() overloads.
   *
   * @tparam Value_set
   *         See apply_static().
   * @param allow_invalid_defaults
   *        `true` if and only if #allow_invalid_defaults_tag_t used.
   * @param static_cfg_path
   *        See apply_static().
   * @param final_validator_func
   *        See apply_static().
   * @param commit
   *        See apply_static().
   * @return See apply_static().
   */
  template<typename... Value_set>
  bool apply_static_impl(bool allow_invalid_defaults,
                         const fs::path& static_cfg_path,
                         const typename Final_validator_func<Value_set>::Type&... final_validator_func,
                         bool commit);

  /**
   * Implements all apply_dynamic() overloads.
   *
   * @tparam Value_set
   *         See apply_dynamic().
   * @param allow_invalid_defaults
   *        `true` if and only if #allow_invalid_defaults_tag_t used.
   * @param dynamic_cfg_path
   *        See apply_dynamic().
   * @param final_validator_func
   *        See apply_dynamic().
   * @param commit
   *        See apply_static().
   * @return See apply_dynamic().
   */
  template<typename... Value_set>
  bool apply_dynamic_impl(bool allow_invalid_defaults,
                          const fs::path& dynamic_cfg_path,
                          const typename Final_validator_func<Value_set>::Type&... final_validator_func,
                          bool commit);

  /**
   * Implements all apply_static_and_dynamic() overloads.
   *
   * @param allow_invalid_defaults
   *        `true` if and only if #allow_invalid_defaults_tag_t used.
   * @param cfg_path
   *        See apply_static_and_dynamic().
   * @param final_validator_func
   *        See apply_static_and_dynamic().
   * @param commit
   *        See apply_static_and_dynamic().
   * @return See apply_static_and_dynamic().
   */
  bool apply_static_and_dynamic_impl(bool allow_invalid_defaults,
                                     const fs::path& cfg_path,
                                     const typename Final_validator_func<S_d_value_set>::Type&... final_validator_func,
                                     bool commit);

  /**
   * Work-horse helper that parses either *all* static value sets *or* *all* dynamic value sets from the specified
   * file into #m_s_d_opt_sets and returns `true` if and only if all have succeeded.  (It stops on first failure.)
   * The canonicalize/reject step is not performed: only parsing and (if that worked) `final_validator_func()`.
   * Recall that `final_validator_func() == S_SKIP`, even though it will cause us to not have changed that `Value_set`'s
   * Option_set::values_candidate(), is still considered success.
   *
   * @tparam Value_set
   *         See apply_static() or apply_dynamic() depending on `dyn_else_st`.
   * @param dyn_else_st
   *        Basically whether apply_static() or apply_dynamic() was called.
   * @param cfg_path
   *        See apply_static() or apply_dynamic() depending on `dyn_else_st`.
   * @param all_opt_names_or_empty
   *        Empty if *any* option names may be present in file (such as to support dynamic config compatibility);
   *        otherwise must contain any option names that are allowed in this file (such as for apply_static()).
   * @param final_validator_func
   *        See apply_static() or apply_dynamic() depending on `dyn_else_st`.
   * @return See apply_static() or apply_dynamic() depending on `dyn_else_st`.
   */
  template<typename... Value_set>
  bool apply_static_or_dynamic_impl
         (bool dyn_else_st,
          const fs::path& cfg_path, const boost::unordered_set<std::string>& all_opt_names_or_empty,
          const typename Final_validator_func<Value_set>::Type&... final_validator_func);

  /**
   * Work-horse helper that parses into one given Option_set in #m_s_d_opt_sets (from opt_set()) from the specified
   * file and returns `true` if and only if successful.  The canonicalize/reject step is not performed: only
   * parsing and (if that worked) `final_validator_func()`.
   * Recall that `final_validator_func() == S_SKIP`, even though it will cause us to not have changed that `Value_set`'s
   * Option_set::values_candidate(), is still considered success.
   *
   * @tparam Value_set
   *         See opt_set().
   * @param opt_set
   *        An opt_set() result.
   * @param baseline_value_set_or_null
   *        If null then ignored; else -- just before attempting to parse and apply values from `cfg_path` -- we
   *        will set `*opt_set` to contain a copy of `Value_set *baseline_value_set_or_null`.  This is to ensure
   *        a consistent state after each dynamic config update as opposed to working incrementally.
   *        Note, also, it is ignored if opt_set->null() (apply_impl() will no-op in any case).
   * @param cfg_path
   *        See apply_static_or_dynamic_impl().
   * @param all_opt_names_or_empty
   *        See apply_static_or_dynamic_impl().
   *        Technically this need not include the ones actually parsed by `*opt_set`; but including them is harmless
   *        (and may be helpful to the caller for simplicity).
   * @param final_validator_func
   *        See apply_static() or apply_dynamic() or apply_static_and_dynamic().
   * @return `true` on success; `false` on failure.
   */
  template<typename Value_set>
  bool apply_impl(Option_set<Value_set>* opt_set, const Value_set* baseline_value_set_or_null, const fs::path& cfg_path,
                  const boost::unordered_set<std::string>& all_opt_names_or_empty,
                  const typename Final_validator_func<Value_set>::Type& final_validator_func);

  /**
   * Helper for the top of `apply_*()` that guards against a call to `apply_Y()` following
   * `apply_X(commit == false) == true` without a reject_candidates() between them.  If this is detected, it
   * effectively "inserts" the "missing" reject_candidates() call -- plus an INFO message.
   *
   * Post-condition: #m_multi_src_update_in_progress equals Update_type::S_NONE.
   *
   * @param this_update_type
   *        The type (not NONE, or behavior undefined -- assert may trip) of `apply_*()` calling us.
   */
  void reject_candidates_if_update_type_changed(Update_type this_update_type);

  /**
   * Little helper that, having assumed #m_d_value_sets_mutex is locked, makes a newly allocated copy of the canonical
   * dynamic config at the given slot and saves a (new) ref-counted pointer to it in the proper #m_d_value_sets slot.
   *
   * @tparam Value_set
   *         See opt_set().
   * @param opt_set
   *        An opt_set() result (dynamic slot).
   * @param d_value_set_idx
   *        One of [0, 1, ..., #S_N_D_VALUE_SETS).
   */
  template<typename Value_set>
  void save_dynamic_value_set_locked(Option_set<Value_set>* opt_set, size_t d_value_set_idx);

  /**
   * Invokes the registered listeners for the given dynamic config slot (synchronously).
   *
   * @param d_value_set_idx
   *        One of [0,1, ... #S_N_D_VALUE_SETS).
   * @param init
   *        For logging (as of this writing): whether this is due to the initial parsing of this dynamic config slot
   *        or a subsequent change.
   */
  void invoke_dynamic_change_listeners(size_t d_value_set_idx, bool init) const;

  /**
   * Helper of state_to_ostream() for #m_s_d_opt_sets slot `value_set_idx`.
   *
   * @param value_set_idx
   *        One of [0, 1, ..., #S_N_VALUE_SETS).
   * @param os
   *        See state_to_ostream().
   */
  template<typename Value_set>
  void state_to_ostream_impl(size_t value_set_idx, std::ostream& os) const;

  /**
   * Helper of help_to_ostream() for #m_s_d_opt_sets slot `value_set_idx`.
   *
   * @param value_set_idx
   *        One of [0, 1, ..., #S_N_VALUE_SETS).
   * @param os
   *        See help_to_ostream().
   */
  template<typename Value_set>
  void help_to_ostream_impl(size_t value_set_idx, std::ostream& os) const;

  // Data.

  /**
   * The static and dynamic value sets, in the same order as the #S_N_VALUE_SETS
   * `S_d_value_set` template args, the specific type for each slot being: `Option_set<S_d_value_set>`.
   * This is the main data store managed by Config_manager.  Each static `S_d_value_set` (the even slots) is the
   * entirety of data having to do with that piece of static config.  Each dynamic `S_d_value_set` (the odd slots) is
   * the *canonical* dynamic config; and a pointer to a *copy* thereof is also maintained in #m_d_value_sets.
   *
   * The slots, in order, are hence:
   *   - Index [0]: static slot 0,
   *   - Index [1]: dynamic slot 0,
   *   - Index [2]: static slot 1,
   *   - Index [3]: dynamic slot 1,
   *   - ...
   *   - Index [`.size() - 2`]: static slot `S_N_S_VALUE_SETS - 1`.
   *   - Index [`.size() - 1`]: dynamic slot `S_N_D_VALUE_SETS - 1`.
   *
   * The actual type stored is `shared_ptr<void>`, with each slot's held pointer actually being an
   * `Option_set<S_d_value_set>*`.
   * opt_set() obtains the actual `Option_set<>` at a given slot, performing the required casting.
   *
   * ### Rationale ###
   * Why use an array of `shared_ptr<void>`, which requires a `static_cast` anytime we need to actually
   * access a config set?  The more compile-time/meta-programm-y approach I (ygoldfel) could conceive of was to use
   * `tuple<S_d_value_set...>`.  That would be quite elegant and lightning-fast (though compilation might be very slow).
   * However then, if I understand correctly, that would prevent any kind of runtime iteration through them.
   * This might be absolutely fine, if every slot were treated identically, but in the current setup it's
   * static, dynamic, static, dynamic, ....  Therefore, by moving some processing to runtime instead of compile-time
   * we gain flexibility.  (Do note my disclaimer in Implementation section in class doc header -- indicating this is
   * my first foray into advanced meta-programming like this.  So perhaps I lack imagination/experience.)
   *
   * So then `shared_ptr<void>` actually stores an `S_d_value_set*` -- `S_d_value_set` being a different type for
   * each slot in the array.  This uses the type erasure pattern of standard C++ smart pointers, wherein because
   * `static_cast<void*>(T*)` is allowed for any type `T`, `shared_ptr<void>` deleter will properly `delete` the `T*`,
   * as long as the `shared_ptr` ctor is passed a `T*` raw pointer.  Do note this is type-safe only if our code
   * knows to cast to the proper `T*` when the pointer is accessed.
   *
   * Why `shared_ptr` -- not `unique_ptr` (which is nominally faster in that there's no ref-counting)?  Answer:
   * `unique_ptr` is apparently deliberately simple and lacks aliasing and type-erasure constructors.
   * `unique_ptr<T>` can only delete a `T` with the default deleter; and `T` being `void` therefore won't compile.
   * It is probably possible to hack it with a custom deleter -- but this seemed like overkill, as these handles
   * are never used except inside an `array`, so reference counting shouldn't even happen and thus cost any perf
   * cycles outside of `*this` ctor and dtor.
   *
   * An alternative, which is somewhat more type-safe in that it would have resulted in an RTTI exception if the
   * wrong type-cast were used on the stored value, was `any`.  However `any` is slower, involving `virtual` lookups
   * (at least in a naive implementation of `any`).
   */
  boost::array<Void_ptr, S_N_VALUE_SETS> m_s_d_opt_sets;

  /**
   * The baseline dynamic value sets, in the same order as #S_N_D_VALUE_SETS *dynamic*
   * `S_d_value_set` template args, the specific type for each slot being `S_d_value_set`.
   *
   * The actual type stored is `shared_ptr<void>`, with each slot's held pointer actually being `S_d_value_set*`.
   * The same type erasure technique used in #m_s_d_opt_sets is used here.
   * d_baseline_value_set() obtains the actual `S_d_value_set` at a given slot, performing the required casting.
   *
   * A null pointer is stored if the corresponding Option_set in #m_s_d_opt_sets is `null()` (a placeholder with 0
   * settings).
   *
   * ### Rationale / explanation ###
   * Suppose a dynamic `Value_set` is to be parsed from file F potentially repeatedly, as dynamic updates come in.
   * Then its Option_set from `m_s_d_opt_sets[]` would `parse_config_file(F)` each time.  However, if that is all
   * we did, then the updates would stack on top of each other incrementally: if one looks at F's contents at a given
   * time, one cannot tell what the `Value_set` in memory would contain upon parsing it; it would depend on any
   * preceding updates.  That's bad: the resulting memory `Value_set` must be consistent, regardless of what
   * F contained in preceding updates.  Therefore, a certain *baseline* state for that `Value_set` must be loaded
   * into the Option_set just before the Option_set::parse_config_file() call in each update.
   *
   * This stores each dynamic slot's baseline `Value_set` state to load as just described.
   *
   * After construction this payload is just the default-cted `Value_set()`.  If the user chooses to execute
   * a one-time apply_static_and_dynamic(), then that payload is replaced by the state after having parsed that
   * baseline state.  Note, for context, that apply_static_and_dynamic() would be presumably loading *not*
   * from file F (which can change repeatedly, as dynamic updates come in) but some other file B, typically storing
   * both static value sets' contents as well as the baseline dynamic value sets'.
   */
  boost::array<Void_ptr, S_N_D_VALUE_SETS> m_d_baseline_value_sets;

  /**
   * The dynamic config ref-counted handles returned by all_dynamic_values().  Each one is a ref-counted pointer
   * to an immutable *copy* of the canonical `S_d_value_set` in that *dynamic* slot of #m_s_d_opt_sets
   * (namely `m_s_d_opt_sets[2 * d_idx + 1]`, where `d_idx` is in [0, 1, ..., #S_N_D_VALUE_SETS)).
   *
   * The actual type stored is `shared_ptr<void>`, with each slot's held pointer actually being
   * `S_d_value_set::Const_ptr*`.  The same type erasure technique used in #m_s_d_opt_sets is used here.
   *
   * Protected by #m_d_value_sets_mutex.
   *
   * apply_dynamic() and apply_static_and_dynamic() set these; all_dynamic_values() and dynamic_values() get them.
   *
   * ### Rationale ###
   * Why is this necessary?  Why not just expose a pointer to immutable `S_d_value_set` directly inside
   * `m_s_d_opt_sets[d_idx]` as we do (via static_values() and all_static_values()) for the static config?  Answer:
   * Because with dynamic config it is important that the user be able to retain (if even for a short time, perhaps to
   * complete some operation -- *during* which `*this` might process a dynamic update via apply_dynamic()) an immutable
   * `S_d_value_set`.  Since by definition that's not possible with dynamic config updates, we must create a copy.  At
   * that point it becomes natural to wrap it in a `shared_ptr`, so that only *one* copy is necessary in the entire
   * application -- and it'll disappear once (1) the canonical config has been updated, replacing `m_d_value_sets[]`,
   * and (2) all user code has dropped the remaining refs to it.
   *
   * The ref-counted-pointer-to-dynamic-config is a common pattern.  The only thing we add that might not be obvious
   * is having the canonical copy be separate from the one we expose via the pointer publicly.  The latter is
   * the natural consequence of the fact that the `Value_set` managed by `Option_set<Value_set>` lives inside
   * the `Option_set` and is not wrapped by any `shared_ptr`.  However Option_set::mutable_values_copy() does provide
   * easy access to a `shared_ptr`-wrapped copy thereof... so we use it.  The perf cost is that of a value-set
   * copy at apply_dynamic() time which is negligible in the grand scheme of things.
   */
  boost::array<Void_ptr, S_N_D_VALUE_SETS> m_d_value_sets;

  /// Mutex protecting #m_d_value_sets.
  mutable util::Mutex_non_recursive m_d_value_sets_mutex;

  /**
   * List of callbacks to execute after #m_d_value_sets members (the pointers) are next assigned: one per
   * each of the #S_N_D_VALUE_SETS dynamic config slots.  Note we do *not*
   * assign a given slot's #m_d_value_sets pointer, unless at least one option has changed within the associated
   * canonical value set in #m_s_d_opt_sets.
   *
   * ### Rationale ###
   * A `list<>` is used here so that iterators to callbacks can be stored in `On_dynamic_change_func_handle`s without
   * the possibility of them being invalidated by insertions to or removals from the list (which would be the case if
   * `vector<>` were used).
   */
  boost::array<std::list<On_dynamic_change_func>, S_N_D_VALUE_SETS> m_on_dynamic_change_funcs;

  /// Starts `false`; set to `true` permanently on successful apply_static_and_dynamic() or apply_dynamic().
  bool m_dynamic_values_set;

  /**
   * In short, truthy if and only if a `commit == false` update is currently in progress,
   * meaning there's an uncommitted *candidate* `Value_set` at the moment.  More precisely:
   *   - If the last-invoked `apply_*()` method (1) returned `false`, or (2) returned `true` but with arg
   *     `commit == true`, or (3) has never been called, or (4) was followed at any point
   *     by reject_candidates(), then this equals Update_type::S_NONE.  I.e., no multi-source update is
   *     currently in progress.  Otherwise:
   *   - This indicates which `apply_*(commit = true) == true` method (multi-source update type) it was.
   *     To the extent the exact type is irrelevant, its being not NONE indicates a multi-source update
   *     is in progress.
   *
   * Inside an `apply_*()` method, this is updated to its new value after parsing into #m_s_d_opt_sets.  Thus
   * during the bulk of such a method's processing we can easily determine whether this is step 1 of a
   * (potentially multi-source) update -- rather than step 2+ -- as this will equal NONE in the former
   * case only.
   *
   * When this is not-NONE, an `apply_*()` impl shall skip a couple of steps it would otherwise perform:
   *   - Individually-validating the default `Value_set()` values: Skip, as the first `apply_*()` in the sequence
   *     would have already done it.  So it's a waste of compute/entropy.
   *   - `apply_dynamic()` applying #m_d_baseline_value_sets onto to #m_s_d_opt_sets: Skip, as the first `apply_*()` in
   *     the sequence would have already done it.  So doing it again would be not only redundant but also destructive,
   *     overwriting any incremental changes made in preceding 1+ `apply_*()` calls.
   *
   * It would have been enough for it to be a `bool` for those tests.  It is an `enum` to be able to detect
   * calling `apply_X()` after `apply_Y(commit = false) == true` without reject_candidates() in-between.
   */
  Update_type m_multi_src_update_in_progress;
}; // class Config_manager

/// Opaque handle for managing a dynamic config change callback.
template<typename... S_d_value_set>
struct Config_manager<S_d_value_set...>::On_dynamic_change_func_handle
{
  /**
   * Opaque handle detail.
   * @internal
   * The dynamic config slot index for the value set which is associated with the callback.  This is used as an index
   * into `m_on_dynamic_change_funcs` to specify the list of callbacks which contains the callback.
   */
  size_t m_d_value_set_idx;

  /**
   * Opaque handle detail.
   * @internal
   * Iterator to the callback within the associated callback list.
   */
  std::list<On_dynamic_change_func>::const_iterator m_pos;
};

// Free functions: in *_fwd.hpp.

// However the following refer to inner type(s) and hence must be declared here and not _fwd.hpp.

/**
 * Returns a value usable as `final_validator_func` arg to Config_manager::apply_static() and others -- for a
 * Null_value_set value set.
 *
 * @return See above.
 */
Final_validator_func<Null_value_set>::Type null_final_validator_func();

/**
 * Returns a value usable as `declare_opts_func_moved` Config_manager ctor arg for a Null_value_set value set.
 * Naturally it does nothing.
 *
 * @return See above.
 */
Option_set<Null_value_set>::Declare_options_func null_declare_opts_func();

// Template implementations.

template<typename... S_d_value_set>
Config_manager<S_d_value_set...>::Config_manager
  (log::Logger* logger_ptr, util::String_view nickname,
   typename Option_set<S_d_value_set>::Declare_options_func&&... declare_opts_func_moved) :
  log::Log_context(logger_ptr, Flow_log_component::S_CFG),
  m_nickname(nickname),
  m_dynamic_values_set(false), // Becomes `true` when apply*_dynamic() succeeds once.
  m_multi_src_update_in_progress(Update_type::S_NONE)
{
  using util::ostream_op_string;

  /* @todo OK; this is really weird; I wanted to make this a static_assert() (and put it near where this constexpr
   * is initialized), but it is then triggered with gcc claiming Config_manager<> (with no tparams!) is
   * compiled -- bizarrely due to a simple FLOW_LOG_INFO() line a few hundred lines below this.  I find this
   * inexplicable; almost seems like a bug, but maybe not.  Look into it. -ygoldfel */
  assert((S_N_VALUE_SETS != 0)
         && "Must have at least 2 template args to Config_manager; otherwise what are you configuring?");

  // (See class doc header Implementation section for explanation of fold-expression (..., ) trick.)

  /* Set each m_s_d_opt_sets[] to store the Option_set<S_d_value_set>, for each S_d_value_set (class tparam-pack).
   * Recall that m_s_d_opt_sets[] stores them in order static, dynamic, static, dynamic, ....
   *
   * Set each m_d_baseline_value_sets[] to store a default-valued S_d_value_set (for dynamic sets only).
   * These may get overwritten optionally (because apply_static_and_dynamic() is optional) once, in
   * apply_static_and_dynamic().  For explanation what this is for see m_d_baseline_value_sets doc header.
   *
   * Initialize each m_d_value_sets[] as well. */
  bool dyn_else_st = false;
  size_t value_set_idx = 0;
  size_t d_value_set_idx = 0;
  (
    ...,
    (
      // I (ygoldfel) tried make_shared() here, but it was too much for that gcc.  The perf impact is negligible anyway.
      m_s_d_opt_sets[value_set_idx]
        = Void_ptr
            (new Option_set<S_d_value_set> // <-- Parameter pack expansion kernel here.
                   (get_logger(),
                    ostream_op_string(m_nickname, dyn_else_st ? "/dynamic" : "/static",
                                      value_set_idx / 2), // 0, 0, 1, 1, ....
                    std::move(declare_opts_func_moved))), // (And here.)

      /* As noted above, fill a m_d_baseline_value_sets[] but for dynamic (non-null()) sets only.
       * Same deal for initializing m_d_value_sets.
       * (This is `if (dyn_else_st) { ... }` in expression form.) */
      dyn_else_st
        // * (This is `if (!(...).null()) { ... }` in expression form.)
        && (opt_set<S_d_value_set>(value_set_idx)->null() // (And here.)
              || (m_d_baseline_value_sets[d_value_set_idx]
                    = Void_ptr(new S_d_value_set), // (And here.)
                  true),
            m_d_value_sets[d_value_set_idx]
              = Void_ptr(new typename S_d_value_set::Const_ptr), // (And here.)
            ++d_value_set_idx,
            true),

      ++value_set_idx,
      dyn_else_st = !dyn_else_st
    )
  );

  FLOW_LOG_TRACE("Config_manager [" << *this << "]: Ready; "
                 "managing [" << S_N_S_VALUE_SETS << "] dynamic+static option set pairs (some may be null/unused).");
} // Config_manager::Config_manager()

template<typename... S_d_value_set>
template<typename Value_set>
Option_set<Value_set>*
  Config_manager<S_d_value_set...>::opt_set(size_t value_set_idx)
{
  // Fish out the value stuffed into the ptr<void> at the given slot by the ctor.

  assert(value_set_idx < S_N_VALUE_SETS);
  return static_cast<Option_set<Value_set>*> // Attn: void* -> T* cast; no RTTI to check our type safety.
           (m_s_d_opt_sets[value_set_idx].get());
}

template<typename... S_d_value_set>
template<typename Value_set>
Value_set*
  Config_manager<S_d_value_set...>::d_baseline_value_set(size_t d_value_set_idx)
{
  // Fish out the value stuffed into the ptr<void> at the given slot by the ctor.

  assert(d_value_set_idx < S_N_D_VALUE_SETS);
  return static_cast<Value_set*> // Attn: void* -> T* cast; no RTTI to check our type safety.
           (m_d_baseline_value_sets[d_value_set_idx].get());
}

template<typename... S_d_value_set>
template<typename Value_set>
const Option_set<Value_set>*
  Config_manager<S_d_value_set...>::opt_set(size_t value_set_idx) const
{
  return const_cast<Config_manager<S_d_value_set...>*>(this)->opt_set<Value_set>(value_set_idx);
}

template<typename... S_d_value_set>
template<typename Value_set>
void Config_manager<S_d_value_set...>::option_set_canonicalize_or_reject
       (Option_set<Value_set>* opt_set, bool canonicalize_else_reject, bool* changed_or_null)
{
  if (opt_set->null())
  {
    // If null() then *opt_set wasn't even parse*()d (see apply_impl()), so certainly it hasn't changed.
    changed_or_null
      && (*changed_or_null = false);
  }
  else
  {
    canonicalize_else_reject ? opt_set->canonicalize_candidate(changed_or_null)
                             : opt_set->reject_candidate();
  }
}

template<typename... S_d_value_set>
template<typename Value_set>
typename Config_manager<S_d_value_set...>::On_dynamic_change_func_handle
  Config_manager<S_d_value_set...>::register_dynamic_change_listener
    (size_t d_value_set_idx, On_dynamic_change_func&& on_dynamic_change_func_moved)
{
  FLOW_LOG_INFO("Config_manager [" << *this << "]: Adding dynamic-change listener for dynamic value set "
                "[" << d_value_set_idx << "].");

  assert((d_value_set_idx < S_N_D_VALUE_SETS) && "Invalid dynamic option set index.");
  assert((!opt_set<Value_set>((d_value_set_idx * 2) + 1)->null())
         && "Do not register dynamic change listener for Null_value_set or otherwise null value set.");

  // Not thread-safe (as advertised) against apply*_dynamic() or unregister_dynamic_change_listener().
  m_on_dynamic_change_funcs[d_value_set_idx].emplace_back(std::move(on_dynamic_change_func_moved));
  const auto& callback_pos = std::prev(m_on_dynamic_change_funcs[d_value_set_idx].end());

  FLOW_LOG_INFO("Config_manager [" << *this << "]: Added dynamic-change listener [@" << &(*callback_pos) <<
                "] for dynamic value set [" << d_value_set_idx << "].");

  return On_dynamic_change_func_handle{ d_value_set_idx, callback_pos };
}

template<typename... S_d_value_set>
void Config_manager<S_d_value_set...>::unregister_dynamic_change_listener (const On_dynamic_change_func_handle& handle)
{
  // Not thread-safe (as advertised) against apply*_dynamic() or register_dynamic_change_listener().

  auto& on_dynamic_change_funcs = m_on_dynamic_change_funcs[handle.m_d_value_set_idx];
  on_dynamic_change_funcs.erase(handle.m_pos);

  FLOW_LOG_INFO("Config_manager [" << *this << "]: Removed dynamic-change listener [@" << &(*handle.m_pos) <<
                "] for dynamic value set [" << handle.m_d_value_set_idx << "].");
}

template<typename... S_d_value_set>
template<typename... Value_set>
bool Config_manager<S_d_value_set...>::apply_static
       (const fs::path& static_cfg_path,
        const typename Final_validator_func<Value_set>::Type&... final_validator_func,
        bool commit)
{
  return apply_static_impl<Value_set...>(false, static_cfg_path, final_validator_func..., commit);
}

template<typename... S_d_value_set>
template<typename... Value_set>
bool Config_manager<S_d_value_set...>::apply_static
       (allow_invalid_defaults_tag_t,
        const fs::path& static_cfg_path,
        const typename Final_validator_func<Value_set>::Type&... final_validator_func,
        bool commit)
{
  return apply_static_impl<Value_set...>(true, static_cfg_path, final_validator_func..., commit);
}

template<typename... S_d_value_set>
template<typename... Value_set>
bool Config_manager<S_d_value_set...>::apply_static_impl
       (bool allow_invalid_defaults,
        const fs::path& static_cfg_path,
        const typename Final_validator_func<Value_set>::Type&... final_validator_func,
        bool commit)
{
  using boost::unordered_set;
  using std::string;
  using std::declval;
  using std::is_same_v;

  FLOW_LOG_INFO("Config_manager [" << *this << "]: Request to apply static config file [" << static_cfg_path << "].  "
                "Invalid defaults allowed if explicitly set in file? = [" << allow_invalid_defaults << "].  "
                "This application (if successful) shall end with canonicalizing values? = [" << commit << "]; "
                "multi-source update already in progress (0=none) = [" << int(m_multi_src_update_in_progress) << "].");

  reject_candidates_if_update_type_changed(Update_type::S_STATIC);

  /* Even if they're all .null(), they must give some validator functions (they'll be ignored and should just
   * be assigned null_final_validator_func() a/k/a Final_validator_func<Null_value_set>::null() anyway,
   * though we don't enforce it). */
  static_assert(sizeof...(Value_set) == S_N_S_VALUE_SETS,
                "You must supply N/2 `final_validator_func`s, where N is # of S_d_value_set params.  "
                  "Use null_final_validator_func() for any Null_value_set.");

  /* (See class doc header Implementation section for explanation of fold-expression (..., ) trick.)
   *
   * Note, though, that (... && expr) (of, say, length 4) evaluates to (((expr1 && expr2) && expr3) && expr4);
   * which means it'll run through the `expr...i...`s until one evaluates to false; then it stops and evaluates
   * no more past that. */

  size_t value_set_idx;
  bool success = true;

  /* Optionally scan the defaults themselves for individual-option-validator validity if so requested.
   * However no need to repeat this, if a multi-source update was already in progress. */
  if ((!allow_invalid_defaults) && (m_multi_src_update_in_progress == Update_type::S_NONE))
  {
    value_set_idx = 0;
    [[maybe_unused]] const auto dummy = // Defeats false gcc warning due to trailing `success` when N_S_VALUE_SETS is 1.
    (
      ...
      &&
      (
        opt_set<Value_set>(value_set_idx) // Param pack expansion kernel here.
          ->validate_values(&success), // Set `success`...
        value_set_idx += 2, // (Skip dynamic ones.)
        success // ...and if it's false, the subsequent option sets shall be skipped.
      )
    );

    if (!success) // It would have logged the exact problem, but we still want to clearly say what happened, high-level:
    {
      FLOW_LOG_WARNING("Config_manager [" << *this << "]: "
                       "Request to apply static config file [" << static_cfg_path << "]:  "
                       "Will not parse file, because stringent check "
                       "of defaults detected at least 1 individually-invalid default value.  Either correct "
                       "the default or use apply_static(allow_invalid_defaults_tag_t) overload.");
    }
  } // if (!allow_invalid_defaults)

  // First collect all valid static option names as required by apply_static_or_dynamic_impl().
  unordered_set<string> all_opt_names; // The result.
  unordered_set<string> const * this_opt_names; // Used for each static slot.
  // Ensure Option_set<>::option_names() returns ref-to-const, so this_opt_names can safely point to it.
  static_assert(is_same_v<decltype(declval<Option_set<Null_value_set>>().option_names()),
                          decltype(*this_opt_names)>,
                "Option_set<>::option_names() must return ref-to-const.  Bug?");
  value_set_idx = 0;
  if (success) // Skip all this if failed above.
  {
    (
      ...,
      (
        this_opt_names = &(opt_set<Value_set> // Param pack expansion kernel here.
                             (value_set_idx)->option_names()),
        all_opt_names.insert(this_opt_names->begin(), this_opt_names->end()),
        value_set_idx += 2 // Skip dynamic ones.
      )
    );

    // Next do the actual parsing of each static Option_set.  Stop at first failure.  Return `true` iff total success.
    success = apply_static_or_dynamic_impl<Value_set...>
                (false, // false => static.
                 static_cfg_path, all_opt_names, final_validator_func...);
  } // if (success) (but it may have become false inside)

  if ((!commit) && success)
  {
    m_multi_src_update_in_progress = Update_type::S_STATIC; // (Was either NONE or already this.)
    FLOW_LOG_INFO("Config_manager [" << *this << "]: Multi-source update has successfully begun or continued.  "
                  "Expecting more sources before we can canonicalize candidate(s).");
    return true;
  }
  // else if (commit || (!success)):

  /* !success => reject, regardless of commit (a failure aborts a multi-source update too).
   * commit => accept or reject depending on `success`.
   *
   * So finally either canonicalize or reject each parse attempt depending on overall `success` (static only).
   * For details on what that means see Option_set<> docs.  Basically it just finalizes the parsing by updating
   * the canonically stored Value_set inside the Option_set<Value_set>.  Until then it's stored to the side as the
   * *candidate*. */
  value_set_idx = 0;
  (
    ...,
    (
      option_set_canonicalize_or_reject(opt_set<Value_set>(value_set_idx), // Param pack expansion kernel here.
                                        success, 0),
      value_set_idx += 2 // Skip dynamic ones.
    )
  );

  if (m_multi_src_update_in_progress != Update_type::S_NONE)
  {
    FLOW_LOG_INFO("Config_manager [" << *this << "]: Multi-source update is now finished.");
    m_multi_src_update_in_progress = Update_type::S_NONE;
  }
  // else { It was NONE and remains so, either due to failure or being a vanilla (single-source) update or both. }

  FLOW_LOG_INFO("Config_manager [" << *this << "]: State that was just made canonical follows.");
  log_state(); // Note this will auto-skip dynamic sets which should all be Null_state and hence .null().

  return success;
} // Config_manager::apply_static_impl()

template<typename... S_d_value_set>
template<typename... Value_set>
bool Config_manager<S_d_value_set...>::apply_static_or_dynamic_impl
       (bool dyn_else_st, const fs::path& cfg_path, const boost::unordered_set<std::string>& all_opt_names_or_empty,
        const typename Final_validator_func<Value_set>::Type&... final_validator_func)
{
  size_t s_d_value_set_idx = 0;
  size_t value_set_idx = dyn_else_st ? 1 : 0;

  bool success;
  return
    (
      ...
      &&
      (
        /* 2nd apply_impl() arg: If we're called from apply_dynamic(), supply the pre-parse baseline state, but only
         * if this isn't the 2nd/3rd/... apply_dynamic() of a multi-source (commit=false) sequence.
         * (If it's a null Option_set, then it'll be ignored by the no-op apply_impl().
         * Pass null.)
         * If we're called from apply_static(), it only happens once and is not relevant.
         * Pass null.
         * If it is the 2nd/3rd/... apply_dynamic() of a multi-source (commit=false) sequence, the pre-parse
         * baseline state has already been applied; and we don't want to blow away the incrementally built-up candidate
         * stored inside Value_set opt_set<>().
         * Pass null.
         * We are never called from apply_static_and_dynamic() which, itself,
         * sets that baseline state for later use in apply_dynamic() and hence here. */
        success = apply_impl(opt_set<Value_set> // Param pack expansion kernel here.
                               (value_set_idx), // 1, 3, 5, ... -or- 0, 2, 4, ....

                             ((m_multi_src_update_in_progress == Update_type::S_NONE)
                                && dyn_else_st && (!opt_set<Value_set>(value_set_idx)->null()))
                               ? d_baseline_value_set<Value_set> // (And here.)
                                   (s_d_value_set_idx)
                               : static_cast<const Value_set*>(0), // (And here.)

                             cfg_path, all_opt_names_or_empty,
                             final_validator_func), // (And here.)
        ++s_d_value_set_idx,
        value_set_idx += 2,
        success // Stop apply_impl()ing on first failure (the `&&` will evaluate to false early).
      )
    );
} // Config_manager::apply_static_or_dynamic_impl()

template<typename... S_d_value_set>
template<typename Value_set>
bool Config_manager<S_d_value_set...>::apply_impl
       (Option_set<Value_set>* opt_set,
        const Value_set* baseline_value_set_or_null,
        const fs::path& cfg_path,
        const boost::unordered_set<std::string>& all_opt_names_or_empty,
        const typename Final_validator_func<Value_set>::Type& final_validator_func)
{
  assert(opt_set);
  auto& opts = *opt_set;

  if (opts.null())
  {
    return true; // By the way notice, as advertised, final_validator_func is ignored.
  }
  // else

  /* For some sets (namely dynamic ones, at most) we need to pre-apply the full set of baseline values.
   * This is explained in m_d_baseline_value_sets doc header; but basically it's so that successive dynamic updates
   * always apply the change in the file on top of a consistent state.  Otherwise it's impossible to know, just by
   * looking at the dynamic config file, what the resulting state will be in memory -- incremental is bad in this
   * context. */
  if (baseline_value_set_or_null)
  {
    opts.parse_direct_values(*baseline_value_set_or_null);
  }

  /* We allow final_validator_func()==SKIP, which means the validator decided the candidate Value_set is not
   * erroneous but has something there that means this particular apply_impl() shall have no effect on the candidate
   * after all.  To make this work we must memorize the pre-parse state; as Option_set doesn't know about
   * final-validation or skipping and will have updated .values_candidate() (otherwise) irreversibly.  We, however,
   * can simply undo it via .parse_direct_values() or .reject_candidate().
   *
   * Subtlety: If, in this possibly-multi-source update, this is the 1st source, then opt_set->values_candidate() may be
   * null (*opt_set is not in PARSING state but CANONICAL still).  One option is to do what we do: save the canonical
   * value-set instead; then if rewinding below, it'll stay in PARSING state but have the unchanged canonical values.
   * Another option is to, instead, save nothing and when rewinding below in that case
   * reject_candidate() and thus bring *opt_set to CANONICAL state again.  That may seem a bit more complex but more
   * elegant, but it leads to a subtle bug: If the possibly-multi-source update's final-validators *all* return SKIP --
   * which is arguably unusual but certainly allowed -- then apply_*() near its end will happily attempt to
   * opt_set->canonicalize_candidate() -- which will fail (possibly assert-trip) due to being invoked in
   * already-CANONICAL state.  So our choices are to go with the just-save-values() route, or to -- elsewhere
   * in apply_*() themselves -- guard ->canonicalize_candidate() with a check for the everything-SKIPped situation.
   * Both are okay, but the former approach is simpler, avoiding at least a couple of if-branches in a couple of
   * places.  It involves potentially 2 extra copy-ops of a Value_set; but so what?  In the grand scheme the perf
   * impact is negligible. */
  const auto candidate_or_null = opt_set->values_candidate();
  const auto pre_parse_candidate = candidate_or_null ? *candidate_or_null : opt_set->values();

  /* We allow unknown (to this Option_set<Value_set> `opts`) options in the file during the initial parse -- but
   * only the ones that are themselves options but in other Option_sets we're also parsing (since non-hard-coded options
   * are not being parsed in this file, no other options should be present -- forward-compatibility, etc., not a
   * factor with a hard-coded file).  Hence simply pass in all option names (including this one's, but they'll
   * just be parsed as belonging to this actual set and hence not be relevant to the unknown-option-name check).
   *
   * If it's not an initial parse then all_opt_names_or_empty will be empty, meaning simply allow all unknown options;
   * due to forward/backward-compatibility we just allow it and log such things but ignore them otherwise. */
  bool ok;
  opts.parse_config_file(cfg_path, true, &ok, all_opt_names_or_empty);

  /* parse_config_file() specifically promises not to individually-validate values not in the actual file.
   * So before returning success (likely causing to finalize values via opts.canonicalize_candidate()) ensure
   * any invalid defaults don't "fall through."  This is unnecessary if defaults were scanned individually
   * due to allow_invalid_defaults==false; but in that case it is harmless (as of this writing won't even log). */
  if (ok)
  {
    opts.validate_values_candidate(&ok);
    if (!ok) // It would have logged the exact problem, but we still want to clearly say what happened, high-level:
    {
      FLOW_LOG_WARNING("Config_manager [" << *this << "]: "
                       "Request to apply config file [" << cfg_path << "] for a particular value set:  "
                       "Individual values present in file were OK, but at least 1 default value (see above) "
                       "was invalid.  Either correct the default or ensure it is properly overridden in file.");
    }
  } // if (ok) (but it may have become false inside)

  if (ok)
  {
    // Lastly, as promised, run their inter-option validation (which can also specify that we skip).
    const auto final_validator_outcome = final_validator_func(*opts.values_candidate());
    switch (final_validator_outcome)
    {
    case Final_validator_outcome::S_ACCEPT:
      break; // Cool then.
    case Final_validator_outcome::S_FAIL:
      ok = false; // Uncool.  It logged.
      break;
    case Final_validator_outcome::S_SKIP: // Cool-ish.
      FLOW_LOG_INFO("Config_manager [" << *this << "]: "
                    "Request to apply config file [" << cfg_path << "] for a particular value set:  "
                    "Individual values present in file were OK, but the final-validator logic specified "
                    "that this file's values not be applied (to skip the file after all).  "
                    "Rewinding the candidate value-set to pre-parse state.  This is not an error.");
      opts.parse_direct_values(pre_parse_candidate);
    } // switch (final_validator_outcome).  Compiler should warn if we failed to handle an enum value.
  } // if (ok) (but it may have become false inside)

  return ok;
} // Config_manager::apply_impl()

template<typename... S_d_value_set>
bool Config_manager<S_d_value_set...>::apply_static_and_dynamic
       (const fs::path& cfg_path,
        const typename Final_validator_func<S_d_value_set>::Type&... final_validator_func,
        bool commit)
{
  return apply_static_and_dynamic_impl(false, cfg_path, final_validator_func..., commit);
}

template<typename... S_d_value_set>
bool Config_manager<S_d_value_set...>::apply_static_and_dynamic
       (allow_invalid_defaults_tag_t,
        const fs::path& cfg_path,
        const typename Final_validator_func<S_d_value_set>::Type&... final_validator_func,
        bool commit)
{
  return apply_static_and_dynamic_impl(true, cfg_path, final_validator_func..., commit);
}

template<typename... S_d_value_set>
bool Config_manager<S_d_value_set...>::apply_static_and_dynamic_impl
       (bool allow_invalid_defaults,
        const fs::path& cfg_path,
        const typename Final_validator_func<S_d_value_set>::Type&... final_validator_func,
        bool commit)
{
  using util::Lock_guard;
  using boost::unordered_set;
  using std::string;

  FLOW_LOG_INFO("Config_manager [" << *this << "]: Request to apply config file [" << cfg_path << "] as both "
                "static config and *baseline* dynamic config.  Parsing in that order for each pair.  "
                "Invalid defaults allowed if explicitly set in file? = [" << allow_invalid_defaults << "].  "
                "This application (if successful) shall end with canonicalizing values? = [" << commit << "]; "
                "multi-source update already in progress (0=none) = [" << int(m_multi_src_update_in_progress) << "].");

  reject_candidates_if_update_type_changed(Update_type::S_STATIC_AND_DYNAMIC);

  /* (See class doc header Implementation section for explanation of fold-expression (..., ) trick.)
   *
   * Note, though, that (... && expr) (of, say, length 4) evaluates to (((expr1 && expr2) && expr3) && expr4);
   * which means it'll run through the `expr...i...`s until one evaluates to false; then it stops and evaluates
   * no more past that. */

  size_t value_set_idx;
  bool success = true;

  /* Optionally scan the defaults themselves for individual-option-validator validity if so requested.
   * However no need to repeat this, if a multi-source update was already in progress. */
  if ((!allow_invalid_defaults) && (m_multi_src_update_in_progress == Update_type::S_NONE))
  {
    value_set_idx = 0;
    (
      ...
      &&
      (
        opt_set<S_d_value_set>(value_set_idx) // Param pack expansion kernel here.
          ->validate_values(&success), // Set `success`...
        ++value_set_idx,
        success // ...and if it's false, the subsequent option sets shall be skipped.
      )
    );

    if (!success) // It would have logged the exact problem, but we still want to clearly say what happened, high-level:
    {
      FLOW_LOG_WARNING("Config_manager [" << *this << "]: Request to apply config file [" << cfg_path << "] as both "
                       "static config and *baseline* dynamic config:  Will not parse file, because stringent check "
                       "of defaults detected at least 1 individually-invalid default value.  Either correct "
                       "the default or use apply_static_and_dynamic(allow_invalid_defaults_tag_t) overload.");
    }
  } // if ((!allow_invalid_defaults) && (!m_multi_src_update_in_progress))

  // First collect all valid static *and* dynamic option names as required by apply_impl().
  unordered_set<string> all_opt_names;
  unordered_set<string> this_opt_names;
  value_set_idx = 0;
  if (success) // Skip all this if failed above.
  {
    (
      ...,
      (
        this_opt_names = opt_set<S_d_value_set>(value_set_idx)->option_names(), // Param pack expansion kernel here.
        all_opt_names.insert(this_opt_names.begin(), this_opt_names.end()),
        ++value_set_idx
      )
    );

    // Next do the actual parsing of each static *and* dynamic Option_set.
    value_set_idx = 0;
    (
      ...
      &&
      (
        success = apply_impl(opt_set<S_d_value_set>(value_set_idx), // Param pack expansion kernel here.
                             // Initial (baseline) parse: no need to apply baseline state.
                             static_cast<const S_d_value_set*>(0), // (And here.)
                             cfg_path, all_opt_names,
                             final_validator_func), // (And here.)
        ++value_set_idx,
        success // Stop apply_impl()ing on first failure.
      )
    );
  } // if (success) (but it may have become false inside)

  if ((!commit) && success)
  {
    m_multi_src_update_in_progress = Update_type::S_STATIC_AND_DYNAMIC; // (Was either NONE or already this.)
    FLOW_LOG_INFO("Config_manager [" << *this << "]: Multi-source update has successfully begun or continued.  "
                  "Expecting more sources before we can canonicalize candidate(s).");
    return true;
  }
  // else if (commit || (!success))

  /* !success => reject, regardless of commit (a failure aborts a multi-source update too).
   * commit => accept or reject depending on `success`.
   *
   * So next either canonicalize or reject each parse attempt depending on overall success (static *and* dynamic). */
  value_set_idx = 0;
  (
    ...,
    (
      /* Note: If parsing stopped before `value_set_idx` due to failure, rejecting Option_set value_set_idx is a
       * harmless no-op by Option_set docs. */
      option_set_canonicalize_or_reject
        (opt_set<S_d_value_set>(value_set_idx), // Param pack expansion kernel here.
         success, 0),
      ++value_set_idx
    )
  );

  if (m_multi_src_update_in_progress != Update_type::S_NONE)
  {
    FLOW_LOG_INFO("Config_manager [" << *this << "]: Multi-source update is now finished.");
    m_multi_src_update_in_progress = Update_type::S_NONE;
  }
  // else { It was NONE and remains so, either due to failure or being a vanilla (single-source) update or both. }

  if (!success)
  {
    return false;
  }
  // else if (success):
  assert(commit && "Maintenance bug?  We should have returned above if `!commit` but success==true.");

  FLOW_LOG_INFO("Config_manager [" << *this << "]: State that was just made canonical follows.");
  log_state();

  /* For dynamic sets only -- we must memorize the current Value_set state into
   * m_d_baseline_value_sets[].  It shall be set as the baseline state before actually parsing dynamic config file
   * contents in apply_dynamic(), each time it is called subsequently. */
  bool dyn_else_st = false;
  value_set_idx = 0;
  size_t d_value_set_idx = 0;
  (
    ...,
    (
      /* As noted above, fill an m_d_baseline_value_sets[] but for dynamic sets only.  Note that this will copy
       * a Value_set onto a Value_set (in its entirety).  We could also replace the shared_ptr, but why bother?
       * The code would be longer, and it would be somewhat slower too (still a copy involved).
       *
       * Also skip any null() dynamic set.  (This is `if (dyn_else_st) { ... }` in expression form.) */
      dyn_else_st
        // (This is `if (!(...).null()) { ... }` in expression form.)
        && (opt_set<S_d_value_set>(value_set_idx)->null()  // Param pack expansion kernel here.
              || (*(d_baseline_value_set<S_d_value_set>(d_value_set_idx)) // (And here.)
                    = opt_set<S_d_value_set>(value_set_idx)->values(), // (And here.)
                  true),
            ++d_value_set_idx,
            true),

      ++value_set_idx,
      dyn_else_st = !dyn_else_st
    )
  );

  // Next: Eureka!  Set the dynamic-values pointers for the first time.
  {
    Lock_guard<decltype(m_d_value_sets_mutex)> lock(m_d_value_sets_mutex);
    value_set_idx = 0;
    dyn_else_st = false;
    (
      ...,
      (
        // Just skip static ones.  (This is `if (dyn_else_st) { ... }` in expression form.)
        dyn_else_st
          && (save_dynamic_value_set_locked<S_d_value_set> // Param pack expansion kernel here.
                (opt_set<S_d_value_set>(value_set_idx), value_set_idx / 2),
              true),
        ++value_set_idx,
        dyn_else_st = !dyn_else_st
      )
    );
  } // Lock_guard lock(m_d_value_sets_mutex);

  // They shouldn't do apply_static_and_dynamic() except as the first apply*_dynamic() thing.
  assert((!m_dynamic_values_set)
         && "User apparently attempted apply_static_and_dynamic(), but dynamic config was already initialized OK.");
  m_dynamic_values_set = true;

  // Finally invoke dynamic "change" listeners for the first time.  (.null() => no listeners registered.)
  for (d_value_set_idx = 0; d_value_set_idx != S_N_D_VALUE_SETS; ++d_value_set_idx)
  {
    invoke_dynamic_change_listeners(d_value_set_idx, true); // true => initial.
  }

  return true;
} // Config_manager::apply_static_and_dynamic_impl()

template<typename... S_d_value_set>
template<typename... Value_set>
bool Config_manager<S_d_value_set...>::apply_dynamic
       (const fs::path& dynamic_cfg_path,
        const typename Final_validator_func<Value_set>::Type&... final_validator_func,
        bool commit)
{
  return apply_dynamic_impl<Value_set...>(false, dynamic_cfg_path, final_validator_func..., commit);
}

template<typename... S_d_value_set>
template<typename... Value_set>
bool Config_manager<S_d_value_set...>::apply_dynamic
       (allow_invalid_defaults_tag_t,
        const fs::path& dynamic_cfg_path,
        const typename Final_validator_func<Value_set>::Type&... final_validator_func,
        bool commit)
{
  return apply_dynamic_impl<Value_set...>(true, dynamic_cfg_path, final_validator_func..., commit);
}

template<typename... S_d_value_set>
template<typename... Value_set>
bool Config_manager<S_d_value_set...>::apply_dynamic_impl
       (bool allow_invalid_defaults,
        const fs::path& dynamic_cfg_path,
        const typename Final_validator_func<Value_set>::Type&... final_validator_func,
        bool commit)
{
  using util::Lock_guard;
  using util::String_ostream;
  using boost::array;
  using std::flush;

  if (m_dynamic_values_set)
  {
    FLOW_LOG_INFO("Config_manager [" << *this << "]: "
                  "Request to apply dynamic config file [" << dynamic_cfg_path << "].  "
                  "Initializing dynamic config? = [false].  "
                  "This application (if successful) shall end with canonicalizing values? = [" << commit << "]; "
                  "multi-source update already in progress "
                  "(0=none) = [" << int(m_multi_src_update_in_progress) << "].");
  }
  else
  {
    FLOW_LOG_INFO("Config_manager [" << *this << "]: "
                  "Request to apply dynamic config file [" << dynamic_cfg_path << "].  "
                  "Initializing dynamic config? = [true].  "
                  "Invalid defaults allowed if explicitly set in file? = [" << allow_invalid_defaults << "].  "
                  "This application (if successful) shall end with canonicalizing values? = [" << commit << "]; "
                  "multi-source update already in progress "
                  "(0=none) = [" << int(m_multi_src_update_in_progress) << "].");
  }

  reject_candidates_if_update_type_changed(Update_type::S_DYNAMIC);

  /* (See class doc header Implementation section for explanation of fold-expression (..., ) trick.)
   *
   * Note, though, that (... && expr) (of, say, length 4) evaluates to (((expr1 && expr2) && expr3) && expr4);
   * which means it'll run through the `expr...i...`s until one evaluates to false; then it stops and evaluates
   * no more past that. */

  size_t value_set_idx;
  bool success = true;

  /* Optionally scan the defaults themselves for individual-option-validator validity if so requested and applicable.
   * However no need to repeat this, if a multi-source update was already in progress. */
  if ((!m_dynamic_values_set) && (!allow_invalid_defaults) && (m_multi_src_update_in_progress == Update_type::S_NONE))
  {
    value_set_idx = 1; // 1, 3, 5, ....
    [[maybe_unused]] const auto dummy = // Defeats false gcc warning due to trailing `success` when N_D_VALUE_SETS is 1.
    (
      ...
      &&
      (
        opt_set<Value_set>(value_set_idx) // Param pack expansion kernel here.
          ->validate_values(&success), // Set `success`...
        value_set_idx += 2, // (Skip static ones.)
        success // ...and if it's false, the subsequent option sets shall be skipped.
      )
    );

    if (!success) // It would have logged the exact problem, but we still want to clearly say what happened, high-level:
    {
      FLOW_LOG_WARNING("Config_manager [" << *this << "]: "
                       "Request to initially-apply dynamic config file [" << dynamic_cfg_path << "]:  "
                       "Will not parse file, because stringent check "
                       "of defaults detected at least 1 individually-invalid default value.  Either correct "
                       "the default or use apply_dynamic(allow_invalid_defaults_tag_t) overload.");
    }
  } // if ((!m_dynamic_values_set) && (!allow_invalid_defaults) && (m_multi_src_update_in_progress == NONE))
  /* else if (m_multi_src_update_in_progress != NONE) { It has been checked already; don't repeat. }
   * else if ((!m_dynamic_values_set) && allow_invalid_defaults) { They don't want default check: don't check it. }
   * else if (m_dynamic_values_set) { As advertised do not check defaults except possibly before first load. } */

  /* Do the actual parsing of each dynamic Option_set.  Recall that due to forward/backward-compatibility concerns
   * we allow any unknown option names, hence the empty set for all_opt_names. */
  success = success
              && apply_static_or_dynamic_impl<Value_set...>(true,
                                                            dynamic_cfg_path, {}, final_validator_func...);

  if ((!commit) && success)
  {
    m_multi_src_update_in_progress = Update_type::S_DYNAMIC; // (Was either NONE or already this.)
    FLOW_LOG_INFO("Config_manager [" << *this << "]: Multi-source update has successfully begun or continued.  "
                  "Expecting more sources before we can canonicalize candidate(s).");
    return true;
  }
  // else if (commit || (!success))

  /* !success => reject, regardless of commit (a failure aborts a multi-source update too).
   * commit => accept or reject depending on `success`.
   *
   * So next either canonicalize or reject each parse attempt depending on overall success (dynamic only). */
  size_t d_value_set_idx = 0;

  /* Zero it (`{}`) to avoid warnings from undefined behavior sanitizer; it’s not a perf-critical path.
   * (In reality the zeroing is not necessary, but we've run into warnings by some tools; might as well avoid.) */
  array<bool, S_N_D_VALUE_SETS> changed{};
  bool some_changed = false;
  (
    ...,
    (
      value_set_idx = (d_value_set_idx * 2) + 1, // 1, 3, 5, ....
      /* Note: If parsing stopped before `value_set_idx` due to failure, rejecting Option_set value_set_idx is a
       * harmless no-op by Option_set docs. */
      option_set_canonicalize_or_reject(opt_set<Value_set>(value_set_idx), // Param pack expansion kernel here.
                                        success,
                                        // If initial dynamic parse, then `changed[] = true` by definition.
                                        m_dynamic_values_set ? &changed[d_value_set_idx] : 0),
      // If initial dynamic parse, then `changed[] = true` by definition.
      changed[d_value_set_idx] = (!m_dynamic_values_set) || changed[d_value_set_idx],
      // some_changed = true iff: changed[x] == true for at least one x.
      some_changed = some_changed || changed[d_value_set_idx],
      ++d_value_set_idx
    )
  );

  if (m_multi_src_update_in_progress != Update_type::S_NONE)
  {
    FLOW_LOG_INFO("Config_manager [" << *this << "]: Multi-source update is now finished.");
    m_multi_src_update_in_progress = Update_type::S_NONE;
  }
  // else { It was NONE and remains so, either due to failure or being a vanilla (single-source) update or both. }

  if (!success)
  {
    return false;
  }
  // else

  if (!some_changed) // Note, again, that if initial dynamic parse then some_changed==true.
  {
    return true; // Meaning, no changes -- but success.
  }
  // else

  // The following is almost exactly log_state() but for dynamic sets only.
  String_ostream os;
  os.os() << "Config_manager [" << *this << "]: (Dynamic) state that was just made canonical follows.\n";
  value_set_idx = 1;
  (
    ...,
    (
      // 1, 3, 5, ....
      state_to_ostream_impl<Value_set>(value_set_idx, os.os()), // Param pack expansion kernel here.
      value_set_idx += 2
    )
  );
  os.os() << flush;
  FLOW_LOG_INFO(os.str());

  /* Next: Eureka!  Set the dynamic-values pointer for each option set that did change.
   *
   * There are some funky-looking logic-op/comma-expr constructions in there.  I (ygoldfel) feel they're understandable
   * enough, and the conciseness is worth it, but arguably it could instead be done with regular `if ()`s instead
   * in a helper function -- though less concise (possible @todo).  BTW generic lambdas could help there, but it would
   * require C++20 (I haven't looked into it thoroughly though). */
  {
    Lock_guard<decltype(m_d_value_sets_mutex)> lock(m_d_value_sets_mutex);
    d_value_set_idx = 0;
    (
      ...,
      (
        value_set_idx = (d_value_set_idx * 2) + 1, // 1, 3, 5, ....
        // Just skip unchanged ones.  (This is `if (x) { f(...); }` in expression form.)
        changed[d_value_set_idx]
          && (save_dynamic_value_set_locked<Value_set> // Param pack expansion kernel here.
                (opt_set<Value_set>(value_set_idx), // (And here.)
                 d_value_set_idx),
              true),
        ++d_value_set_idx
      )
    );
  } // Lock_guard lock(m_d_value_sets_mutex);

  const bool was_dynamic_values_set = m_dynamic_values_set;
  m_dynamic_values_set = true; // Might already be true (if not initial parse).

  /* Finally invoke dynamic change (or "change," if initial parse) listeners (possibly for the first time).
   * (.null() => no listeners registered.) */
  for (d_value_set_idx = 0; d_value_set_idx != S_N_D_VALUE_SETS; ++d_value_set_idx)
  {
    if (changed[d_value_set_idx])
    {
      invoke_dynamic_change_listeners(d_value_set_idx, !was_dynamic_values_set);
    }
  }

  return true;
} // Config_manager::apply_dynamic()

template<typename... S_d_value_set>
void Config_manager<S_d_value_set...>::reject_candidates()
{
  if (m_multi_src_update_in_progress == Update_type::S_NONE)
  {
    FLOW_LOG_INFO("Config_manager [" << *this << "]: Request to abandon multi-source update in-progress; but none "
                  "is in progress.  Doing nothing.");
    return; // Nothing to cancel.
  }
  // else

  FLOW_LOG_INFO("Config_manager [" << *this << "]: Request to abandon multi-source update in-progress.  Doing so.");

  // (See class doc header Implementation section for explanation of fold-expression (..., ) trick.)
  size_t value_set_idx = 0;
  (
    ...,
    (
      /* This will be silent if it is a no-op for a given Value_set; namely if it's a Null_value_set, and/or
       * it pertains to a static Value_set after an apply_dynamic() or dynamic after an apply_static(). */
      option_set_canonicalize_or_reject
        (opt_set<S_d_value_set>(value_set_idx), // Param pack expansion kernel here.
         false, 0), // false <= reject.
      ++value_set_idx
    )
  );

  m_multi_src_update_in_progress = Update_type::S_NONE;
} // Config_manager::reject_candidates()

template<typename... S_d_value_set>
void Config_manager<S_d_value_set...>::reject_candidates_if_update_type_changed(Update_type this_update_type)
{
  assert(this_update_type != Update_type::S_NONE);
  if ((m_multi_src_update_in_progress == Update_type::S_NONE) // Nothing to cancel; all is cool.
      || (m_multi_src_update_in_progress == this_update_type)) // It's a multi-source update; all is cool.
  {
    return; // Nothing to cancel.
  }
  // else if (m_multi_src_update_in_progress is truthy but != this_update_type)

  FLOW_LOG_INFO("Config_manager [" << *this << "]: An update of type [" << int(this_update_type) << "] "
                "is interrupting (canceling) an in-progress multi-source update of type "
                "[" << int(m_multi_src_update_in_progress) << "].  Reverting any uncommitted values "
                "from the latter.  Suggest invoking reject_candidates() explicitly if intentional; this "
                "message will then go away, while the code will be more deliberate.");

  reject_candidates();
  assert(m_multi_src_update_in_progress == Update_type::S_NONE);
} // Config_manager::reject_candidates_if_update_type_changed()

template<typename... S_d_value_set>
template<typename Value_set>
void Config_manager<S_d_value_set...>::save_dynamic_value_set_locked
       (Option_set<Value_set>* opt_set_ptr, size_t d_value_set_idx)
{
  assert(opt_set_ptr);
  auto& opt_set = *opt_set_ptr;

  /* Make a copy of the canonical dynamic Value_set, wrapped in ptr-to-mutable; upgrade to ptr-to-immutable.
   * @todo It's not-great this is inside the locked section.  There's no great alternative -- maybe make all these
   * copies before the locked section, which can be done; it's just rather hairy due to the param-pack expanding. */
  *(static_cast<typename Value_set::Const_ptr*>(m_d_value_sets[d_value_set_idx].get()))
    // Attn: --^ void* -> T* cast; no RTTI to check our type safety.
    = Value_set::const_ptr_cast(opt_set.mutable_values_copy());
}

template<typename... S_d_value_set>
void Config_manager<S_d_value_set...>::invoke_dynamic_change_listeners(size_t d_value_set_idx, bool init) const
{
  FLOW_LOG_INFO("Config_manager [" << *this << "]: "
                "Dynamic update for dynamic option set [" << d_value_set_idx << "] (0-based) of "
                "[" << S_N_D_VALUE_SETS << "] (1-based); trigger was "
                "[" << (init ? "initial-update" : "change-detected") << "].");

  const auto& on_dynamic_change_funcs = m_on_dynamic_change_funcs[d_value_set_idx];

  size_t idx = 0;
  for (const auto& on_dynamic_change_func : on_dynamic_change_funcs)
  {
    FLOW_LOG_INFO("Invoking listener [" << idx << "] (0-based) of [" << on_dynamic_change_funcs.size() << "] "
                  "(1-based).");
    on_dynamic_change_func();
    ++idx;
  }
  FLOW_LOG_INFO("Done invoking change listeners for dynamic option set.");
} // Config_manager::invoke_dynamic_change_listeners()

template<typename... S_d_value_set>
template<typename... Value_set>
void Config_manager<S_d_value_set...>::all_static_values(const Value_set**... value_set_or_null) const
{
  static_assert(sizeof...(Value_set) == S_N_S_VALUE_SETS,
                "You must supply N/2 `value_set_or_null`s, where N is # of S_d_value_set params.  "
                  "Use nullptr for any Null_value_set and/or Value_set of no interest.");

  size_t s_value_set_idx = 0;
  size_t value_set_idx;
  (
    ...,
    (
      value_set_idx = s_value_set_idx * 2,
      // I.e., if (x && y) { *x = ... }.
      (value_set_or_null && (!opt_set<Value_set> // Param pack expansion kernel here (x2).
                                (value_set_idx)->null()))
        && (*value_set_or_null = &(opt_set<Value_set>(value_set_idx)->values()), // (And here, x2.)
            true),
      ++s_value_set_idx
    )
  );
} // Config_manager::all_static_values()

template<typename... S_d_value_set>
template<typename... Value_set>
void Config_manager<S_d_value_set...>::all_static_values_candidates(const Value_set**... value_set_or_null) const
{
  static_assert(sizeof...(Value_set) == S_N_S_VALUE_SETS,
                "You must supply N/2 `value_set_or_null`s, where N is # of S_d_value_set params.  "
                  "Use nullptr for any Null_value_set and/or Value_set of no interest.");

  size_t s_value_set_idx = 0;
  size_t value_set_idx;
  (
    ...,
    (
      value_set_idx = s_value_set_idx * 2,
      (value_set_or_null && (!opt_set<Value_set> // Param pack expansion kernel here (x2).
                                (value_set_idx)->null()))
        /* Emitted pointer will be equivalent to what is provided by all_static_values() unless a
         * parsed-but-not-canonicalized value set is available (i.e. this is being called from a validator function
         * being executed during parsing).  This is achieved here because Option_set<>::values_candidate() will return
         * the candidate when in PARSING state, but nullptr when in CANONICAL state (in which case the canonical values
         * provided by Option_set<>::values() is then used, here, instead). */
        && (*value_set_or_null = opt_set<Value_set>(value_set_idx)->values_candidate() // (And here x2.)
                                   // (?: with omitted middle operand is gcc extension).
                                   ?: &(opt_set<Value_set>(value_set_idx)->values()), // (And here.)
            true),
      ++s_value_set_idx
    )
  );
} // Config_manager::all_static_values_candidates()

template<typename... S_d_value_set>
template<typename Value_set>
const Value_set& Config_manager<S_d_value_set...>::static_values(size_t s_value_set_idx) const
{
  assert((s_value_set_idx < S_N_S_VALUE_SETS) && "Invalid static option set index.");

  return opt_set<Value_set>(s_value_set_idx * 2)->values();
} // Config_manager::static_values()

template<typename... S_d_value_set>
template<typename Value_set>
const Value_set& Config_manager<S_d_value_set...>::static_values_candidate(size_t s_value_set_idx) const
{
  assert((s_value_set_idx < S_N_S_VALUE_SETS) && "Invalid static option set index.");

  const auto opt = opt_set<Value_set>(s_value_set_idx * 2);

  /* Return value will be equivalent to what is provided by static_values() unless a parsed-but-not-canonicalized value
   * set is available (i.e. this is being called from a validator function being executed during parsing).  This is
   * achieved here because ->values_candidate() will return the candidate when in PARSING state, but nullptr when in
   * CANONICAL state (in which case the canonical values provided by ->values() is used, instead). */
  const Value_set* values = opt->values_candidate();
  if (!values)
  {
    values = &(opt->values());
  }

  return *values;
} // Config_manager::static_values_candidate()

template<typename... S_d_value_set>
template<typename... Value_set>
void Config_manager<S_d_value_set...>::all_dynamic_values
       (typename Value_set::Const_ptr*... value_set_or_null) const
{
  using util::Lock_guard;

  static_assert(sizeof...(Value_set) == S_N_D_VALUE_SETS,
                "You must supply N/2 `value_set_or_null`s, where N is # of S_d_value_set params.  "
                  "Use nullptr for any Null_value_set and/or Value_set of no interest.");

  // As promised just copy out the values atomically and return them.  Next time it might return different ones.
  Lock_guard<decltype(m_d_value_sets_mutex)> lock(m_d_value_sets_mutex);

  size_t d_value_set_idx = 0;
  size_t value_set_idx;
  (
    ...,
    (
      value_set_idx = (d_value_set_idx * 2) + 1,
      // I.e., if (x && y) { *x = ... }.
      (value_set_or_null && (!opt_set<Value_set> // Param pack expansion kernel here (x2).
                                (value_set_idx)->null()))
        && (*value_set_or_null // (And here.)
              = *(static_cast<typename Value_set::Const_ptr*>(m_d_value_sets[d_value_set_idx].get())), // (And here.)
              // Attn: --^ void* -> T* cast; no RTTI to check our type safety.
            true),
      ++d_value_set_idx
    )
  );
} // Config_manager::all_dynamic_values()

template<typename... S_d_value_set>
template<typename Value_set>
typename Value_set::Const_ptr
  Config_manager<S_d_value_set...>::dynamic_values(size_t d_value_set_idx) const
{
  using util::Lock_guard;

  assert((d_value_set_idx < S_N_D_VALUE_SETS) && "Invalid dynamic option set index.");

  // As promised just copy out the value atomically and return it.  Next time it might return different pointer.
  Lock_guard<decltype(m_d_value_sets_mutex)> lock(m_d_value_sets_mutex);
  return *(static_cast<typename Value_set::Const_ptr*>(m_d_value_sets[d_value_set_idx].get()));
  // Attn: --^ void* -> T* cast; no RTTI to check our type safety.
}

template<typename... S_d_value_set>
void Config_manager<S_d_value_set...>::state_to_ostream(std::ostream& os) const
{
  os << "Config_manager [" << *this << "]: Current state + config documentation:\n";

  size_t value_set_idx = 0;
  (
    ...,
    state_to_ostream_impl<S_d_value_set>(value_set_idx++, os) // Param pack expansion kernel here.
  );
}

template<typename... S_d_value_set>
template<typename Value_set>
void Config_manager<S_d_value_set...>::state_to_ostream_impl(size_t value_set_idx, std::ostream& os) const
{
  const auto& opts = *(opt_set<Value_set>(value_set_idx));
  if (opts.null())
  {
    return; // Print nothing at all.
  }
  // else

  const size_t s_d_value_set_idx = value_set_idx / 2;
  const bool dyn_else_st = (value_set_idx % 2) == 1;

  os << "-- [" << (dyn_else_st ? "DYNAMIC" : "STATIC") << "] [" << s_d_value_set_idx << "] --\n";

  /* Helper that prints any pending commit=false data that hasn't been upgraded to canonical via
   * apply_*(commit = true). */
  const auto output_uncanonicalized = [&]()
  {
    if (m_multi_src_update_in_progress != Update_type::S_NONE)
    {
      /* The apply_*() sequence in-progress may be apply_dynamic(), while we're static, or vice versa.
       * In that case `Option_set opts` is in PARSING, not CANONICAL state, so there is nothing to print. */
      const auto candidate_or_none = opts.values_candidate();
      if (candidate_or_none)
      {
        os << "  <incomplete multi-source update type [" << int(m_multi_src_update_in_progress) << "] in-progress / "
              "current candidate values follow>\n";
        opts.values_to_ostream(os, candidate_or_none);
      }
    }
  }; // output_uncanonicalized =

  if (!dyn_else_st)
  {
    opts.values_to_ostream(os);
    output_uncanonicalized();
    return;
  }
  // else if (dyn_else_st)

  if (m_dynamic_values_set)
  {
    opts.values_to_ostream(os);
    output_uncanonicalized();
    return;
  }
  // else if (no dynamic values yet loaded)

  /* Print just help (no current values: we have none -- only defaults).
   * Granted the same could be said of static sets too, before apply_static*(), but keeping track of
   * a hypothetical m_static_values_set just to be able to do this same thing here for static options, for
   * something with such a simple usage lifecycle (apply_static*() is done exactly once, probably near the top
   * of the module/application), seems like overkill. */
  os << "<no dynamic config init payload / option docs follow instead>\n";
  opts.help_to_ostream(os);

  output_uncanonicalized();
} // Config_manager::state_to_ostream_impl()

template<typename... S_d_value_set>
void Config_manager<S_d_value_set...>::log_state(log::Sev sev) const
{
  using util::String_ostream;
  using std::flush;

  String_ostream os;
  state_to_ostream(os.os());
  os.os() << flush;
  FLOW_LOG_WITH_CHECKING(sev, os.str());
} // Config_manager::log_state()

template<typename... S_d_value_set>
void Config_manager<S_d_value_set...>::help_to_ostream(std::ostream& os) const
{
  os << "Config_manager [" << *this << "]: Config help:\n";

  size_t value_set_idx = 0;
  (
    ...,
    help_to_ostream_impl<S_d_value_set>(value_set_idx++, os) // Param pack expansion kernel here.
  );
}

template<typename... S_d_value_set>
template<typename Value_set>
void Config_manager<S_d_value_set...>::help_to_ostream_impl(size_t value_set_idx, std::ostream& os) const
{
  const auto opts = opt_set<Value_set>(value_set_idx);
  if (opts->null())
  {
    return; // Print nothing at all.
  }
  // else

  const size_t s_d_value_set_idx = value_set_idx / 2;
  const bool dyn_else_st = (value_set_idx % 2) == 1;

  os << "-- [" << (dyn_else_st ? "DYNAMIC" : "STATIC") << "] [" << s_d_value_set_idx << "] --\n";
  opts->help_to_ostream(os);
} // Config_manager::help_to_ostream_impl()

template<typename... S_d_value_set>
void Config_manager<S_d_value_set...>::log_help(log::Sev sev) const
{
  using util::String_ostream;
  using std::flush;

  String_ostream os;
  help_to_ostream(os.os());
  os.os() << flush;
  FLOW_LOG_WITH_CHECKING(sev, os.str());
} // Config_manager::log_help()

template<typename Value_set>
typename Final_validator_func<Value_set>::Type Final_validator_func<Value_set>::null() // Static.
{
  return [](const Value_set&) -> Final_validator_outcome { return Final_validator_outcome::S_ACCEPT; };
}

template<typename... S_d_value_set>
std::ostream& operator<<(std::ostream& os, const Config_manager<S_d_value_set...>& val)
{
  return os << '[' << val.m_nickname << "]@" << &val;
}

} // namespace flow::cfg
