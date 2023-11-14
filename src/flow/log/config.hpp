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

#include "flow/log/log.hpp"
#include <boost/unordered_map.hpp>
#include <atomic>
#include <typeinfo>
#include <utility>

namespace flow::log
{

// Types.

/**
 * Class used to configure the filtering and logging behavior of `Logger`s; its use in your custom `Logger`s is
 * optional but encouraged; supports dynamically changing filter settings even while concurrent logging occurs.
 *
 * If you are reading this to know how to configure an existing Logger, then you don't need further background; just
 * see this API to know how to configure such `Logger`s in a uniform way.
 *
 * If you are, instead, reading this when implementing a new custom Logger, then please see implementation
 * recommendations in Logger doc header before continuing here.
 *
 * @todo Class Config doc header is wordy and might be hard to follow; rewrite for clarity/flow/size.
 *
 * ### Synopsis: How do I use it to configure a Logger?  Just tell me! ###
 * Really it's pretty easy to use it, but it's much easier to see how it's done by example rather than a
 * formal-ish description (which is nevertheless below, in the next section of this doc header).
 * To use Config with a `Config`-supporting Logger in your flow::log-using module M:
 *
 *   - Declare the component `enum` inside the module M.  Declare and define the associated index-to-name map for
 *     that `enum`.  While it is possible to manually do this, we provide some tools to utterly minimize the
 *     boiler-plate required (which would be considerable otherwise).  The files `config_enum_{start|end}.macros.[hc]pp`
 *     are those tools.
 *     - Start with config_enum_start_hdr.macros.hpp.
 *   - In module M, use this new component `enum` (totally separate from any others, such as Flow's
 *     flow::Flow_log_component) at log call sites.  More precisely, either pass a value to Log_context ctor or
 *     to FLOW_LOG_SET_CONTEXT().
 *     - An example is the pervasive logging done by various Flow sub-modules with the exception of flow::log itself
 *       (which ironically doesn't do all that much logging, chicken/egg).  Look for Log_context and
 *       FLOW_LOG_SET_CONTEXT() mentions.
 *   - Outside module M, in the program which uses module M -- perhaps along with other modules such as Flow itself --
 *     probably around startup, set up your new `Config`.
 *     Call Config::init_component_to_union_idx_mapping() and Config::init_component_names() to register M's
 *     `enum` -- AND again for every other logging module besides M (such as Flow itself).  Call
 *     Config::configure_default_verbosity() and Config::configure_component_verbosity() (and friend) to configure
 *     verbosity levels.  Finally, create the specific Logger and give it a pointer to this Config.
 *     - Examples of this can almost certainly be found in certain test programs outside of Flow code proper.
 *       Again, setting Config and specific `Logger` object's config shouldn't be done in libraries/modules but normally
 *       at the program level, such as around `main()`.
 *     - After this, the logging itself can begin (including from multiple threads).  Concurrently with logging --
 *       which will often call output_whether_should_log() and potentially output_component_to_ostream() -- you
 *       may not safely call any of the above `init_*()` or `configure_*()` methods, with the following important
 *       exceptions:
 *       - You may call Config::configure_default_verbosity() safely concurrently with logging to dynamically change
 *         verbosity setting.
 *       - You may call Config::configure_component_verbosity() or Config::configure_component_verbosity_by_name()
 *         similarly.
 *
 * ### What Config controls and how ###
 * Let's get into it more formally.
 *
 * Firstly, Config simply stores simple scalars controlling output behavior.  For example, the public
 * member #m_use_human_friendly_time_stamps controls the style of time stamps in the final output.  It's just
 * a data store for such things.
 *
 * Secondly, Config knows how to *understand* the *component* values supplied at every single log call site in
 * your program.  (This also ties in to the next thing we discuss, verbosity config.)  See Component doc header.
 * Now, here's how Config understands components.  In your program, you will use various `flow::log`-using libraries
 * or modules -- including (but not necessarily limited to!) Flow itself.  Each module is likely to feature their own
 * component table, in the form of an `enum class`.  For example, Flow itself has `enum class Flow_log_component`
 * (see common.hpp).
 *   - So, suppose there are two modules, M1 and M2 (e.g., M1 might be Flow itself).  Each will define
 *     a component table `enum class`, C1 and C2, respectively.  You are to *register* C1; and separately/similarly C2;
 *     using an init_component_to_union_idx_mapping() for each of C1 and C2.  (All of this applies for any number
 *     of modules and enums, not just two.)
 *   - The Config maintains a single, merged *component union* table, which will (after registration) map
 *     *every* `C1::` enumeration member AND *every* `C2::` member to a distinct *component union index* integer.
 *     (For example, in the init_component_to_union_idx_mapping() call, you can specify that C1's members will map
 *     to union indices 1000, 1001, ...; and C2's to 2000, 2001, ....)  These flat union indices between C1 and C2
 *     must never clash.
 *   - The Config also maintains a single, distinct *name* string, for each component.  An optional feature is provided
 *     to auto-prepend a prefix configured for each registered `enum class` (e.g., "C1_" for all `C1::` members,
 *     "C2_" for all `C2::` members).  One provides (for each of C1, C2) this optional prefix and a simple map from
 *     the `enum` members to their distinct string names.  Use init_component_names().
 *   - After registering each of C1 and C2 with init_component_to_union_idx_mapping() and init_component_names(),
 *     in your *program* (e.g., in `main()`) but outside the modules M1 and M2 *themselves*, the Config now has:
 *     - The ability to map an incoming log::Component (as supplied to Logger::do_log()) to its index in the flat
 *       `Config`-internal union of all component `enum` values.
 *     - Having obtained that flat index, quickly obtain its distinct component name.
 *     - Conversely, suppose one passes in a distinct component name.  Config can now quickly map that string to
 *       its flat index in the union table of all component `enum` values.
 *   - The following two features are built on this mapping of multiple individual component tables onto a single
 *     flat "union" table of components built inside the Config; as well as on the aforementioned mapping of
 *     union table index to component name string; and vice versa.
 *
 * Thirdly, and crucially, the verbosity filtering (Logger::should_log()) for the client Logger is entirely implemented
 * via the output_whether_should_log() output method; so Logger::should_log() can simply forward to that method.  Here
 * is how one configures its behavior in Config.  At construction, or in a subsequent configure_default_verbosity()
 * call, one sets the *default verbosity*, meaning the most-verbose log::Sev that `should_log()` would let through
 * when no per-component verbosity for the log call site's specified Component is configured.  In addition, assuming
 * more fine-grained (per-component) verbosity config is desired, one can call `configure_component_verbosity*()`
 * to set the most-verbose log::Sev for when `should_log()` is passed that specific Component payload.
 *
 * Setting per-component verbosity can be done by its `enum` value.  Or one can use the overload that takes the
 * component's distinct (among *all* source component `enum` tables registered before) string name; in which case
 * the aforementioned performant name-to-index mapping is used internally to set the proper union component's verbosity.
 *
 * (Note that order of calls matters: configure_default_verbosity() wipes out effects of any individual, per-component
 * `configure_component_verbosity*()` executed prior to it.  It is typical (in a given round of applying config) to
 * first call configure_default_verbosity() and then make 0 or more `configure_component_verbosity*()` calls for various
 * individual components.)
 *
 * Fourthly, output_component_to_ostream() is a significant helper for your Logger::do_log() when actually printing
 * the ultimate character representation of the user's message and metadata.  The metadata (Msg_metadata) includes
 * a Component; that method will output its string representation to the `ostream` given to it.  To do so it will
 * use the aforementioned ability to quickly map the `C1::` or `C2::` member to the flat union index to that index's
 * distinct name (the same name optionally used to configure verbosity of that component, as explained above).
 *
 * Or one can opt to print the flat numeric index of the component instead; in which case the reverse name-to-union-idx
 * is not needed.
 *
 * ### Thread safety ###
 * Formally, Config is thread-safe for all operations when concurrent access is to separate `Config`s.
 * There are no `static` data involved.  Formally, Config is generally NOT thread-safe when concurrent read and write
 * access is w/r/t to a single Config; this includes read/write of any public data members and read/write in the form
 * `const`/otherwise method calls.  Informally, one could use an outside mutex, including in any Logger
 * implementation that uses `*this`, but we recommend against this for performance reasons; and see below "exception."
 *
 * Also formally for a given `*this`: The logging phase is assumed to begin after all `init_*()` calls and any
 * initial `configure_*()` calls; at this point output_whether_should_log() and output_component_to_ostream() may be
 * used at will by any thread; but the pre-logging-phase non-`const` calls are no longer allowed.
 *
 * There is an important exception to the assertion that Config `*this` one must NOT call any write methods once
 * the logging phase has begun.  Informally, this exception should make it possible to use Config safely *and yet*
 * dynamically allow changes to Config without any expensive outside mutex.  The exception is as follows:
 *
 * Assume, as is proper, that you've called all needed init_component_to_union_idx_mapping() and init_component_names()
 * before any concurrent logging and have now started to log -- you are in the logging phase.
 * Now assume you want to change verbosity settings during this logging-at-large phase;
 * this is common, for example, when some dynamic config file changes verbosity settings
 * for your program.  The following is safe: You may call configure_default_verbosity() and/or
 * `configure_component_verbosity*()` while expecting the changes to take effect promptly in all threads; namely,
 * output_whether_should_log() will reflect the change there; and output_component_to_ostream() does not care.
 * Perf-wise, little to nothing is sacrified (internally, a lock-free implementation is used).
 *
 * Corner case: It is also safe to call configure_default_verbosity() and/or `configure_component_verbosity*()`
 * concurrently with themselves.  Naturally, it is a race as to which thread "wins."  Moreover,
 * configure_default_verbosity() with `reset == true` is equivalent to removing the verbosity setting for each
 * individual component; but only each removal itself is atomic, not the overall batch operation; so concurrent
 * execution with another `configure_*_verbosity()` call may result in an interleaved (but valid) verbosity table.
 * Informally, we recommend against any design that would allow concurrent `configure_*()` calls on `*this`; it does
 * not seem wise.  It does however result in well-defined behavior as described.  The real aim, though, is not this
 * corner case but only the main case of a series of `configure_*()` calls in thread 1, while logging may be going on
 * in other threads.
 *
 * ### Optional: Thread-local verbosity override ###
 * In a pinch, it may be desirable -- *temporarily* and *in a given thread of execution only* -- to change the
 * current verbosity config.  To do so in a given scope `{}` simply do this:
 *
 *   ~~~
 *   {
 *     // In this { scope } temporarily let-through only error messages or more severe.
 *     const auto overrider = flow::log::Config::this_thread_verbosity_override_auto(flow::log::Sev::S_WARNING);
 *
 *     // Now let's create 3,000 threads each of which would normally log a few "starting thread" startup INFO messages!
 *     m_thread_pool = std::make_unique<flow::async::Cross_thread_task_loop>(get_logger(), "huge_pool", 3000);
 *     m_thread_pool->start();
 *   } // Previous thread-local verbosity is restored (whether there was one, or none) upon exit from {block}.
 *   ~~~
 * You may also query the current setting via `*(this_thread_verbosity_override())`.  Direct assignment of a #Sev
 * to the latter is allowed, but generally it is both safer and easier to use the RAII
 * pattern via this_thread_verbosity_override_auto() for setting/restoring the override.
 *
 * The value Sev::S_END_SENTINEL indicates the thread-local verbosity override is disabled; and is the initial (default)
 * state in any thread.  However, generally, it is recommended to use the RAII pattern via
 * this_thread_verbosity_override_auto() instead of direct assignment, for safe and easy save/restore.
 *
 * Note there are no thread-safety concerns with this feature, as it is entirely thread-local.
 */
class Config
{
public:
  // Types.

  /**
   * Unsigned index into the flat union of component tables maintained by a Config, combining potentially multiple
   * user component `enum` tables.  Also suitable for non-negative offsets against such indices.
   */
  using component_union_idx_t = Component::enum_raw_t;

  /**
   * Short-hand for a function that takes a Component (storing a payload of some generic component `enum` member of
   * the logging user's choice) and returns its corresponding flat union component index.
   */
  using Component_to_union_idx_func = Function<component_union_idx_t (const Component&)>;

  // Constants.

  /// Recommended default/catch-all most-verbose-severity value if no specific config is given.
  static const Sev S_MOST_VERBOSE_SEV_DEFAULT;

  // Constructors/destructor.

  /**
   * Constructs a conceptually blank but functional set of Config.  Namely, no component `enum`s are yet registered
   * (call init_component_to_union_idx_mapping() and init_component_names() to register 0 or more such `enum` tables).
   * The default verbosity is set to `most_verbose_sev_default`.
   *
   * While you can and should register `enum`s after this, if you don't then the object's outputs will act as follows:
   *   - output_whether_should_log() will simply return `true` if and only if `sev` is no more verbose than
   *     `most_verbose_sev_default` arg to this ctor.  `component` is ignored in the decision.
   *   - output_component_to_ostream() will print nothing to the `ostream` arg and hence will return `false`.
   *
   * Note that -- particularly in a pinch and in simple applications -- this is perfectly reasonable, simple behavior.
   * One doesn't always need per-component verbosity configuration abilities; and one definitely doesn't always need
   * to print the component name/index in the log output.  But if one does need these things, then you can
   * register `enum`s as explained in class doc header.
   *
   * @param most_verbose_sev_default
   *        Same as in configure_default_verbosity().
   */
  explicit Config(Sev most_verbose_sev_default = S_MOST_VERBOSE_SEV_DEFAULT);

  /**
   * Copy-constructs `*this` to be equal to `src` config object.
   *
   * Performance-wise, this will copy internal per-component
   * tables (in addition to a few scalars).  These tables are conceptually unions of potentially multiple long
   * `enum`s; so this probably shouldn't be done often, but typically Config is
   * constructed at startup or during rare config change events.
   *
   * @warning If this copy construction occurs very soon after a configure_default_verbosity() or
   *          `configure_component_verbosity*()` call in a *different* thread completes, then it is possible
   *          that call's effect won't register in the resulting `*this`.  In the case of
   *          configure_default_verbosity() with `reset == true` the clearing of the per-component verbosities may
   *          register only partially in that situation, though `*this` will still be valid.  Since "very soon" cannot
   *          be formally defined, it is therefore best to make such a copy in the same thread as the last
   *          verbosity-modifying call on `src`.  (On the other hand, even otherwise results in valid behavior, but
   *          it may not be quite as deterministic as preferred and clean.)
   *
   * @internal
   * The warning above is due to the subtlety in the Atomic_raw_sev copy ctor doc header.
   * @endinternal
   *
   * @param src
   *        Source object.
   */
  Config(const Config& src);

  /**
   * For now at least there's no reason for move-construction.
   * @todo Reconsider providing a Config move constructor.  I just didn't need to deal with it.
   */
  Config(Config&&) = delete;

  // Methods.

  /**
   * For now at least there's no reason for copy assignment.
   * @todo Reconsider providing a Config copy assignment.  I just didn't need to deal with it.
   */
  void operator=(const Config&) = delete;

  /**
   * For now at least there's no reason for move assignment.
   * @todo Reconsider providing a Config move assignment.  I just didn't need to deal with it.
   */
  void operator=(Config&&) = delete;

  /**
   * A key output of Config, this computes the verbosity-filtering answer to Logger::should_log() based on the
   * given log-call-site severity and component and the verbosity configuration in this Config, including
   * the value at `*(this_thread_verbosity_override())`, the value from configure_default_verbosity(),
   * and the config from `configure_component_verbosity*()`.
   *
   * Call this only after the full round of construction, init_component_to_union_idx_mapping(), and (initial)
   * `configure_..._verbosity()`.  In addition, it is specifically safe to concurrently set verbosity
   * via configure_default_verbosity() and/or `configure_component_verbosity*()`.
   *
   * ### Thread safety on `*this` ###
   * The last sentence means it's possible to change verbosities even while logging (which invokes us), as long
   * as the `init_*()` stuff has all been completed.  For formal details see notes on thread safety in class Config
   * doc header.
   *
   * ### Algorithm for computing return value ###
   * It's a matter of comparing `sev` to `S`, where `S` is the applicable log::Sev verbosity setting.
   * Return `true` if and only if `sev <= S`.
   *
   * What is `S`?  It is the first available value of the following three bits of config:
   *   -# `S = *(this_thread_verbosity_override())`...
   *      - ...unless it's Sev::S_END_SENTINEL (disabled, which is default); then:
   *   -# `S` = the verbosity configured via `configure_component_verbosity*()` for `component`.
   *      - ...unless no such per-component verbosity was set; then:
   *   -# `S` = the value given to configure_component_verbosity() or ctor, whichever happened later.
   *      - This is always available.
   *
   * @see this_thread_verbosity_override() and this_thread_verbosity_override_auto().
   * @see configure_default_verbosity() and Config().
   * @see configure_component_verbosity() and configure_component_verbosity_by_name().
   *
   * @param sev
   *        See Logger::should_log().
   * @param component
   *        See Logger::should_log().
   * @return `true` if we recommend to let the associated message be logged; `false` to suppress it.
   */
  bool output_whether_should_log(Sev sev, const Component& component) const;

  /**
   * An output of Config, this writes a string representation of the given component value to the given `ostream`,
   * if possible.  Returns `true` if it wrote anything, `false` otherwise.
   * Call this only after the full round of construction, init_component_to_union_idx_mapping(), and
   * init_component_names().
   *
   * If the component's type (`component.payload_type()`) has not been properly registered via
   * init_component_to_union_idx_mapping(), it returns `false` and writes nothing.  Otherwise, if no name was registered
   * (either because it wasn't included in a init_component_names() call, or because in that call
   * `output_components_numerically == true`), it will output the component's numeric index in the flat union table;
   * and return `true`.  Finally, if a name is indeed registered, it will output that string (details
   * in init_component_names() doc header) and also return `true`.
   *
   * @param os
   *        Pointer (not null) to the `ostream` to which to possibly write.
   * @param component
   *        The component value from the log call site.  `component.empty()` (no component) is allowed.
   * @return `true` if 1 or more characters have been written to `*os`; else `false`.
   */
  bool output_component_to_ostream(std::ostream* os, const Component& component) const;

  /**
   * Registers a generically-typed `enum class` that represents the full set of the calling module's possible
   * component values that it will supply at subsequent log call sites from that module.  The caller supplies:
   *   - The template argument `Component_payload`, which is an `enum` and is thus castable to the unsigned
   *     integer type `component_union_idx_t`.
   *   - The signed integer that shall be *added* to any log-call-site-supplied `enum` value in order to yield the
   *     flat-union index in `*this` merged table of all component `enum`s.  For example, if we assume that no module
   *     will ever exceed 1,000 components in its `enum`, then module 1 can register its `enum` C1 with
   *     `enum_to_num_offset` 1,000, module 2 with 2,000, module 3 with 3,000, etc.  Then the various C1 `enum` values
   *     0, 1, ... will map to merged 1,000, 1,001, ...; C2's 0, 1, ... to 2,000, 2,001, ...; etc.
   *     - This can be negative, because why not?  Be careful.
   *   - The "sparse size" of the `enum`.  Details are below.
   *
   * @note If this is not acceptable -- maybe you want to pack them more tightly, or you have some other clever mapping
   *       in mind -- then Config might require a new feature (likely an overload of this method) which lets one simply
   *       provide the mapping in function (callback) form.  In fact, in a previous version of Flow, this was provided;
   *       and in fact the present overload merely wrapped that more-general overload.
   *       We removed this primarily for perf reasons: Always using this numeric-offset technique allowed for an inlined
   *       implementation in the very-frequently-called (at every log call site) output_whether_should_log().  It would
   *       be possible to add it back in while *also* optimizing for the expected-to-be-used-typically offset technique,
   *       thus having essentially the best of both worlds (perf when possible, flexibility when necessary).  However,
   *       since the "flexible" API appears unlikely to be needed, we decided it's over-engineering to keep it in --
   *       unless the need does appear in the future.  In that case it should be possible to look in source control
   *       history and bring back its core elements (without removing the inlined offset-technique code path).  At this
   *       stage this is not a formal to-do -- more of a note for posterity.
   *
   * Behavior is undefined if an index collision occurs here or in a subsequent `init_*()` or other relevant call.
   * In particular take care to provide sufficient slack space (e.g., if you use `enum_to_num_offset` which are
   * multiples of 5, then a collision will probably occur at some point).
   *
   * If one has called `init_component_to_union_idx_mapping<T>()` with the same `T` in the past, then behavior
   * is undefined, so don't.  (Informally, depending on whether/how one has called init_component_names() and
   * `configure_component_verbosity*()`, this can actually be done safely and with well-defined results.  However,
   * I did not want to go down that rabbit hole.  If it becomes practically necessary, which I doubt, we can revisit.
   * This is not a formal to-do as of this writing.)
   *
   * @internal
   * ### Rationale for `enum_sparse_length` ###
   * A design is possible (and indeed was used for a very brief period of time) that avoids the need for this arg.
   * Internally #m_verbosities_by_component can be a nice, elegant `unordered_map<component_union_idx_t, Sev>`, in which
   * case we need not know anything about how many `enum` values there can be, and as long as the various `enum`s'
   * numeric values don't clash -- which must and should easily be avoided by the user calling us here -- everything
   * works fine with this non-sparse data structure.  However, to make #m_verbosities_by_component a sparse `vector<>`
   * (that uses `component_union_idx_t` as the key) -- yet never need to `resize()` it when
   * configure_component_verbosity() is called -- one must know `enum_sparse_length` ahead of time to ensure there
   * is enough space in the `vector` ahead of time.  Why would one use such an ugly (and somewhat space-wasting)
   * structure instead, you ask?  Answer: Short version: for efficient thread safety of configure_component_verbosity()
   * and output_whether_should_log().  Long version: See #m_verbosities_by_component doc header.
   * @endinternal
   *
   * @see Component::payload_type() doc header.
   *
   * @tparam Component_payload
   *         See the doc header for the template param `Payload` on Component::payload().
   *         In addition, in our context, it must be convertible to `component_union_idx_t` (an unsigned integer).
   *         Informally, `Component_payload` must be a sane unsigned `enum` with end sentinel `S_END_SENTINEL`.
   *         The various input Component_payload types are distinguished via `typeid(Component_payload)`
   *         and further `type_index(typeid(Component_payload))`.  I provide this implementation detail purely for
   *         general context; it should not be seen as relevant to how one uses the API.
   * @param enum_to_num_offset
   *        For each further-referenced `Component_payload` value C, its flat union index shall be
   *        `component_union_idx_t(C) + enum_to_num_offset`.
   *        So this is the "base" index for the `enum` you are registering, in the final flat table of components.
   * @param enum_sparse_length
   *        Formally, one plus the highest numeric value of a `Component_payload` value that will ever be passed to
   *        configure_component_verbosity() (directly; or indirectly if using configure_component_verbosity_by_name()).
   *        Informally, we recommend that you (a) use the config_enum_start_hdr.macros.hpp mechanism to create
   *        `Component_payload` type in the first place; and (b) therefore use
   *        `standard_component_payload_enum_sparse_length<Component_payload>()` for the present arg's value.
   */
  template<typename Component_payload>
  void init_component_to_union_idx_mapping(component_union_idx_t enum_to_num_offset,
                                           size_t enum_sparse_length);

  /**
   * Registers the string names of each member of the `enum class Component_payload` earlier registered via
   * `init_component_to_union_idx_mapping<Component_payload>()`.  These are used subsequently (as of this writing)
   * to (1) map name to index in one of the `configure_component_verbosity*()` methods; and
   * (2) to map index to name in output_component_to_ostream().
   *
   * Behavior undefined if `init_component_to_union_idx_mapping<Component_payload>()` hasn't yet been called.
   * Behavior undefined if `init_component_names<Component_payload>()` has already been called.  (Informally, something
   * safe might happen, depending, but in general it's a weird/bad idea, so don't.)  Behavior undefined if
   * any value in `component_names` is empty.
   *
   * The recommended (but not mandatory) way to auto-generate a `component_names` map (as normally doing so by hand is
   * tedious) is to use `config_enum_{start|end}_macros.[hc]pp`.  As an example, Flow itself does it in
   * common.hpp and common.cpp, defining both flow::Flow_log_component (the `enum`) and
   * flow::S_FLOW_LOG_COMPONENT_NAME_MAP (the `component_names` map).  Basically, via macro magic it names each
   * component according to the `enum` member identifier's own name.
   *
   * ### `component_names` meaning ###
   * Some subtleties exist in interpreting `component_names`.
   *
   * Firstly, each value (string name) in `component_names` -- as well as `payload_type_prefix_or_empty` -- is
   * internally pre-normalized before any other work.  Name normalization consists of conversion to upper case according
   * to the classic ("C") locale.  output_component_to_ostream() will print in normalized form (if applicable);
   * and configure_component_verbosity_by_name() will normalize the input arg string before lookup.
   *
   * If empty, `payload_type_prefix_or_empty` has no effect.  Otherwise, its effect is as if it *were* empty, but
   * as if `component_names[X]` had `payload_type_prefix_or_empty` prepended to its actual value at all times.
   * Less formally, it's a constant to prefix every name; then if the program (perhaps around `main()`) simply manually
   * provides a distinct, cosmetically useful "namespace-like" prefix in each init_component_names() call, then
   * it can 100% guarantee no name clashes, even if accidentally one of module X's component names A happened to
   * equal an unrelated module Y's component name B.  For example, A = B = "UTIL" is fairly likely to collide otherwise.
   * It won't be an issue, if they end up being called "X_UTIL" and "Y_UTIL" ultimately, by supplying
   * prefixes "X_" and "Y_" X and Y's init_component_names() calls.
   *
   * Within `component_names` if a value (name) is present 2+ times, behavior is undefined.
   * Furthermore if `payload_type_prefix_or_empty + X`, where `X` is in `component_names`, is already
   * stored in `*this`, behavior is undefined.  Either way it's a name collision which should be entirely avoidable
   * using `payload_type_prefix_or_empty` as shown above.
   *
   * It is a multi-map, and key K is allowed to be present 2+ times mapping to 2+ distinct names.
   * The reason this is supported is so one can (discouraged though it is -- but for historical reasons tends to
   * come up at times) declare an `enum` that includes a few mutually "aliased" members:
   *   - `Sweet_components::S_COOL_ENGINE` <=> "COOL_ENGINE" <=> 5
   *   - `Sweet_components::S_ENGINE_ALIAS1` <=> "ENGINE_ALIAS1" <=> 5
   *   - `Sweet_components::S_ENGINE_ALIAS2` <=> "ENGINE_ALIAS2" <=> 5
   * In that example, any of `S_{COOL_ENGINE|ENGINE_ALIAS{1|2}}` maps to the rather long name
   * "COOL_ENGINE,ENGINE_ALIAS1,ENGINE_ALIAS2"; and *each* of "COOL_ENGINE", "ENGINE_ALIAS1",
   * "ENGINE_ALIAS2" maps backwards to a single entry in the component-to-verbosity table.  Hence if I configure
   * verbosity X (using `configure_component_verbosity*()`) for COOL_ENGINE_ALIAS1, then verbosity X config will equally
   * affect subsequent messages with specified component COOL_ENGINE and ENGINE_ALIAS2 as well.
   *
   * Detail: When concatenating component output names as just described, the prefix `payload_type_prefix_or_empty`
   * is prepended only once.  So, if the prefix is "SWEET-", then any one of the above 3 `enum` example members
   * maps to the name "SWEET-COOL_ENGINE,ENGINE_ALIAS1,ENGINE_ALIAS2".
   *
   * `output_components_numerically`, if and only if set to `true`, suppresses the default behavior which is to
   * memorize the string to output (in output_component_to_ostream()) for a given `enum` value;
   * instead it doesn't memorize this forward mapping.  As a result, output_component_to_ostream() will simply output
   * the *numerical* value of the `enum` member from the flat union component table.  This is a cosmetic output choice
   * some prefer to the long-looking component names.
   *
   * In addition, even if `!output_components_numerically`, but a subsequent output_component_to_ostream() call
   * encounters an `enum` value that you neglected to register via init_component_names() (omitting it in
   * `component_names` in particular), then it will also be printed numerically as if `output_components_numerically`.
   *
   * Finally, even if `output_components_numerically == true`, the backwards mapping (from string name to component)
   * is still memorized.  Therefore one can still set configure_component_verbosity_by_name() by string name.
   * Again, in practice, I have seen this: Config files will refer to component verbosities by component name, not
   * unhelpful-looking number; but output log files still print them as numbers for brevity.
   *
   * @param component_names
   *        Mapping of each possible Component_payload value to its string representation, for both output and
   *        per-component config (namely verbosity config) subsequently.  Details above.
   *        Empty names lead to undefined behavior.
   * @param output_components_numerically
   *        If and only if `true`, output_component_to_ostream() will output the flat numeric index for all
   *        `Component_payload`-passing log call sites; else it will print the string name from the map
   *        (but if not *in* the map, then it'll fall back to the flat index again).
   * @param payload_type_prefix_or_empty
   *        Optional prefix helpful as a disambiguating "namespace" to preprend to values in `component_names`.
   *        Details above.
   * */
  template<typename Component_payload>
  void init_component_names(const boost::unordered_multimap<Component_payload, std::string>& component_names,
                            bool output_components_numerically = false,
                            util::String_view payload_type_prefix_or_empty = util::String_view());

  /**
   * Sets the default verbosity to the given value, to be used by subsequent output_whether_should_log() calls whenever
   * one supplies it a component for which no per-component verbosity is configured at that time; optionally wipes out
   * all existing per-component verbosities for a constructor-like reset.
   *
   * This is fairly intuitive; the only aspect one might find non-obvious is `reset == true` mode.
   * In that mode all per-component verbosities are forgotten, as after construction.  An intended use scenario is
   * when reading a hypothetical config file describing new, dynamic *overall* verbosity settings to replace any
   * existing ones.  Such a config file would probably specify the catch-all (default) verbosity; then 0 or more
   * per-component "exception" verbosities.  Hence once would call this method with `reset == true` accordingly to
   * reset everything and set the default; then one would call `configure_component_verbosity*()` for each "exception."
   *
   * `reset == true` is technically slower than otherwise, though it is doubtful one would call us frequently enough
   * for it to matter.  The perf cost of `!reset` is constant time and basically that of a scalar assignment.
   * The perf cost of `reset == true` is that plus the cost of about N configure_component_verbosity() calls, where
   * N is the highest flat-union-component-table implied by the `enum_sparse_length` arg
   * to init_component_to_union_idx_mapping() calls to date.  In practice doing this when
   * outside config changes is unlikely to be a perf issue.
   *
   * ### Thread safety on `*this` ###
   * If called, it must be called after all init_component_to_union_idx_mapping() calls have completed.
   * It is safe to call concurrently with output_whether_should_log(), meaning dynamic config of verbosities is
   * allowed.  See formal details in thread safety notes in class Config doc header.
   *
   * @param most_verbose_sev_default
   *        The most-verbose (numerically highest) `Sev sev` value such that output_whether_should_log() will return
   *        `true`, when `Component component` is either null or has no per-component verbosity configured at that time.
   * @param reset
   *        If `false` then per-component verbosities are left unchanged; else they are wiped out, meaning only the
   *        catch-all setting has subsequent effect in output_whether_should_log().
   */
  void configure_default_verbosity(Sev most_verbose_sev_default, bool reset);

  /**
   * Sets the per-component verbosity for the given component to the given value, to be used by subsequent
   * output_whether_should_log() calls whenever one supplies it the same component value.  See also
   * configure_default_verbosity().
   *
   * This only works (and will return `true`) if `init_component_to_union_idx_mapping<Component_payload>()` has been
   * called.  Otherwise it returns `false` (caller may `assert()` against this result if it is felt justified).
   *
   * See class doc header section "What Config controls and how" for more discussion.
   *
   * @param most_verbose_sev
   *        The most-verbose (numerically highest) `Sev sev` value such that output_whether_should_log() will return
   *        `true`, when `Component component` is not null and has `Component::payload<Component_payload>()`
   *        return a value equal to `component_payload`.
   * @param component_payload
   *        The component for which verbosity is being set.
   * @return `true` on success; `false` otherwise.  See above for more.
   */
  template<typename Component_payload>
  bool configure_component_verbosity(Sev most_verbose_sev, Component_payload component_payload);

  /**
   * Like configure_component_verbosity(), but the component is to be specified by its registered string
   * name, well suited to interpreting text config files.  The meaning of verbosity is the same as in the other
   * overload.
   *
   * This only works (and will return `true`) if init_component_names() has been called in such a way as to
   * successfully associate a component in the flat union table with the name equal (after normalization of both sides)
   * to `component_name`.  If the name is unknown, it returns `false`.
   *
   * Name normalization consists of conversion to upper case according to the classic ("C") locale.
   *
   * See class doc header section "What Config controls and how" for more discussion.
   *
   * @param most_verbose_sev
   *        The most-verbose (numerically highest) `Sev sev` value such that output_whether_should_log() will return
   *        `true`, when `Component component` is not null and has an associated string name equal to `component_name`
   *        (post-normalization of both sides of comparison).
   * @param component_name
   *        The component for which verbosity is being set.
   * @return `true` on success; `false` otherwise.  See above for more.
   */
  bool configure_component_verbosity_by_name(Sev most_verbose_sev,
                                             util::String_view component_name);

  /**
   * Returns highest numeric value in the given component-payload `enum`, plus 1, assuming that
   * `enum` was created using the config_enum_start_hdr.macros.hpp mechanism with all requirements followed by
   * user.  This is useful for most invocations of init_component_to_union_idx_mapping() for its
   * `enum_sparse_length` argument.
   *
   * For example, if one wants to store a `vector` that uses `size_t(X)`, where `X` is a `Component_payload`,
   * as an associative-key-like index, then the `vector` would have to be sized whatever the present method
   * returns to guarantee no out-of-bounds error.
   *
   * @see init_component_to_union_idx_mapping(), particularly the `enum_sparse_length` argument.

   * @tparam Component_payload
   *         An `enum class` type created by the mechanism prescribed in config_enum_start_hdr.macros.hpp, using that
   *         mechanism and with user following the documented requirements therein.  Alternatively (though it's
   *         not recommended) the following is sufficient if one makes the type some other way:
   *         `Component_payload::S_END_SENTINEL` must have the highest numeric value, without ties;
   *         and the backing type is unsigned and with a bit width no higher than that of `size_t`
   *         (Component::enum_raw_t is what the aforementioned standard mechanism uses as of this writing).
   * @return See above.
   */
  template<typename Component_payload>
  static size_t standard_component_payload_enum_sparse_length();

  /**
   * Returns pointer to this thread's *mutable* verbosity override, for querying or assignment alike.
   * The value of this override, at any given time, shall affect output_whether_should_log() return value.
   * See output_whether_should_log().
   *
   * If you would like to query the current setting, use this method.
   *
   * If you would like to *modify* the current setting, it is safer and easier to use
   * this_thread_verbosity_override_auto() which supplies RAII-style auto-restore.
   *
   * For each thread:
   *   - Let `S = *(this_thread_verbosity_override())`.
   *   - Originally `S == Sev::S_END_SENTINEL`.
   *     - output_whether_should_log() in this case will therefore disregard `S`, per its doc header.
   *   - You may set `S` to any valid log::Sev value via direct assignment.
   *     - output_whether_should_log() in this case will follow `S` and only `S`, per its doc header.
   *     - this_thread_verbosity_override_auto() is a convenient way of doing this, as it'll take care of the next
   *       step with minimal effort:
   *   - You may set `S` back to Sev::S_END_SENTINEL.
   *     - output_whether_should_log() will begin disregarding `S` again.
   *
   * @return See above.  Note that, for any given thread, the returned pointer shall always be the same.
   */
  static Sev* this_thread_verbosity_override();

  /**
   * Sets `*(this_thread_verbosity_override()) = most_verbose_sev_or_none`; and returns an object that shall restore
   * it to its current value when it goes out of scope.  See class doc header for an example of use.
   *
   * It is recommended to use this method instead of direct assignment to the location this_thread_verbosity_override(),
   * as then it'll be auto-restored.
   *
   * @see this_thread_verbosity_override() and output_whether_should_log().
   *
   * @param most_verbose_sev_or_none
   *        A value suitable for configure_default_verbosity(); or the special value Sev::S_END_SENTINEL which
   *        disables the override (meaning `C.output_whether_should_log()` shall actually follow the config in `Config
   *        C` and not any override).
   * @return Object that, when it is destroyed, will restore the verbosity override to what it was before this
   *         call.  (The object cannot be copied, to prevent a double-restore.  It can however be moved-from.)
   */
  static util::Scoped_setter<Sev> this_thread_verbosity_override_auto(Sev most_verbose_sev_or_none);

  // Data.  (Public!)

  /**
   * Config setting: If `true`, time stamps will include a (deterministically formatted) date, time, time zone, all in
   * the OS's current time zone; else raw # of seconds passed since POSIX (Unix) Epoch (1970, Jan 1, 00:00, GMT).
   * In both cases time is expressed with microsecond resolution (but the accuracy is only as good as the computer's
   * clock hardware and OS software allow, presumably, though this isn't in the purview of class Config).
   */
  bool m_use_human_friendly_time_stamps;

private:
  // Types.

  /**
   * The set of config stored for each distinct (as determined by Component::payload_type(), essentially
   * C++ built-in `std::type_info`) component payload type (in English -- component `enum class`).
   *
   * As I write this, there is only one member, so we did not need a `struct`, but it *really* felt like
   * it might get extended later -- and there is zero performance loss from this, and the extra code is minimal.
   */
  struct Component_config
  {
    // Data.

    /// See the `enum_to_num_offset` arg in init_component_to_union_idx_mapping() doc header.
    component_union_idx_t m_enum_to_num_offset;
  };

  /// Short-hand for fast-lookup map from distinct `Component_payload` type to the config for that component `enum`.
  using Component_payload_type_to_cfg_map = boost::unordered_map<std::type_index, Component_config>;

  /// How we store a log::Sev (a mere `enum` itself) in a certain data structure.
  using raw_sev_t = uint8_t;

  /**
   * Trivial wrapper of `atomic<raw_sev_t>` which adds a couple of things to make it possible to construct, and
   * therefore use, a `vector` of such `atomic`s.
   *
   * The primary reason it exists is that it's not possible to `push_back()` (and therefore `resize()`) or surprisingly
   * even `reserve()` (hence `emplace_back()` is also impossible) onto a `vector<atomic<>>`.  A copy constructor
   * is required.  Without one, I (ygoldfel) was able to *almost* get it to build, except it turns out that even
   * `reserve()` (which essentially is required for anything that grows the sequence) needs to be able to copy
   * `atomic<>`s -- albeit uninitialized ones -- even if one plans to construct in-place via `emplace_back()` (etc.).
   * We hence provide a copy constructor ourselves in the wrapper; but note that it has certain stringent requirements
   * for safe use; see its doc header.
   *
   * A side nicety is we can set the default constructor to, instead of keeping the value unspecified (probably
   * uninitialized but definitely resulting in a valid object), init it to our special value `raw_sev_t(-1)`, which
   * is used in Config::Component_union_idx_to_sev_map to represent the concept "index-not-in-map."  That's mere
   * syntactic sugar, but it works for us.
   *
   * @see I (ygoldfel) discovered and solved this all myself when trying to compile; but it's a generally easily
   *      Googlable situation and solution (that confirmed my findings); e.g.:
   *      https://stackoverflow.com/questions/12003024/error-with-copy-constructor-assignment-operator-for-a-class-which-has-stdatomi
   */
  class Atomic_raw_sev : public std::atomic<raw_sev_t>
  {
  public:
    // Constructor/destructor.

    /**
     * Constructs the atomic to the given value, guaranteed to be `load()`ed as such in all subsequent accesses incl
     * from other threads, even if `memory_order_relaxed` is used.
     *
     * @param init_val
     *        The value whose copy to set as the payload of this `atomic<>`.
     */
    Atomic_raw_sev(raw_sev_t init_val = raw_sev_t(-1));

    /**
     * Constructs the atomic to hold the value X held by the given other atomic accoding to a `memory_order_relaxed`
     * load of the latter.
     *
     * There is a subtlety: One generally expects that a just-constructed `atomic<> X(a)` must yield
     * `X.load(memory_order_relaxed) == a` no mater the thread where the load is done or how soon after construction.
     * Yet the present ctor by definition essentially performs `atomic<> X(src.load(memory_order_relaxed))`, hence
     * the subtlety: The `src.load()` needs to return the "true" value intended to be stored inside `src`, not some
     * cached/whatever older value in the current thread.  One way to guarantee this is to both construct (and
     * optionally modify) `src` *only* in the present thread before calling the present ctor.
     *
     * Be very careful to ensure you've considered this when copy-contructing.
     * Otherwise, it will probably work the *vast* majority of the
     * time; yet it may silently result in `this->load(memory_order_relaxed)` returning some unexpected value,
     * depending on whether the "master" value of `src` has propagated to the present thread.  Formally speaking
     * behavior is undefined outside of the aforementioned condition.  In actual fact, we do guarantee this
     * when growing Config::m_verbosities_by_component; and the other call site is the `default`
     * Config copy constructor/etc., whose doc header mentions this limitation.
     *
     * @param src
     *        Other object.  Behavior undefined if `(this == &src)`.
     */
    Atomic_raw_sev(const Atomic_raw_sev& src);
  }; // class Atomic_raw_sev

  /**
   * Short-hand for fast-lookup, thread-safe-for-RW mapping from flat-union-component-table index to max
   * allowed severity for that component.
   *
   * ### Rationale ###
   * 2 things will jump out at many readers: 1: It is a `vector`, instead of the seemingly more elegant and essentially
   * as-fast `unordered_map<component_union_idx_t, Sev>`. 2: It stores `atomic<raw_sev_t>` and not simply `Sev`.
   * The reason for both is thread safety, to wit for 2 operations: writing to it via
   * `configure_component_verbosity*()` vs. reading from it in output_whether_should_log().
   * In one makes the two mutually safe for concurrent execution in a given `*this`, it becomes possible to safely
   * configure per-component verbosity even while the rest of the system is concurrently logging like crazy.
   * So how does this type accomplish that?  Tackling the 2 decisions in order:
   *
   * Firstly assume for simplicity that `component_union_idx_t` is `size_t`; and that `Sev` and `raw_sev_t` are
   * essentially the same thing (enough so for this discussion anyway).
   * Now consider an `X` of this map type.  If it were a `..._map<size_t, ...>`, then `X[A] = B;` might change the
   * internal structure of the map (hash table, tree, whatever); accessing `X[C]` for *any* `C` would not be safe, as
   * it could catch it in a halfway state among other dangers.  If it is a `vector<...>`, however, then
   * `X[A] = B;` either results in undefined-behavior -- if `X.size() >= A` -- or works fine, in that the buffer inside
   * `X` does not change structure.  (However, this does indeed require that `X` is sized to allow for all possible
   * values of `A`.  It also makes `X` sparse and hence larger than it otherwise would require.  The latter is seen
   * as likely negligible given the number of components used in reality vs. how much RAM modern computers have;
   * to shrink it further and improve the set-all operation's speed is why we use single-byte `raw_sev_t` and not `Sev`.
   * As for pre-sizing, that explains the need for init_component_to_union_idx_mapping() arg `enum_sparse_length`.
   * TL;DR: It makes it so that the address in the reference `X[A]` never changes for a given `A`, as long as no
   * `resize()` or equivalent occurs throughout.
   *
   * Given that, why `atomic<raw_sev_t>` and not `raw_sev_t` alone?  Well, even though `X[A]` address never changes
   * and is thus thread-safe at the `X` level, the `= B;` assignment itself isn't thread-safe against reading the value.
   * Now, the extent to which it's not "thread-safe" depends on hairy low-level details; wrapping it in
   * `atomic<>` allows one to explicitly control the level of thread safety and the associated performance trade-off
   * (potentially significant since output_whether_should_log() is called at every single log call site).
   * With `atomic<>` one can use `memory_order` specifications when storing and reading to control this.
   * Further details are discussed at the read and write sites, but that's why `atomic<>` is used.
   *
   * ### Why the wrapper Atomic_raw_sev around the `atomic<>`? ###
   * Short version: It's because `vector<>` effectively cannot be initialized due to `atomic<>` not being
   * copy-constructible.  (However Atomic_raw_sev, after construction, is identical to `atomic<raw_sev_t>`, effectively
   * a `using`-alias at that point.)  Long version: See doc header for Atomic_raw_sev.
   *
   * ### Performance ###
   * The lookup by index could not be any faster: it adds the index to a base integer and done.  This is even somewhat
   * faster than an `unordered_map<>` which is also constant-time but a bit more involved.  The writing and reading
   * of the `atomic<>` can be faster than with an explicit mutex lock/unlock bracketing but can be somewhat slower than
   * an unprotected assignment/read; or it can be exactly equal; the point is that is under our control
   * via `memory_order` spec; again, see details at the read and write sites.
   */
  using Component_union_idx_to_sev_map = std::vector<Atomic_raw_sev>;

  /* Ensure it's fine to use `vector` index (`size_t` by definition) as a conceptual equivalent of a
   * `component_union_idx_t` key in a map. */
  static_assert(std::numeric_limits<component_union_idx_t>::is_integer
                  && (!std::numeric_limits<component_union_idx_t>::is_signed)
                  && (!std::numeric_limits<size_t>::is_signed)
                  && (sizeof(size_t) >= sizeof(component_union_idx_t)),
                "size_t must be able to act as equivalent to a component_union_idx_t key.");

  /// Short-hand for fast-lookup map from normalized component name to its flat-union-component-table index.
  using Component_name_to_union_idx_map = boost::unordered_map<std::string, component_union_idx_t>;

  /// Short-hand for map that is essentially the inverse of `Component_name_to_union_idx_map`.
  using Component_union_idx_to_name_map = boost::unordered_map<component_union_idx_t, std::string>;

  // Methods.

  /**
   * Normalized version of given component name.
   *
   * @param name
   *        Source name.
   * @return See above.
   */
  static std::string normalized_component_name(util::String_view name);

  /**
   * Normalizes given component name in place.
   *
   * @param name
   *        Pointer (not null) to string to potentially modify.
   */
  static void normalize_component_name(std::string* name);

  /**
   * Given a component in the form user provides it at log call sites, returns its index in the flat component union
   * table, as registered via init_component_to_union_idx_mapping(); or `component_union_idx_t(-1)` if
   * `component.payload_type()` was not registed.
   *
   * @param component
   *        A Component value as from a log call site.  Undefined behavior if `component.empty()` (null Component).
   * @return Index into the flat component union table (0 or higher); or `component_union_idx_t(-1)`.  See above.
   */
  component_union_idx_t component_to_union_idx(const Component& component) const;

  /**
   * Helper that for the given flat-union-component-index saves the given per-component verbosity, or removes it.
   *
   * @param component_union_idx
   *        Index into #m_verbosities_by_component.  -1 leads to undefined behavior.  Out-of-bounds leads to undefined
   *        behavior.  For rationale for the latter decision see doc header for #m_verbosities_by_component.
   * @param most_verbose_sev_or_none
   *        Either a cast of a valid log::Sev value indicating the most-verbose (highest) severity allowed to
   *        pass the output_whether_should_log() filter; or -1 to remove the per-component verbosity.
   *        Note that `Sev::S_NONE` is valid in this context and would disable all logging for that component.
   *        Conversely -1 would mean the component is removed from the conceptual "map."
   */
  void store_severity_by_component(component_union_idx_t component_union_idx, raw_sev_t most_verbose_sev_or_none);

  // Data.

  /**
   * Fast-lookup map from distinct `Component_payload` type to the config for that component `enum`.
   * The key is `std::type_index(std::type_info)`, a/k/a Component::payload_type_index().
   */
  Component_payload_type_to_cfg_map m_component_cfgs_by_payload_type;

  /**
   * Most verbose (highest) log::Sev for which output_whether_should_log() will return true, when the input
   * component is null or lacks a per-component configured verbosity.  Note that some log systems will choose to
   * use only this and not even allow any elements in #m_verbosities_by_component (see its doc header for definition
   * of containing or lacking an element).
   *
   * ### Rationale: Why Atomic_raw_sev instead of just log::Sev? ###
   * The reasoning is identical to that found in the discussion of why `atomic` is used in
   * #Component_union_idx_to_sev_map; see its doc header.  (We don't have to store a #raw_sev_t, as it's one lousy value
   * and not a bulk container in this case, but we do anyway just to get the copyability for free without having
   * to parameterize Atomic_raw_sev into a template.  It's a tiny bit cheesy to avoid the latter just to keep the code
   * briefer, but probably well within reasonableness.)
   */
  Atomic_raw_sev m_verbosity_default;

  /**
   * Maps from flat union component index to most verbose (highest) log::Sev for which output_whether_should_log() will
   * return true, when the input component is not null and maps to that flat union index via component_to_union_idx().
   * First, read doc header for Component_union_idx_to_sev_map which described why and how the data structure works
   * as a map; then return here.
   *
   * Semantics are as follows:
   *   - If `m_verbosities_by_component[X]` equals -1 cast appropriately, then that component
   *     key is *not* in the conceptual map (`map::find()` would return `end()`).  This means no verbosity is configured
   *     for that individual component; note this is common-place in practice.  One should then fall back to
   *     #m_verbosity_default.
   *   - If `m_verbosities_by_component[X]` is out of range, same thing.  However, that means they didn't properly
   *     call init_component_to_union_idx_mapping() to allow for `X`.  Nevertheless, that is allowed when *reading*
   *     (in output_whether_should_log()) to avoid crashing for no great reason.  It is however not allowed
   *     when *writing* (in `configure_*_verbosity()`), so they'd better get it right at that level.
   *     The idea is to be permissive at log call sites, which should not care about ANY of this and just want to
   *     log with their component in peace; but not-permissive when configuring the log system, done at the program
   *     driver level.
   *   - If `m_verbosities_by_component[X]` is in range and not -1, then that value cast to log::Sev is the verbosity
   *     for that individual component.  (In particular, Sev::S_NONE which happens to be 0, means all logging is
   *     disabled for that component.)
   *
   * Note that some log systems will choose to not even allow any elements in #m_verbosities_by_component and leave
   * the config to #m_verbosity_default exclusively; in this case `m_verbosities_by_component` is filled with
   * -1 copies (or might even be empty, if they had never configured any components for whatever reason; by above
   * semantics those have identical meanings).
   */
  Component_union_idx_to_sev_map m_verbosities_by_component;

  /**
   * Maps each flat union component index to its *output* component name as registered in init_component_names().
   * Note that a given index not being present in this map doesn't mean it's not a real component; but
   * init_component_names() caller may have intentionally not supplied it a forward-lookup name, so that instead
   * the number itself would be output.
   *
   * @see init_component_names()
   */
  Component_union_idx_to_name_map m_component_names_by_union_idx;

  /**
   * Maps each distinct component name as registered in init_component_names() to its flat union component index.
   *
   * @see init_component_names()
   */
  Component_name_to_union_idx_map m_component_union_idxs_by_name;
}; // class Config

// Template implementations.

template<typename Component_payload>
void Config::init_component_to_union_idx_mapping(component_union_idx_t enum_to_num_offset,
                                                 size_t enum_sparse_length)
{
  using std::type_index;

  assert(enum_sparse_length >= 1);

  const component_union_idx_t component_union_idx_max = enum_to_num_offset + enum_sparse_length - 1;
  assert(component_union_idx_max >= enum_to_num_offset);
  assert(component_union_idx_max >= enum_sparse_length);

  /* This would equal Component(C).payload_type_index(), where C is a value of type Component_payload.  That is how
   * this mapping would be used subsequently after this call.  Component(C) is routinely provided at log call sites. */
  const type_index idx(typeid(Component_payload));

  /* It's not necessarily bad to replace an existing per-Component_paylod-offset.  However in doc header I said behavior
   * is undefined, as it requires looking into the various implications -- not currently worth it.  So for now: */
  assert(!util::key_exists(m_component_cfgs_by_payload_type, idx));

  m_component_cfgs_by_payload_type[idx] = { enum_to_num_offset };

  /* Finally make the per-component verbosities table able to hold all keys component_to_union_idx(C) could ever
   * return when C.payload_type() == typeid(Component_payload). See doc header for
   * m_verbosities_by_component then come back here. */
  if (component_union_idx_max >= m_verbosities_by_component.size())
  {
    /* Every single element starts at -1 which means "not in the conceptual map."  Note this resize() is the only way
     * elements are added to the vector.  After this it's all atomic<>::store() to existing elements and
     * atomic<>::load()s from there.
     * (Note: OK, technically there's another way m_verbosities_by_component is set, in the copy ctor.  See copy ctor's
     * doc header.)
     * (Note: This requires copying of a Atomic_raw_sev(raw_sev_t(-1)) N times, where N is the number of elements,
     * if any, added to m_verbosities_by_component.  That's why Atomic_raw_sev() has a copy ctor unlike the
     * underlying atomic<raw_sev_t>.  Its use can be unsafe, in terms of propagation across threads, as said in the
     * copy ctor's doc header; but since we do the source construction and N copies in the same thread, per that doc
     * header we are safe.) */
    m_verbosities_by_component.resize(component_union_idx_max + 1); // Implied 2nd arg = Atomic_raw_sev(-1).
  }
} // Config::init_component_to_union_idx_mapping()

template<typename Component_payload>
void Config::init_component_names
       (const boost::unordered_multimap<Component_payload, std::string>& component_names,
        bool output_components_numerically,
        util::String_view payload_type_prefix_or_empty)
{
  using std::type_index;
  using std::string;

  /* There is much subtle (not super-exciting) stuff going on below.  It helps to read the contract of the function
   * in some detail; see doc header first, then continue here. */

  /* Assuming for each distinct init_component_names() call they provide a distinct, non-empty one of these, it
   * guarantees no 2 equal component names from different invocations ever ultimately collide.  In other words
   * it's a poor man's namespace. */
  const string payload_type_prefix_normalized(normalized_component_name(payload_type_prefix_or_empty));

  /* This looks straigtforward, but there's a surprisingly weird sublety.  The doc header talks about it:
   * component_names is a multi-map (meaning this iteration might yield the same enum_val 2+ times in a row).
   * This is best explained by example.  This is an explicitly legal component enum fragment:
   *   S_COOL_ENGINE = 5,
   *   S_ENGINE_ALIAS1 = 5,
   *   S_ENGINE_ALIAS2 = 5,
   *
   * Moreover, the typically-auto-generated-via-#macro index-to-name map fragment will be as follows:
   *   S_COOL_ENGINE -> "COOL_ENGINE"
   *   S_ENGINE_ALIAS1 -> "ENGINE_ALIAS1"
   *   S_ENGINE_ALIAS2 -> "ENGINE_ALIAS2"
   *
   * Looks fine, right?  But, when indexing by enum class values, all three S_* keys cited are equal!  They all,
   * really, equal 5.  So a non-multi-map would not be able to even store that mapping; one of the 3 names would
   * "win," the others being forgotten entirely.  So it's a multi-map.  So now what?  To be continued inside
   * the loop. */
  for (auto const & enum_val_and_name : component_names)
  {
    const Component_payload enum_val = enum_val_and_name.first;

    auto name_sans_prefix_normalized = enum_val_and_name.second;
    assert(!name_sans_prefix_normalized.empty()); // Advertised as not allowed.  Implications would be too weird.
    normalize_component_name(&name_sans_prefix_normalized);

    string name_normalized;
    name_normalized.reserve(payload_type_prefix_normalized.size() + name_sans_prefix_normalized.size());
    name_normalized += payload_type_prefix_normalized;
    name_normalized += name_sans_prefix_normalized;

    auto const component_union_idx = component_to_union_idx(Component(enum_val));

    // Presumably they didn't call init_component_to_union_idx_mapping<Component_payload>() yet, if this trips.
    assert(component_union_idx != component_union_idx_t(-1));
    // If this trips, they have allowed a name collision (behavior undefined, as promised).
    assert(!util::key_exists(m_component_union_idxs_by_name, name_normalized));

    // Set up reverse lookup (for verbosity setting by name, at least).
    m_component_union_idxs_by_name[name_normalized] = component_union_idx;
    /* Ready!  Let's analyze the multi-key corner case mentioned before, by example.  This will, across 3 iterations,
     * result in the appropriate mappings:
     *   "COOL_ENGINE" -> component_to_union_idx(S_COOL_ENGINE) -> X
     *   "ENGINE_ALIAS1" -> component_to_union_idx(S_ENGINE_ALIAS1) -> X (same value)
     *   "ENGINE_ALIAS2" -> component_to_union_idx(S_ENGINE_ALIAS2) -> X (same value)
     * This is the advertised behavior.  For config purposes, any of the 3 names linked with 1 enum numerical value
     * refer to the same knob. */

    // Set up forward lookup (for component -> string output, at least).  They can choose to turn this off.
    if (!output_components_numerically)
    {
      string& output_name_so_far = m_component_names_by_union_idx[component_union_idx];
      if (output_name_so_far.empty())
      {
        assert(!name_normalized.empty());
        output_name_so_far = std::move(name_normalized);
      }
      else
      {
        assert(!name_sans_prefix_normalized.empty());
        output_name_so_far += ',';
        output_name_so_far += name_sans_prefix_normalized;
      }
      /* Ready!  Let's analyze the multi-key corner case mentioned before, by example.  This will, across 3 iterations,
       * result in the appropriate single mapping:
       *   X -> "<prefix>COOL_ENGINE,ENGINE_ALIAS1,ENGINE_ALIAS2"
       * where X == component_to_union_idx(S_COOL_ENGINE == S_ENGINE_ALIAS1 == S_ENGINE_ALIAS2).
       * This is the advertised behavior.  For output purposes, any of the enum values linked with 1 enum num value
       * refer to the same component name which contains all 3 individual strings comma-joined. */
    }
  } // for (enum_val_and_name : component_names)
} // Config::init_component_names()

template<typename Component_payload>
bool Config::configure_component_verbosity(Sev most_verbose_sev, Component_payload component_payload)
{
  using std::type_index;

  const auto component_union_idx = component_to_union_idx(Component(component_payload));
  if (component_union_idx == component_union_idx_t(-1))
  {
    /* If they called init_component_to_union_idx_mapping<Component_payload>(), this cannot happen.
     * If they did not, then this is the advertised behavior, but probably they're doing something wrong. */
    return false;
  }
  // else

  store_severity_by_component(component_union_idx, raw_sev_t(most_verbose_sev));
  return true;
} // Config::configure_component_verbosity()

template<typename Component_payload>
size_t Config::standard_component_payload_enum_sparse_length() // Static.
{
  return size_t(Component_payload::S_END_SENTINEL);
  /* To reiterate, if one just plops S_END_SENTINEL at the end of an `enum class` -- with no manually assigned value --
   * its value will be correct for our purposes.  That does assume, though, that the immediately preceding member
   * has otherwise the highest numeric value (which is the case if, but not only if, one lists them in numeric order
   * as opposed to out of order).  Additionally that assumes, essentially, its backing type is unsigned and no bigger
   * than our return type size_t.  (A tigher, less restrictive requirement could be written out, but this isn't a
   * math proof over here.)  In particular: Certainly all of these things are guaranteed if one uses
   * config_enum_start_hpp.macros.hpp, as documented therein, to create Component_payload. */
}

} // namespace flow::log
