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
#include "flow/log/log.hpp"
#include "flow/error/error.hpp"
#include "flow/cfg/detail/cfg_fwd.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/unordered_set.hpp>

namespace flow::cfg
{
// Types.

/// Un-templated base for Option_set.
class Option_set_base
{
public:
  // Types.

  /**
   * Internal-use type to use with Option_set::Declare_options_func callback.
   * The user of the class need not understand this nor use it directly.
   *
   * @internal
   *
   * The reason for the Option_set::Declare_options_func call, originally from FLOW_CFG_OPTION_SET_DECLARE_OPTION().
   */
  enum class Declare_options_func_call_type
  {
    /**
     * Internal use only through macro.  Do not use directly.
     *
     * @internal
     * Add option entries into Option_set::m_opts_for_parsing using Option_set::declare_option_for_parsing().
     */
    S_FILL_PARSING_ROLE_OPT_TABLE,

    /**
     * Internal use only through macro.  Do not use directly.
     *
     * @internal
     * Add option entries into Option_set::m_opts_for_help using Option_set::declare_option_for_help().
     */
    S_FILL_OUTPUT_HELP_ROLE_OPT_TABLE,

    /**
     * Internal use only through macro.  Do not use directly.
     *
     * @internal
     * Add option entries into an Option_set::Opt_table that shall be output to an `ostream` to print a `Value_set`'s
     * payload, using declare_option_for_output().
     */
    S_FILL_OUTPUT_CURRENT_ROLE_OPT_TABLE,

    /**
     * Internal use only through macro.  Do not use directly.
     *
     * @internal
     * Compare recently parsed values in Option_set::m_iterable_values_candidate to the canonical
     * Option_set::m_values values, using Option_set::scan_parsed_option().
     */
    S_COMPARE_PARSED_VALS,

    /**
     * Internal use only through macro.  Do not use directly.
     *
     * @internal
     * Load values in Option_set::m_values_candidate and Option_set::m_iterable_values_candidate from a
     * ready-made `Value_set` -- as if the latter was parsed from some config source --
     * using load_option_value_as_if_parsed().
     */
    S_LOAD_VALS_AS_IF_PARSED,

    /**
     * Internal use only through macro.  Do not use directly.
     *
     * @internal
     * Validate values stored -- perhaps manually in code, not through parsing -- in a `Value_set` structure
     * using the same per-individual-option validator checks as configured into Option_set::m_opts_for_parsing
     * via Declare_options_func_call_type::S_FILL_PARSING_ROLE_OPT_TABLE, using validate_parsed_option().
     */
    S_VALIDATE_STORED_VALS
  }; // enum class Declare_options_func_call_type

  // Methods.

  /**
   * Internal-through-macro helper function; the user shall not call this directly but only through
   * FLOW_CFG_OPTION_SET_DECLARE_OPTION() (see Option_set main constructor doc header).
   *
   * @internal
   * Loads an entry into `target_opts` which will enable the proper text to appear for that option
   * in Option_set::values_to_ostream() or Option_set::log_values(): including its name, default value, current values,
   * and description.
   * @endinternal
   *
   * @tparam Value
   *         Type of the value inside a `Value_set` object.  It must be reasonably copyable; and it must be supported by
   *         some version (including specialization(s) and overload(s)) of value_to_ostream().
   * @param name
   *        See Option_set::declare_option_for_parsing().
   * @param target_opts
   *        The `Opt_table` that shall be filled out for output to `ostream`.
   * @param value_default
   *        See Option_set::declare_option_for_help().
   * @param current_value
   *        Current value to show to the user.
   * @param description
   *        See Option_set::declare_option_for_help().
   */
  template<typename Value>
  static void declare_option_for_output(util::String_view name, opts::options_description* target_opts,
                                        const Value& value_default, const Value& current_value,
                                        util::String_view description);

  /**
   * Internal-through-macro helper function; the user shall not call this directly but only through
   * FLOW_CFG_OPTION_SET_DECLARE_OPTION() (see Option_set main constructor doc header).
   *
   * @internal
   * Runs the validator function, with the same semantics as in declare_option_for_parsing(), for the given
   * option.  If the validation passes, no-op.  If the validation fails throws exception containing (in its message)
   * a user-friendly description of what failed.
   * @endinternal
   *
   * @tparam Value
   *         Type of the value inside a `Value_set` object.  It must be reasonably copyable.
   * @param name
   *        See Option_set::declare_option_for_parsing().
   * @param value
   *        Value to validate.
   * @param validator_func_moved
   *        See Option_set::declare_option_for_parsing().
   * @param validator_cond_str
   *        See Option_set::declare_option_for_parsing().
   */
  template<typename Value>
  static void validate_parsed_option(util::String_view name, const Value& value,
                                     Function<bool (const Value& val)>&& validator_func_moved,
                                     util::String_view validator_cond_str);

protected:
  // Types.

  /**
   * Short-hand for boost.program_options config options description, each of which is used for parsing and/or
   * describing (to humans) one or more config option/its value.  boost.program_options allows for a wide variety
   * of features and capacities for this object type, but our goal is to use it for our specific and more
   * constrained config context, and hence we use this type in more specific/constrained ways which we describe
   * here.
   *
   * We use it in our context in two primary roles; and since storage space and processor cycle are not
   * practically affected, we tend to store separate copies of an `Opt_table` in *each* role (possibly
   * even more than 1, as there can be sub-roles of each).
   * The roles are:
   *   - Parsing: Used during a parsing pass from a given config source (like a config file) solely to parse
   *     values and store the resulting bits into a target `Values` and a mirroring
   *     `Iterable_values`.  `opts::{parse_*|store}()` and other such functions take
   *     this `Opt_table` and a config source as input.  Such an `Opt_table`
   *     stores a table of options; for each option storing:
   *     - Name: A string to be found in a config source like a config file.
   *     - C++ type: Essentially a size in bytes plus a `>>` stream input operator for reading values
   *       into the target location from the config source.
   *     - Target location: Pointer to the actual value, within a `Values`, which to load with the parsed bits
   *       obtained from the config source.
   *     - NOTE: There is *no* default value stores in this *parsing-role* `Opt_table`.
   *       We expect each target value to already contain the default before parsing begins; if the option
   *       is not present in the particular config source, then that value remains untouched.  Moreover note that
   *       the pre-parse value is not necessarily the user-specified default per se; it might be, for example,
   *       the value set by the previous config source's parsing phase; or even (in the case of dynamic option
   *       sets) in a much earlier config application.
   *     - Note: There's no reason to store a textual description for this role, as the computer doesn't care
   *       about human explanations when parsing.
   *   - Output: Used in various settings to print out -- to humans -- both the semantic info and actual values of
   *     the options maintained by an Option_set.  To use this capability of boost.program_options -- which
   *     it intends as help text, but we can leverage it in slightly different roles also -- simply
   *     output this *output-role* `Opt_table` via a stream `<<`.  For each option:
   *     - Name: Same as for the *parsing-role*.
   *     - Description: Textual description of the meaning/semantics of the option.
   *       - If it's not output of help text/similar, then description might be omitted, depending on usefulness.
   *     - C++ type: Essentially a size in bytes plus a `<<` stream output operator for writing values
   *       via standard stream output.
   *     - Value: A pointer to a value to print out along with the previous name and description.
   *       boost.program_options supports printing in one 2 ways:
   *       - Via `<<` stream output operator.  In this case that operator needs to be (conceptually) stored too.
   *       - As an explicit string.  Sometimes, for human readability, we might want to provide custom output
   *         for certain types beyond what `<<` would do; for example if we store a `Fine_duration` (typically
   *         outputting which would be in nanoseconds, because it usually stores nanoseconds), it's often more
   *         convenient to round down (if precision would not be lost) and output using coarser units like sec or
   *         even minutes, etc.  In this case we would prepare this value as an `std::string` and store that;
   *         instead of storing a value in the original type as in *parsing-role* `Opt_table`.
   *       At least these reasons exist to print this value:
   *       - As the *default* in a help message.  The user would glean that not specifying a value in any config
   *         source will lead to this value being taken.
   *       - As the *current* stored value after the last round of parsing.  If it is also desired to output
   *         a default at the same time, it can be added into the description.
   */
  using Opt_table = opts::options_description;

  // Methods.

  /**
   * Returns a function that wraps a `Value`->Boolean validator function, as passed to
   * declare_option_for_parsing() and others, in code that will throw an exception with a human-useful message
   * if that validator function indicates the `Value` passed to it is invalid; else will no-op.
   *
   * @tparam Value
   *         See Option_set::declare_option_for_parsing().
   * @param name
   *        See Option_set::declare_option_for_parsing().  For nice human-readable message formation.
   * @param validator_func_moved
   *        See Option_set::declare_option_for_parsing().
   * @param validator_cond_str
   *        See Option_set::declare_option_for_parsing().  For nice human-readable message formation.
   * @return See above.
   */
  template<typename Value>
  static Function<void (const Value& val)> throw_on_invalid_func
                                             (util::String_view name,
                                              Function<bool (const Value& val)>&& validator_func_moved,
                                              util::String_view validator_cond_str);
  /* @return Function that acts as described above.
   * @todo @return just above breaks Doxygen for some reason (bug).  Work around it or something. */
}; // class Option_set_base

/**
 * The core config-parsing facility, which builds parsing/comparison/output capabilities on top of a given
 * simple config-holding object, of the type `Value_set`, a template argument to this class template.
 *
 * ### General use pattern ###
 * - First, create your `Value_set` (named however you want, of course) `struct`.  The values stored therein must
 *   be reasonably deep-copyable; must have standard-stream `<<` and `>>` operators; and must reasonably implement
 *   `==` comparison.  `Value_set` itself must be copy-constructible and copy-assignable in a reasonable way.
 *   Lastly, and very importantly, the no-arg ctor `Value_set()` must initialize all configured
 *   members to reasonable defaults: it is not possible to declare options as "required."
 *   - If you use the optional features mutable_values_copy(), #Mutable_values_ptr, and/or #Values_ptr, then, also,
 *     `Value_set` shall derive from util::Shared_ptr_alias_holder.  (Don't worry: it's easy.)
 * - Next, write a function fitting Option_set::Declare_options_func, which shall be used (when passed into
 *   Option_set ctor) by Option_set to enable parsing (and human-readable help/other output to streams) of various
 *   members of `Value_set`, which officially turns them into *options*.
 *   - This is documented more fully in the `Option_set()` ctor doc header; but essentially this function
 *     shall use FLOW_CFG_OPTION_SET_DECLARE_OPTION() macro to enumerate every parseable member, turning it into
 *     an option *managed by* the containing Option_set.
 *   - (The data members that thus become options need not be direct members of `Value_set`.  Composition via directly
 *     nested, and nested via pointer, `struct`s is supported.  See FLOW_CFG_OPTION_SET_DECLARE_OPTION() doc header for
 *     details.)
 * - Finally, create an instance of `Option_set<Value_set>`, and use parse_config_file() (or any other `parse_*()`
 *   methods that might exist) to parse things at will.
 *   - "At rest," Option_set is in CANONICAL state.  values_candidate() returns null, and values() is the *canonical*
 *     (official, current) set of parsed config; originally it equals `Value_set()`.
 *     It returns a reference to immutable internally stored `Value_set`.
 *   - Invoking parse_config_file() (or any other `parse_*()`) either enters or continues PARSING state.
 *     In this state `*values_candidate()` starts equal to values(); and is then potentially modified by each
 *     `parse_*()` operation.
 *   - Call `canonicalize_candidate()` to set values() to `*values_candidate()`, thus canonicalizing the parsed
 *     values, and return to CANONICAL state.
 *     - Alternatively, if a `parse_*()` or `validate_values*()` fails, or any manual check of values_candidate() fails,
 *       one might want to instead call reject_candidate() to discard any parse attempts and return to CANONICAL state.
 *     - See notes in Validation below.
 *   - If one desires to model multiple dynamic updates, meaning calling canonicalize_candidate() multiple times --
 *     once for each update -- it may be desirable to use parse_direct_values() to load a baseline state at entry
 *     into PARSING state, and only then `parse_*()` from actual config source(s).
 *     (It would be trivial to save a previous state using values() to be that baseline state.)
 *
 * A typical expected scenario -- although more complex ones can be devised -- and one assumed by
 * Config_manager -- is to have 2 `struct`s of config; one for static and one for dynamic config, the latter being
 * something that can be parsed-into repeatedly over time, as new config is delivered to the process.
 * In that case one would use one `Option_set<Static_value_set>` and one `Option_set<Dynamic_value_set>` and invoke
 * their parsing at appropriate times (1+ times for the latter, once for the former).  It is also possible to
 * allow, on startup, to read both sets from the same config source(s) (e.g., a static config file); then read
 * the dynamic `Option_set<>` from the dynamic config source(s) once (for any initial dynamic values overriding
 * the initial baseline static ones); and after that accept any further updates of the latter config source(s), as
 * they come in (e.g., if the dynamic config file is modified externally).  The `allow_unregistered` argument
 * to `parse_*()` allows 2+ `Option_set<>`s to share one config file.
 *
 * ### Validation ###
 * Option validation *mandatorily* occurs in the following places.
 *   - When `parse_*()` is invoked, the config source (e.g., config file in case of parse_config_file()) may have
 *     plainly illegal contents, such as an option expecting a number but receiving alphanumerics, or a syntax
 *     error.  This will cause `parse_*()` to indicate error.
 *   - If that passes, the individual-option-validator checks are performed *on values in fact read+parsed*.
 *     For example, a duration option may need to be non-negative, or a
 *     value in bytes must be a multiple of 4KiB.  To provide any such additional
 *     *per-option*, *independent-of-other-options* conditions, use the `ARG_bool_validate_expr` argument to
 *     FLOW_CFG_OPTION_SET_DECLARE_OPTION() when declaring the options (see above).  This is somewhat reminiscent
 *     of writing `assert()` conditions.  If a validator fails, `parse*()` will transparently fail, not dissimilarly
 *     to what happens if some line is straight-up illegal (previous bullet point).
 *
 * However this is likely insufficient validation in at least some use cases.  You should also concern yourself with
 * the following.
 *   - Note that parse_config_file() (and any similar `parse_*()` that scans+parses strings) will *only* validate
 *     (via individual-option-validator checks) values actually present in the config source.
 *     Defaults (from `Value_set()`) or baseline values (from parse_direct_values()) are not *mandatorily* checked.
 *     If one performs no additional validation calls, it will not be possible to know of bad defaults or baseline
 *     values, and one can canonicalize_candidate() invalid values.  This is allowed, for flexibility, but in most cases
 *     one will want to reject_candidate() instead.  The following abilities are provided to resolve this.
 *     For flexibility we do not here enforce any particular approach.  It is your reponsibility to trace the
 *     possibilities.  (For example Config_manager chooses certain conventions as it marshals various `Option_set`s.)
 *     - To validate defaults (default-constructed `Value_set`) themselves, call `validate_values(bool*)` overload --
 *       just after construction.
 *       (Informally: This is a stringent convention.  It is possible to instead establish the convention wherein
 *       intentionally invalid defaults are allowed, to force that a config source must specify such values explicitly.
 *       In that case: one would not call validate_values() this way but rather... see next bullet point.)
 *     - To validate any defaults and/or baseline values that "got through" in PARSING mode, by virtue of not being
 *       present in a parsed config source (e.g., file) so far, call validate_values_candidate().
 *       (Informally: Typically you will not want to canonicalize_candidate() when the values_candidate() might have
 *       invalid values.  So, just ahead of the decision to canonicalize_candidate()
 *       or reject_candidate(), call validate_values_candidate().  If it indicates failure, probably you'll want
 *       to reject_candidate().)
 *     - Lastly: parse_direct_values() intentionally does not validate the supplied `Value_set`.
 *       You are free to call validate_values_candidate() right after it to detect problems, if it suits your needs.
 *       (Informally: Whether this is necessary really depends on your setup, including the source of the values
 *       applied -- which may have already been validated -- and whether you're going to be calling
 *       validate_values_candidate() before canonicalizing anyway.)
 *   - You may also need a validation for inner consistency; e.g., if one setting must not exceed another setting.
 *     Do this yourself: Before calling canonicalize_candidate() check whatever is necessary in `*values_candidate()`.
 *     If you find problems call reject_candidate() instead.
 *     - Informal recommendation: It is better to not leave individual-option checking to this step; make use
 *       of FLOW_CFG_OPTION_SET_DECLARE_OPTION() to its fullest.  This will result in more maintainable, reusable code
 *       and likely improve the experience (e.g., the individual validator machinery will always print the value of
 *       the offending option and, `assert()`-style, a string representation of the condition that failed).
 *
 * Lastly, and optionally, you may validate a given `Value_set` object "offline," meaning outside of any
 * `Option_set<Value_set>` -- which need not even exist or ever parse anything; only the function fitting
 * Option_set::Declare_options_func must exist, as-if for `Option_set` ctor.  Simply call a validate_values()
 * API on your object; it will yield failure given at least 1 invalid value.  This can be useful, at least, when
 * setting values manually through assignment (perhaps in a simpler program not requiring external config; or when
 * unit-testing); just because you aren't parsing it through Option_set does not mean you don't want to sanity-check it
 * for validity.
 *
 * ### Change detection ###
 * For dynamic options, it may be necessary to detect whether the set of option values has changed or even individual
 * changes (and then, in many cases, invoke config-changed hooks).  Option_set is designed with this in mind, as it
 * is able to provide such checks *without* forcing the user to write an `operator==(Value_set, Value_set)` (a laborious
 * and error-prone procedure).
 *
 * Change detection occurs at the canonicalize_candidate() stage.  That is, once you've parsed all the different config
 * sources in a parse pass, canonicalize_candidate() will scan for changes.  As of this writing that method will
 * optionally return a Boolean indicating whether at least one value changed vs. the canonical values().  It is possible
 * to add an API for detecting individual option changes.  As I write this, it doesn't exist, but it may be added after
 * I write this.  Option_set is eminently capable of it; and in fact it at least logs an INFO message for each
 * option that has changed, at canonicalize_candidate() time.
 *
 * ### Input/output ###
 * Input of configurable values in `Value_set` is required to be able to parse the corresponding options (parse_*()).
 * Output thereof is required in:
 *   - help_to_ostream(), log_help(): A usage/help message describing all options, and their default values,
 *     in human-friendly form.
 *   - values_to_ostream(), log_values(): A message describing all options, their default values, and their *current*
 *     (presumably parsed at some point) values, is printed (in human-friendly form).  You can use this to output
 *     any `Value_set`, but if none is specified, it will log the canonical one (values()).
 *
 * For input (parsing), every configurable value in `Value_set` must have an `istream>>` operator.
 *
 * For output (in help and current-values output), every such value must have an `ostream<<` operator.
 * In addition, it is sometimes desirable to further massage output in config help/current-values output but not
 * in a general `ostream<<` operator for that type.  (E.g., you would not want to, or be able to usually, override
 * stream output for `std::string` or `chrono::duration`.)  To do this, provide a specialization or overload of
 * the free function `cfg::value_to_ostream<Value_set>()`.  Its generic implementation simply forwards to
 * `ostream<<`, but you can do something different.  As of this writing we already provide an overload for
 * `cfg::value_to_ostream<chrono::duration<...>>`, which will automatically print the duration (which is typically
 * stored in nanoseconds) in the coarsest possible units that would lose no precision (e.g., 60billion nanoseconds =>
 * "1 minute").
 *
 * ### Recommended conventions ###
 * Firstly, it is recommended to store all durations in your `Value_set` as util::Fine_duration instead of using
 * coarser units like `chrono::seconds()` or even `chrono::milliseconds()`.  This tends to lead to more consistent
 * and maintainable code, in the author's (ygoldfel) opinion, as util::Fine_duration *can* store durations expressed
 * in essentially any units, without losing precision; *does* use the same underlying storage type -- `int64_t` --
 * and hence presents no performance or overflow difficulties (usually); and changing the desired units of a duration
 * config value is fairly common.  Simply put `Fine_duration` supports 99.99999999% of units and use cases without
 * perf overhead.  *In addition*, `value_to_ostream<>` is overloaded in such a way as to output `Fine_duration`
 * members of `Value_set` in the most convenient possible units -- automagically.  So you're in no way forcing humans
 * to work with nanoseconds: you can use any units in input and in code.
 *
 * @note For all duration values, config sources can specify *any* units (convertible without loss of precision to
 *       the specific `chrono` type); and indeed *should* specify units.  E.g., "5 ms" and "5 milliseconds" will both
 *       work.  Since util::Fine_duration is in nanoseconds internally, almost any conceivable units (from "hours" to
 *       "nanoseconds") are accepted equally well as inputs in config sources.
 *
 * Secondly, when working with non-durations, specify units as a suffix in the member name.
 *   - Exception: if it's in bytes, `_bytes` or `_b` shall be omitted.
 *   - If it's in multiples of bytes, use `_kb`, `_mb`, etc., knowing that this stands for KiB (1024), MiB (1024^2),
 *     etc.  In the rare case that actual KILObytes, etc., are needed, spell it out: `_kilob`, `_megab`, etc.
 *     - I (ygoldfel) am not recommending `_kib`, etc., only because the convention in the relevant organization(s) is
 *       quite strong to say KB for KiB, etc.
 *   - If, for some reason, you must use a duration with a max resolution lower than ns, then use these suffixes:
 *     `_hr`, `_min`, `_sec`, `_msec`, `_usec`, `_nsec`.  Please still use `chrono::duration`, though, even if you
 *     chose something other than `Fine_duration`.  (Do not use an integer type.)
 *
 * @todo Add individual-option-changed detection API(s) in addition to the existing Option_set overall-value-set-changed
 * detection API.  This is contingent on a use case needing it.  The existing code already detects this internally
 * and logs a message for each changed option (in canonicalize_candidate()).
 *
 * @tparam Value_set
 *         The value type stored inside `*this`, and returned by values().  Requirements on this type are at least
 *         informally explained above.
 */
template<typename Value_set>
class Option_set :
  public Option_set_base,
  public log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /**
   * Short-hand for the template parameter type `Value_set`.  E.g.: `Cool_option_set::Values`, where one aliased
   * `Cool_option_set` to `Option_set<Cool_value_set>`.
   */
  using Values = Value_set;

  /**
   * Short-hand for ref-counted pointer to an immutable `Value_set` (config payload storable in an Option_set).
   *
   * The name is not, say, `Const_values_ptr`, because we would expect such objects to be passed around in `const`
   * form most of the time, including when made accessible from within a config-holding API.
   * When a mutable #Values is desired, one would typically create it from an Option_set by using mutable_values_copy().
   */
  using Values_ptr = typename Values::Const_ptr;

  /// Short-hand for ref-counted pointer to a mutable #Values (config payload storable in an Option_set).
  using Mutable_values_ptr = typename Values::Ptr;

  /**
   * Internal-use structure to use with #Declare_options_func callback.  The user of the class need not understand this
   * nor use it directly.
   *
   * @internal
   *
   * This `union`-y `struct` is used to pass values into `declare_option_*()`, `scan_parsed_option()`, or
   * `validate_parsed_option()`.
   * The `enum` specifies which part of the `union` is in effect; then each `struct` inside the `union` corresponds
   * to that #Declare_options_func call purpose.
   */
  struct Declare_options_func_args
  {
    // Types.

    /// Short-hand for type of #m_call_type.
    using Call_type = Option_set_base::Declare_options_func_call_type;

    // Data.

    /// Why #Declare_options_func is being called.
    Call_type m_call_type;

    /**
     * The args to pass in depending on #m_call_type.
     * @internal
     * @todo `union` used inside Option_set implementation should be replaced with the type-safe, equally-fast,
     * more-expressive `std::variant`.  The author was not aware of the latter's existence when originally writing
     * this.
     */
    union
    {
      /// Corresponds to Call_type::S_FILL_PARSING_ROLE_OPT_TABLE.
      struct
      {
        /// `m_option_set->m_opts_for_parsing` shall be filled out.
        Option_set* m_option_set;
        /**
         * `m_option_set->m_values_candidate`, the parsing-in-progress `Value_set` filled out whenever parsing using
         * the completed Option_set::m_opts_for_parsing.
         */
        Values* m_values_candidate;
        /**
         * `m_option_set->m_values_default`: the defaults loaded into #m_values_candidate at the top of each
         * parse, for any option specified as non-accumulating.  (Any members whose corresponding option is a regular
         * accumulating one is, in this context, ignored; instead the preceding parse's final value is kept unchanged.)
         */
        const Values* m_values_default_no_acc;
      } m_fill_parsing_role_opt_table_args;

      /// Corresponds to Call_type::S_FILL_OUTPUT_HELP_ROLE_OPT_TABLE.
      struct
      {
        /// `m_option_set->m_opts_for_help` shall be filled out.
        Option_set* m_option_set;
        /// `m_option_set->m_values_default`: defaults loaded into `Option_set::m_opts_for_help` will originate here.
        const Values* m_values_default;
      } m_fill_output_help_role_opt_table_args;

      /// Corresponds to Call_type::S_FILL_OUTPUT_CURRENT_ROLE_OPT_TABLE.
      struct
      {
        /**
         * #m_target_opts shall be filled out based on the defaults earlier saved into #m_values_default and
         * current values in #m_values_current.
         */
        opts::options_description* m_target_opts;
        /// `m_option_set->m_values_default`: the defaults loaded into `*m_target_opts` will originate here.
        const Values* m_values_default;
        /// The current values loaded into `*m_target_opts` (e.g., via description text) will originate here.
        const Values* m_values_current;
      } m_fill_output_current_role_opt_table_args;

      /// Corresponds to Call_type::S_COMPARE_PARSED_VALS.
      struct
      {
        /**
         * Canonical values in `m_option_set->values()` shall be compared with parsed values in
         * `m_option_set->m_iterable_values_candidate`.
         */
        Option_set* m_option_set;
      } m_compare_parsed_vals_args;

      /// Corresponds to Call_type::S_LOAD_VALS_AS_IF_PARSED.
      struct
      {
        /**
         * Values in `*(m_option_set->values_candidate())` (and related) shall be loaded from #m_values_to_load, as-if
         * parsed from some config source.
         */
        Option_set* m_option_set;

        /**
         * `m_option_set->m_values_candidate`, the structure to load from #m_values_to_load.
         */
        Values* m_values_candidate;

        /// Values shall be copied from this external `Value_set`.
        const Values* m_values_to_load;
      } m_load_val_as_if_parsed_args;

      /// Corresponds to Call_type::S_VALIDATE_STORED_VALS.
      struct
      {
        /// Each validator-enabled (via a #Declare_options_func) member in this structure shall be validated.
        const Values* m_values_to_validate;
      } m_validate_stored_vals_args;
    } m_args;
  }; // struct Declare_options_func_args

  /**
   * Short-hand for the ever-important callback passed to the main Option_set constructor.  The user of the class
   * need not understand the meanings of the args, because the FLOW_CFG_OPTION_SET_DECLARE_OPTION() macro will
   * take care of using them properly.
   *
   * @internal
   *
   * The idea is that the body of a user-provided callback of this type will consist of invoking
   * FLOW_CFG_OPTION_SET_DECLARE_OPTION() for each parseable option, in some consistent order, and that macro (which
   * we control) will invoke handling of that particular option -- whose stored type can be anything and different from
   * the other options', meaning we cannot iterate through them at runtime (nor is it possible at compile time without
   * some kind of insane meta-programming).  So, depending on what arguments we pass to this callback, the macro will --
   * upon performing an operation only a macro can do (namely `#ARG_whatever` and the like) -- forward those args
   * to a function templated on the type of the option's stored value, namely one of `declare_option_*()`,
   * scan_parsed_option(), load_option_value_as_if_parsed(), or validate_parsed_option().  That function will then
   * perform the needed handling for that particular option.
   *
   * See Declare_options_func_args for the possible args' meanings.
   */
  using Declare_options_func = Function<void (const Declare_options_func_args& args)>;

  // Constructors/destructor.

  /**
   * Constructs an option set in CANONICAL state with a default-valued values() payload and options declared
   * by synchronously invoking the callback `declare_opts_func()`.  See below for details on the latter.
   *
   * Post-condition: `values()` is equal to `Values()`; values_candidate() is null (so the state is initially
   * CANONICAL).  (Therefore `Value_set` no-args ctor must by definition be written so as to initialize all its
   * relevant members to their defaults.  Recall `Value_set` is a template parameter type with certain requirements.)
   *
   * Use `parse_*()` (once per config source) and `canonicalize_candidate()` (once) to move to PARSING state and back
   * to CANONICAL state respectively.
   *
   * ### Declaring options via `declare_opts_func()` callback ###
   * `declare_opts_func` above denotes an internally saved callback `move()`d from the corresponding arg
   * `declare_opts_func_moved`.  `declare_opts_func()` is also invoked synchronously
   * from within various relevant output APIs; their doc headers
   * mention it.  (E.g., invoking stream output `os << *this` will call `declare_opts_func()`.)
   *
   * Informally, the callback must declare every parseable (and therefore stream-printable-semantic-description-having)
   * option, linking it to some data member within a target `Values` whose address is passed to the callback as an
   * arg; in a consistent order; and as many times through the lifetime of `*this` (including during this ctor) as
   * `*this` deems necessary.  Use FLOW_CFG_OPTION_SET_DECLARE_OPTION() to describe all the options.
   * Formally, it must follow the following exact rules:
   *   - Its signature right above the `{ body }` must name its args *exactly* as shown in the value of the
   *     #Declare_options_func alias.
   *   - Its body must execute N>=1 invocations of the macro
   *     FLOW_CFG_OPTION_SET_DECLARE_OPTION() -- see its doc header -- each corresponding to a distinct
   *     data member of `Values`, always in the same order and the same N.  Clues as to how this works:
   *     - The naming of the args is so specific in order to make the macro minimally concise to use.
   *     - Macro machinery will auto-determine the main name of the option (as seen in config sources like files)
   *       based on the naming of the `m_` member.
   *     - The macro will call a public API that is not to be called directly, passing in various
   *       data just mentioned, plus the text description passed to it (if relevant) and the option name passed to it.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param nickname
   *        Brief string used for logging subsequently.
   * @param declare_opts_func_moved
   *        See above.
   */
  explicit Option_set(log::Logger* logger_ptr, util::String_view nickname,
                      Declare_options_func&& declare_opts_func_moved);

  // Methods.

  /**
   * Externally immutable internally stored canonical (current) config values as last constructed or parsed,
   * whichever happened more recently.
   *
   * @see mutable_values_copy() to get a copy.
   *
   * @return See above.
   */
  const Values& values() const;

  /**
   * Convenience method that heap-allocates a copy of the internally stored `values()` and wraps in a ref-counted
   * handle suitable for speedy passing around the rest of the application.
   *
   * @return See above.
   */
  Mutable_values_ptr mutable_values_copy() const;

  /**
   * Returns null in CANONICAL state; or in PARSING state a pointer to the not-yet-canonical values after the last
   * successful `parse_*()` API call.
   *
   * Rationale: It is supplied publicly in case the caller wans to log it or something; or perhaps to check
   * for current state (CANONICAL if and only it returns null).
   *
   * @return See above.
   */
  const Values* values_candidate() const;

  /**
   * Validates the current contents of values() using the validators `*this` `Option_set<Value_set>` is configured
   * to use via constructor.  If at least one option
   * is invalid according to a validator declared by a FLOW_CFG_OPTION_SET_DECLARE_OPTION() invocation in
   * `declare_opts_func()`, then this function shall indicate failure; else success.
   *
   * On failure throws an exception, if `success_or_null` is null; otherwise set `*success_or_null` to `false`.
   * On success simply returns or sets `*success_or_null` to `true` respectively.  In the exception case the message
   * will indicate every reasonable detail about the option value that went wrong.  This info is logged regardless.
   *
   * Informally, the use case driving the presence of this overload is discussed in the Validation section of our
   * class doc header; see that.  To restate: If you want to stringently ensure the defaults are themselves valid,
   * simply invoke this right after construction, at which point values() are by definition at their defaults.
   *
   * @param success_or_null
   *        If null exceptions mark failure; otherwise the pointed-to value shall indicate success or failure.
   */
  void validate_values(bool* success_or_null = 0) const;

  /**
   * Validates an arbitrary `Value_set`, as parseable by *an* `Option_set<Value_set>`, according to the
   * given option-registering function suitable for `Option_set<Value_set>` constructor.  If at least one option
   * is invalid according to a validator declared by a FLOW_CFG_OPTION_SET_DECLARE_OPTION() invocation in
   * `declare_opts_func()`, then this function shall indicate failure; else success.
   *
   * On failure throws an exception, if `success_or_null` is null; otherwise set `*success_or_null` to `false`.
   * On success simply returns or sets `*success_or_null` to `true` respectively.  In the exception case the message
   * will indicate every reasonable detail about the option value that went wrong.  This info is logged regardless.
   *
   * Use this, at least, if you've filled out a `Value_set` through manual assignment or some other source, rather
   * than parsing through an Option_set -- and don't even *use* an Option_set, as you don't parse from config --
   * but still want to check it for validity.
   *
   * You can also use the non-`static` overload to reuse the #Declare_options_func passed to an existing
   * parsing-capable `Option_set<Value_set>`.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param values_to_validate
   *        The values in this structure shall be validated in the same way as they are upon parsing
   *        by a hypothetical `Option_set<Value_set>` into whose ctor the same `declare_opts_func` were passed.
   * @param declare_opts_func
   *        See Option_set constructor.
   * @param success_or_null
   *        If null exceptions mark failure; otherwise the pointed-to value shall indicate success or failure.
   */
  static void validate_values(log::Logger* logger_ptr,
                              const Values& values_to_validate, const Declare_options_func& declare_opts_func,
                              bool* success_or_null = 0);

  /**
   * Validates an arbitrary `Value_set`, using the same validators `*this` `Option_set<Value_set>` is configured to use
   * when parsing config sources.  This essentially means using the `static` overload and passing to it
   * `declare_opts_func` equal to the one earlier passed by the user to `*this` constructor.  The success/failure
   * semantics are identical to the other overload's (see that doc header).
   *
   * You can also use the `static` overload, if you aren't at all parsing from config sources but still want to
   * validate.
   *
   * @param values_to_validate
   *        The values in this structure shall be validated in the same way as they are upon `this->parse_*()`ing from
   *        a config source.
   * @param success_or_null
   *        If null exceptions mark failure; otherwise the pointed-to value shall indicate success or failure.
   */
  void validate_values(const Values& values_to_validate, bool* success_or_null = 0) const;

  /**
   * Equivalent to `validate_values(success_or_null)` but validates `*values_candidate()` instead of values().
   * Behavior is undefined (assertion may trip) if not in PARSING mode currently (i.e., values_candidate() is null).
   *
   * Informally, the use case driving the presence of this overload is discussed in the Validation section of our
   * class doc header; see that.  To restate: If you want to ensure no invalid defaults or baseline values have
   * "gotten through" in the current PARSING state, then invoke this -- particularly just ahead of the decision to
   * either canonicalize_candidate() or reject_candidate().
   *
   * @param success_or_null
   *        If null exceptions mark failure; otherwise the pointed-to value shall indicate success or failure.
   */
  void validate_values_candidate(bool* success_or_null = 0) const;

  /**
   * Writes a multi-line user-suitable representation of the current values in a #Values object, at some point
   * perhaps initialized or parsed by `*this`, to the given stream.  This should typically be preceded by
   * a newline but not followed by one, unless one desires a blank line there.
   *
   * @param os
   *        Stream to which to serialize.
   * @param values_or_null
   *        Values to serialize; if null then we act as-if it's `&(values())`.
   */
  void values_to_ostream(std::ostream& os, const Values* values_or_null = 0) const;

  /**
   * Logs the given values payload using values_to_ostream().
   *
   * @param values_or_null
   *        See values_to_ostream().
   * @param summary
   *        Brief summary of what this payload represents.
   * @param sev
   *        Severity to use for the log message.
   */
  void log_values(util::String_view summary, const Values* values_or_null = 0, log::Sev sev = log::Sev::S_INFO) const;

  /**
   * Prints a multi-line help message about the set of options that `*this` can parse.  This should typically
   * be preceded by a newline but not followed by one, unless one desires a blank line there.
   *
   * @param os
   *        Stream to which to serialize.
   */
  void help_to_ostream(std::ostream& os) const;

  /**
   * Logs a multi-line help message using help_to_ostream().
   *
   * @param summary
   *        Brief summary of the help message.
   * @param sev
   *        Severity to use for the log message.
   */
  void log_help(util::String_view summary, log::Sev sev = log::Sev::S_INFO) const;

  /**
   * Enters into (from CANONICAL state) or continues in PARSING state by parsing the config source in the form of
   * the given file in the file-system.  On success values_candidate() is updated; on failure it is untouched.
   *
   * On failure throws an exception, if `success_or_null` is null; otherwise set `*success_or_null` to `false`.
   * On success simply returns or sets `*success_or_null` to `true` respectively.  Information is logged regardless.
   *
   * A failure may occur due to invalid contents in the config source.  A failure may occur due to a validator
   * condition failed (see FLOW_CFG_OPTION_SET_DECLARE_OPTION()).
   *
   * However: the latter applies only to settings actually specified in `cfg_path` file.  Any default or baseline or
   * otherwise-previously-set, but not overridden in `cfg_path`, values are not validated -- neither before nor after
   * scanning `cfg_path`.  You are, however, free to do so by next (or previously, or both) invoking
   * validate_values_candidate().  (If "previously," and not yet in PARSING state, then use validate_values().)
   * See Validation in class doc header for discussion.
   *
   * @see canonicalize_candidate() which will finalize the values_candidate() constructed so far.
   * @see reject_candidate() which will snap back to CANONICAL state rejecting any successful parsing done.
   *      In particular it would make sense in many cases to do this if parse_config_file() indicates failure.
   * @param cfg_path
   *        Path to parse.
   * @param allow_unregistered
   *        If `true`, if an unknown option is encountered it may be allowed (not considered a failure),
   *        subject to `allowed_unregistered_opts_or_empty`, though an INFO
   *        message is still logged for each; if `false` it is an error like any other illegal config setting.
   *        One reason for `true` is if another `Option_set<>` will be parsed from the same config source.
   *        Another is if there could be forward- or backward-compatibility concerns.
   * @param allowed_unregistered_opts_or_empty
   *        Meaningful only if `allow_unregistered == true`, this is the list of all option names (compared
   *        case-sensitively) that will not cause a validation error; or empty to allow *all* unknown options.
   * @param success_or_null
   *        If null exceptions mark failure; otherwise the pointed-to value shall indicate success or failure.
   */
  void parse_config_file(const fs::path& cfg_path, bool allow_unregistered,
                         bool* success_or_null = 0,
                         const boost::unordered_set<std::string>& allowed_unregistered_opts_or_empty = {});

  /**
   * Enters into (from CANONICAL state) or continues in PARSING state by simply setting `*values_candidate()`
   * to equal the #Values payload given as an argument.  Typically precedes other `parse_*()` calls such as
   * parse_config_file().  If already in PARSING state, note that any changes accumulated in `*values_candidate()`
   * so far will be overwritten entirely.
   *
   * The values in `src_values` are not checked for validity according to the validators configured
   * in the FLOW_CFG_OPTION_SET_DECLARE_OPTION() invocations in `declare_opts_func()` passed to ctor.
   * You are, however, free to do so by next invoking validate_values_candidate().
   * See Validation in class doc header for discussion.
   *
   * ### Rationale ###
   * This is useful, particularly, when one plans to repeatedly apply updates to one `*this`, but a certain *baseline*
   * state is desired before each update.  Consider the example of an Option_set that stores dynamically changeable
   * values.  Suppose each update consists only of a single `parse_config_file(F)` call, where `F` is some file that
   * might get changed at various times to deliver dynamic updates.  Then consider this series:
   *   -# Initial Option_set construction.  End state: `values() == Values()` (default).
   *   -# First update occurs: `parse_config_file(F)`, followed by canonicalize_candidate().
   *      End state: `values()` == defaults + changes in file `F` at time 1.
   *   -# Second update occurs: `parse_config_file(F)`, followed by canonicalize_candidate().
   *      End state: `values()` == defaults + changes in file `F` at time 1 +
   *      changes in file `F` at time 2.
   *   -# (etc.)
   *
   * In this case values() is incrementally changed by each dynamic update, as the file `F` keeps changing.
   * E.g., if at time 1 it contained only option A, and at time 2 it contained only option B, then values()
   * would contain both time-1 option A value and time-2 option B value -- even though the file `F` at
   * time 2 contains only the latter.  This *might* be what you want, but since it's fully incremental, there are
   * some usability landmines.  Mainly: If one looks at `F` contents at any given time, they can't know what the
   * resulting state would be; it depends on what updates preceded it.
   *
   * To resolve this, one can save a *baseline* state of values() by copy; and then apply it via this
   * parse_direct_values() call before parsing the file in each dynamic update.  The baseline state could just be
   * defaults (`Values()`), or it could come from some special "baseline" config file that is not `F` which one knows
   * to never change.  (Such a file could also typically store static config managed by a separate Option_set.)
   *
   * So then the sequence might become not parse_config_file(), canonicalize_candidate(), parse_config_file(),
   * canonicalize_candidate(), ...; but rather:
   *   -# Baseline parse: `parse_config_file(B)`, canonicalize_candidate().
   *   -# Save values() copy into `Values baseline`.
   *   -# Update 0: `parse_direct_values(baseline)`, `parse_config_file(F)`, canonicalize_candidate().
   *   -# Update 1: `parse_direct_values(baseline)`, `parse_config_file(F)`, canonicalize_candidate().
   *   -# Update 2: `parse_direct_values(baseline)`, `parse_config_file(F)`, canonicalize_candidate().
   *   -# ...
   * (The first `parse_direct_values(baseline)` here is a no-op but included for clarity/symmetry, as usually one
   * would just do the same thing for each update.)
   *
   * It is also sometimes desirable to "rewind" the state of values_candidate() (by first memorizing it, then
   * parse_config_file() or similar, then if some value in the resulting value set indicates the file should not
   * apply after all, parse_direct_values() to "undo."  Config_manager uses it when its multi-source feature is
   * engaged -- `commit = false`.)
   *
   * @param src_values
   *        The values set loaded into `*values_candidate()`, as-if parsed from some config source.
   */
  void parse_direct_values(const Values& src_values);

  /**
   * In PARSING state enters CANONICAL state, finalizing values() from values_candidate().
   * Any cumulative changes are INFO-logged on a per-changed-option basis; and if at least one option's value indeed
   * changed then `*change_detected` is set to `true` (else `false`).  (Leave the arg null, if you do not care.)
   *
   * @note Individual option validation should be done via the validation condition argument to
   *       FLOW_CFG_OPTION_SET_DECLARE_OPTION().  However, it may be necessary to perform a check for internal
   *       consistency among the final values.  The proper time to do this is just before calling
   *       canonicalize_candidate().  If the final check fails, typically one would instead call
   *       reject_candidate().
   *
   * @param change_detected
   *        If null, ignored; otherwise `*change_detected` is set to `true` if a setting changed; else `false`.
   */
  void canonicalize_candidate(bool* change_detected = 0);

  /**
   * In PARSING state, returns to CANONICAL state, as if no parse attempts have occurred.  In CANONICAL state, a no-op.
   * Calling this is typically a good idea when a `parse_*()` attempt indicates failure.
   */
  void reject_candidate();

  /**
   * Internal-through-macro helper function; the user shall not call this directly but only through
   * FLOW_CFG_OPTION_SET_DECLARE_OPTION() (see Option_set main constructor doc header).
   *
   * @internal
   * Loads an entry into #m_opts_for_parsing which will enable the parsing into `*target_value` from config sources
   * such as config files.
   * @endinternal
   *
   * @tparam Value
   *         Type of the value inside a #Values object.  It must be reasonably copyable.
   * @param name
   *        Main option name: as specified by the user in a config source.
   *        As of this writing FLOW_CFG_OPTION_SET_DECLARE_OPTION() uses macro magic to automatically form this
   *        from the identifier name within a #Values object (e.g., `Value_set::m_cool_option` => "cool-option").
   * @param target_value
   *        When deserializing a value from a config source, the bits shall be written there.
   *        This must point inside `m_values_candidate`.
   * @param value_default_if_no_acc
   *        Usually -- with regular (accumulating) options -- null; otherwise pointer to the default value for
   *        `*target_value` (as from `Values()`), inside `m_values_default`.  In the latter case (non-null) this
   *        indicates this is an option marked by the user as non-accumulating
   *        (see FLOW_CFG_OPTION_SET_DECLARE_OPTION_NO_ACC() and similar), meaning each time a config source
   *        (e.g., a file) is parsed `*target_value` is first reset to this default; then overwritten with the value in
   *        the config source if, else left at the default.  An accumulating option in the latter case would instead
   *        keep its existing value already in `*target_value`.
   * @param validator_func_moved
   *        Function F, such that F(V) shall be called from `parse_*()` when this option's successfully parsed value V
   *        is being validated before final storage in `m_values_candidate`.  Return `false` if the validation
   *        shall fail; otherwise return `true`.
   * @param validator_cond_str
   *        String containing the Boolean code that `validator_func_moved()` would need to evaluate to `true` to pass
   *        validation.  As of this writing FLOW_CFG_OPTION_SET_DECLARE_OPTION() uses macro magic to automatically form
   *        this from a Boolean expression fragment.
   */
  template<typename Value>
  void declare_option_for_parsing(util::String_view name, Value* target_value, const Value* value_default_if_no_acc,
                                  Function<bool (const Value& val)>&& validator_func_moved,
                                  util::String_view validator_cond_str);

  /**
   * Internal-through-macro helper function; the user shall not call this directly but only through
   * FLOW_CFG_OPTION_SET_DECLARE_OPTION() (see Option_set main constructor doc header).
   *
   * @internal
   * Loads an entry into #m_opts_for_help which will enable the proper help text to appear for that option
   * in help_to_ostream() or log_help().
   * @endinternal
   *
   * @tparam Value
   *         Type of the value inside a #Values object.  It must be reasonably copyable; and it must be supported by
   *         some version (including specialization(s) and overload(s)) of value_to_ostream().
   * @param name
   *        See declare_option_for_parsing().
   * @param value_default
   *        Default value to show to the user.
   * @param description
   *        The description text to show to the user.
   */
  template<typename Value>
  void declare_option_for_help(util::String_view name, const Value& value_default, util::String_view description);

  /**
   * Internal-through-macro helper function; the user shall not call this directly but only through
   * FLOW_CFG_OPTION_SET_DECLARE_OPTION() (see Option_set main constructor doc header).
   *
   * @internal
   * Scans a canonical (current) value in #m_values, comparing it to the (possibly) parsed one in
   * #m_iterable_values_candidate, erasing it from the latter if and only if it is unchanged.
   * @endinternal
   *
   * @tparam Value
   *         Type of the value inside a #Values object.  It must have a meaningful `==` operation.
   * @param name
   *        See declare_option_for_parsing().
   * @param canonical_value
   *        Current value in `m_values`.
   */
  template<typename Value>
  void scan_parsed_option(util::String_view name, const Value& canonical_value);

  /**
   * Internal-through-macro helper function; the user shall not call this directly but only through
   * FLOW_CFG_OPTION_SET_DECLARE_OPTION() (see Option_set main constructor doc header).
   *
   * @internal
   * To be called only when #m_parsing is `true`, sets a value in #m_values_candidate to equal one
   * in an external #Values object; and mirrors this in #m_iterable_values_candidate to maintain its invariant.
   * As a result, it is as-if that value was parsed from some config source such as a file.
   * Note that, as with parsing from actual config sources like files, one must still scan_parsed_option()
   * at some point to slim down #m_iterable_values_candidate to store merely the changed options.
   * @endinternal
   *
   * @tparam Value
   *         Type of the value inside a #Values object.  It must have a meaningful `=` operation.
   * @param name
   *        See declare_option_for_parsing().
   * @param target_value
   *        Target value inside `m_values_candidate`.
   * @param source_value
   *        Value to load into `*target_value` (as-if it was parsed from a config source).
   */
  template<typename Value>
  void load_option_value_as_if_parsed(util::String_view name, Value* target_value, const Value& source_value);

  /**
   * Return `true` if and only if the option-declaring function passed to the constructor declared no options.
   * This value is always the same for a given `*this`.
   *
   * ### Rationale ###
   * While likely of little value when the user instantiates an Option_set directly, this can be useful in
   * generic meta-programming, wherein multiple `Option_set`s might be instantiated at compile time, but the
   * coder doesn't know how many while coding.  For example Config_manager uses null() to bypass confusingly-logging
   * parse_config_file() calls on empty Option_set objects, such as if a static config set has no dynamic config
   * counterpart.
   *
   * @return See above.
   */
  bool null() const;

  /**
   * Returns set of all option names declared by the option-declaring function passed to the constructor.
   * This can be useful to supply to parse_config_file(), for example, when parsing another Option_set from the
   * same file: then other options can be passed to that function as not causing a parse error if encountered;
   * hence 2 or more `Option_set`s can parse_config_file() the same file despite having disjoint option name sets --
   * yet totally extraneous options in none of the `Option_set`s would cause an error as desired.
   *
   * This returns the same value (and reference) each time it is called.
   *
   * @return See above.
   */
  const boost::unordered_set<std::string>& option_names() const;

  // Data.

  /// See `nickname` ctor arg.
  const std::string m_nickname;

private:
  // Types.

  /**
   * Similar to a boost.program_options parsing results `variables_map`, which conceptually mirrors the results of a
   * parsing operation in the target `Values` structure,
   * mapping `string` to variable-type values.  As explained in the doc header for
   * Option_set::Opt_table, a parsing operation takes in a config source (config file, etc.)
   * and a *parsing-role* `Opt_table`; and outputs a `Values` and an object of the present type.
   * `Values` is the main point Option_set exists; it's what the application accesses, via simple
   * `->m_*` access of various values, to observe the parsed config -- so why do we need `Iterable_values`?
   *
   * The reason we use this is: we need to be able to iterate through all the options that were found in a
   * parsing pass, at least to detect changes as needed at least for dynamic option features (triggering
   * on-changed hooks, etc.).  (Requiring a manual `operator==(Value_set, Value_set)` or similar is too onerous
   * and error-prone for the implementor of the `Value_set`, as every `->m_` would require manual mention.)
   *
   * A `variables_map` is classically filled via the aforementioned parsing operation `opts::store()`;
   * and is itself an `std::map`.  Hence one can copy all the pairs from that into an #Iterable_values; and
   * an #Iterable_values can be manually pre-filled with pre-parse values from `Values` for easy
   * comparison after the `store()` (and other such algorithms).  Note, also, it is deep-copyable, as long as
   * all types stored within are deep-copyable.
   *
   * ### Rationale ###
   * - Why not simply use `opts::variables_map`?  It is after all very close to what this is.  Answer: I (ygoldfel)
   *   started that way, but its API is a bit hairy, and it's scary to have to depend on how boost.program_options
   *   will act when `variables_map` isn't empty before the parsing begins, for example (e.g., `store()` docs say
   *   it will NOT overwrite any "non-defaulted" value -- having to rely on such nuances doesn't seem great).
   *   It is safer, if slower (but we don't care), to treat a fresh `variables_map` that's `store()`d into as an
   *   intermediary.
   * - Why not `unordered_map`?  It's faster after all.  Answer: An alphabetical iteration order seems nice when
   *   (e.g.) iterating over the keys to show differences in values to humans.
   */
  using Iterable_values = std::map<std::string, boost::any>;

  // Data.

  /// See null().
  bool m_null;

  /// See option_names().
  boost::unordered_set<std::string> m_opt_names;

  /**
   * The #Opt_table-filling, or #m_iterable_values_candidate-scanning, callback passed to the constructor and invoked
   * with various input args (see #Declare_options_func doc header).  Depending on those args it will either fill
   * `Opt_table` in various roles (see #Opt_table doc header), or it will scan the values just loaded into
   * #m_iterable_values_candidate (e.g., for equality to the values in canonical #m_values).
   */
  Declare_options_func m_declare_opts_func;

  /// See values().
  Values m_values;

  /// Copy of values() when it is first constructed; i.e., the defaults.
  Values m_values_default;

  /// See values_candidate() -- `true` if and only if that returns non-null (PARSING state; else CANONICAL state).
  bool m_parsing;

  /// See values_candidate().  When that returns null (`m_parsing == false`) this value is meaningless.
  Values m_values_candidate;

  /**
   * Structure mirroring #m_values_candidate, where the values are the parsing-enabled members of #m_values_candidate,
   * and each value's key is it main config option name; if an option has not changed between #m_values and
   * #m_values_candidate (according to its `==` operator), then that option's key is omitted.  In particular
   * it is `.empty() == true` when in CANONICAL mode (`m_parsing == false`).
   */
  Iterable_values m_iterable_values_candidate;

  /**
   * The *parsing-role* #Opt_table (see extensive explanation in #Opt_table doc header).  Briefly: it's used
   * to actually load `*m_values_candidate` -- and ultimately #m_values -- with values parsed from config sources such
   * as files.  This must not change after the first `parse_*()` API begins execution.
   */
  Opt_table m_opts_for_parsing;

  /**
   * The *output-role* #Opt_table, help-text sub-role (see extensive explanation in #Opt_table doc header).
   * Briefly: it's used to output help text to output streams for user's convenience, in fact simply by piping it
   * to an `ostream` via `<<`.  This should not change after the first `parse_*()` API begins execution (lest the
   * user become confused).
   */
  Opt_table m_opts_for_help;
}; // class Option_set

// Free functions: in *_fwd.hpp.

// Macros.

/**
 * Macro the user must use, as prescribed in the flow::cfg::Option_set constructor doc header discussing
 * `declare_opts_func` in their flow::cfg::Option_set::Declare_options_func callback, when declaring each individual
 * config option as linked to a given data member of their config-bearing `struct` used as the `Value_set` in
 * `Option_set<Value_set>`.
 *
 * @note The resulting option name, as found in config sources such as config files, will be auto-computed from
 *       the string `#ARG_m_value`.  So if `ARG_m_value` is `m_cool_thing.m_cool_guy`, then the option name
 *       might be computed to be, say, `"cool-thing.cool-duration"`, with a config file line:
 *       `"cool-thing.cool-duration=2 milliseconds"`.
 * @note It may be desirable to *not* auto-name a given option; a known use case for this is legacy option names,
 *       if renaming is difficult in the field.  Use FLOW_CFG_OPTION_SET_DECLARE_OPTION_MANUALLY_NAMED() if indeed
 *       you want to custom-name your option.
 *
 * @param ARG_m_value
 *        Supposing `Value_set` is the template arg to `Option_set`, and `Value_set V` is being registered,
 *        then `ARG_m_value` is such that `V.ARG_m_value` is the scalar into which the particular config option shall
 *        be deserialized.  It shall be a scalar data member name, optionally within a compound data member, which is
 *        itself optionally within a compound, and so forth, within `Value_set V`.  Each optional compounding may
 *        be via simple object composition (`.`) or pointer dereference (`->`).
 *        Examples: `m_direct_member`, `m_group_obj.m_indirect_member`,
 *        `m_group.m_group1->m_pointed_group->m_cool_scalar`.  Reminder: each of these must be accessible within
 *        a `Value_set` via `.` compounding.  E.g.: `V.m_direct_member` a/k/a `Value_set::m_direct_member`.
 * @param ARG_description
 *        Description of the option as shown to the user in help text.  Use 1+ sentence(s), starting with capital and
 *        ending with punctuator.  Indicate all key semantics, including special values, but (1) try to keep to one
 *        sentence and line if possible; and (2) save extended discussion for the comment on the data member declaration
 *        itself.  Omit discussion of defaults; the default will be auto-printed and might change in code later.
 *        Reminder: This is a brief summary -- not a `man` page equivalent... but a `man` page equivalent *should* be
 *        in the actual code.
 * @param ARG_bool_validate_expr
 *        In a fashion similar to `assert()` arg, an expression convertible to `bool` such that if and only if it
 *        evaluates to `false` at the time of parsing a value for `ARG_m_value` from a config source, then the value
 *        is considered invalid, causing parse failure for that config source.  The expression shall assume that
 *        the parsed value is in the variable of type `Value` named `val`.  (If no further validation is needed, you
 *        may simply pass in `true`.)
 */
#define FLOW_CFG_OPTION_SET_DECLARE_OPTION(ARG_m_value, ARG_description, ARG_bool_validate_expr) \
  FLOW_CFG_OPTION_SET_DECLARE_OPTION_WITH_KNOBS(ARG_m_value, ARG_description, ARG_bool_validate_expr, false)

/**
 * Identical to FLOW_CFG_OPTION_SET_DECLARE_OPTION(), except the option is marked as *non-accumulating*, which
 * means that each time a config source (such as file) is parsed, this option's value is reset to default and then
 * only overwritten with a potential non-default value if explicitly specified in the config source.  (Otherwise
 * the option is accumulating, hence whatever value is already stored in the `Value_set` is left unchanged unless
 * specified in the config source.)
 *
 * @note This may be useful, in particular, when used in flow::cfg::Config_manager in multi-file updates for special
 *       options that implement conditionality.  It does not make sense to accumulate such options' values from file to
 *       file in a given update, so this `_NO_ACC` variant macro makes sense to use.
 *
 * @param ARG_m_value
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_description
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_bool_validate_expr
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 */
#define FLOW_CFG_OPTION_SET_DECLARE_OPTION_NO_ACC(ARG_m_value, ARG_description, ARG_bool_validate_expr) \
  FLOW_CFG_OPTION_SET_DECLARE_OPTION_WITH_KNOBS(ARG_m_value, ARG_description, ARG_bool_validate_expr, true)

/**
 * Identical to FLOW_CFG_OPTION_SET_DECLARE_OPTION(), but with support for setting a value at a container subscript.
 *
 * It works as follows.  Normally `ARG_m_value`, when stringified via `#` preprocessor trick, looks like a sequence of
 * `m_...` of identifiers joined with `.` and/or `->`.  Call these *terms*.  In this case, however, exactly one of the
 * terms -- but not the trailing term -- must end with the postfix `[...]`, where `...` may be 1 or more characters
 * excluding `]`.  As in FLOW_CFG_OPTION_SET_DECLARE_OPTION(), the option name will be auto-computed based on
 * the stringification; but with one extra step at the end: `[...]` shall be replaced by the added macro
 * arg `ARG_key`, which must be `ostream<<`able.
 *
 * This can be used to fill out fixed-key-set containers of `struct`s concisely.  Otherwise one would have to
 * tediously unroll the loop and manually provide the name via FLOW_CFG_OPTION_SET_DECLARE_OPTION_MANUALLY_NAMED().
 * Perhaps best shown by example:
 *
 *   ~~~
 *   // In Value_set:
 *     constexpr size_t S_N_GRAND_PARENTS = 4;
 *     struct Grand_parent { string m_name; bool m_male_else_female = false; };
 *     array<Grand_parent, S_N_GRAND_PARENTS> m_grand_parents;
 *   // When declaring options for Option_set<Value_set>:
 *     for (size_t idx = 0; idx != Value_set::S_N_GRAND_PARENTS; ++idx)
 *     {
 *       FLOW_CFG_OPTION_SET_DECLARE_OPTION_KEYED(m_grand_parents[idx].m_name, "The grandparent's name.", true, idx);
 *       FLOW_CFG_OPTION_SET_DECLARE_OPTION_KEYED(m_grand_parents[idx].m_male_else_female, "Their gender.", true, idx);
 *     }
 *   // Resulting config options:
 *     "grand-parents.0.name"
 *     "grand-parents.0.male-else-female"
 *     "grand-parents.1.name"
 *     "grand-parents.1.male-else-female"
 *     "grand-parents.2.name"
 *     "grand-parents.2.male-else-female"
 *     "grand-parents.3.name"
 *     "grand-parents.3.male-else-female"
 *   // Example file:
 *     [grand-parents.0]
 *     name=Alice Eve
 *     male-else-female=false
 *     [grand-parents.1]
 *     name=Ray Liotta
 *     male-else-female=true
 *     ...
 *   ~~~
 *
 * Tip: In this example `idx` is a number; and the container is an `array`.  However, it can be any container with
 * both keys and values; it just needs to always have the same key set after `Value_set` construction: `map`, `vector`,
 * etc.  Accordingly the key can be anything `ostream<<`-able (e.g., `string` works).  However -- behavior is undefined
 * if the value, when `ostream<<`ed, would not be accepted by boost.program_options as an option name postfix.
 * Informally: keep it to alphanumerics and underscores.
 *
 * @note Do not confuse this with the situation where *one* option is *itself* a `vector<T>`, where `T` is a scalar
 *       (from boost.program_options' point of view).  E.g., a `vector<int>` can simply be a single option, and
 *       the user can supply as many or as few elements as they want, at runtime (in config file, one option per line).
 *       For example the above `Value_set` can *itself* contain a `vector<>` in *each* `struct` in the array:
 *
 *   ~~~
 *   // In Value_set:
 *     struct Grand_parent { ...; vector<int> m_kid_ages; };
 *   // When declaring options for Option_set<Value_set>:
 *     for (size_t idx = 0; idx != Value_set::S_N_GRAND_PARENTS; ++idx)
 *     {
 *       ...
 *       FLOW_CFG_OPTION_SET_DECLARE_OPTION_KEYED(m_grand_parents[idx].m_kid_ages, "Children's age list.", true, idx);
 *     }
 *   // Resulting config options:
 *     ...
 *     "grand-parents.0.kid-ages"
 *     ...
 *     "grand-parents.1.kid-ages"
 *     ...
 *     "grand-parents.2.kid-ages"
 *     ...
 *     "grand-parents.3.kid-ages"
 *   // Example file:
 *     [grand-parents.0]
 *     name=Alice Eve
 *     male-else-female=false
 *     kid-ages=5
 *     kid-ages=7
 *     kid-ages=25
 *     [grand-parents.1]
 *     name=Ray Liotta
 *     male-else-female=true
 *     kid-ages=32
 *     ...
 *   ~~~
 *
 * @param ARG_m_value
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_description
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_bool_validate_expr
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_key
 *        See above.
 */
#define FLOW_CFG_OPTION_SET_DECLARE_OPTION_KEYED(ARG_m_value, ARG_description, ARG_bool_validate_expr, ARG_key) \
  FLOW_UTIL_SEMICOLON_SAFE \
    ( \
      const ::std::string FLOW_CFG_SET_DECL_OPT_KEYED_name \
        = ::flow::cfg::value_set_member_id_to_opt_name_keyed(#ARG_m_value, ARG_key); \
      FLOW_CFG_OPTION_SET_DECLARE_OPTION_MANUALLY_NAMED \
        (ARG_m_value, FLOW_CFG_SET_DECL_OPT_KEYED_name.c_str(), ARG_description, ARG_bool_validate_expr, false); \
    )

/**
 * Identical to FLOW_CFG_OPTION_SET_DECLARE_OPTION_KEYED_NO_ACC(), except the option is marked as *non-accumulating*
 * in the same sense as for FLOW_CFG_OPTION_SET_DECLARE_OPTION_NO_ACC().
 *
 * @see FLOW_CFG_OPTION_SET_DECLARE_OPTION_NO_ACC() for a brief explanation of how non-accumulating options work.
 *
 * @param ARG_m_value
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_description
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_bool_validate_expr
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_key
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION_KEYED().
 */
#define FLOW_CFG_OPTION_SET_DECLARE_OPTION_KEYED_NO_ACC(ARG_m_value, ARG_description, ARG_bool_validate_expr, ARG_key) \
  FLOW_UTIL_SEMICOLON_SAFE \
    ( \
      const ::std::string FLOW_CFG_SET_DECL_OPT_KEYED_name \
        = ::flow::cfg::value_set_member_id_to_opt_name_keyed(#ARG_m_value, ARG_key); \
      FLOW_CFG_OPTION_SET_DECLARE_OPTION_MANUALLY_NAMED \
        (ARG_m_value, FLOW_CFG_SET_DECL_OPT_KEYED_name.c_str(), ARG_description, ARG_bool_validate_expr, true); \
    )

/**
 * As of this writing is identical to either FLOW_CFG_OPTION_SET_DECLARE_OPTION() or
 * FLOW_CFG_OPTION_SET_DECLARE_OPTION_NO_ACC(), depending on the value of `ARG_no_accumulation` argument.
 *
 * Rationale: Currently we don't necessarily expect this to be widely used directly in place of the aforementioned two
 * macros, but it seems reasonable to have a single macro with all various knobs given as parameters with certain
 * knob combinations potentially invoked via macro(s) based on this one.  As of this
 * writing there is just the one extra knob, `ARG_no_accumulation`, but in the future we might expand to more
 * variations in which case more args would be added here.
 * 
 * @param ARG_m_value
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_description
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_bool_validate_expr
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_no_accumulation
 *        `bool` such that `false` causes FLOW_CFG_OPTION_SET_DECLARE_OPTION() behavior, while `true`
 *        causes FLOW_CFG_OPTION_SET_DECLARE_OPTION_NO_ACC() behavior.
 */
#define FLOW_CFG_OPTION_SET_DECLARE_OPTION_WITH_KNOBS(ARG_m_value, ARG_description, ARG_bool_validate_expr, \
                                                      ARG_no_accumulation) \
  FLOW_UTIL_SEMICOLON_SAFE \
    ( \
      const ::std::string FLOW_CFG_SET_DECL_OPT_name = ::flow::cfg::value_set_member_id_to_opt_name(#ARG_m_value); \
      FLOW_CFG_OPTION_SET_DECLARE_OPTION_MANUALLY_NAMED \
        (ARG_m_value, FLOW_CFG_SET_DECL_OPT_name.c_str(), ARG_description, ARG_bool_validate_expr, \
         ARG_no_accumulation); \
    )

/**
 * Identical to FLOW_CFG_OPTION_SET_DECLARE_OPTION_WITH_KNOBS(), except the user must specify the option's string name
 * manually as an argument to the functional macro.  (FLOW_CFG_OPTION_SET_DECLARE_OPTION_WITH_KNOBS() itself
 * is basically FLOW_CFG_OPTION_SET_DECLARE_OPTION() plus certain knobs which the former sets to commonly-used values
 * for concision.)
 *
 * This macro, which can be used directly but normally is not, is internally invoked (potentially indirectly) by any
 * other `FLOW_CFG_OPTION_SET_DECLARE_OPTION*()` macro.  As such it is the essential core such macro.
 * 
 * Informally: it is best to avoid its direct use, as it can break the auto-naming conventions maintained by
 * FLOW_CFG_OPTION_SET_DECLARE_OPTION() and similar.  That said a couple of use cases might be:
 *   - FLOW_CFG_OPTION_SET_DECLARE_OPTION_KEYED() uses it in its impl to add a convention for container subscripts.
 *   - One can specify a legacy option-name alias.  (E.g., one can FLOW_CFG_OPTION_SET_DECLARE_OPTION()
 *     an option and then FLOW_CFG_OPTION_SET_DECLARE_OPTION_MANUALLY_NAMED() a manually-named option targeting
 *     the same `Value_set` member.  Then either the auto-generated name or the legacy name can be used in a config
 *     source, until the latter is deprecated-out successfully in the field.)
 *
 * @param ARG_opt_name_c_str
 *        The option's manually specified string name.  Type: directly convertible to `const char*`; typically
 *        a string literal.
 * @param ARG_m_value
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_description
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_bool_validate_expr
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION().
 * @param ARG_no_accumulation
 *        See FLOW_CFG_OPTION_SET_DECLARE_OPTION_WITH_KNOBS().
 */
#define FLOW_CFG_OPTION_SET_DECLARE_OPTION_MANUALLY_NAMED(ARG_m_value, ARG_opt_name_c_str, \
                                                          ARG_description, ARG_bool_validate_expr, \
                                                          ARG_no_accumulation) \
  FLOW_UTIL_SEMICOLON_SAFE \
    ( \
      char const * const FLOW_CFG_SET_DECL_OPT_MANUAL_name_c_str = ARG_opt_name_c_str; \
      /* Subtlety: This is only safe to use here synchronously. */ \
      const ::flow::util::String_view FLOW_CFG_SET_DECL_OPT_MANUAL_name_view(FLOW_CFG_SET_DECL_OPT_MANUAL_name_c_str); \
      const bool FLOW_CFG_SET_DECL_OPT_MANUAL_no_acc = ARG_no_accumulation; \
      switch (args.m_call_type) \
      { \
      case ::flow::cfg::Option_set_base::Declare_options_func_call_type::S_FILL_PARSING_ROLE_OPT_TABLE: \
      { \
        using Value = decltype(args.m_args.m_fill_parsing_role_opt_table_args.m_values_candidate->ARG_m_value); \
        /* Subtlety: Copy input NUL-terminated char* into lambda string capture just in case they give us */ \
        /* not the usual `static` string literal as ARG_opt_name_c_str. */ \
        flow::Function<bool (const Value&)> FLOW_CFG_SET_DECL_OPT_MANUAL_validator_func \
          = [FLOW_CFG_SET_DECL_OPT_MANUAL_name_std_str = std::string(FLOW_CFG_SET_DECL_OPT_MANUAL_name_c_str)] \
              ([[maybe_unused]] const Value& val) -> bool { return ARG_bool_validate_expr; }; \
        args.m_args.m_fill_parsing_role_opt_table_args.m_option_set \
          ->declare_option_for_parsing \
              (FLOW_CFG_SET_DECL_OPT_MANUAL_name_view, \
               &args.m_args.m_fill_parsing_role_opt_table_args.m_values_candidate->ARG_m_value, \
               /* Default is irrelevant if option accumulates from parse to parse.  Pass null. */ \
               /* Default from Value_set() shall be in effect at construction time, but that's it. */ \
               /* However if it's set as a non-accumulating option via knob, then pass-through the default: */ \
               /* each parse via boost.program_options shall first reset option to that default; then if present */ \
               /* overwrite that default.  Hence the value from any preceding parse is always forgotten. */ \
               FLOW_CFG_SET_DECL_OPT_MANUAL_no_acc \
                 ? &args.m_args.m_fill_parsing_role_opt_table_args.m_values_default_no_acc->ARG_m_value \
                 : nullptr, \
               ::std::move(FLOW_CFG_SET_DECL_OPT_MANUAL_validator_func), \
               ::flow::util::String_view(#ARG_bool_validate_expr)); \
        break; \
      } \
      case ::flow::cfg::Option_set_base::Declare_options_func_call_type::S_FILL_OUTPUT_HELP_ROLE_OPT_TABLE: \
        args.m_args.m_fill_output_help_role_opt_table_args.m_option_set \
          ->declare_option_for_help \
              (FLOW_CFG_SET_DECL_OPT_MANUAL_name_view, \
               args.m_args.m_fill_output_help_role_opt_table_args.m_values_default->ARG_m_value, \
               ARG_description); \
        break; \
      case ::flow::cfg::Option_set_base::Declare_options_func_call_type::S_FILL_OUTPUT_CURRENT_ROLE_OPT_TABLE: \
        ::flow::cfg::Option_set_base::declare_option_for_output \
          (FLOW_CFG_SET_DECL_OPT_MANUAL_name_view, \
           args.m_args.m_fill_output_current_role_opt_table_args.m_target_opts, \
           args.m_args.m_fill_output_current_role_opt_table_args.m_values_default->ARG_m_value, \
           args.m_args.m_fill_output_current_role_opt_table_args.m_values_current->ARG_m_value, \
           ARG_description); \
        break; \
      case ::flow::cfg::Option_set_base::Declare_options_func_call_type::S_COMPARE_PARSED_VALS: \
        args.m_args.m_compare_parsed_vals_args.m_option_set \
          ->scan_parsed_option \
              (FLOW_CFG_SET_DECL_OPT_MANUAL_name_view, \
               args.m_args.m_compare_parsed_vals_args.m_option_set->values().ARG_m_value); \
        break; \
      case ::flow::cfg::Option_set_base::Declare_options_func_call_type::S_LOAD_VALS_AS_IF_PARSED: \
        args.m_args.m_load_val_as_if_parsed_args.m_option_set \
          ->load_option_value_as_if_parsed \
              (FLOW_CFG_SET_DECL_OPT_MANUAL_name_view, \
               &args.m_args.m_load_val_as_if_parsed_args.m_values_candidate->ARG_m_value, \
               args.m_args.m_load_val_as_if_parsed_args.m_values_to_load->ARG_m_value); \
        break; \
      case ::flow::cfg::Option_set_base::Declare_options_func_call_type::S_VALIDATE_STORED_VALS: \
      { \
        /* Set up validator func similarly to above; see those comments. */ \
        using Value = decltype(args.m_args.m_validate_stored_vals_args.m_values_to_validate->ARG_m_value); \
        flow::Function<bool (const Value&)> FLOW_CFG_SET_DECL_OPT_MANUAL_validator_func \
          = [FLOW_CFG_SET_DECL_OPT_MANUAL_name_std_str = std::string(FLOW_CFG_SET_DECL_OPT_MANUAL_name_c_str)] \
              ([[maybe_unused]] const Value& val) -> bool { return ARG_bool_validate_expr; }; \
        /* Throw if invalid; else no-op. */ \
        ::flow::cfg::Option_set_base::validate_parsed_option \
          (FLOW_CFG_SET_DECL_OPT_MANUAL_name_view, \
           args.m_args.m_validate_stored_vals_args.m_values_to_validate->ARG_m_value, \
           ::std::move(FLOW_CFG_SET_DECL_OPT_MANUAL_validator_func), \
           ::flow::util::String_view(#ARG_bool_validate_expr)); \
        break; \
      } \
      /* No `default:` intentionally: most compilers should catch a missing enum value and warn. */ \
      } \
    ) // FLOW_UTIL_SEMICOLON_SAFE()

// Template implementations.

template<typename Value_set>
Option_set<Value_set>::Option_set(log::Logger* logger_ptr, util::String_view nickname,
                                  Declare_options_func&& declare_opts_func_moved) :
  log::Log_context(logger_ptr, Flow_log_component::S_CFG),
  m_nickname(nickname),
  m_null(true),
  m_declare_opts_func(std::move(declare_opts_func_moved)),
  m_parsing(false)
{
  // Avoid INFO logging, in case they're only creating us to print some help and exit.  Not essential info here.
  FLOW_LOG_TRACE("Option_set[" << *this << "]: Created with default values payload.  Options table setup begins.");

  // Refer to the doc headers for Opt_table and Declare_options_func to understand the next couple of calls.

  // Load the *parsing-role* Opt_table m_opts_for_parsing.  Its contents won't change after this.
  Declare_options_func_args args;
  args.m_call_type = Declare_options_func_args::Call_type::S_FILL_PARSING_ROLE_OPT_TABLE;
  args.m_args.m_fill_parsing_role_opt_table_args = { this, &m_values_candidate, &m_values_default };
  m_declare_opts_func(args);
  // m_null is now permanently true or false.

  /* Load the *output-role* (help text sub-role) Opt_table.  The defaults source is the current Value_set m_values
   * itself (since it was just loaded up with defaults by definition).  It again won't change after this. */
  args.m_call_type = Declare_options_func_args::Call_type::S_FILL_OUTPUT_HELP_ROLE_OPT_TABLE;
  args.m_args.m_fill_output_help_role_opt_table_args = { this, &m_values_default };
  m_declare_opts_func(args);

  FLOW_LOG_TRACE("Option_set[" << *this << "]: Options table setup finished.  Null set? = [" << null() << "].");
} // Option_set::Option_set()

template<typename Value_set>
bool Option_set<Value_set>::null() const
{
  return m_null;
}

template<typename Value_set>
const Value_set& Option_set<Value_set>::values() const
{
  return m_values;
}

template<typename Value_set>
typename Option_set<Value_set>::Mutable_values_ptr Option_set<Value_set>::mutable_values_copy() const
{
  return Mutable_values_ptr(new Values(values()));
}

template<typename Value_set>
const Value_set* Option_set<Value_set>::values_candidate() const
{
  return m_parsing ? &m_values_candidate : 0;
}

template<typename Value_set>
void Option_set<Value_set>::values_to_ostream(std::ostream& os, const Values* values_or_null) const
{
  /* Provide more real estate than the default 80, since this is chiefly for value output, probably in logs.
   * And just put the description on the next line always. */
  constexpr unsigned int LINE_LENGTH = 1000;
  constexpr unsigned int DESC_LENGTH = LINE_LENGTH - 1;

  Opt_table opts_for_output(LINE_LENGTH, DESC_LENGTH);

  Declare_options_func_args args;
  args.m_call_type = Declare_options_func_args::Call_type::S_FILL_OUTPUT_CURRENT_ROLE_OPT_TABLE;
  args.m_args.m_fill_output_current_role_opt_table_args
    = { &opts_for_output, &m_values_default,
        values_or_null ? values_or_null : &m_values };
  m_declare_opts_func(args);

  os << opts_for_output; // Leverage boost.program_options to print the nicely formatted current+defaults+descriptions.
}

template<typename Value_set>
void Option_set<Value_set>::log_values(util::String_view summary, const Value_set* values_or_null, log::Sev sev) const
{
  using util::String_ostream;
  using std::flush;

  String_ostream os;
  values_to_ostream(os.os(), values_or_null);
  os.os() << flush;
  FLOW_LOG_WITH_CHECKING(sev, "Option_set[" << *this << "]: Values payload [" << summary << "]:\n" << os.str());
}

template<typename Value_set>
void Option_set<Value_set>::help_to_ostream(std::ostream& os) const
{
  os << m_opts_for_help; // Leverage boost.program_options to print the nicely formatted defaults+descriptions.
}

template<typename Value_set>
void Option_set<Value_set>::log_help(util::String_view summary, log::Sev sev) const
{
  using util::String_ostream;
  using std::flush;

  String_ostream os;
  help_to_ostream(os.os());
  os.os() << flush;
  FLOW_LOG_WITH_CHECKING(sev, "Config usage [" << summary << "]:\n" << os.str());
}

template<typename Value_set>
const boost::unordered_set<std::string>& Option_set<Value_set>::option_names() const
{
  return m_opt_names;
}

template<typename Value_set>
template<typename Value>
void Option_set<Value_set>::declare_option_for_parsing(util::String_view name_view,
                                                       Value* target_value, const Value* value_default_if_no_acc,
                                                       Function<bool (const Value& val)>&& validator_func_moved,
                                                       util::String_view validator_cond_str_view)
{
  using util::String_ostream;
  using opts::value;
  using std::string;
  using std::flush;

  assert(target_value);
  string name(name_view);

  m_opt_names.insert(name);

  // Avoid INFO logging, in case they're only creating us to print some help and exit.  Not essential info here.
  FLOW_LOG_TRACE("Option_set[" << *this << "]: Options table of parsing-role: "
                 "Declaring parsing-capable option [" << name << "]; raw shallow size [" << sizeof(Value) << "].");

  /* *Parsing-role* target is within m_values_candidate.  No default: if not in config, it'll just be left untouched.
   * The validator function we create shall throw on validation failure, having been passed `const Value& val` by the
   * options engine. */
  const auto val_spec = value<Value>(target_value)
                          ->notifier(throw_on_invalid_func(name_view, std::move(validator_func_moved),
                                                           validator_cond_str_view));
  /* However: if non-accumulating mode is enabled then, in fact, set the value from Value_set()
   * as default_value(); so that starting to parse a config source (e.g., config file) shall always reset to default
   * first instead of accumulating from a previous parse (if any).  This matters for a given parse only if this
   * option is not specified in that config source. */
  if (value_default_if_no_acc)
  {
    // Subtlety re. default_value(2 args): Same as in declare_option_for_help().
    String_ostream default_str_os;
    value_to_ostream(default_str_os.os(), *value_default_if_no_acc);
    default_str_os.os() << flush;

    val_spec->default_value(*value_default_if_no_acc, default_str_os.str());
  }

  m_opts_for_parsing.add_options()(name.c_str(), val_spec);

  m_null = false;
} // Option_set::declare_option_for_parsing()

template<typename Value>
Function<void (const Value& val)> Option_set_base::throw_on_invalid_func
                                    (util::String_view name_view,
                                     Function<bool (const Value& val)>&& validator_func_moved,
                                     util::String_view validator_cond_str_view) // Static.
{
  using error::Runtime_error;
  using util::String_ostream;
  using std::string;
  using std::flush;

  return [validator_func = std::move(validator_func_moved),
          validator_cond_str = string(validator_cond_str_view),
          name = string(name_view)]
           (const Value& val)
  {
    if (!(validator_func(val)))
    {
      String_ostream msg_os;
      msg_os.os() << "Option [" << name << "]: "
                     "Validation failed; the following condition must hold: [" << validator_cond_str << "].  "
                     "Option value `val` = [";
      value_to_ostream(msg_os.os(), val);
      msg_os.os() << "]." << flush;
      throw Runtime_error(msg_os.str());
    }
  };
} // Option_set_base::throw_on_invalid_func()

template<typename Value_set>
template<typename Value>
void Option_set<Value_set>::declare_option_for_help(util::String_view name,
                                                    const Value& value_default, util::String_view description)
{
  using util::String_ostream;
  using opts::value;
  using std::string;
  using std::flush;

  assert(!description.empty());

  /* Subtlety: The is also a ->default_value(1 arg) overload, which always uses ostream<< for output, but (as
   * elsewhere) run it through our possible output massaging machinery by providing the string to output.  Hence use
   * the 2-arg version which takes an explicit string.  @todo Could also implement a proxy type with an operator<<
   * ostream overload to make these call sites more elegant/concise. */
  String_ostream default_str_os;
  value_to_ostream(default_str_os.os(), value_default);
  default_str_os.os() << flush;

  m_opts_for_help.add_options()
    (string(name).c_str(),
     // *Output-role* has no target; it does output, not input.
     value<Value>()
       ->default_value(value_default, default_str_os.str()),
     string(description).c_str());

  // No need to log: logs are only interesting when setting up the parsing table.  This'd just be verbose/redundant.
} // Option_set::declare_option_for_help()

template<typename Value>
void Option_set_base::declare_option_for_output(util::String_view name, Opt_table* target_opts,
                                                const Value& value_default, const Value& current_value,
                                                util::String_view description) // Static.
{
  using util::String_ostream;
  using opts::value;
  using std::string;
  using std::flush;

  assert(!description.empty());

  /* This is similar to declare_option_for_help() with 2 differences:
   *   - The target is passed to us, *target_opts, as they will be printing that, and it depends on a current value
   *     which changes over time, hence we can't just plant this in some data member the way we did with
   *     m_opts_for_help.
   *   - In addition to the default, add the current value into the output by massaging the description text.
   *     We are printing a current value which will go into the description text here, while the default will
   *     go into the default slot of the "usage" output to ostream. */
  String_ostream description_os;

  // Visually match how boost.program_options prints the default value above us.

  description_os.os() << "    (";
  /* Always pass any user output of Value_set members through our possible specialization/overload around ostream<<,
   * as we may want to apply some us-specific massaging. */
  value_to_ostream(description_os.os(), current_value);
  description_os.os() << ") <==current | default==^\n"
                         "      " << description << flush;

  // Subtlety re. default_value(2 args): Same as in declare_option_for_help().
  String_ostream default_str_os;
  value_to_ostream(default_str_os.os(), value_default);
  default_str_os.os() << flush;

  target_opts->add_options()
    (string(name).c_str(),
     // *Output-role* has no target; it does output, not input.
     value<Value>()
       ->default_value(value_default, default_str_os.str()),
     description_os.str().c_str());

  // No need to log: logs are only interesting when setting up the parsing table.  This'd just be verbose/redundant.
} // Option_set_base::declare_option_for_output()

template<typename Value_set>
template<typename Value>
void Option_set<Value_set>::scan_parsed_option(util::String_view name_view, const Value& canonical_value)
{
  using boost::any_cast;
  using std::string;
  string name(name_view);

  /* In this mode we are basically to check whether the value just parsed for `name`, which is in
   * m_iterable_values_candidate[name], is actually *different* from the current canonical value, which is
   * in canonical_value.  If so we are to leave it there; if not, we are to remove it,
   * thus maintaining the invariant wherein m_iterable_values_candidate mirrors m_values_candidate, except for those
   * options whose values have not changed. */

  const auto it = m_iterable_values_candidate.find(name);
  if (it == m_iterable_values_candidate.end())
  {
    return; // Option was not parsed.
  }
  // else

  const auto& new_val = any_cast<const Value&>(it->second);
  const auto& old_val = canonical_value;

  if (old_val == new_val)
  {
    m_iterable_values_candidate.erase(name);
  }
  return;
} // Option_set::scan_parsed_option()

template<typename Value_set>
template<typename Value>
void Option_set<Value_set>::load_option_value_as_if_parsed(util::String_view name_view,
                                                           Value* target_value,
                                                           const Value& source_value)
{
  using std::string;
  string name(name_view);

  assert(target_value);
  assert(m_parsing);

  /* Getting here is hairy to understand, but our task is simple: Modify *this as-if
   * *target_value -- which is a member of m_values_candidate -- was parsed from some config source, yielding
   * in valid value source_value.  We must update our state (given that m_parsing is true) accordingly. */

  // This is the obvious part: set the thing inside m_values_candidate.
  *target_value = source_value;

  // Now mirror it in m_iterable_values_candidate to maintain that invariant.
  m_iterable_values_candidate[name] = source_value;

  /* That's it.  Just picture it as if <name_view contents>=<encoding of source_value> was in a config file,
   * and parse_config_file() just did a store()/notify() of that line alone and hence updated
   * m_iterable_values_candidate accordingly.  What we did above equals that; it's just much simpler to do
   * than actual parsing, since we have the value given to us directly in source_value. */
}

template<typename Value>
void Option_set_base::validate_parsed_option(util::String_view name_view, const Value& value,
                                             Function<bool (const Value& val)>&& validator_func_moved,
                                             util::String_view validator_cond_str_view) // Static.
{
  const auto validate_or_throw_func = throw_on_invalid_func(name_view, std::move(validator_func_moved),
                                                            validator_cond_str_view);
  validate_or_throw_func(value);
} // Option_set_base::validate_parsed_option()

template<typename Value_set>
void Option_set<Value_set>::parse_config_file
       (const fs::path& cfg_path, bool allow_unregistered, bool* success_or_null,
        const boost::unordered_set<std::string>& allowed_unregistered_opts_or_empty)
{
  using error::Runtime_error;
  using util::ostream_op_string;
  using util::key_exists;
  using opts::notify;
  using opts::store;
  using opts::variables_map;
  using boost::system::system_category;
  using fs::ifstream;
  using std::exception;
  using std::string;

  // If they invoked us in non-throwing mode, call selves recursively in throwing mode and catch it.
  if (success_or_null)
  {
    try
    {
      parse_config_file(cfg_path, allow_unregistered, 0, allowed_unregistered_opts_or_empty);
    }
    catch (const exception& exc)
    {
      // We already logged on exception; be quiet.
      *success_or_null = false;
      return;
    }
    *success_or_null = true;
    return;
  }
  // else: Throw an exception on error.  Perf not a real concern.

  if (!m_parsing)
  {
    m_parsing = true;
    FLOW_LOG_INFO("Option_set[" << *this << "]: State CANONICAL: Request to parse file [" << cfg_path << "]; "
                  "entering state PARSING.  Initial candidate values payload initialized from current canonical "
                  "values payload.  Details follow (TRACE log level).");

    assert(m_iterable_values_candidate.empty());
  }
  else
  {
    FLOW_LOG_INFO("Option_set[" << *this << "]: State PARSING: Request to parse file [" << cfg_path << "]; "
                  "continuing by parsing this next config source.  "
                  "The values payload going into it is as it was after the previous config source.  "
                  "Details follow (TRACE log level).");
  }

  log_values("pre-parsing candidate config", &m_values_candidate, log::Sev::S_TRACE);

  ifstream ifs(cfg_path);
  if (!ifs)
  {
    const Error_code sys_err_code(errno, system_category());
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();

    throw Runtime_error(sys_err_code, ostream_op_string("Could not open file [", cfg_path, "]."));
  }
  // else

  /* parse_() can throw on invalid option values and so on!  In case it managed to touch m_values_candidate -- save it.
   * Our doc header(s) guarantee config payload is untouched unless parsing succeeds.
   * @todo This may not be necessary.  boost.program_options is solid, but there's an uncommon (for Boost) looseness
   * to the documentation, including about error handling, so we don't find it trustworthy enough not to do this and
   * stay maintainable.  Still consider revisiting this and not catching the exception and not worrying about
   * backing this up -- if that's really how it works. */
  const auto values_candidate_backup = m_values_candidate;
  opts::parsed_options parsed_options(&m_opts_for_parsing);
  try
  {
    parsed_options = opts::parse_config_file(ifs, m_opts_for_parsing, allow_unregistered);
  }
  catch (const exception& exc)
  {
    m_values_candidate = values_candidate_backup;
    // m_iterable_values_candidate is set below, so it hasn't been touched from this config source yet.

    FLOW_LOG_WARNING("Option_set[" << *this << "]: State PARSING: "
                     "Parsing file [" << cfg_path << "]: Exception occurred; parse failed.  Values payload untouched.  "
                     "Message = [" << exc.what() << "].");
    throw;
  }
  // Got here?  No problem parsing (but we haven't validated).  First a detour:

  /* If we were told to allow at least some (possibly all) unregistered options:
   * Log (at appropriate level) the unregistered options; log only the name part of the tokens for each (ignoring
   * any values); and fail if an un-approved unknown option shows up. */
  if (allow_unregistered)
  {
    for (const auto& option : parsed_options.options)
    {
      if (option.unregistered)
      {
        assert(!option.original_tokens.empty());
        const string opt_name = option.original_tokens.front();

        if (allowed_unregistered_opts_or_empty.empty())
        {
          // Do not use INFO level; this can get verbose.
          FLOW_LOG_TRACE("Option_set[" << *this << "]: State PARSING: Ignoring unregistered option named "
                         "[" << opt_name << "].  We were not given an approved-list.  Perhaps this file contains "
                         "other option sets, or it has to do with forward/backward-compatibility.");
        }
        else
        {
          if (!key_exists(allowed_unregistered_opts_or_empty, opt_name))
          {
            // Same deal as above.
            m_values_candidate = values_candidate_backup;

            FLOW_LOG_WARNING("Option_set[" << *this << "]: State PARSING: Unregistered option named "
                             "[" << opt_name << "] is not approved; parse failed.  Values payload untouched.");
            throw Runtime_error(ostream_op_string("Unregistered option named [", opt_name,
                                                  "] is not approved; parse failed."));
          }
          // else

          // Do not use INFO level; this can get verbose.
          FLOW_LOG_TRACE("Option_set[" << *this << "]: State PARSING: Ignoring unregistered option named "
                         "[" << opt_name << "]; it is in the approved-list.  Perhaps this file contains "
                         "other option sets.");
        } // if (!allowed_unregistered_opts_or_empty.empty())
      } // if (option.unregistered)
    } // for (option : parsed_options.options)
    // We don't care about them further.
  }
  // else { Apparently there were no unknown options encountered, as it didn't throw when parsing. }

  // Store into a variables_map (a glorified map<string, boost::any>) and m_values_candidate.

  FLOW_LOG_TRACE("Option_set[" << *this << "]: State PARSING: Parsed general format successfully.  "
                 "Will now parse each option's value.");

  /* As explained in ###Rationale### in Iterable_values doc header, we use a fresh opts::variables_map as an
   * intermediary and load the results from that into our Iterable_values, overwriting any values already there
   * that were indeed set during this parse_*().  In other words, for every configured option N, it either does not
   * exist in m_iterable_values_candidate (in which case the value is still at its canonical m_values value), or it does
   * as m_iterable_values_candidate[N] (in which case it has been parsed at least once in a previous config source's
   * parse step *and* is different from m_values).  Then after store() it will either be created or changed or remain
   * unchanged.  In the latter case we should remove it to maintain the invariant, wherein only keys for
   * changed-from-canonical options exist. */
  variables_map fresh_iterable_values;
  try
  {
    store(parsed_options, fresh_iterable_values);
  }
  catch (const exception& exc)
  {
    // Same deal as above.
    m_values_candidate = values_candidate_backup;

    FLOW_LOG_WARNING("Option_set[" << *this << "]: State PARSING: "
                     "Parsed file [" << cfg_path << "]: Exception occurred; individual option parsing failed.  "
                     "Values payload untouched.  Message = [" << exc.what() << "].");
    throw;
  }

  FLOW_LOG_TRACE("Option_set[" << *this << "]: State PARSING: Parsed each individual option successfully.  "
                 "Will now further validate each option value logically and store on success.");

  /* notify() will finish things by loading m_values_candidate members and validating them.
   * notify() can throw, particularly due to our own custom validating notifiers (see declare_option_for_parsing()). */
  try
  {
    notify(fresh_iterable_values);
  }
  catch (const exception& exc)
  {
    // Same deal as above.
    m_values_candidate = values_candidate_backup;

    FLOW_LOG_WARNING("Option_set[" << *this << "]: State PARSING: "
                     "Parsed file [" << cfg_path << "]: Exception occurred; individual logical validation failed.  "
                     "Values payload untouched.  Message = [" << exc.what() << "].");
    throw;
  }

  // As noted above transfer from the clean variables_map into our Iterable_values.
  for (const auto& pair : fresh_iterable_values)
  {
    const auto& name = pair.first;
    const auto& var_value = pair.second.value(); // boost.any.

    assert((!key_exists(m_iterable_values_candidate, name) ||
           (m_iterable_values_candidate[name].type() == var_value.type())));

    m_iterable_values_candidate[name] = var_value; // Copying boost.any copies the payload it holds.
  }

  // This is the part that removes each option such that it was just parsed, but the new value makes it == canonical.
  Declare_options_func_args args;
  args.m_call_type = Declare_options_func_args::Call_type::S_COMPARE_PARSED_VALS;
  args.m_args.m_compare_parsed_vals_args.m_option_set = this;
  m_declare_opts_func(args);

  /* m_values_candidate and its iterable mirror m_iterable_values_candidate have been filled in!
   * By definition the former contains the pre-parsing values, plus whatever was in the config source overwriting
   * zero or more of those.  m_iterable_values_candidate fully mirrors by including only those values different from
   * m_values_candidate.  This fact may be used in canonicalize_candidate().  (For now, though, we don't care about
   * m_iterable_values_candidate outside of keeping it updated after each parse.) */

  FLOW_LOG_TRACE("Option_set[" << *this << "]: State PARSING: Validate/store succeded.  "
                 "Further validation deferred until attempt to enter state CANONICAL.  "
                 "Updated values payload details follow.");
  log_values("post-parsing candidate config", &m_values_candidate, log::Sev::S_TRACE);
} // Option_set::parse_config_file()

template<typename Value_set>
void Option_set<Value_set>::parse_direct_values(const Values& src_values)
{
  if (!m_parsing)
  {
    m_parsing = true;
    FLOW_LOG_INFO("Option_set[" << *this << "]: State CANONICAL: Request to load values from source struct "
                  "[" << &src_values << "]; entering state PARSING.  "
                  "Any preceding canonical state will be ignored.  Details follow (TRACE log level).");

    assert(m_iterable_values_candidate.empty());
  }
  else
  {
    FLOW_LOG_INFO("Option_set[" << *this << "]: State PARSING: Request to load values from source struct "
                  "[" << &src_values << "]; continuing by parsing this next config source.  "
                  "Any current candidate values payload will be overwritten entirely.  "
                  "Details follow (TRACE log level).");
  }

  /* The best way to explain our task is... to me (ygoldfel), it's like parse_config_file() of an imaginary file
   * that would have contained every possible option, with values src_values.  We need to do the stuff to end up
   * in the same state.  (We already updated m_parsing if needed.)  Of course it's a lot easier to just load the values
   * as opposed to really parsing them (which can fail validation and other difficulties).  However we still need
   * to update the same m_* state that the parsing would have.  Namely:
   *   - m_values_candidate should equal src_values (as far as each individual option goes).
   *   - m_iterable_values_candidate must be updated accordingly (see its doc header). */

  /* This work-horse will indeed update m_values_candidate as needed and mirror each value in
   * m_iterable_values_candidate[name].  In parse_config_file() that is what occurs through the step where
   * m_iterable_values_candidate is loaded from the variable_map generated by store()/notify(). */
  Declare_options_func_args args;
  args.m_call_type = Declare_options_func_args::Call_type::S_LOAD_VALS_AS_IF_PARSED;
  args.m_args.m_load_val_as_if_parsed_args = { this, &m_values_candidate, &src_values };
  m_declare_opts_func(args);

  /* Now, m_iterable_values_candidate contains every (config-enabled) member of a Values struct.
   * Note, in particular, that this overwrites anything that may have been parsed from other config sources
   * until that point (as our log messages indicate) -- as all of src_values was copied over.
   *
   * All we need now is to ensure m_iterable_values_candidate stores *only* the values that were changed
   * compared to the canonical m_values.  For that we just do the same thing parse_config_file() would at this stage. */
  args.m_call_type = Declare_options_func_args::Call_type::S_COMPARE_PARSED_VALS;
  args.m_args.m_compare_parsed_vals_args.m_option_set = this;
  m_declare_opts_func(args);

  log_values("post-loading (perhaps baseline or rewound) candidate config", &m_values_candidate, log::Sev::S_TRACE);
} // Option_set::parse_direct_values()

template<typename Value_set>
void Option_set<Value_set>::canonicalize_candidate(bool* change_detected)
{
  using util::key_exists;

  assert(m_parsing);

  FLOW_LOG_INFO("Option_set[" << *this << "]: State PARSING: Entering state CANONICAL.  "
                "Values payload finalized.  Details follow (TRACE log level).");
  log_values("entering-CANONICAL-state finalized config", &m_values_candidate, log::Sev::S_TRACE);

  /* Before finalizing m_values from m_values_candidate let's examine changes.
   * m_iterable_values_candidate maintains the invariant of only include those options whose values have changed
   * vs. m_values.  So this is trivial. */

  for (const auto& pair : m_iterable_values_candidate)
  {
    const auto& name = pair.first;
    FLOW_LOG_INFO("Option_set[" << *this << "]: Detected change in option named [" << name << "].");
  }

  if (change_detected)
  {
    *change_detected = (!m_iterable_values_candidate.empty());
  }

  // All is right with the world.

  m_parsing = false;
  m_values = m_values_candidate;
  m_iterable_values_candidate.clear();
} // Option_set::canonicalize_candidate()

template<typename Value_set>
void Option_set<Value_set>::reject_candidate()
{
  if (m_parsing)
  {
    FLOW_LOG_INFO("Option_set[" << *this << "]: State PARSING: Rejecting candidate payload built; "
                  "reverting to state CANONICAL.");
    m_parsing = false;
    m_iterable_values_candidate.clear();
    // m_values remains unchanged.  m_values_candidate becomes meaningless (see its doc header: m_parsing == false now).
  }
}

template<typename Value_set>
void Option_set<Value_set>::validate_values(bool* success_or_null) const
{
  validate_values(values(), success_or_null);
}

template<typename Value_set>
void Option_set<Value_set>::validate_values_candidate(bool* success_or_null) const
{
  const auto candidate = values_candidate();
  assert(candidate && "Do not invoke validate_values_candidate() in CANONICAL state; only while PARSING.");
  validate_values(*candidate, success_or_null);
}

template<typename Value_set>
void Option_set<Value_set>::validate_values(log::Logger* logger_ptr, const Values& values_to_validate,
                                            const Declare_options_func& declare_opts_func,
                                            bool* success_or_null) // Static.
{
  using std::exception;

  // If they invoked us in non-throwing mode, call selves recursively in throwing mode and catch it.
  if (success_or_null)
  {
    try
    {
      validate_values(logger_ptr, values_to_validate, declare_opts_func);
    }
    catch (const exception& exc)
    {
      // We already logged on exception; be quiet.
      *success_or_null = false;
      return;
    }
    *success_or_null = true;
    return;
  }
  // else: Throw an exception on error.  Perf not a real concern.

  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_CFG);

  Declare_options_func_args args;
  args.m_call_type = Declare_options_func_args::Call_type::S_VALIDATE_STORED_VALS;
  args.m_args.m_validate_stored_vals_args.m_values_to_validate = &values_to_validate;
  try
  {
    /* Simple as that!  This will invoke FLOW_CFG_OPTION_SET_DECLARE_OPTION() for each option; and that will
     * simply call validate_parsed_option() with all the needed info like name, current value, validation condition
     * string, and most importantly the validator function created from the validation Boolean expression user
     * passed to FLOW_CFG_OPTION_SET_DECLARE_OPTION().  validate_parsed_option() will throw when invoked on an invalid
     * value.  Catch it to log it, as promised, but then re-throw it, as if it fell through. */
    declare_opts_func(args);
  }
  catch (const exception& exc)
  {
    FLOW_LOG_WARNING("Stand-alone validation check failed: Details: [" << exc.what() << "].");
    throw;
  }
} // void Option_set::validate_values()

template<typename Value_set>
void Option_set<Value_set>::validate_values(const Values& values_to_validate, bool* success_or_null) const
{
  validate_values(get_logger(), values_to_validate, m_declare_opts_func, success_or_null);
}

template<typename Value_set>
std::ostream& operator<<(std::ostream& os, const Option_set<Value_set>& val)
{
  return os << '[' << val.m_nickname << "]@" << &val;
}

template<typename Value>
void value_to_ostream(std::ostream& os, const Value& val)
{
  os << val;
}

template<typename Rep, typename Period>
void value_to_ostream(std::ostream& os, const boost::chrono::duration<Rep, Period>& val)
{
  using util::ostream_op_string;
  using util::String_view;
  using boost::chrono::ceil;
  using boost::chrono::nanoseconds;
  using boost::chrono::microseconds;
  using boost::chrono::milliseconds;
  using boost::chrono::seconds;
  using boost::chrono::minutes;
  using boost::chrono::hours;
  using std::string;

  using raw_ticks_t = uint64_t; // Should be big enough for anything.

  // Nanoseconds should be the highest precision we use anywhere.  It won't compile if this can lose precision.
  nanoseconds val_ns(val);

  // Deal with positives only... why not?
  raw_ticks_t raw_abs_ticks = (val_ns.count() < 0) ? (-val_ns.count()) : val_ns.count();

  /* We can of course simply `os << val_ns` -- and it'll print something like "<...> ns" which is correct.
   * Instead we'd like to use more convenient units -- but without losing precision due to conversion; so convert
   * up to the most coarse units possible.  First, though, take care of the special case 0: */
  if (raw_abs_ticks == 0)
  {
    os << seconds::zero(); // Seconds are a nice default.  It's strange to say "0 hours" or what-not.
    return;
  }
  // else

  // Then keep trying this until can no longer try a coarser unit without losing precision.
  const auto try_next_unit = [&](auto pre_division_dur, raw_ticks_t divide_by) -> bool
  {
    if ((raw_abs_ticks % divide_by) == 0)
    {
      // Dividing current raw_abs_ticks by divide_by does not lose precision; so at worst can output post-division dur.
      raw_abs_ticks /= divide_by;
      return false;
    }
    // else: It does lose precision, so we must stop here and print it in pre-division units.
    os << ostream_op_string(pre_division_dur);
    return true;
  };

  if (!(try_next_unit(val_ns, 1000)
        || try_next_unit(ceil<microseconds>(val), 1000)
        || try_next_unit(ceil<milliseconds>(val), 1000)
        || try_next_unit(ceil<seconds>(val), 60)
        || try_next_unit(ceil<minutes>(val), 60)))
  {
    os << ceil<hours>(val);
  }
} // value_to_ostream()

template<typename Element>
void value_to_ostream(std::ostream& os, const std::vector<Element>& val)
{
  for (size_t idx = 0; idx != val.size(); ++idx)
  {
    os << '[';
    value_to_ostream<Element>(os, val[idx]);
    os << ']';
    if (idx != (val.size() - 1))
    {
      os << ' ';
    }
  }
} // value_to_ostream()

template<typename Key>
std::string value_set_member_id_to_opt_name_keyed(util::String_view member_id, const Key& key)
{
  using util::String_view;
  using util::ostream_op_string;
  using boost::regex;
  using boost::regex_match;
  using boost::smatch;
  using std::string;

  // Keep in harmony with value_set_member_id_to_opt_name() of course,

  /* <anything, long as possible>[<replaced stuff>]<.blah... or ->blah... though we don't enforce it here>
   *   =>
   * <anything, long as possible>.<`key`><.blah... or ->blah... though we don't enforce it here> */

  smatch matched_groups;
  /* @todo Should I be imbuing the regex with std::locale::classic() or something?
   * @todo Can probably make it work directly on String_view member_id; seems to conflict with `smatch`; but
   * probably it can be figured out.  Perf at this level is hardly of high import so not worrying. */
  const string member_id_str(member_id);
#ifndef NDEBUG
  const bool matched_ok =
#endif
  regex_match(member_id_str, matched_groups, VALUE_SET_MEMBER_ID_TO_OPT_NAME_KEYED_REGEX);
  assert(matched_ok && "Member identifier must look like: <longest possible stuff>[<key to replace>]<the rest>");

  constexpr char INDEX_SEP_BEFORE('.');

  /* Don't post-process or pre-process a single value_set_member_id_to_opt_name() result.
   * The replacer in that function shouldn't run across the substituted-in `key` (which could contain, e.g., "m_..." --
   * which would get incorrectly replaced by just "..."); and running the replacer followed by making the
   * [...]-by-dot-key substution is at least aesthetically perf-wasteful (those characters get replaced anyway later).
   * Anyway, all that aside, the following is easily defensible as clearly not having any such issues. */
  return ostream_op_string
           (value_set_member_id_to_opt_name(String_view(&*matched_groups[1].first, matched_groups[1].length())),
            INDEX_SEP_BEFORE,
            key, // ostream<< it.
            value_set_member_id_to_opt_name(String_view(&*matched_groups[2].first, matched_groups[2].length())));
} // value_set_member_id_to_opt_name_keyed()

} // namespace flow::cfg
