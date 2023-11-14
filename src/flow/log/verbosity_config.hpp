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

#include "flow/log/verbosity_config_fwd.hpp"
#include "flow/log/config.hpp"
#include <utility>
#include <string>
#include <vector>

namespace flow::log
{

// Types.

/**
 * Optional-use structure encapsulating a full set of verbosity config, such that one can parse it from a config source
 * (like an options file) in concise form and apply it to a log::Config object.
 *
 * Put simply, one can read a string like `"ALL:2;COOL_COMPONENT:4;ANOTHER_COMPONENT:TRACE"` from an `istream` via
 * `>>`; then `this->apply_to_config(&cfg)`, where `cfg` is a `log::Config`; resulting in the necessary
 * `Config::configure_default_verbosity()` and `Config::configure_component_verbosity_by_name()` calls that will set
 * the default verbosity to INFO and those specific components' verbosities to DATA and TRACE respectively in order.
 */
class Verbosity_config
{
public:
  // Types.

  /// Short-hand for the configuration capable of being encapsulated by Verbosity_config.
  using Component_sev_pair_seq = std::vector<std::pair<std::string, Sev>>;

  // Constants.

  /// String that Verbosity_config::parse() treats as the default/catch-all verbosity's "component" specifier.
  static const std::string S_ALL_COMPONENT_NAME_ALIAS;

  /// Separates component/severity pairs in a Verbosity_config specifier string.
  static const char S_TOKEN_SEPARATOR;

  /// Separates component and severity within each pair in a Verbosity_config specifier string.
  static const char S_PAIR_SEPARATOR;

  // Constructors/destructor.

  /**
   * Constructor a Verbosity_config that resets all severity config and sets the default/catch-all to
   * Config::S_MOST_VERBOSE_SEV_DEFAULT.
   */
  Verbosity_config();

  // Methods.

  /**
   * Deserializes `*this` from a standard input stream.  Reads a single space-delimited token
   * from the given stream.  Separates that into a sequence of #S_TOKEN_SEPARATOR-delimited pairs, each representing an
   * element of what will be returnable via component_sev_pairs(), in order.  Each pair is to be in one of
   * the following forms:
   *   - No #S_PAIR_SEPARATOR; just a Severity readable via its `istream<<` operator (as of this writing,
   *     a number "1", "2", ... or a string "WARNING", "INFO", etc.).  Then this is taken to be the severity, and
   *     the component name for component_sev_pairs() is taken to be as-if "" (the default).
   *   - ":sev" (where the colon stands for #S_PAIR_SEPARATOR): As-if simply "sev" (previous bullet point).
   *   - "ALL:sev" (ditto re. colon): `ALL` is #S_ALL_COMPONENT_NAME_ALIAS: Same meaning as previous bullet point.
   *   - "name:sev" (ditto re. colon): `name` is the component name; `sev` is the Severity readable via its
   *     `istream<<` operator.
   *
   * Returns `true` if the token was legal; else `false`.  last_result_message() can then be invoked to get
   * further details suitable for display to the user in the case of `false`.
   *
   * If `true` returned, the existing payload is completely overwritten.  Otherwise it is untouched.
   *
   * @param is
   *        Input stream.
   * @return `true` if and only if successfully deserialized.  See also last_result_message().
   */
  bool parse(std::istream& is);

  /**
   * Applies `*this` to to the given log::Config.  Typically one would parse into `*this` first.
   *
   * Returns `true` if this succeeded; else `false`.  last_result_message() can then be invoked to get
   * further details suitable for display to the user in the case of `false`.  Typically this would only occur, as of
   * this writing, on an unknown component name according to what has been registered on `*target_config`.
   *
   * If `true` returned, the Config verbosity config is completely overwritten; otherwise it is reset and completely
   * overwritten but may represent only a partial application (valid but typically not advisable to use).
   *
   * @param target_config
   *        The log::Config to reset.
   * @return `true` if and only if successfully applied.  See also last_result_message().
   */
  bool apply_to_config(Config* target_config);

  /**
   * To be used after parse() or `operator<<` or apply_to_config(), returns "" on success or a message describing
   * the problem on failure.
   *
   * @return See above.
   */
  const std::string& last_result_message() const;

  /**
   * Read-only access to encapsulated config; specifies the verbosity-setting calls to make on a Config in order,
   * given as component-name-to-severity pairs, with an empty component name specifying the default severity used
   * when no component name is given.
   *
   * The assumed semantics as applied to a given log::Config `cfg` are as follows:
   *   -# If empty, or the first pair's component name is *not* "", then act as-if there was an additional
   *      front-inserted pair `("", Config::S_MOST_VERBOSE_SEV_DEFAULT)`.
   *   -# For the first pair: `cfg.configure_default_verbosity(v, true);`, where `v` is the log::Sev in that slot.
   *      In other words the config is reset, and the catch-all default verbosity is initialized to that severity.
   *   -# For each subsequent pair `(name, v)`:
   *      - If the `name` is "": `cfg.configure_default_verbosity(v, false);`.
   *      - Otherwise: `cfg.configure_component_verbosity_by_name(v, name);`.
   *
   * @return See above.
   */
  const Component_sev_pair_seq& component_sev_pairs() const;

private:
  // Data.

  /// See component_sev_pairs().
  Component_sev_pair_seq m_component_sev_pairs;

  /// See last_result_message().
  std::string m_last_result_message;
}; // struct Verbosity_config

// Free functions: in *_fwd.hpp.

} // namespace flow::log
