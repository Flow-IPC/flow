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
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/any.hpp>
#include <vector>
#include <string>
#include <ostream>

/**
 * Flow module that facilitates configuring modules, such as applications and APIs, via statically and/or dynamically
 * parsed sets of name/value pairs from config sources like files and command lines.  (It is also possible to use a
 * subset of the provided features to simplify option-related tasks even without parsing them from a file/etc.)
 *
 * ### Main concepts: your `Value_set`s and flow::cfg::Option_set ###
 * "Configuration" has lots of possible meanings, and flow::cfg certainly doesn't cover any huge range of what
 * could be called configuration.  Instead it is focused on a fairly common basic object type: a straightforward
 * `struct` of essentially scalar values.  (Composition/nested `struct`s are also supported, but don't worry about it
 * yet.)  Such `struct`s are often passed around larger programs and used to configure various modules.
 * It is typically a given that each type V for a data member in such a `struct`:
 *   - is copyable in a resonable way;
 *   - is comparable via `==` in a reasonable way.
 *
 * It is also reasonable that each member has some default value, set in the `struct` no-args ctor, so that
 * the full set of values in a default-cted instance of the `struct` represents a reasonable overall config for the
 * corresponding module.
 *
 * It is also often desirable to print dumps of the current contents of such a `struct`.  In that case we add
 * the requirement on each type V:
 *   - it has a standard `ostream` output `<<` operator.
 *
 * Often (though not always) one needs to *parse* into the member of this `struct`, from a stream/file
 * or perhaps command line (one or more *config sources*).  If this is indeed required then add the requirement on
 * each V:
 *   - it has a standard `istream` input `>>` operator.
 *
 * Sometimes such a `struct` is essentially *static*, meaning once the values are filled out (manually, or from
 * a config source), they shouldn't change.  More rarely they are *dynamic*, meaning
 * values can change from time to time, particularly on outside events like a SIGHUP triggering reading
 * values from a file.  In the latter case it is typical to wrap them in `shared_ptr`s; then a piece of code can
 * save a copy of such a handle to an immutable instance of the `struct`, wherein it can rely on a consistent
 * set of config, even while dynamic config updates will cause more such instances to be generated over time.
 * Hence add this requirement on the `struct` itself:
 *  - it should derive from util::Shared_ptr_alias_holder, so as to gain `Ptr` and `Const_ptr` aliases
 *    (to `shared_ptr<>`).
 *
 * Call the `struct` type satisfying the above requirements `Value_set`.  All facilities dealing with such value sets
 * are templates parameterized on `Value_set`.  Generally speaking it is straightforward to satisfy these
 * requirements; for example they hold for ~all built-in scalar types, plus `chrono::duration`, `std::string`,
 * and many more oft-used types; and deriving from `Shared_ptr_alias_holder` is just a line or two.
 *
 * Maintaining a `Value_set` like that is straighforward, but the things one wants to do with with an *entire*
 * `Value_set` -- as opposed to individual members in it -- tend to be laborious, anti-consistent, and error-prone
 * to do.  To wit:
 *   - stream output (tediously list members along with laborious and inconsistent `ostream<<` logic);
 *   - parsing (it's just hard -- and even with boost.program_options there's a ton of boiler-plate to write and
 *     tons of degrees of freedom);
 *     - documenting the available settings/meanings/semantics to the user (help message);
 *   - comparison `==` (tediously list `==` checks for each member);
 *   - validation/checking for valid values (tedious `if` statements and even more tedious stream output.
 *
 * Just about the only thing that *is* easy is simply accessing the values in a `Value_set`.  Indeed, this is the one
 * "feature" built-into `Value_set` that we want to leave available at all costs; it is concise, compile-time-checked,
 * and fast (all of which is less true or untrue of, say, a `map<string, boost::any>` and similar solutions).
 *
 * The main value of flow::cfg, as of this writing, is the Option_set class template.  `Option_set<Value_set>`
 * adds concise ways of doing all of the above in a streamlined way.  Even if you need not actually parse your
 * `Value_set` from a file, it still lets you concisely output, compare, validate the `Value_set`s themselves.
 *
 * Therefore see Option_set doc header.  Do realize that it is expected to be common-place to have multiple
 * `Value_set` types -- and therefore separate `Option_set<>` instances for each.  A server program, for example,
 * might start with just a `Static_value_set` for initial, immutable config; and later add a `Dynamic_value_set`
 * for settings that can change since startup.  Flow itself has flow::net_flow, wherein (e.g.)
 * net_flow::Peer_socket has a set of socket options, some static and some dynamically changeable once a connection
 * is open; hence it might have a `struct` for dynamic socket options and another for static.
 *
 * ### Config_manager ###
 * An Option_set is designed to be used in flexible combination with other `Option_set`s.  The variations on how
 * one might use them are equally as unpredictable and varied as how one might use `Value_set`-type `struct`s.
 *
 * Config_manager is an optional feature built on a couple of `Option_set`s, one for dynamic and one for static
 * config.  It exists to provide a simple but often-sufficient config manager for use by a server process.  Create
 * one of these, fill out your process's overall static and dynamic config `struct`s, and you should be able to
 * have a working config system with minimal boiler-plate, based on a static config file and command line; and
 * a separate dynamic config file (with the optional ability to set initial dynamic setting values from the
 * static config sources).
 *
 * If Config_manager is insufficient, one can build their own system on top of Option_set.
 *
 * @see Option_set class template, the main feature of flow::cfg.
 */
namespace flow::cfg
{
// Types.

// Find doc headers near the bodies of these compound types.

template<typename Root, typename Target,
         typename Target_ptr
           = typename std::pointer_traits<typename Root::Ptr>::template rebind<Target const>>
class Dynamic_cfg_context;

class Option_set_base;

template<typename Value_set>
class Option_set;


/* (The @namespace and @brief thingies shouldn't be needed, but some Doxygen bug necessitated them.
 * See flow::util::bind_ns for explanation... same thing here.) */

/**
 * @namespace flow::cfg::fs
 * @brief Short-hand for `namespace boost::filesystem`.
 */
namespace fs = boost::filesystem;

/**
 * @namespace flow::cfg::opts
 * @brief Short-hand for `namespace boost::program_options`.
 */
namespace opts = boost::program_options;

// Free functions.

/**
 * Serializes (briefly) an Option_set to a standard output stream.
 *
 * @relatesalso Option_set
 *
 * @tparam Value_set
 *         See Option_set doc header.
 * @param os
 *        Stream to which to serialize.
 * @param val
 *        Value to serialize.
 * @return `os`.
 */
template<typename Value_set>
std::ostream& operator<<(std::ostream& os, const Option_set<Value_set>& val);

/**
 * Utility, used by FLOW_CFG_OPTION_SET_DECLARE_OPTION() internally but made available as a public API
 * in case it is useful, that converts a string containing a conventionally formatted data member name into the
 * corresponding auto-determined config option name.
 *
 * @note An example for convenience, accurate as of the time of this writing:
 *       `m_cool_object.m_cool_sub_object->m_badass_sub_guy.m_cool_option_name` transforms to
 *       `cool-object.cool-sub-object.badass-sub-guy.cool-option-name`.
 *
 * The format for the contents of `member_id` shall be as follows: It shall consist of one or more
 * identifiers following the Flow coding guide, each starting with `m_`, concatenated with C++ object separator
 * sequences, each sequence chosen to be either `.` (object dereference) or `->` (pointer dereference).  `m_` for
 * each identifier is optional for this function -- though the coding guide requires it as of this writing anyway.
 *
 * Note that boost.program_options allows config files (when used as config sources, e.g,
 * Option_set::parse_config_file()) to declare a `[config-section]` which results in each `option-name` listed
 * in that section to be treated as-if named `config-section.option-name`.  This has synergy with nested objects
 * within a config value set being separated by dots also (or `->` if desired for orthogonal reasons, such as
 * if a smart pointer is used).
 *
 * @param member_id
 *        Identifier, perhaps obtained via the preprocessor `#feature` from an argument to a functional macro.
 * @return See above.
 */
std::string value_set_member_id_to_opt_name(util::String_view member_id);

/**
 * Similar to value_set_member_id_to_opt_name() but used by FLOW_CFG_OPTION_SET_DECLARE_OPTION_KEYED() internally
 * (also made available as a public API in case it is useful), that does the job of
 * value_set_member_id_to_opt_name() in addition to substituting the last `[...]` fragment with
 * a dot separator, followed by the `ostream` encoding of `key`.
 *
 * @note An example for convenience, accurate as of the time of this writing:
 *       `m_cool_object.m_cool_sub_object->m_badass_sub_guy[cool_key].m_cool_option_name`, with `cool_key == 3`,
 *       transforms to `cool-object.cool-sub-object.badass-sub-guy.3.cool-option-name`.
 *
 * Behavior is undefined if the `[...]` part doesn't exist or is preceded or succeeded by nothing.
 * In reality for things to work as expected that part should also be followed by a C++ object separator
 * as in value_set_member_id_to_opt_name() doc header; so, e.g., `m_blah[idx]->m_blah` or `m_blah[idx].m_blah`.
 *
 * @tparam Key
 *         An `ostream<<`able type.  Common: `size_t` and `std::string`.
 * @param member_id
 *        Identifier, perhaps obtained via the preprocessor `#` feature from an argument to a functional macro.
 * @param key
 *        The value to splice in when replacing the key fragment inside `[]` (after the inserted `.`).
 * @return See above.
 */
template<typename Key>
std::string value_set_member_id_to_opt_name_keyed(util::String_view member_id, const Key& key);

/**
 * Serializes a value of type `Value` to the given `ostream` suitably for output in Option_set-related output to user
 * such as in help messages.  The generic version simply forwards to `ostream<<` operator; but specializations/overloads
 * can massage/customize the output more suitably for usability.  The user may provide their own specializations or
 * overload on top of any already provided.
 *
 * @see E.g.: the `Value = chrono::duration` overload.
 * @tparam Value
 *         Type of `val`.  For this generic implementation `ostream << Value` operator must exist.
 * @param os
 *        Stream to which to serialize.
 * @param val
 *        Value to serialize.
 */
template<typename Value>
void value_to_ostream(std::ostream& os, const Value& val);

/**
 * Overload that serializes a value of `chrono`-based `duration` including `Fine_duration` --
 * which is recommended to use for Option_set-configured time durations -- to the given `ostream` suitably for output
 * in Option_set-related output to user such as in help messages.  As of this writing it improves upon the default
 * `ostream<<` behavior by converting to coarser units without losing precision (e.g., not `"9000000000 ns"` but
 * `"9 s"`).
 *
 * @tparam Rep
 *         See `chrono::duration`.
 * @tparam Period
 *         See `chrono::duration`.
 * @param os
 *        Stream to which to serialize.
 * @param val
 *        Value to serialize.
 */
template<typename Rep, typename Period>
void value_to_ostream(std::ostream& os, const boost::chrono::duration<Rep, Period>& val);

/**
 * Overload that serializes a list value (with `Element` type itself similarly serializable)
 * to the given `ostream` suitably for output in Option_set-related output to user such as in help messages.
 *
 * @tparam Element
 *         Any type for which `value_to_ostream(Element_type)` is available; but as of this writing
 *         it cannot itself be `std::vector<>` (no lists of lists).
 * @param os
 *        Stream to which to serialize.
 * @param val
 *        Value to serialize.
 */
template<typename Element>
void value_to_ostream(std::ostream& os, const std::vector<Element>& val);

} // namespace flow::cfg

/// We may add some ADL-based overloads into this namespace outside `flow`.
namespace boost::filesystem
{
// Free functions.

/**
 * ADL-based overload of boost.program_options `validate()` to allow for empty `boost::filesystem::path` values
 * in flow::cfg config parsing as well as any other boost.program_options parsing in the application.
 * Likely there's no need to call this directly: it is invoked by boost.program_options when parsing `path`s.
 *
 * More precisely: This has ~two effects:
 *   - flow::cfg::Option_set (and flow::cfg::Config_manager and anything else built on `Option_set`) will
 *     successfully parse an empty (or all-spaces) value for a setting of type `boost::filesystem::path`, resulting
 *     in a `path` equal to a default-constructed `path()`.
 *     - You may still disallow it via a validator expression in FLOW_CFG_OPTION_SET_DECLARE_OPTION() as usual.
 *       (Sometimes an empty "path" value is useful as a special or sentinel value.)  Such flow::cfg-user-specified
 *       individual-option-validator checking, internally, occurs after (and only if) the lower-level parsing already
 *       succeeded.
 *   - Any other use (even outside of flow::cfg) of boost.program_options to parse a `path` will now also allow
 *     an empty value (on command line, in file, etc.) in the same application.
 *
 * ### Rationale ###
 * Allowing empty `path`s in flow::cfg is required for usability.  It was a common use case to allow for a blank special
 * value for path settings.  However trying to in fact provide an empty value in a flow::cfg file (e.g., simply
 * `log-file=`) resulted in an "Invalid argument" error message and refusal of
 * flow::cfg::Option_set::parse_config_file() to successfully parse.
 *
 * (The following discussion is about implementation details and would normally be omitted from this public-facing API
 * doc header.  However, in this slightly unusual (for Flow) situation the solution happens to subtly affect
 * code outside of flow::cfg.  Therefore it is appropriate to discuss these internals here.)
 *
 * The reason for this problem: flow::cfg uses boost.program_options for config source (especially file) parsing.
 * By default, when parsing a string into the proper type `T` (here `T` being `path`),
 * boost.program_options uses `istream >> T` overload.  Turns out that reading a blank from a stream into `path` causes
 * the `istream` bad-bit to set, meaning `lexical_cast` throws `bad_lexical_cast`; boost.program_options then manifests
 * this as an arg parse error.  (If `T` were `std::string`, by contrast, no such problem would occur:
 * `istream >> string` will happily accept a blank string.)
 *
 * boost.program_options clearly suggests the proper way to custom-parse types at
 * https://www.boost.org/doc/libs/1_78_0/doc/html/program_options/howto.html#id-1.3.32.6.7 --
 * namely, define a `validate()` overload in the same namespace as type `T`, with a `T`-typed arg.
 * ADL (argument-dependent lookup) will then use that overload -- instead of the default one, which simply invokes
 * `lexical_cast` (i.e., `istream>>`) -- and reports an error if that throws.  This is the same technique used
 * to conveniently select `hash_value()` for `unordered_*`; `swap()` for STL containers; etc.
 *
 * This solves the problem.  However, somewhat unusually, this imposes the same more-permissive semantics on
 * *all* other uses of boost.program_options in any application linking Flow (which includes this overload).
 * This is arguably not-great; ideally we'd affect flow::cfg and nothing else.  That said (1) there's no apparent
 * alternative in boost.program_options; and more to the point (2) this feels like either an improvement or
 * neutral.  If an empty path must be disallowed, this can be done via `notifier()` (or just a manual check).
 * So, all in all, this seemed fine.
 *
 * @param target
 *        `target` shall be loaded with the `path` on success.  Otherwise we shall throw, per required
 *        boost.program_options semantics.
 * @param user_values
 *        The (trimmed) raw strings from the user for this occurrence of the setting.
 *        In our case there must be only one, since a `path` has only one value.
 */
void validate(boost::any& target, const std::vector<std::string>& user_values, path*, int);

} // namespace boost::filesystem
