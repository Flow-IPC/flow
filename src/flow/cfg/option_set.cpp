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
#include "flow/cfg/option_set.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/trim_all.hpp>

namespace flow::cfg
{
// Static/etc. initializers.

/// @cond
// -^- Doxygen, please ignore the following.  It gets confused thinking this is a macro (@todo fix whenever).

// Just see the guy that uses it.  I wish it could be a local `constexpr`.  @todo Maybe it can?
const boost::regex VALUE_SET_MEMBER_ID_TO_OPT_NAME_KEYED_REGEX("(.+)\\[[^]]+\\](.+)");

// -v- Doxygen, please stop ignoring.
/// @endcond

// Implementations.

std::string value_set_member_id_to_opt_name(util::String_view member_id)
{
  using util::String_view;
  using std::string;
  using boost::algorithm::replace_all;
  using boost::algorithm::starts_with;
  using boost::algorithm::is_any_of;
  using boost::algorithm::trim_fill_if;
  using std::replace;

  /* E.g. m_cool_object.m_cool_sub_object->m_badass_sub_guy.m_cool_option_name
   *   => cool-object.cool-sub-object.badass-sub-guy.cool-option-name.
   * Note, by the way, that in config files as parsed by boost.program_options, one can declare a section like:
   *   [cool-object]
   *   cool-option-1
   *   badass-option-2
   * which will parse the 2 as cool-object.cool-option-1 and cool-object.badass-option-2.  Synergy!
   *
   * Also #blah does not remove (at least some) white-space that's part of the original source code `blah`, so we have
   * to eliminate all those first.  For reference here's what gcc docs say about the "stringizing" algorithm.  (I have
   * not verified how this relates to C/C++ standards.)
   *   "All leading and trailing whitespace in text being stringized is ignored."
   *   - Nevertheless we'd still nuke it.
   *   "Any sequence of whitespace in the middle of the text is converted to a single space in the stringized result."
   *   - Cool, but in any case we nuke all space-y chars.
   *   "Comments are replaced by whitespace long before stringizing happens, so they never appear in stringized text."
   *   - That's good.
   *
   * Other activity involves auto-escaping string/char literals inside `blah`, but that doesn't apply to us. */

  constexpr String_view M_PFX("m_");
  constexpr String_view CONCAT_OK("."); // Dots remain (but if followed by m_, get rid of m_).
  constexpr String_view CONCAT_REPLACED("->"); // -> become dots (and again if m_ follows, get rid of it).
  constexpr char SEP_REPLACED = '_'; // From this...
  constexpr char SEP_REPLACEMENT = '-'; // ...to this.

  string opt_name{member_id};

  /* Nuke space-y chars.
   * Am paranoid about locales; and also didn't feel like using <locale> std::isspace(..., std::locale("C")). */
  trim_fill_if(opt_name, "", is_any_of(" \f\n\r\t\v"));

  // Should now be a contiguous identifier-composed compound.

  // Eliminate leading M_PFX.
  if (starts_with(opt_name, M_PFX))
  {
    opt_name.erase(0, M_PFX.size());
  }

  // Normalize object separators.
  replace_all(opt_name, CONCAT_REPLACED, CONCAT_OK);

  // Now any leading M_PFX gone; and object separators normalized to CONCAT_OK.  Can eliminate remaining M_PFX:
  replace_all(opt_name, string{CONCAT_OK} + string{M_PFX}, CONCAT_OK);

  // Lastly transform the word-separators.
  replace(opt_name.begin(), opt_name.end(), SEP_REPLACED, SEP_REPLACEMENT);

  return opt_name;
} // value_set_member_id_to_opt_name()

} // namespace flow::cfg

namespace boost::filesystem
{
// Implementations.

void validate(boost::any& target, const std::vector<std::string>& user_strings, path*, int)
{
  // Read our doc header for all background.  Hence only light comments below.

  using flow::error::Runtime_error;
  using flow::util::ostream_op_string;
  namespace opts = flow::cfg::opts;
  using boost::any;
  using boost::lexical_cast;
  using boost::bad_lexical_cast;
  using std::string;

  // Note this is auto-trimmed of spaces on left and right.
  const string& user_string = opts::validators::get_single_string(user_strings);
  /* That threw if there's more than 1 string.  (I believe impossible in config file; but on command line they
   * could try, e.g., "--log-path some-file.txt some-other-file.txt" -- that would throw.) */

  path result_path; // Empty.
  if (!user_string.empty())
  {
    /* If not empty then just do what the default validate() would've done: lexical_cast<> (i.e., istream>>) it.
     * Not actually sure what could still trip the exception other than ""; but re-throw it with a nice error, because
     * we can. */
    try
    {
      result_path = lexical_cast<decltype(result_path)>(user_string);
    }
    catch (const bad_lexical_cast& exc)
    {
      throw Runtime_error{ostream_op_string
                            ("Error converting [", user_string,
                             "] to boost::filesystem path.  Is there something strange in that string?")};
    }
  }
  // else { Leave result_path at empty.  lexical_cast<path>("") would have yielded bad_lexical_cast, but we allow it. }

  target = result_path; // Load variable-type `target` with a `path`.
} // validate(fs_path)

} // namespace boost::filesystem
