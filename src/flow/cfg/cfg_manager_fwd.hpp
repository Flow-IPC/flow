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

namespace flow::cfg
{
// Types.

// Find doc headers near the bodies of these compound types.

template<typename... S_d_value_set>
class Config_manager;

template<typename Value_set>
struct Final_validator_func;

struct Null_value_set;

template<typename Value_set>
class Static_config_manager;

/**
 * Result enumeration for a Final_validator_func::Type function which is used by a Config_manager user
 * when parsing a config source (ex: file).  In short such a function can consider the file as one to skip
 * entirely, as okay to accept, or as erroneous.
 */
enum class Final_validator_outcome
{
  /**
   * The holistically-checked cumulative `Value_set` has no problems and shall be accepted into the candidate
   * `Value_set`; if this is the final config-source (ex: file), that candidate shall be canonicalized.
   * `apply_*()` shall return `true` (success).
   */
  S_ACCEPT,

  /**
   * The holistically-checked cumulative `Value_set` has contents such that the validator function decided
   * that the *current* `apply_*()` shall have no effect, meaning the `Value_set` candidate shall remain
   * unchanged from just-before that *current* `apply_*()`; if this is the final config-source (ex: file),
   * that unchanged candidate shall be canonicalized.  `apply_*()` shall return `true` (success).
   */
  S_SKIP,

  /**
   * The holistically-checked cumulative `Value_set` has invalid contents; the candidate shall be rejected,
   * and `apply_*()` shall return `false` (failure).
   */
  S_FAIL
}; // enum class Final_validator_outcome

// Free functions.

/**
 * Serializes (briefly) a Config_manager to a standard output stream.
 *
 * @relatesalso Config_manager
 *
 * @tparam S_d_value_set
 *         See Config_manager doc header.
 * @param os
 *        Stream to which to serialize.
 * @param val
 *        Value to serialize.
 * @return `os`.
 */
template<typename... S_d_value_set>
std::ostream& operator<<(std::ostream& os, const Config_manager<S_d_value_set...>& val);

} // namespace flow::cfg

// Macros.

/**
 * Convenience macro particularly useful in the `final_validator_func()` callback taken by various
 * Config_manager APIs; checks the given condition; if `false` logs a FLOW_LOG_WARNING() containing the failed
 * condition and executes `outcome = flow::cfg::Final_validator_func::S_FAIL;`; otherwise no-op.  Note the context
 * must be such that FLOW_LOG_WARNING() compiles and acts properly.
 *
 * If `auto outcome = flow::cfg::Final_validator_func::S_ACCEPT;` precedes multiple invocations of this macro, then
 * by the end of those invocations `outcome == S_ACCEPT` if and only if all checks passed.  All errors will be printed
 * if `outcome != S_ACCEPT`.
 *
 * Informally: If you have a condition that would cause your `final_validator_func()` to return
 * flow::cfg::Final_validator_outcome::S_SKIP, then it is probably best to check for this potential condition,
 * and `return flow::cfg::Final_validator_outcome::S_SKIP` if it holds, before any further checks
 * (FLOW_CFG_OPT_CHECK_ASSERT() calls).
 *
 * @param ARG_must_be_true
 *        An expression convertible to `bool` that should evaluate to `true` to avoid WARNING and `outcome = S_FAIL;`.
 */
#define FLOW_CFG_OPT_CHECK_ASSERT(ARG_must_be_true) \
  FLOW_UTIL_SEMICOLON_SAFE \
    ( \
      if (!(ARG_must_be_true)) \
      { \
        FLOW_LOG_WARNING("Validation failed; the following condition must hold: [" #ARG_must_be_true "]."); \
        outcome = ::flow::cfg::Final_validator_outcome::S_FAIL; \
      } \
    )
