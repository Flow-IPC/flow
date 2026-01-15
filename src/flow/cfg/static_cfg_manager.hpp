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

#include "flow/cfg/cfg_manager_fwd.hpp"
#include "flow/cfg/cfg_manager.hpp"

namespace flow::cfg
{

// Types.

/**
 * A `Config_manager`-related adapter-style class that manages a simple config setup involving a single
 * (though arbitrarily complex) `Option_set<>`-ready raw value `struct` config store type `Value_set`, meant to
 * be used only in static fashion.  That is to say, the parsed config values are not meant to be accessed
 * *while* the config is being read from file.
 *
 * If you desire dynamic config (which can be read from file(s) at any time), and/or you need to manage more than
 * one config `struct` (e.g., you're controlling 2+ entirely separate modules), then please use
 * Config_manager which supports all that.  (You can also develop your own handling of `Option_set<Value_set>`
 * instead.  See Config_manager doc header.)
 *
 * The life cycle and usage are simple.  Define your `Value_set` (see Option_set doc header for formal requirements,
 * but basically you'll need a `struct`, an option-defining function using FLOW_CFG_OPTION_SET_DECLARE_OPTION(),
 * and possibly an inter-option validator function).  Construct the `Static_config_manager<Value_set>`.
 * Call apply() to read a file.  (You can do this more than once, potentially for different files.  As of this writing
 * only files are supported, but adding command line parsing would be an incremental change.)  Then,
 * call values() to get the reference to the immutable parsed `Value_set`.  This reference can be passed around the
 * application; values() will always return the same reference.  Technically you could call apply() even after
 * using values(); but it is not thread-safe to do so while accessing config values which would change concurrently
 * with no protection.
 *
 * Also you may create a `const` (immutable) Static_config_manager via its constructor and then just use it to output
 * a help message (log_help() or help_to_ostream()).  This could be used with your program's `--help` option or similar,
 * and that's it (no parsing takes place).
 *
 *   ~~~
 *   // Example of a Static_config_manager created just to log the help, and that's it.
 *   flow::cfg::Static_config_manager<Static_value_set>
 *     (get_logger(), "cfg", &static_cfg_declare_opts)
 *     .log_help();
 *   ~~~
 *
 * @note Config_manager provides the optional `commit == false` mode in apply_static(); this enables the
 *       "multi-source parsing and source skipping" described in its doc header.  Static_config_manager, to keep
 *       its mission simple, cuts off access to this feature, meaning implicitly it always takes
 *       `commit` to equal `true`.  That is, it is expected you'll be loading static config from exactly
 *       one file/one apply() call per attempt.  Your Final_validator_func::Type `final_validator_func()`
 *       *should* return either Final_validator_outcome::S_ACCEPT or Final_validator_outcome::S_FAIL.  It *can* return
 *       Final_validator_outcome::S_SKIP, but that would mean apply() will return `true` (success)
 *       but simply no-op (not update the canonical config as returned by values()).
 *
 * ### Rationale ###
 * Config_manager is in the author's opinion not difficult to use, but it *does* use template parameter packs
 * (`typename... Value_set`), and its API can be somewhat difficult to grok when all you have is the aforementioned
 * simple use case.
 *
 * @tparam Value_set
 *         The settings `struct` -- see Option_set doc header for requirements.
 */
template<typename Value_set>
class Static_config_manager :
  private Config_manager<Value_set, Null_value_set>
{
public:
  // Types.

  /**
   * Tag type: indicates an apply() method must *allow* invalid defaults and only complain if the config source
   * does not explicitly supply a valid value.  Otherwise the defaults themselves are also stringently checked
   * regardless of whether they are overridden.  This setting applies only to individual-option-validators.
   * Final_validator_func validation is orthogonal to this.
   */
  enum allow_invalid_defaults_tag_t
  {
    S_ALLOW_INVALID_DEFAULTS ///< Sole value for tag type #allow_invalid_defaults_tag_t.
  };

  /**
   * The class we `private`ly subclass (in HAS-A fashion, not IS-A fashion).  It is `public` basically so that we can
   * refer to it in various forwarding `using` method directives below.
   */
  using Impl = Config_manager<Value_set, Null_value_set>;

  // Constructors/destructor.

  /**
   * Constructs a Static_config_manager ready to read static config via apply() and access it via values().
   *
   * ### Logging assumption ###
   * `*logger_ptr` is a standard logging arg.  Note, though, that the class will assume that log verbosity may not have
   * yet been configured -- since this Static_config_manager may be the thing configuring it.  Informal recommendations:
   *   - You should let through INFO and WARNING messages in `*logger_ptr`.
   *   - If you plan to use `*this` only for log_help() (such as in your `--help` implementation), you should
   *     *not* let through TRACE-or-more-verbose.
   *   - Once (and if) you engage any actual parsing (apply()), TRACE may be helpful
   *     in debugging as usual.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param nickname
   *        Brief string used for logging subsequently.
   * @param declare_opts_func_moved
   *        The declare-options callback as required by
   *        `Option_set<Value_set>` constructor; see its doc header for instructions.
   */
  explicit Static_config_manager(log::Logger* logger_ptr, util::String_view nickname,
                                 typename Option_set<Value_set>::Declare_options_func&& declare_opts_func_moved);

  // Methods.

  /**
   * Invoke this after construction to load the permanent set of static config from config sources including
   * a static config file.  See also apply() overload with #allow_invalid_defaults_tag_t tag.
   *
   * After this runs and succeeds, you may use values() to access the loaded values (but see notes on
   * `final_validator_func` arg and return value, regarding Final_validator_outcome::S_SKIP w/r/t
   * `final_validator_func` arg).
   *
   * On failure returns `false`; else returns `true`.  In the former case the overall state is equal to that
   * at entry to the method.  Tip: On failure you may want to exit program with error; or you
   * can continue knowing that values() will return default values according to `Value_set{}` no-arg ctor.
   * WARNING(s) logged given failure.
   *
   * Before apply(), or after it fails, the contents of what values() returns will be the defaults from your `Value_set`
   * structure in its constructed state.  This may or may not have utility depending on your application.
   *
   * apply() will *not* be tolerant of unknown option names appearing in the config source.
   *
   * @note `final_validator_func()` can be made quite brief by using convenience macro
   *       FLOW_CFG_OPT_CHECK_ASSERT().  This will take care of most logging in most cases.
   *
   * @todo Add support for command line as a config source in addition to file(s), for static config in
   * cfg::Config_manager.
   *
   * @param cfg_path
   *        File to read.
   * @param final_validator_func
   *        If parsing and individual-option-validation succeed, the method shall return success if
   *        `final_validator_func(V)` returns Final_validator_outcome::S_ACCEPT or Final_validator_outcome::S_SKIP,
   *        where V is the parsed `Value_set`; and in the former case values() post-this-method will return V.
   *        Informally: Please place individual-option validation
   *        into FLOW_CFG_OPTION_SET_DECLARE_OPTION() invocations; only use `final_validator_func()` for
   *        internal consistency checks (if any).
   *        Informally: It is unlikely, with Static_config_manager, that it should return SKIP; that feature
   *        is only useful with the multi-file-update feature which is not accessible through
   *        Static_config_manager.  See the note about this in our class doc header.
   * @return `true` if and only if successfully parsed config source and validated all settings including
   *         `final_validator_func() != S_FAIL`; and defaults were also all individually
   *         valid.  If `true`, and `final_validator_func() == S_ACCEPT`, then values() shall return the
   *         parsed `Value_set`.  If `true`, but `final_validator_func() == S_SKIP`, then values() shall return
   *         the default-cted `Value_set`
   */
  bool apply(const fs::path& cfg_path,
             const typename Final_validator_func<Value_set>::Type& final_validator_func);

  /**
   * Identical to similar apply() overload without #allow_invalid_defaults_tag_t tag; but skips the stringent
   * check on individual defaults' validity.
   *
   * @see #allow_invalid_defaults_tag_t doc header and/or return-value doc just below.
   *
   * @param cfg_path
   *        See other apply().
   * @param final_validator_func
   *        See other apply().
   * @return See other apply().  However the latter will return `false` if a default is invalid, even if
   *         file `cfg_path` explicitly sets it to a valid value.  This tagged overload will not.
   */
  bool apply(allow_invalid_defaults_tag_t,
             const fs::path& cfg_path,
             const typename Final_validator_func<Value_set>::Type& final_validator_func);

  /**
   * Equivalent to the other apply() but with no inter-option validation (meaning the per-option validation
   * passed to constructor is sufficient).  See also apply() overload with #allow_invalid_defaults_tag_t tag.
   *
   * @param cfg_path
   *        File to read.
   * @return `true` if and only if successfully parsed config source(s) and validated all settings;
   *         and defaults were also all individually valid.
   */
  bool apply(const fs::path& cfg_path);

  /**
   * Identical to similar apply() overload without #allow_invalid_defaults_tag_t tag; but skips the stringent
   * check on individual defaults' validity.
   *
   * @see #allow_invalid_defaults_tag_t doc header and/or return-value doc just below.
   *
   * @param cfg_path
   *        See other apply().
   * @return See other apply().  However the latter will return `false` if a default is invalid, even if
   *         file `cfg_path` explicitly sets it to a valid value.  This tagged overload will not.
   */
  bool apply(allow_invalid_defaults_tag_t,
             const fs::path& cfg_path);

  /**
   * Returns (always the same) reference to the managed `Value_set` structure.  Before successful apply() these
   * values will be at their defaults.  Tip: It should be sufficient to pass around only the `const`
   * ref obtained here all around the app -- no `Value_set` copying should be needed.
   *
   * @return See above.  To reiterate: always the same reference is returned.
   */
  const Value_set& values() const;

  /**
   * Prints a human-targeted long-form summary of our contents, doubling as a usage message and a dump of current
   * values where applicable.  This is not thread-safe against several non-`const`- methods.
   *
   * @param os
   *        Stream to which to serialize.
   */
  using Impl::state_to_ostream;

  /**
   * Logs what state_to_ostream() would print.  This is not thread-safe against several non-`const`- methods.
   *
   * @param sev
   *        Severity to use for the log message(s).
   */
  using Impl::log_state;

  /**
   * Prints a human-targeted long-form usage message that includes all options with their descriptions and defaults.
   * This is thread-safe against all concurrent methods on `*this` and can be invoked anytime after ctor.
   *
   * @param os
   *        Stream to which to serialize.
   */
  using Impl::help_to_ostream;

  /**
   * Logs what help_to_ostream() would print.
   *
   * @param sev
   *        Severity to use for the log message(s).
   */
  using Impl::log_help;

private:
  /// @cond
  // -^- Doxygen, please ignore the following.  It gets confused by the following non-member `friend`.

  // Friend for access to `private` base.
  template<typename Value_set2>
  friend std::ostream& operator<<(std::ostream& os, const Static_config_manager<Value_set2>& val);

  // -v- Doxygen, please stop ignoring.
  /// @endcond
}; // class Static_config_manager

// Free functions.

/**
 * Serializes (briefly) a Static_config_manager to a standard output stream.
 *
 * @relatesalso Static_config_manager
 *
 * @tparam Value_set
 *         See Static_config_manager doc header.
 * @param os
 *        Stream to which to serialize.
 * @param val
 *        Value to serialize.
 * @return `os`.
 */
template<typename Value_set>
std::ostream& operator<<(std::ostream& os, const Static_config_manager<Value_set>& val);

// Template implementations.

template<typename Value_set>
Static_config_manager<Value_set>::Static_config_manager
  (log::Logger* logger_ptr, util::String_view nickname,
   typename Option_set<Value_set>::Declare_options_func&& declare_opts_func_moved) :
  Impl(logger_ptr, nickname, std::move(declare_opts_func_moved), null_declare_opts_func())
{
  // Nope.
}

template<typename Value_set>
bool Static_config_manager<Value_set>::apply(const fs::path& cfg_path,
                                             const typename Final_validator_func<Value_set>::Type& final_validator_func)
{
  return this->template apply_static<Value_set>(cfg_path, final_validator_func);
  /* Yes, that ->template thing is highly weird-looking.  Without it gcc was giving me
   * "expected primary-expression before '<'".  I got the answer -- which reminds me of `typename` -- here:
   *   https://stackoverflow.com/questions/610245/where-and-why-do-i-have-to-put-the-template-and-typename-keywords
   *   https://stackoverflow.com/questions/37995745/expected-primary-expression-before-token
   *
   * It also doesn't like it, if I remove `this->`. */
}

template<typename Value_set>
bool Static_config_manager<Value_set>::apply(allow_invalid_defaults_tag_t,
                                             const fs::path& cfg_path,
                                             const typename Final_validator_func<Value_set>::Type& final_validator_func)
{
  return this->template apply_static<Value_set>(Impl::S_ALLOW_INVALID_DEFAULTS, cfg_path, final_validator_func);
}

template<typename Value_set>
bool Static_config_manager<Value_set>::apply(const fs::path& cfg_path)
{
  return apply(cfg_path, Final_validator_func<Value_set>::null()); // 2nd arg is a no-op/always-passes validator.
}

template<typename Value_set>
bool Static_config_manager<Value_set>::apply(allow_invalid_defaults_tag_t, const fs::path& cfg_path)
{
  return apply(S_ALLOW_INVALID_DEFAULTS, cfg_path, Final_validator_func<Value_set>::null());
}

template<typename Value_set>
const Value_set& Static_config_manager<Value_set>::values() const
{
  return this->template static_values<Value_set>(0); // The managed value set is in slot 0.
}

template<typename Value_set>
std::ostream& operator<<(std::ostream& os, const Static_config_manager<Value_set>& val)
{
  return os << static_cast<const typename Static_config_manager<Value_set>::Impl&>(val);
}

} // namespace flow::cfg
