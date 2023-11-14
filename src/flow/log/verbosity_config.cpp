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
#include "flow/log/verbosity_config.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

namespace flow::log
{

// Static initializations.

const std::string Verbosity_config::S_ALL_COMPONENT_NAME_ALIAS("ALL");
const char Verbosity_config::S_TOKEN_SEPARATOR(';');
const char Verbosity_config::S_PAIR_SEPARATOR(':');

// Implementations.

Verbosity_config::Verbosity_config()
{
  using std::string;
  using std::make_pair;

  // As promised:
  m_component_sev_pairs.push_back(make_pair<string, Sev>(string(), Sev(Config::S_MOST_VERBOSE_SEV_DEFAULT)));
  assert(m_component_sev_pairs.size() == 1);
}

bool Verbosity_config::parse(std::istream& is)
{
  using util::ostream_op_string;
  using boost::algorithm::split;
  using boost::algorithm::is_any_of;
  using boost::algorithm::to_upper_copy;
  using boost::lexical_cast;
  using boost::bad_lexical_cast;
  using std::vector;
  using std::string;
  using std::make_pair;
  using std::locale;

  string tokens_str;
  is >> tokens_str; // As promised read up to (not including) first space.

  Component_sev_pair_seq result_pairs;

  /* Everything below follows directly from the doc header of component_sev_pairs().
   * Keeping comments light but do ensure it all matches when maintaining this code. */

  if (!tokens_str.empty()) // Degenerate case.
  {
    vector<string> tokens;
    split(tokens, tokens_str, is_any_of(string(1, S_TOKEN_SEPARATOR)));
    result_pairs.reserve(tokens.size()); // Little optimization.

    const auto is_pair_sep = is_any_of(string(1, S_PAIR_SEPARATOR));
    for (const auto& token : tokens)
    {
      if (token.empty()) // Trying to split "" may still produce non-empty leaf_tokens... just eliminate corner case.
      {
        m_last_result_message = ostream_op_string("A pair token is empty in [", tokens_str, "].");
        return false;
      }
      // else

      vector<string> leaf_tokens;
      split(leaf_tokens, token, is_pair_sep);

      if (leaf_tokens.empty() || (leaf_tokens.size() > 2)) // The former shouldn't happen really.  @todo assert() it?
      {
        m_last_result_message = ostream_op_string("Pair token [", token,
                                                  "] in [", tokens_str, "] must contain 1-2 `", S_PAIR_SEPARATOR,
                                                  "`-separated leaf tokens: `<component>",
                                                  S_PAIR_SEPARATOR, "<sev>` or `",
                                                  S_ALL_COMPONENT_NAME_ALIAS, S_PAIR_SEPARATOR,
                                                  "<sev>` or `", S_PAIR_SEPARATOR, "<sev> or just `<sev>`.");
        return false;
      }
      // else

      if (leaf_tokens.size() == 1) // "sev" treated as-if ":sev".
      {
        leaf_tokens.insert(leaf_tokens.begin(), "");
      }
      else
      {
        if (leaf_tokens[0] == S_ALL_COMPONENT_NAME_ALIAS) // "ALL:sev" treated as-if ":sev".
        {
          leaf_tokens[0].clear();
        }
      }

      /* Parse `sev` in "name:sev" from string to Sev via lexical_cast via its operator<<(is).
       * Subtlety: Store the component name in normalized (upper-case) form.  Then ostream<<() output will be pretty
       * and match S_ALL_COMPONENT_NAME_ALIAS.
       * @todo Should reuse Config::normalized_component_name().  Though its action is officially documented as
       * performing to-upper-case, so this is correct. */
      assert(leaf_tokens.size() == 2);

      Sev sev;
      try
      {
        /* As of this writing Sev<<istream is quite permissive -- anything matching [A-Za-z_]* is accepted;
         * but what's not obvious is: Suppose leaf_tokens[1] is [A-Za-z_]+ followed by junk like other characters.
         * For example, say they wrote WARNING,INFO by mistake.  Sev<<istream will read up to and not including the
         * comma; load WARNING into the Sev.  Then lexical_cast<> will throw bad_cast_exception, because
         * operator<< stopped reading and left stuff in the istream (that's the non-obvious thing).  So catch it. */
        sev = lexical_cast<Sev>(leaf_tokens[1]);
      }
      catch (const bad_lexical_cast&)
      {
        m_last_result_message = ostream_op_string("Leaf token [", leaf_tokens[1],
                                                  "] in [", tokens_str, "] must contain a severity composed of "
                                                  "alphanumerics and underscores but appears to contain other "
                                                  "characters.");
        return false;
      }
      result_pairs.push_back
        (make_pair<string, Sev>
           (string(to_upper_copy(leaf_tokens[0], locale::classic())),
            std::move(sev)));
    } // for (token : tokens)
  } // if (!tokens_str.empty())
  // else if (tokens_str.empty()) { No problem: we handle result_pairs.empty() just below. }

  // As promised there must be a leading default-verbosity pair.
  if (result_pairs.empty() || (!result_pairs.front().first.empty()))
  {
    result_pairs.insert
      (result_pairs.begin(),
       make_pair<string, Sev>(string(), Sev(Config::S_MOST_VERBOSE_SEV_DEFAULT)));
  }

  // Finalize only if all succeeded only (as promised).
  m_component_sev_pairs = std::move(result_pairs);
  // result_pairs is now hosed.

  m_last_result_message.clear();
  return true;
} // Verbosity_config::parse()

bool Verbosity_config::apply_to_config(Config* target_config_ptr)
{
  using util::ostream_op_string;

  assert(target_config_ptr);
  auto& target_config = *target_config_ptr;

  /* Everything below follows directly from the doc header of component_sev_pairs().
   * Keeping comments light but do ensure it all matches when maintaining this code. */

  // First pair must specify initial default/catch-all verbosity.  We never allow ourselves to enter a different state.
  assert((!m_component_sev_pairs.empty()) && m_component_sev_pairs.front().first.empty());
  target_config.configure_default_verbosity(m_component_sev_pairs.front().second, true); // Reset!

  // Now handle the rest (if any).
  for (size_t idx = 1; idx != m_component_sev_pairs.size(); ++idx)
  {
    const auto& pair = m_component_sev_pairs[idx];
    const auto& component_name = pair.first;
    const auto sev = pair.second;

    if (component_name.empty())
    {
      target_config.configure_default_verbosity(sev, false); // Do not reset.
    }
    else if (!target_config.configure_component_verbosity_by_name(sev, component_name))
    {
      m_last_result_message = ostream_op_string("Component name [", component_name, "] is unknown.");
      return false;
    }
    // else { We set it fine.  Keep going. }
  }

  m_last_result_message.clear();
  return true;
} // Verbosity_config::apply_to_config()

const std::string& Verbosity_config::last_result_message() const
{
  return m_last_result_message;
}

const Verbosity_config::Component_sev_pair_seq& Verbosity_config::component_sev_pairs() const
{
  return m_component_sev_pairs;
}

std::istream& operator>>(std::istream& is, Verbosity_config& val)
{
  val.parse(is);
  return is;
}

std::ostream& operator<<(std::ostream& os, const Verbosity_config& val)
{
  const auto& component_sev_pairs = val.component_sev_pairs();
  for (size_t idx = 0; idx != component_sev_pairs.size(); ++idx)
  {
    const auto& pair = component_sev_pairs[idx];
    const auto& component_name = pair.first;

    os << (component_name.empty() ? Verbosity_config::S_ALL_COMPONENT_NAME_ALIAS : component_name);
    os << Verbosity_config::S_PAIR_SEPARATOR;
    os << pair.second;

    if (idx != (component_sev_pairs.size() - 1))
    {
      os << Verbosity_config::S_TOKEN_SEPARATOR;
    }
  }

  return os;
}

bool operator==(const Verbosity_config& val1, const Verbosity_config& val2)
{
  return val1.component_sev_pairs() == val2.component_sev_pairs();
}

bool operator!=(const Verbosity_config& val1, const Verbosity_config& val2)
{
  return !(operator==(val1, val2));
}

} // namespace flow::log
