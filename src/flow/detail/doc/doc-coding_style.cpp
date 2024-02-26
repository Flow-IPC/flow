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

/**
 * @file
 *
 * Doc file (meant for reading, not compiling) that formally defines the coding style for the rest of the project.
 *
 * @todo There are a few little to-dos inside the Doxygen-skipped main body of doc-coding_style.cpp; check them out.
 *
 * @todo A script/tool/something that would auto-scan the code for violations of cosmetic conventions described
 * in doc-coding_style.cpp.  It need not be perfect or comprehensive.  Humans tend to have difficulty following lots
 * of conventions, and it then clutters up code reviews subsequently.
 *
 * @todo In all of Flow, scan for the unintentional use of the period character in auto-brief summaries in many doc
 * headers.  A period will be assumed to be the end of the brief summary.  We've been disciplined about this, but
 * I (ygoldfel) just realized that, e.g., "boost.asio" has a period and yet is present in at least some doc headers.
 * Fix those in some consistent yet palatable way, so that the Doxygen output for those isn't prematurely truncated.
 * (Check that Doxygen isn't smart enough to figure it out already.  If it is, delete this to-do.)
 */

/* This is basically `#if 0` but fools most syntax highlighters to still treat the body as active C++ to highlight.
 * `#if 0` often leads to annoying graying out.  Also this ensures Doxygen doesn't try to scan it for documentation
 * to generate.  This file is meant to be read straight up (if ideally with syntax highlighting). */
#ifndef FLOW_DOXYGEN_ONLY
#  ifdef FLOW_DOXYGEN_ONLY

// -- Synopsis / Table of Contents --

/* - About
 * - Style guide [cosmetic, objective, fairly exhaustive]
 *   - Folder level, namespace basics
 *   - File level
 *   - Basics: Line width; indent; comments; doc header basics
 *   - Identifier formatting
 *   - Order of declarations
 *   - Spacing and indentation
 *   - Misc/loose ends
 *   - Header files and forwarding; _fwd.hpp pattern
 *     - Exceptions to _fwd.hpp pattern
 *     - _fwd.hpp pattern odds/ends: Macros, constexpr globals
 * - Best practices [conceptual, subjective, non-exhaustive]
 *   - Comments
 *   - Inlining
 *   - Doxygen doc header deep-dive
 *   - Namespaces, libraries
 *   - Error handling
 *   - Logging
 *   - Types and type safety
 *   - General design do's and don'ts */

// -- About --

/* This source file is meant to be read by all coders of Flow (not coders *using* Flow).  Do not compile it.
 * It aims to answer the question: What is the formal style for Flow code (API and internals)?  The answer is *slightly*
 * subtle.  The coding style is formally as follows:
 *   - The rules in this file are to be followed, unless there is an excellent, rare reason not to.
 *     - Many rules are not given as explicit rules but by example.  E.g., if a code snippet formats a `static`
 *       non-member variable with certain formatting and doc header in this file, then that is to be taken as an
 *       rule that that is how such declarations are to be done.
 *     - Either way, following these rules helps *consistency* as well as hopefully clarity, maintainability, etc.
 *   - If some situation is not specified in this file (either as an explicit rule or by example), and the coder is
 *     asking oneself what this means about how one should proceed, then these are the possibilities:
 *     -# There is no convention to follow.  Coder should do what they want while being guided by personal dedication
 *        to clarity, maintainaibility, etc.  All that will be lacking, then, is potentially *consistency* with what
 *        some other code snippet did in that situation.
 *        - This certainly happens, but it is worth a little effort to push for consistency and attempt the following
 *          bullet point instead.  If that fails, OK, come back here.
 *     -# There is a convention to follow, though not in the present file.  Check existing Flow code for how it tends
 *        to be done.  If there's a clear answer, copy that.
 *        - In addition, if it appears to be an ironclad 95%+ convention in practice, add it to the present style
 *          doc to eliminate the ambiguity next time.
 *
 * Flow is extremely consistent and places a high value on aesthetics, clarity, maintainability, and documentation.
 * Because it was originally written by one person and with an emphasis on these, it's in a relatively rare state for
 * a code base of this size: Good style with near-100% consistency is actually achieved.  So, let's keep achieving it
 * in inductive fashion, so to speak.
 *
 * Really, it is quite realistic to accomplish this by simply following the rule,
 * "if in doubt how to present some code, (1) it matters what you decide, and (2) use existing Flow code to guide
 * you by example."  Hence the present file is not *necessary*; or it could contain nothing but the present commentary
 * and nothing else.  Hence the following snippets and comments exist for *convenience* only: so that one can easily
 * pop on over to here and most times find the desired convention/rule (as opposed to fishing around the existing
 * code base).
 *
 * The rest is split up into 2 groups of guidelines.
 *  - STYLE GUIDE: These are cosmetic rules.  They're fairly objective, and to achieve consistency one must follow
 *    them (unless excellent reason to not).  Basically this is, like, indentation and ordering and formatting, etc.
 *  - BEST PRACTICES: These are subjective coding style rules.  The aim here is no longer mere cosmetic consistency
 *    but the more subjective goals of readability, maintainability, and elegance.  These are things like: avoid
 *    `protected` non-`const` data members; prefer iteration to massive recursion; etc. */

// -- STYLE GUIDE: Folder level, namespace basics --

/* For *all* code under `flow` -- even though (as noted in common.hpp) the various namespaces are orthogonal to each
 * other in functionality -- the following conventions apply.  We basically use similar conventions to Boost and
 * all/most popular STL implementations, plus some additional conventions.  To wit:
 *
 *   - There is a (nearly) 1-to-1 relationship between the tree of `namespace`s and the directory tree.
 *     That is: `flow` is the top level directory and `namespace`; within it are directories X1, X2, ..., each of which
 *     is 1-to-1 to `namespace`s X1, X2, ....  This pattern continues recursively as well.
 *     - A potential exception to this is that it may make sense to create a sub-directory, even if doesn't have a
 *       corresponding namespace, from time to time, for special organizational purposes.
 *     - Conversely, a potential exception is namespace X has sub-namespace Y, yet there is no X/Y directory
 *       for stuff from `Y`.  We generally recommend against this, except when all of the stuff in `namespace Y`
 *       can be declared in a very small number of headers... namely at most Y.hpp and/or Y_fwd.hpp.
 *     - Since macros (which we generally avoid except where necessary, usually for logging only) do not reside in
 *       `namespace`s, we simulate "macro namespaces" via prefixes.  See elsewhere in the present file for details.
 *   - Suppose flow/A/B is a directory corresponding to namespace `flow::A::B`.  Then any file of form flow/A/B/x.hpp
 *     must contain *only* symbols in `flow::A::B` *that are intended to be publicly available*.  (Of course, function
 *     bodies -- notably function templates -- must sometimes also reside in header files, alongside the interface.)
 *     - Symbols necessary to implement anything else in flow/A/B, but that are *not* intended for public use, must
 *       reside in flow/A/B/detail.
 *       - The user is explicitly not allowed to `include` them directly.  However this is not enforced and not
 *         enforceable.  (Note: The same is true of all STLs, Boost, and many other products.)  (A public header
 *         *may* include a detail/ header if needed, such as when implementing function templates at times;
 *         however the user is by convention not allowed to directly include them.)
 *       - Similarly, the user is explicitly disallowed to reference such symbols directly.  Again, this isn't enforced
 *         or (in many cases) enforceable.
 *       - At this time we do not require (or encourage) to segregate such non-public symbols in
 *         `detail` sub-`namespace`s.  (Boost/STL sometimes does this; sometimes does not.)  Reason is it's annoying to
 *         move stuff between namespaces when making a non-public API public or vice versa; since user isn't meant
 *         to use such things regardless, it is sufficient to merely *indicate* what is public and what isn't; for this
 *         the choice of file/directory is sufficient and no actual `namespace` is required.  We may change this policy.
 *     - However: It is typical that a file of form x.hpp requires an x.cpp counterpart.  Obviously, this would not
 *       be exported into any public `include/` directory (as it is not a header file).  The rule is x.cpp is to be
 *       in the same directory as x.hpp (alongside it).
 *       - In the rare case where a file z.cpp does not have a z.hpp
 *         counterpart, z.cpp should usually reside under detail/ after all.  Sometimes it might make sense to place
 *         it near its most-related .hpp file.  This particular decision is left to
 *         be made on a case-by-case basis however and shouldn't come up very often.  It is also of (relatively) low
 *         importance, as .cpp files are not part of the exported API.
 *   - Headers must end in .hpp.  Others must end in .cpp.  In the rare case of a C-language-user-supporting header, use
 *     .h.  At this time we don't use .inl (and similar) files, but if there's a good reason we might do so, in which
 *     case the convention should be clearly described here.
 *   - Do not use redundant prefixes/postfixes in file names, when the containing directory already contains the same
 *     info -- even if this would create two same-named files at different places in the directory tree.
 *     E.g., if you have D/node.hpp and D/asio/node.hpp, that's fine; don't name the latter D/asio/asio_node.hpp.
 *     - For that matter: follow the same guideline when naming types, functions, and so on.  I.e.,
 *       don't have asio::Asio_node; just asio::Node.  Yes, this may require disambiguation (by more fully qualifying
 *       identifiers at times) in code.  Nevertheless.  We must take a stand! */

// -- STYLE GUIDE: File level --

/// @file
#pragma once

/* - Every source file begins with @file, so that Doxygen will add it to its Files page in the generated docs.
 * - Every header then begins with that `pragma` to avoid redefinitions from multiple `#include`s.
 *
 * - File names are to be all-lower-case words, underscore-separated.
 *   - @todo Defining formal rules beyond that feels tedious... but technically we should.
 *     Until then follow the many existing examples.
 *   - Header file name ends with .hpp.
 *     - .h if file is intended as a C API (even if supports C++ use also).  This is rare.
 *   - Translation unit names ends with .cpp. */

// This illustrates how to properly indent #directives.
#ifdef FLOW_SOME_DEFINE
#  if FLOW_SOME_OTHER_DEFINE
#    include "conditionally_included_file.hpp"
#  endif
#endif
// The indentation of #directives is 100% orthogonal to indentation of actual code, as this shows:
void f(bool flag)
{
  if (flag)
  {
#if FLOW_YET_ANOTHER_DEFINE
    do_something_conditionally_compiled();
#endif
  }
}
/* Also, use the spirit of the rules for parenthesizing expressions to express priority even when unnecessary,
 * which are described elsewhere in this guide, not just for actual code expressions but also within #if directives.
 * There is however no need to surround the entire thing in (parentheses), as a regular `if ()` forces it, while
 * #if does not. */
#if defined(FLOW_THING) && ((!defined(FLOW_OTHER_THING)) || defined(FLOW_THIRD_THING))
  // [...]
#endif

// -- STYLE GUIDE: Line width; indent; comments; doc header basics --

/* - Lines are up to 120 columns, inclusive.  [Try <110 to leave space for later edits.]
 * - One indent level = 2 spaces.  No tab characters.
 * - Comments should be composed of sentence-ish things.  Each sentence starts with a Capital and ends with a period
 *   or similar. [Optional: Use two spaces between sentence-ish things.  Many find this annoying; hence optional.]
 *
 * Multi-line comments use C-style comment markers like this. */

// Single-line comments are C++-style like this.

/**
 * - Comments that start with 2 asterisks are *doc headers* and are understood by Doxygen to be documenting the entity
 *   immediately following.  Essentially *every* entity not inside the {braces} of a function -- files, macros,
 *   `class/struct/union/enum`s, functions, variables/constants, namespaces, aliases, ??? -- *must*
 *   have a doc header.  In this case we're documenting a `class`.  When documenting a function, always document
 *   its body-less prototype only.
 *   - We explain Doxygen details elsewhere in this file though.
 * - Do *not* place any doc headers inside {braces} of a function, such as for local variables.  Just comment, or not,
 *   as you see fit for clarity/etc.
 *
 * - Note doc headers have 1 extra leading and 1 trailing lines vs. non-Doxygen comments, as seen in this block.
 */
class Cool_class
{
  // [...]

  /// Single-line *doc headers* are relatively rare but allowed and start with 3 slashes instead of 2.
  static const unsigned int S_COOL_CONSTANT_BEING_DOCUMENTED_WITH_A_ONE_LINE_DOC_HEADER;
  //< BAD: Don't use trailing doc headers like this.  [Rationale: Why add inconsistency?]

  // [...]
}; // class Cool_class
/* ^-- Closing {braces} and possibly (parentheses) spanning screenfuls should usually feature a terminating comment
 * like that `// class Cool_class` one.  Typical conventions for how these end-comments look can be found across
 * Flow by example.  This style helps readability (but may make maintenance somewhat more annoying). */

// -- STYLE GUIDE: Identifier formatting --

/* Words-separated-by-underscores formatting is to be used by ALL identifers!
 *   - Do not use CamelCase or any variation.
 *   - Do not use wordsmooshing.  Is it two words?  Yes?  Then put an underscore _ between them.  Period!
 *
 * [Rationale: This decision to forego CamelCase in favor of underscores does not imply the latter is superior
 * in and of itself.  A case can be made for either approach, certainly with pros and cons for each.
 * The deciding factor for choosing underscores for Flow originally was that the STL *and* Boost ecosystems
 * so heavily affected the project that it seemed strange to intentionally allow the visual clashing between
 * STL/Boost identifiers and Flow ones when seen side-by-side in real code (in and out of Flow itself).
 *
 * A certain large code project used CamelCase and thus provided a counterpoint.  However, even that unnamed
 * project had tons of core code that used under_score_style after all; and there's plenty of wordsmooshing too.
 * So if it's not even consistent in the first place... meh.  We do what's best.] */

/* - Every identifier consists of a root; a possible prefix (s_, m_, S_); and a possible suffix (_t).
 * - The root = always 1+ words, separated by underscores if 2+.
 * - Each words consists of ASCII letters and numbers only.  The first word in root must start with letter.
 *
 * - Most roots are all-lower-case: */
word;
two_words;
three_or_4_words;
identifier_version1_or_v2;
acronyms_are_just_words_eg_url;
wordsmooshing_abbrevs_like_uint_are_ok_if_very_well_known;

// However, compound types must start with a single Capital letter but otherwise all-lower-case:
class First_letter_of_a_class_is_capital_while_rest_always_lower_case;
struct Same_format_for_struct;
union Same_format_for_union;
enum Same_format_for_old_enum; // Non-`class` `enum` is considered a compound type for our purposes.
enum class Same_format_for_new_enum; // In any case always use `enum class` if possible, not plain `enum`.
using Typedefs_thereof_as_well = /* [anything resolving to a compound type] */;
// However, if an alias is to a non-compound type -- typically integers, etc. -- do *not* capitalize but add _t suffix:
using uint8_t = unsigned char;
using ptr_diff_t = signed long long;
/* BAD: Do not use `typedef`.
 * Use `using` (for consistency + its extra features, esp. template parameterization, shown just below).
 *   typedef signed long long ptr_diff_t;
 *   typedef [anything resolving to a compound type] Typedefs_thereof_as_well; */
template<typename Param_type>
using Parameterized_alias
  = /* [anything resolving to a compound type, with Param_type as part of this alias definition] */;
// Now one can use Parameterized_alias<X> for any X.

// Most roots are lower-case (first letter aside possibly), but a couple of things must use ALL-CAPS:
// `#define`d symbols:
#define MACROS_ARE_IN_CAPS /* [...] */
#define FUNC_MACROS_ARE_2(/* [...] */) /* [...] */
// Constants in func body:
{
  static const int STATIC_CONSTANT = /* [...] */;
  const float LOCAL_NON_STATIC_CONSTANT = 2.3; // Value known at compile time => consider it a constant => all caps.
  // Value not straightforwardly known at compile time => not a "constant" in this context => all-lower-case after all.
  const auto& blah_ref = *blah_ptr_arg;
}
enum class /* [...] */
// or: enum /* [...] */
{
  S_ENUM_VALUES_ARE_CONSTANTS_TOO, // S_ prefix explained below.
  S_ENUM_VALUES_ARE_CONSTANTS_3 = 3
};

// So, those are the roots.  What about prefixes and postfixes?  Simple:

// Only one postfix exists: _t, used for aliases of non-compound types.  See 2 examples above.
// Three prefixes exist: s_, m_, S_:

/* Anything `static` must be prefixed with s_ or S_ depending on capitalization chosen above for the root.
 * That includes file-level, compound member, even local `static`s; plus `enum` members (technically not `static`). */
static Cool_class s_singleton_instance; // Not a constant.
static const std::string S_STRING_VAR; // Constant.
// `enum` values are constants.  They must be prefixed with S_ (see `S_ENUM_VALUES_ARE_CONSTANTS_3` above).

/* Any non-static member variable of a compound type, a/k/a *data member*, must be prefixed with m_.
 * Since a member constant is by definition `static`, there is no M_ prefix but an S_ prefix.
 * A static member variable has s_ prefix.
 * A local variable, or a local constant, carries no prefix.
 * A global non-member non-static constant (probably rare) carries no prefix.
 * Same with global non-member non-static variable (even rarer). */
extern int no_prefix_because_not_a_member_nor_static; // .hpp
extern int NO_PREFIX_BECAUSE_NOT_A_MEMBER_NOR_STATIC; // .hpp
int no_prefix_because_not_a_member_nor_static; // .cpp
int NO_PREFIX_BECAUSE_NOT_A_MEMBER_NOR_STATIC; // .cpp
class Some_class
{
  // [...]
  const Some_class& m_data_member_is_a_ref;
  Some_enum m_enum_data_member;
  static const Some_type S_CONSTANT_MEMBER;
  static constexpr int S_CONSTANT_MEMBER = 32;
  static Some_type s_variable_member;
  void some_func()
  {
    int no_prefix_because_not_a_member;
    const int NO_PREFIX_BECAUSE_NOT_A_MEMBER = 2;
    constexpr int NO_PREFIX_BECAUSE_NOT_A_MEMBER = 3;
    // [...]
  }
  // [...]
}

// -- STYLE GUIDE: Order of declarations --

/* Flow style cares about ordering things consistently and readably, more than usual in my opinion.
 *
 * - Basic principle: Whenever an API and implementation for anything is written -- including APIs used only
 *   internally -- the API (black box) must come first; implementation (white box) second.  In other words,
 *   when *declaring*, start with public, end with private (protected in middle if applicable).
 *   [Rationale: User of entity X cares how to use X, not what's inside X -- ideally.  So the public parts come first.
 *   Plus it adds consistency.]
 *   - The function *bodies* need not follow any such order, so just do what feels right.
 * - Basic principle: Whenever in some section of an API or implementation (notably a `public:` or `private:` section
 *   of a `class` or `struct`), follow the same consistent order of grouped items:
 *   - The order is: types, constructors/destructor, functions (including `operator`s), constants (i.e., all-caps
 *     things), data members.  [Rationale: Roughly speaking it follows a black box->white box ordering.]
 *     (`static` or otherwise).
 *     - However, data-store `struct`s should place constructors/destructor and functions at the bottom instead.
 * - Basic principle: There is a standard order of items in each file but especially for headers .hpp.
 *   Follow the order in the snippets below.
 * - Basic principle: Human nature: The ordering guidelines are easy to forget to follow.  To encourage it, label
 *   each grouping with a standard comment (e.g., "// Constructors/destructor.", "// Data.", etc.).  Snippets below.
 *   - Do NOT have "empty" such headers (e.g., if there are no data members then no "// Data." comment either).
 * - Basic principle: Exceptions to these guidelines will happen and aren't the end of the world at all. */

// order.hpp follows: Illustrates above principles/shows proper ordering by example, in a header file:

/// @file
#pragma once

#include "own_headers_included_first.hpp"
#include <boost/higher_level_angly_headers_go_next.hpp>
#include <vector> // STL- and lower-level includes typically bring up the rear.

#if 0
/* As prescribed just above, precede groupings of like things with these standard, single-line comments like:
 * Again, this helps maintain discipline and set a good example.
 * Reiterate: DO omit such a heading when that "section" is empty. */
#endif

// Types.

/// Short-hand for boost::any.  Any ordering is fine within the //Types grouping.
using Cool_type = boost::any;

/**
 * A class that accomplishes so-and-so.
 * Similarly with structs, unions, whatever.
 */
class Cool_class
{
public:
  // [...Follow order as shown in private: below....]
protected:
  // [...Follow order as shown in private: below....]
private:
  // Friends.

  /**
   * Server_socket must be able to forward `accept()`, etc. to Cool_class.
   * This documents a `friend` relationship.
   *
   * Can also have function `friend`s here, though they don't require official doc headers.
   */
  friend class Server_socket;

  // Types.

  /// Short-hand for ref-counted pointer to this class.
  using Ptr = boost::shared_ptr<Cool_class>;

  /// An inner class (struct, union, enum, enum class, etc.).  Declare its body outside whenever possible!
  struct Inner_class_of_cool;

  // Constructors/destructor.

  /// Constructs the object.
  Cool_class();

  /**
   * Copies object into newly created `*this`.
   * @param src
   *        Source.
   */
  Cool_class(const Cool_class& src);

  /**
   * Explicitly deleted (possibly would-have-been-auto-generated) ctors and operators are so declared.
   * @param src
   *        Source.
   */
  Cool_class(Cool_class&&) = delete;

  /// Boring destructor.
  virtual ~Cool_class();

  // Methods.

  /**
   * Copies object `src` onto `*this`.
   * Operators are considered just methods in this context but tend to come first due to `operator=()` being
   * conveniently placed near copy constructors and similar.
   *
   * @param src
   *        Source.
   */
  Cool_class& operator=(const Cool_class& src)

  /**
   * A regular method.
   *
   * Prototype only!  Template and inline function bodies MUST be outside class body (see below).
   */
  void cool_method();
  /* ^-- BTW, no m_ (or other prefix) to indicate a method is private.
   * [Rationale: Some like such a convention, but in my experience it's too easy to forget and a huge pain to maintain,
   * as public things often become private and vice versa... no one wants to do that renaming.  Goes bad even with good
   * intentions in my experience.] */

  /**
   * A method template.
   * @tparam Some_type
   *         Document any template parameters.
   */
  template<typename Some_type>
  Some_type cool_template_method();

#if 0
/* If this is a pure data-store `struct`, ideally move the below (constants/data) above
 * constructors/destructor/functions, as in that case the data are the "star." */
#endif

  // Constants.

  /**
   * Constants formally come before other data; and are always static.
   * Reminder: If it doesn't have a value straightforwardly known at compile time, it's not a constant and not
   * upper-case.
   */
  static const float S_PI;

  // Data.

  /// Carefully document each data member.  Same as above but for not-constants.
  Inner_class_of_cool* m_cool_ptr;
}; // class Cool_class

#if 0
/* More types (classes/structs/unions/`using` aliases/enums/enum classes) continue here if needed; including inner
 * classes.
 *
 * - However, leave private inner classes (like Cool_class::Inner_class_of_cool) until the end, ideally, following
 *   the aforementioned basic principle: API first, implementation second. */
#endif

/**
 * @private
 *
 * The body of an inner class should be kept outside of its containing class, for readability.
 * [Sometimes C++ circular reference rules make this hard or impossible, so exceptions to this are acceptable.]
 *
 * Use @private as shown above *if and only if* the inner class is private.  This is due to a Doxygen quirk
 * (arguably bug).
 *
 */
struct Cool_class::Inner_class_of_cool
{
  // [...same order of stuff as shown in Cool_class]
};

// Free functions.

/**
 * Free functions aren't super-common but definitely legitimate and go in this area.  Prototype only!
 *
 * @param c1
 *        First operand.
 * @param c2
 *        Second operand.
 * @return New object that sums the operands.
 */
Cool_class operator+(Cool_class c1, Cool_class c2);

/**
 * Output of Cool_class `c` to a `basic_ostream`.
 * This is another free function, but this one is a template.  They can go in any order though.
 *
 * @tparam Ostream
 *         Document the template param type.
 * @param os
 *        Stream to write to.
 * @param c
 *        Object to write.
 * @return `os`.
 */
template<typename Ostream>
Ostream& operator<<(Ostream& os, Cool_class c);

// Constants.

/// The mathematical `e`.  Non-member constants are fairly rare but would go in this area.
extern const float S_EXPONENT;

// Data.

/// Non-member non-constant/variable: these are arguably even more rare but would go here.
extern Cool_class s_singleton_instance;

#if 0
/* Everything above was the API!  The separation helps readability, as implementation details come later to the extent
 * possible.  This isn't 100% true: `private` isn't API; `private` inner classes aren't API.  Still, pretty close. */
#endif

// Template/inline implementations.  [We actually forbid explicit inlining elsewhere, but just in case, they'd go here.]

#if 0
/* Finally the "actual code" (function bodies) go here!  Recall, do NOT duplicate doc headers.
 * *Every* body *must* have a prototype present somewhere earlier: class/etc. methods inside class{},
 * free functions outside.  Then all the bodies go either here (templates, inline) or in .cpp counterpart (others). */
#endif

template<typename Ostream>
Ostream& operator<<(Ostream& os, Cool_class c)
{
  // [...] This is a free function, but member functions (methods) go in this area as well:
}

template<typename Some_type>
Some_type Cool_class::cool_template_method()
{
  // [...]
}

// order.cpp follows: Illustrates above principles/shows proper ordering by example, in a counteraprt to header file:

/// @file

#include "[...]/order.hpp"
// [...includes...]

#if 0
/* We forego tediously describing the ordering in here.  In general, because this is not the API (unlike in .hpp),
 * things are more lax in here.
 *
 * - Still precede each grouping of stuff with headers.
 * - Typically, you will only have the following, sans doc headers (see below):
 *   - // Static initializers.
 *   - // Implementations.
 *
 * Nevertheless, there are many other things that might be necessary less commonly.  So I just list everything I can
 * think of, below; copy/paste into your .cpp file perhaps; delete what's not relevant to you; and proceed to fill them
 * out.
 */
#endif

// Static initializers.  [Common: Initializations of constants, static data, etc. declared/documented in .hpp.]

// Local types.  [Rare: Types used only in this .cpp.  This is rare in C++, but who knows?]

// Local functions.  [Rare: Prototypes *and doc headers* ONLY!  `static` non-member functions are rare in C++.]

// Local constants and their initializers.  [Rare: Constants used only in this .cpp.  Again, rare in C++.]

// Local data and their initializers.  [Ditto.]

// Implementations.  [Common: The bodies of functions declared in .hpp and (uncommonly) local ones from just above.]
#if 0
/* It also helps, when there are 2+ classes/structs/etc. with bodies, to have a separate section for each:
 *   // Cool_class implementations.
 *   // Cool_class::Inner_class_of_cool implementations.
 * It's left to your discretion however. */
#endif

// -- STYLE GUIDE: Spacing and indentation --

/* Beyond the basics (no tab characters; 120 columns per line; indent level = 2) there are various conventions
 * regarding white-space and indentation.  Many style guides omit some of these details, and inconsistency is common.
 * Please follow the following rules as shown by example. */

namespace flow::submodule
{

/* A new { block } *always* means new indentation level, *except* due to being within namespace {}.
 * Nested namespaces themselves shall use C++17 syntax, wherein one writes `namespace a::b::c` instead of
 * `namespace a { namespace b { namespace c`.  This is shown above.
 * (Even if 1 file declares members of 2+ different namespaces, one shall declare each namespace, no matter its
 * depth level, using `namespace a::b::c` syntax, where `a` is always an outer namespace.)
 * The actual members declared within a given namespace shall begin indentation at column 0, as seen in the following
 * `cool_method()` declaration.
 * [Rationale: The latter avoids losing one indent-level of real estate on practically every line of code with little
 * benefit.
 * @todo Since we are now on C++17, and hence mandate the use of `namespace a::b::c {}` syntax, consider updating style
 * guide -- and all code -- to indent properly even inside namespaces, while continuing to demand the use of
 * `namespace a::b::c` syntax.  One level on indent shall be lost by this, but since with the C++17 syntax it *is*
 * at most just the 1 indent-level -- as opposed to typically 2 or more before C++17 syntax -- the prettier and more
 * consistent-overall indentation may be worth it.] */

void Cool_class::cool_method(int a, int b)
{
  if (a < b)
  {
    /* *Always* use {} around one-statement flow-control bodies even when unnecessary, such as here.
     * [Rationale: Doing otherwise leads to frequent maintenance bugs.  Also this helps simplify rule set.] */
    return;
  }

  // else
  // [^-- Little markers like this are common in Flow.  Not mandatory but consider doing same.]

  /* - *Always* place spaces around binary/ternary operators (`a * b`, `?:`), never in front of unary ones (`-a`).
   * - *Always* use `()` to enforce order of operations, even when redundant to build-in language rules!
   *   [Rationale: It hugely helps avoid bugs.  It also helps reassure reader of the coder's intent.]
   *   - Exception: The `.`, `->`, `*`, `&` operators.  They're so high-priority that there's really no need.
   *     Same for function calls `func()`.
   *   - Exception: The `=` and other assignment ops.  They're so low-priority that there's really no need.
   * - If an expression is long or complicated, use your judgment to optionally make it into multiple lines for clarity.
   *   - Use column-lined-up intra-expression indentation as shown here.
   *   - If an operator (in this case `?` and `:` and `&&`) can go either at the end of line N or start of line (N + 1),
   *     prefer the latter.
   *     [Rationale: There are tedious pros/cons.  I chose this for consistency, but the other way would've been OK.] */
  a = b = (m_other_flag || (m_flag
                            && (((-a) * rnd()) > 3)))
            ? (m_val1.m_sub_val + cool_func1())
            : (m_val2_ptr->m_sub_val + cool_func2);

  /* A separate `statement;` per stack variable declaration, even when you can combine them.
   * [Rationale: `int* a, b;` misleads one into thinking `b` is a pointer like `a`.  And simplicity+consistency.] */
  int* a;
  int b;
  float& c = m_floatie; // The type is `float&`; the value is `c`.  Do NOT do: `float &c;`.
  float* d = m_ptr; // Same.  Type is `float*` (not `float *`); value is `d`.

  // `const` ordering in type names:
  const char* str; // This is fine; it's so typical out in the world that we couldn't possibly disallow it.
  char const * str; // This is arguably better; reading backwards is useful: pointer to / const / char.
  // What if you want the pointed-to value to be immutable AND the pointer *itself*?
  char const * const str; // Read R2L: const / pointer to / const / char.
#if 0 // Don't do this when it gets complex with multiple consts; use the R2L ordering to avoid confusion (prev line).
  const char * const str; // Don't do this.
#endif

  /* Logging is very common and has some tiny subtleties to improve readability.
   * Intra-message indentation is to be avoided.  [Rationale: Just cosmetics.  It looks nicer.] */
  FLOW_LOG_WARNING("In logging/diagnostics/etc. place [" << a << "] around variables.  "
                   "Like comments, use sentence-like things, start with capital, end with period/etc. and 1-2 spaces "
                   "unless end of the message.  Optional but highly encouraged.");
  // But when there are 2+ arguments to the call, add intra-arg indentation back in.
  FLOW_LOG_WITHOUT_CHECKING
    (Sev::S_WARNING,
     "Async_file_logger [" << this << "] @ [" << m_log_path << "]: do_log() just tried to log msg when file stream "
       "was in good state, but now file stream is in bad state, probably because the write failed.  "
       "Giving up on that log message.  Will retry file stream for next message.");
  // ^-- Note indentation in 2nd arg, so it's clear at glance there are 2 args and not 4.

  // Personal discretion about when to break-and-indent, vs. when to line up by column.
  FLOW_LOG_WITHOUT_CHECKING(Sev::S_WARNING, "Some message with stuff in it."); // Cool.
  FLOW_LOG_WITHOUT_CHECKING(Sev::S_WARNING,
                            "Some message with stuff in it."); // Cool as well.
  FLOW_LOG_WITHOUT_CHECKING
    (Sev::S_WARNING, "Some message with stuff in it."); // Perfectly cool also.
  FLOW_LOG_WITHOUT_CHECKING
    (Sev::S_WARNING,
     "Some message with stuff in it."); // Excessive here (since it's short), probably, but it's also fine.

  /* Finally, when assembling long strings (such as for logging, or inside FLOW_LOG_...()) via `ostream`,
   * it is OK to somewhat break other conventions for readability.  To quote real code: */
  os
    <<
    "--- Basic socket state ---\n"
    "Internal state: [" << m_int_state_str << "].\n"
    "Client or server: [" << (m_is_active_connect ? "client" : "server") << "].\n"
    // Also visible below in the options but show it separately for convenience.
    "Reliability mode: [" << (m_sock_opts.m_st_rexmit_on ? "reliable/rexmit on" : "unreliable/rexmit off") << "].\n"
    "--- Buffers/queues/lists ----\n"
    "Receive window (free space): [" << m_rcv_wnd << "]\n"
    "  = limit\n"
    "  - Receive buffer size: [" << m_rcv_buf_size << "]\n"
    "  - reassembly queue total data size: [" << m_rcv_reassembly_q_data_size << "] (0 if rexmit off)\n"
    "      " << (m_sock_opts.m_st_rexmit_on ? "Reassembly queue" : "Dupe check list") <<
    " length: [" << m_rcv_packets_with_gaps << "].\n"
    "  Last advertised: [" << m_rcv_wnd_last_advertised << "].\n";
  /* ^-- Note that this combines compiler-concatenated string-literals and trailing << (the latter being discouraged
   * elsewhere in this guide) for readability, so the final output is somewhat easier to visualize. */

  /* Lambdas are HEAVILY used yet very unusual-looking vs. rest of C++.  Formatting decisions can be challenging.
   * Use the following conventions. */

  /* A lambda can (typically SHOULD NOT but use discretion!) begin anywhere, in any expression, even though it
   * represents a function/closure thing.  In this case it begins smack in the middle of a regular function call inside
   * another function call.
   *   - Always include a [capture section], even if empty.
   *   - Always include an (args list), even if empty.
   *     - Always include an explicit `-> type` -- even if it can be auto-figured-out -- on same line as (args list).
   *       - Except if the type would have been `void`; then omit it.
   *
   * [Rationale: It's quite subjective, but I've (ygoldfel) found this leads to visual pattern recognition of a lambda;
   * yet without excessive amounts of boiler-plate characters.  Plus, it creates discipline to carefully craft
   * each lambda as opposed to "shooting from the hip" as scripting languages like JavaScript encourage.] */
  sched_task
    = util::schedule_task_at(get_logger(), wait_until, &task_engine,
                             m_make_serial.wrap // Lambda begins as the arg to this function call:
                               ([this, timeout_state, on_result, would_block_ret_val, event_set]
                                (bool) -> bool
  {
    /* *Main point*: The lambda's { body } *always* starts on the same column as the current *statement*.
     * This is regardless of there the lambda itself -- the [captures] -- started.
     * [Rationale: This preserves a sane indentation level! A { body } that starts on some column in the middle of
     * a line is just a nightmare of inconsistency and maintenance.] */

    FLOW_LOG_TRACE("[User event loop] "
                   "Timeout fired for async op [" << event_set << "]; clean up and report to user.");

    Error_code dummy_prevents_throw;
    event_set->async_wait_finish(&dummy_prevents_throw);

    return true; // Always use an explicit `return x;`, if you must return a value.  No funny lambda business.
  })); // timer.async_wait() callback.

#if 0 // NO.  Do not start the body lined up like this.  Line it up with the statement, as shown above.
  sched_task
    = util::schedule_task_at(get_logger(), wait_until, &task_engine,
                             m_make_serial.wrap // Lambda begins as the arg to this function call:
                               ([this, timeout_state, on_result, would_block_ret_val, event_set]
                                (bool) -> bool
                                {
                                  // [...Body...]
                                })); // timer.async_wait() callback.
#endif

  // You may put lambda { body } on same line as the rest of it -- for VERY short bodies like this.
  func([this](){ hooray(m_data_member); });

  // While in-line lambdas are allowed (as just above), they *must* then be the last part of the statement.

  func(1, 2, [this](){ hooray(m_data_member); }); // A bit ugly arguably, but this is allowed.
#if 0 // NOT allowed.  There is more "stuff" after the lambda, so don't in-line it.
  func([this](){ return m_data_member; }, 1, 2);
#endif
  // In that case save it in a variable.
  const auto lambda = [this](){ hooray(m_data_member); };
  func(lambda, 1, 2);

  /* To be clear, in general, if an in-line lambda is looking even a little obnoxious, err on the side of
   * "just save it in a `const auto`," as shown above. */
} // Cool_class::cool_method() [<-- Note the convention for terminating `}` comment of non-tiny function bodies.]

/* [Optional but strongly encouraged:] If a function takes 1 or more function-style arguments (which
 * in practice end up being created from lambdas, usually), put them at the end of the function's arg list.
 * [Rationale: It helps make it possible to in-line lambdas in most situations, if desired.] */
void Cool_class::cool_method2(log::Logger* logger, const flow::Function<void ()>& on_completion_func)
{
  // [...]
  cool_method2(logger, [this]()
  {
    hooray(m_data_member);
  }); // Can always in-line it, because it's the last arg to cool_method2(), so nothing can follow it.
  // [...]
}

} // namespace flow::submodule

// Label-type thingies (ctor init; public/private/protected; `case X:`/`default:`; the rare actual goto label):
class X
{
public: // Label starts on same column as preceding thing.
  X();

private: // Ditto.
  C m_data_member1;
  int m_data_member2;
};

X::X() : // ATTN: initializers start on next line and indented to match the { body }.
  m_data_member1(rnd()),
  m_data_member2(0)
{
  switch (/* [...] */) // @todo This is the current convention, but it's kinda ugly TBH.  Consider adding indentation.
  {
  case 0: // Label again starts on same column as preceding thing.
    do_something0();
    break;
  case 1:
  { // If you have declarations, use a { block } to segregate it;
    auto x = rnd();
    do_something1(x);
    break;
  }
  default:
    goto get_out;
  }

  // Alternative `switch` formatting, when all case bodies are single-statement, short returns:
  switch (op_result())
  {
    case Xfer_op_result::S_FULLY_XFERRED: return "FULLY_XFERRED";
    case Xfer_op_result::S_PARTIALLY_XFERRED: return "PARTIALLY_XFERRED";
    case Xfer_op_result::S_ERROR: return "ERROR";
    default: assert(false);
  }

get_out: // A rare actual label.
} // X::X()

// -- STYLE GUIDE: Misc/loose ends --

#if 0 // Do not do file-scope `using`.  [Rationale: It's criminal in .hpp; arguably obnoxious in long `.cpp`s.]
using namespace std; // Just, no.
using std::string; // More defensible, in .cpp, but we still don't do it.
#endif

// However, `using` (not `using namespace`) is ubiquitous at function scope.
void Cool_class::some_method()
{
#if 0
  using namespace std; // Still, just, no.
#endif
  /* It is allowed and *strongly encouraged* [but optional] to avoid all/most namespace `qualifiers::`
   * by pre-qualifying them via `using` and namespace aliases.
   *   - Always at the top of function.  Almost never in a lower scope.
   *   - Types and functions can be thus `using`ed.
   *   - Namespace aliases can go here too. */
  namespace util = flow::log::util;
  using std::string;
  using util::cool_util_method;

  // 2 things are pre-qualified above; 1 thing is explicitly qualified.  The latter is usually avoided but is OK.
  string x = cool_util_method() + util::some_free_function();
}

// A bunch of subtleties when using complicated C++11 initializers, so just kinda follow some examples:
const boost::unordered_map<Event_set::Event_type, Event_set::Func_ptr>
        // @todo It's a bit ambiguous what the indentation "anchor" column should be here....
        Event_set::S_EV_TYPE_TO_IS_ACTIVE_NODE_MTD
          // Use judgment to either have {} innards indented on separate line(s) or in-line.  Examples of both:
          ({
             { Event_set::Event_type::S_PEER_SOCKET_READABLE, &Node::sock_is_readable },
             // ^-- Spaces around `contents` in `{ contents }`, when it's all on one line. --v
             { Event_set::Event_type::S_PEER_SOCKET_WRITABLE, &Node::sock_is_writable },
             { Event_set::Event_type::S_SERVER_SOCKET_ACCEPTABLE, &Node::serv_is_acceptable }
           });

// Misc. convention:
T* x = nullptr; // This is OK, but Flow was written before nullptr was available; we just use the following:
T* x = 0; // This is the Flow convention.  Note, no `NULL`!  NULL is C stuff, no need.

// -- STYLE GUIDE: Header files and forwarding; _fwd.hpp pattern --

/* Flow now follows the forwarding-header pattern observed in various Boost libraries, although I (ygoldfel) have not
 * seen this pattern formalized anywhere and first arrived at it in a certain other (currently larger) project which
 * depends on Flow.  I then back-applied this pattern to Flow itself.  While the presented convention looks laborious,
 * we assure you over time it will save time and pain.  It will help compile times and prevent circular-dependency
 * awfulness that would otherwise plague larger APIs.
 *
 * Note this convention applies not merely to the API exported to the user; it applies, also, to internal impl
 * code as it goes about its internal business (typically residing in detail/ sub-dirs).
 *
 * First the basic rules.  Consider any declared symbol at the file level (i.e., direct
 * member of a `namespace` -- not a data member): aggregate type (`struct`, `class`, `union`) or template thereof;
 * `enum` or `enum class` (note: elsewhere we recommend avoiding the former) or template thereof; type alias
 * (`typedef` or `using`, though note: elsewhere we recommend avoiding the former); variable or constant
 * (non-`constexpr`); free function including `operator`s.  (We exclude macros and `constexpr` "variables" for
 * now; but see discussion below in separate section.)  Note we are speaking only of symbols exported beyond a
 * given translation unit; items local to a translation unit (anon-namespace or `static` items in a .cpp file, etc.)
 * are not relevant here.
 *
 *   - Aggregate types and templates thereof:
 *     - It must be forward-declared (e.g., `class X;`, `template<typename T> struct Y;`) exactly once.
 *     - It must be separately defined (meaning with its body provided), in compatible fashion, exactly once.
 *       - As shown elsewhere, this definition must be fully Doxygen-documented, including a leading doc header.
 *   - Enumerations and templates thereof:
 *     - It must be declared/defined exactly once, together with its full Doxygen docs (including leading
 *       doc header).  Do not forward-declare.  (Rationale omitted: But trust us; trying to fwd-declare it is
 *       more trouble than it is worth; and avoiding it generally plays with with other nearby conventions.)
 *   - Type aliases:
 *     - It must be declared/defined exactly once (as required by C++), together with its Doxygen doc header.
 *   - Varibles and constants (non-`constexpr`):
 *     - It must be forward-declared exactly once with `extern` keyword: `extern T var;`, `extern const T CONSTANT;`,
 *       together with its doc header.
 *     - It must be separately defined (initialized) exactly once: `T var; // Possibly initialized also.`,
 *       `const T CONSTANT = ...;`, `const T CONSTANT(...);`.
 *   - Free functions:
 *     - It must be forward-declared (a/k/a prototyped) exactly once, together with its Doxygen docs including
 *       doc header.
 *     - It must be separately defined (meaning with its body provided) exactly once in compatible fashion.
 *
 * For each type of symbol, there are (as you can see) either 1 forward-declaration and 1 definition, or just
 * 1 declaration/definition.  You already will know from elsewhere in this doc (and general C++
 * knowledge) where to put most of these.  Briefly:
 *   - aggregate type definition/body goes in some header .hpp;
 *     - (but what about fwd declaration?);
 *   - same for enumeration definition/body;
 *   - same for type alias definition;
 *   - variable/constant `extern` declaration goes in some header .hpp; initialization in some .cpp;
 *   - free function prototype (fwd declaration) goes in some header .hpp;
 *     - free function definition/body goes in .cpp except for templates, `constexpr`s (functions), and
 *       `inline`s (which we elsewhere discourage in favor of LTO) which go in .hpp.
 *
 * Without the _fwd.hpp pattern, the more detailed conventions are as follows:
 *   - When something goes into an .hpp, put it in whatever .hpp file seems right.
 *   - If something requires a declaration and a *separate* definition, and the above says that both belong in
 *     .hpp, then just put them both into the same .hpp (declaration up-top; definition down below in impl area).
 *     The Doxygen doc header should be on the declaration, not the definition, so the user sees it first.
 *     - And furthermore fwd declarations of compound types are simply not specified... throw them in when
 *       circular definitions arise, but generally, whatever; just wing it.  However naturally the Doxygen docs
 *       shall be on the class/struct/etc. body itself and not any fwd-declaration of it.
 *
 * However -- in this pattern that is *not* correct (at least not usually).  Rather:
 *
 * **Key rule**: The forward-declaration, or only declaration+definition, shall usually be placed in a
 * *forward-header* file.  A forward-header shall always be named with form X_fwd.hpp.  This is not an *absolute*
 * rule, and exceptions to it are not all criminal; but they should be rare.  The more exceptions there are, the more
 * pain is likely to occur down the line.  We talk about good exception cases below.  For now let's forge on:
 *
 * That rule, in and of itself, is pretty simple.  Got a thing that must be declared/defined or forward-declared?
 * Put it in a _fwd.hpp.  If there's only one definitin/declaration, then that's that.  If there's also a 2nd
 * definition/declaration (aggregate types/templates thereof; variables/constants; free functions/templates) then:
 *   - Any code requiring use of a given symbol can include the relevant _fwd.hpp when only a forward-declaration
 *     is required; e.g., if only referring to forward-declared type `T` by pointer or reference.
 *   - If it is insufficient (e.g., when `sizeof T` must be known to compiler), the code can include the non-_fwd.hpp
 *     header.
 *
 * This allows for (1) shorter build times; and (arguably more importantly) (2) the methodical pre-disentangling of
 * hairy circular-reference nightmares during to C++'s single-pass compilation model.
 *
 * The convention gets somewhat annoying when it comes to the details of *which* _fwd.hpp to place the
 * declaration.  We note that different libraries do different things and not very consistently at that.  We give
 * it our best shot to nail it down and remove ambiguity for Flow.  Let's go: The _fwd.hpp files by convention are:
 *   - In top-level module/namespace flow::X, flow/X/X_fwd.hpp, a/k/a/ *top-level forward-header*.  Essentially this one
 *     always exists, assuming flow::X exports any symbol.
 *     - In some cases a top-level module may contain a sizable sub-module, in a sub-namespace; so namespace
 *       flow::X::...::Y.  Then there is potentially (not necessarily) flow/X/.../Y/Y_fwd.hpp, a/k/a
 *       *sub-level forward header*.
 *   - In a given detail/ sub-directory (direct or otherwise) of flow/X, so flow/X/.../Y/detail, there is potentially
 *     (not necessarily) flow/X/.../Y/detail/Y_fwd.hpp.  (Note: not detail_fwd.hpp or forward.hpp or ....)
 *     This is known as *detail-level forward-header*.
 *     - If the detail/ aspect of some module is particularly complex, there could be more sub-dirs under detail/;
 *       technically it may be desirable to have a lower-level Y/detail/A/.../B/B_fwd.hpp.  Hopefully you can reason
 *       out what to do on your own based on the below discussion, but we won't formally spend time getting into it.
 *       Use common sense/analogy thinking.
 *   - At any dir level, for a given C.hpp focused on central aggregate type (or aggregate type template, or namespace,
 *     or compact feature or ...) named ~C, there is potentially (not necessarily) C_fwd.hpp in the same dir.
 *     This is known as *header-specific forward-header.
 *
 * First decide which of the ~3 locations your declaration belongs in; then create that _fwd.hpp if needed; and
 * then put the declaration there in the appropriate section.  So which one should it be?  Answer: it shall be either
 *   - in header-specific forward-header C_fwd.hpp; or
 *   - in a top-level or sub-level forward header flow/X/X_fwd.hpp or flow/X/.../Y/Y_fwd.hpp; or
 *   - in detail-level forward-header detail/Y_fwd.hpp (or possibly an even deeper-down one, but as noted we won't
 *     get into this and leave it as exercise to reader).
 *
 * Place a symbol info C_fwd.hpp if (1) it has to do with (e.g., free functions are common) with type/feature/whatever
 * (often class) C; and (2) the *total* set of symbols to-do with C (free functions at least usually identified with
 * Doxygen @relatesalso command) is subjectively *larger* than the following boring/vanilla set:
 * ostream<< operator, swap().  In other words if class/struct C comes with a sizable free-function/etc. API operating
 * on C-ish things, then stylistically they all belong in a segregated C_fwd.hpp that shall accompany C.hpp.
 * (So then C_fwd.hpp will contain free functions, class/etc. fwd-declarations, variable/constant `extern`
 * declarations, enum declarations, alias definitions; C.hpp will contain class/etc. bodies/definitions.)
 * Otherwise -- the typical case being a mere boring ostream<<C operator -- put it in a non-C-specific catch-all
 * _fwd.hpp shared among C and typically other entities.
 *
 * Supposing you chose a non-header-specific _fwd.hpp (which is typically indeed the easiest choice, except for
 * Cs with large free-function APIs), then you just need to pick which _fwd.hpp applies.  If your symbol
 * is under detail/, then use a detail-level forward-header; otherwise use a top-level or sub-level forward-header.
 *
 * In the latter case (which, again, is quite common), should it go into a top-level or sub-level forward-header?
 * Obviously if the symbol is directly in flow/X, then place it in the top-level X_fwd.hpp.  If however it's in
 * flow/X/.../Y, then it's a subjective decision.
 *   - Simplicity is best; so if it does not seem particularly offensive, just don't create sub-level forward-headers;
 *     just use the catch-all one at the top.
 *   - However, if truly a sub-module of flow/X is its own (large) animal that is intentionally segregated from
 *     the rest of flow/X, then you'll want to bite the bullet and create its own flow/X/.../Y/Y_fwd.hpp.
 *
 * Note on doc headers, in case it doesn't flow naturally from the above:
 *   - For the guys where there is only declaration/definition, obviously the doc headers shall be on that one.
 *   - For the guys where there is a declaration/prototype + separate definition/body:
 *     - Aggregate types: doc headers go above and throughout the definition/body (hence .hpp).
 *     - Constants/variables: doc header goes above the `extern` declaration (hence _fwd.hpp).
 *     - Free functions: doc header goes above the prototype (hence _fwd.hpp).
 *
 * I (ygoldfel) am sad at how many words I have written; and worry it might be hard to understand.  Nevertheless
 * I had to try to write it, if only to clarify my own thoughts on the matter for myself.  Hopefully it helps.
 *
 * That said, the most important thing is to be disciplined in placing the forward (or only) declaration in a _fwd.hpp
 * file.  It really does save pain as a project grows. */

/* - Corollary: In more complex modules _fwd.hpp files will at times need to `include` other _fwd.hpp files, whether
 *   from the same module or others.  This should be straightforward to reason about; you'll see.  One corollary
 *   is that, if one follows the above rules, a _fwd.hpp will ~never contain struct/class/union or free-function bodies,
 *   only their forward-declarations and prototypes respectively.  A corollary of *that* is that if _fwd.hpp includes
 *   another (in-project) header, then it should ~always itself be _fwd.hpp.  Otherwise you've broken the point of
 *   the outer _fwd.hpp's existence which is to *only* forward-declare things.  Otherwise compilation times may
 *   increase, and tricky circular issues may arise. */

// -- STYLE GUIDE: Exceptions to _fwd.hpp pattern --

/* As noted above: Above are not *absolute* rules, and exceptions to it are not all criminal; but they should be rare.
 * The more exceptions there are, the more pain is likely to occur down the line.
 *
 * Here are known reasonable exceptions as of this writing.
 *   - Usually a _fwd.hpp should not include a struct/class/union (or template thereof) body... but sometimes
 *     it is okay to do so.  Hand-wavily speaking this is the case when the type C is a *glorified alias* or
 *     *empty*.
 *     - Empty types, usually `struct C {}`, are used in the tag-parameter pattern among other things.
 *       They're fine to include directly in _fwd.hpp.  Omit the forward-declaration; declare directly in _fwd.hpp;
 *       include the doc header as you would normally in non-_fwd.hpp.
 *       - Similarly for types (for example flow::Container_traits) that lack any data but have only `static` constants
 *         and/or type aliases.
 *     - Consider a class/class template that non-virtually subclasses another class/class template but adds no
 *       data members outside of `static` constants -- so only a ctor/method API and/or static constants.  If this
 *       added API + constants is small enough -- e.g., adding convenience constructor(s)
 *       and/or a couple compatibility accessors -- then it can go into _fwd.hpp.  Omit the forward-declaration;
 *       declare directly in _fwd.hpp; okace the doc header as you would normally in non-_fwd.hpp.  This class
 *       is "almost" an alias to its superclass.
 *       - If the class/class template C is too large to stylistically belong in a module-level _fwd.hpp, you might
 *         place it into its own C.hpp; but it is then okay to `include` in that _fwd.hpp.  If C is oft-used, this
 *         might be convenient for the user.
 *       - (For example Flow defines flow::Function<> which subclasses std::function<> and adds some minor convenience
 *         APIs but no data.)
 *     - Similarly consider a struct or class that merely wraps a single item, for type safety and/or to provide
 *       a little wrapper API.  For example: a `struct Native_handle { int m_fd; }` with a few niceties like
 *       an ostream<< operator and perhaps a couple convenience methods.  It is ~always copied by value, like an int.
 *       So it's "almost" an alias to the int.  So it can go into _fwd.hpp.  Again: Omit the forward-declaration;
 *       declare directly in _fwd.hpp; include the doc header as you would normally in non-_fwd.hpp.
 *       - Again you might instead put it into its own X.hpp... but then `include ".../X.hpp"` in module-level
 *         _fwd.hpp.
 *   - Top-level flow/common.hpp is something of a special case: It lives outside of (above) any top-level module;
 *     and it *is* a forward-header... but not named that way.  Still, that is what it is.  Anyway... it's a special
 *     case, and as noted in that file things should be added to it *very* sparingly in any case.  What if
 *     we want something "heavy" (a class body, for example) in common.hpp?  Doesn't that break the convention of
 *     not doing that in forward-headers?  Answer: yes, it hypothetically would, so you shouldn't do it... but
 *     as noted common.hpp is intentionally *very* limited/small.  If something is that big then it should go into
 *     flow::util or some other module, not directly into namespace `flow` and thus common.hpp.
 *     - Incidentally it happens to feature the aforementioned flow::Function<>, essentially or "almost" an alias
 *       to std::function. */

// -- STYLE GUIDE: _fwd.hpp pattern odds/ends: Macros, constexpr globals --

/* Where does a (functional) macro go?  _fwd.hpp or not?  If the former then which one?  For now we leave the answer
 * mostly as an exercise to the reader.  We'll only say this:
 *   - Macros (as noted elsewhere) should be avoided whenever possible (e.g., flow::log uses them, as there's ~no
 *     way to avoid it given the perf needs involved).  If you do need to export a macro, then remember *what* a macro
 *     is: The #define itself does not "do" anything: something only executes when this guy is actually invoked in
 *     user code.  The #define does not affect compile times really, nor do circular dependencies really apply to it.
 *   - One rule of thumb might be:
 *     - Macro goes in _fwd.hpp if and only if *every* symbol it needs -- directly or indirectly -- can be and is
 *       (directly or indirectly) `include`d by the same _fwd.hpp (which means each lives in this or other _fwd.hpp),
 *       enough so to compile without additional `include`s by the user.
 *     - Otherwise it goes in non-_fwd.hpp (which must also, directly or indirectly, supply all necessarily
 *       symbols by `include`ing the proper headers to avoid compile or link error). /

/* What about a constexpr global (a constant)?  For now we skip this topic.  The current suggestion is to not use
 * such constexpr globals; `static constexpr` class/struct members should be sufficient, if indeed constexpr is
 * required.  If this changes we'll develop a convention in this spot.  It might rely on the C++17 `inline constexpr`
 * syntax perhaps.  TODO: Cross that bridge when we come to it. */

// -- BEST PRACTICES --

/* The rest of the doc lists various best practices, which we've already explained are guidelines that subjectively
 * help readability, maintainability, and elegance.  To be clear:
 *   - These are no more or less mandatory than the cosmetic STYLE GUIDE rules above.  However, due to their subjective
 *     nature, their correctness is much more arguable; and (as a corollary to that) good reasons to bend or break
 *     a "best practice" tend to be much more frequent than for the cosmetic guidelines.
 *   - The cosmetic rules come close to being exhaustive, meaning you're free to do whatever you want, as long as
 *     you don't break the existing cosmetic conventions.  The BEST PRACTICES, however, are nowhere near exhaustive!
 *     Good style is something picked up over years, even decades.  Entire famous books are written about it
 *     (e.g., "Effective C++" by Meyer).  Some patterns are hard to even explain due to various details and caveats.
 *     - *Therefore*, the following isn't even a little exhaustive: more like a list of things that:
 *       - have come to mind; and
 *       - are fairly uncontroversial; and
 *       - can be described in a few paragraphs at most -- ideally only 1 -- including the [rationale] if needed; and
 *       - are sufficiently high-impact to deserve the real estate in this doc.
 *
 * Note: Add more of these, as they come up.  Anyone can add them but must pass code review, like regular code.
 * Like, one can't just decide for everyone that multiple inheritance isn't allowed -- without review.
 * Ideas that feel controversial should be kicked up to a wider team as well. */

// -- BEST PRACTICES: Comments --

/* - Explain anything *possibly* unclear to a solid-but-unfamiliar coder, via comments written in plain English. -
 *
 * If something could *possibly* be unclear to a solid coder *unfamiliar* with the code base or specific subject matter,
 * just explain it with a comment!  Use plain English, not cryptic scribblings, even if it's less pithy.
 *
 *   JUST.  EXPLAIN.  THINGS.  IN ENGLISH.  UNLESS ABSOLUTELY OBVIOUS.
 *
 * If you're unsure whether something is obvious or not, just assume not.  Worst-case, it'll be redundant.  So?
 * It's worth it.
 *
 * Whether it's laziness or the inexplicable feeling that long comments are just not "cool," most coders are very
 * lacking in this area by default.  Let's not be that way. */

/* - Doc header (comment) of *any* function *must* describe the black-box contract of the function. -
 *
 * It *must* describe fully how to *use* f() as black box, without
 * requiring one to look at its code (if that were even possible, which it often isn't).  This black-box description
 * must not include implementation details.  This is a/k/a the function's *contract*, and it is holy.
 *   - You may use the language of "pre-conditions" and "post-conditions."  It's not mandatory but can be helpful for
 *     clarity for more complex `f()`s.
 *
 * The f() comment *may* discuss implementation details in additional, clearly marked 1+ paragraphs.
 *   - Typically implementation is best-discussed inside the { body }, so putting it in doc header is fairly rare.
 *     (*Sometimes* user's "how to use" insight is increased by knowing some key aspect of implementation.)
 *
 * This rule can be regularly bent in only 1 instance: A `private` (or conceptually equivalent, like `static` free)
 * f() can be documented in a more white-box fashion when a formal black-box contract would feel silly.  Basically,
 * little obscure helpers can bend the rule within reason.  At your discretion, say something like "Helper of [...]'
 * that [...does basically so-and-so...]; see its code for further info. "  However, @param, @tparam, and @return must
 * all be formally commented even so.
 *
 * [Rationale: The benefits are:
 *   - The obvious: Reader/maintainer can quickly understand f() without looking inside f() {} (which might even be
 *     unavailable).
 *   - The subtle but important: It helps avoid spaghetti code!  Essentially, f() is an abstract component that
 *     performs a black-box task.  By forcing coder to explicitly describe it *as* a black box, it tends to discourage
 *     abstraction-breaking designs that require the caller to worry about the callee's innards.  It's
 *     a powerful anti-spaghetti technique in our long experience.] */

/* - Every class (and other compound types) *must* have a user-guide-like doc header with a black-box contract. -
 *
 * In spirit it's similar to the guideline above re. function doc headers.  In practice the class doc header requires
 * more effort however.  Rule of thumb for what to include in a class/struct/etc. doc header (in order):
 *   - The first pre-period sentence ("brief" in Doxygen parlance): Describe what an object of this type represents
 *     and any major capabilities, within reason.
 *   - The following paragraph (or the rest of the 1st one) may expand on that, featuring the most essential black-box
 *     information.
 *   - Next, explain *how to use* the class/whatever (a/k/a an object's life cycle).  Like: First, you construct it with
 *     purpose/details X.  Then, you use so-and-so methods; then after that you may use these other methods.  Destroy
 *     the object when Y.  Spend multiple paragraphs if helpful for clarity.  Pithy is good but clear is better!
 *     - All the members (including functions) must be documented with doc headers; so no need to get into that except
 *       for clarity.  Worst-case, they'll go and look at the method/whatever, especially if you mention the thing
 *       as noteworthy.
 *   - (Optional, often not needed)  Discuss implementation strategy.  This must be clearly marked, same as with f()
 *     doc headers.  However, unlike with f(), it is often *better* to discuss implementation strategy in the doc
 *     header, as there's no good other place (unlike with functions, where f(){ } comments are a fine place for it).
 *     - If the class/struct/whatever is part of the public API, place `@internal` before this discussion, so that
 *       the public API generated docs omit these blatantly white-box (internal) explanations.
 *   - A list of any `@todo`s.  Black-box/feature to-dos typically go at the end of black-box write-up;
 *     Internal/white-box to-dos at the end of the optional implementation write-up.  Each @todo is like one of these:
 *     - @todo One pre-period sentence sufficiently describing a simple to-do.
 *     - @todo One pre-colon sentence summarizing the complex to-do: Then get into as much detail as desired within
 *       reason, to make the to-do easier to "do" for the person reading it much later.  Keep going and going but
 *       absolutely no more than one paragraph.  Indentation should be as seen here.
 *     - Note: Following 1 of those 2 formats will produce a nice-looking generated TO-DO page via Doxygen.
 *     - Note: `@todo`s on functions and other members are fine too.  Class/etc. doc headers are just a natural place
 *       for the bulk of them.
 *   - Similarly a list of any `@note` and/or `@see` paragraphs.  Indent them similarly to @param.  There's no
 *     special page generated by Doxygen for these, so this is purely for readability pleasantness and could instead
 *     be directly in regular doc header text.  Whatever seems clearest is fine.
 *
 * All but the first 1-2 bullet points can often be omitted for simple compound types such as data-store `struct`s.
 *
 * [Rationale: In our experience, few coders do this by default.  Class doc headers tend to be non-existent or very
 * lacking.  Yet also in our experience these tend to be the most useful comments and hence most painful when omitted.
 * Classes are central entities, so these comments (when good) tend to help the most in explaining how stuff works
 * at a high level (a/k/a the architecture).  If I don't broadly know what the class is *for*, I will tend to write
 * ad-hoc (spaghetti) code due to lacking this conceptual understanding and instead understanding it "heuristically,"
 * by observation of how it's already used.  And if those use sites aren't well commented... then....] */

// - If something is already *clearly* expressed with code, don't add a redundant *in-line* comment saying it again. -
#if 0 // NO.  Not helpful.
if (mutex.locked()) // Mutex being locked here means we are in trouble.
{
  throw_error();
}
#endif

// -- BEST PRACTICES: Inlining --

/* - Do not use explicit or implicit inlining; let compiler/linker handle it. -
 *
 * This requires some discussion.  As you may well know, there are two ways to make a function or method inlined
 * in the generated code:
 *   - Declare the thing `inline` in X.hpp, then later in same X.hpp provide the { body }.
 *   - For class/struct methods only, place the { body } directly at the method's declaration inside the
 *     class/struct { body }.  (It's commonly used for accessors; also for example code snippets.)
 *
 * Either way, this will (in most cases, excluding recursion and such) force the compiler to inline the function.
 * The rule here is:
 *   - Simply don't do that ever.
 *   - Instead, we will use `gcc -O3` or equivalent, so that the compiler/linker will decide for us when things
 *     should be inlined.
 *
 * [Rationale: The positives are as follows.  Firstly, compilation is much faster in many situations: changing
 * { body } in X.cpp instead of in X.hpp means only X.cpp must be recompiled; otherwise potentially *everything*
 * must be recompiled.  This can speed up day-to-day work by a ton; much less waiting for compilation to complete.
 * Secondly, it's better stylistically: declarations go in headers; implementations go in .cpp; and this is always
 * consistent, except for templates where "c'est la vie."  Thirdly, it removes the onus from the developer in terms of
 * deciding what is worth inlining and what isn't; the compiler can deal with it.
 *
 * There *is* a significant negative however.  The compiler is great at auto-inlining things within a given
 * translation unit (.cpp->.o, basically).  However if X.cpp calls something inlined in Y.cpp, then X.cpp invoking
 * that function cannot actually inline; since separates compilations are separate, the compiler command to
 * compile X.cpp has no choice but to call the real, non-inlined function (Y.cpp's own invocations of that function
 * are still inlined).  Fortunately they thought of this (eventually): link-time optimization a/k/a LTO or FLTO.
 * Essentially in this mode the linker works together with the compiler and removes this negative.
 * Unfortunately the support in compilers for this is somewhat uneven.  In my (ygoldfel) experience, clang fully
 * implements this out of the box these days, and I've been using it on Mac delightfully; however in Linux I've found
 * more work -- like swapping out the linker for a fancy clang-associated one -- was necessary and, while achievable,
 * I personally never spent the required time to fully achieve it.  gcc supports the feature, but it stands to reason
 * that to turn it on successfully is a multi-day project, and the newer the gcc version the easier it will probably
 * be.
 *
 * Given the last paragraph, why are we still placing the rule here?  Answer: It's a trade-off.  The positives are
 * clear and obvious; while the negative's actual perf impact is theoretically there but in practice questionable.
 * So we've made the call to go with known positives and to deal with the negative if or when necessary in practive.
 * Since LTO exists in the compilers we use, we know the path to take when/if needed.
 *
 * Update: Now that Flow is an open-source project with a nice CI/CD pipeline, we auto-test with a number of modern
 * clang and gcc versions; all of them support LTO; and this support works just fine.  Hence the above objection
 * to the tune of "uneven" compiler support is no longer accurate; the support is quite good and even.
 *
 * To summarize, some may see this as a controversial rule.  We can iterate on this if someone feels strongly the
 * above logic does not stand up to scrutiny.  In terms of my (ygoldfel) experiences, which span now almost a decade,
 * the lack of full-on inlining has not been a practical perf issue.  It is even quite possible that the gains from
 * letting the compiler decide (`gcc -O3`) instead of the human constantly having to make the judgment call about what
 * to inline (`gcc -O2` + explicit inlining by dev) exceed the losses from LTO not being in effect yet.  Update:
 * with widespread LTO support in all modern gcc and clang compilers this complaint is mitigated/eliminated.] */

// -- BEST PRACTICES: Doxygen doc header deep-dive --

/* In "BEST PRACTICES: Comments" above we mentioned doc headers.  Let's get into the topic more.
 *
 * Every class/`struct`/union, function, macro, constant, variable, namespace,
 * file, exception, function parameter, template parameter, macro parameter, and anything else I forgot to list
 * MUST be accompanied by a "Doxygen comment," possibly shared with one or more other items (e.g., a single
 * Doxygen comment will cover the *function* itself, each of its *parameters* and *template parameters*, and its
 * *return* value).  Run Doxygen as covered in the aforementioned README; you've documented what you needed to
 * if any only if Doxygen does not complain, and the doc generation script finishes successfully (exit code 0).
 * A "Doxygen comment" is, to be clear, simply a comment with an extra one or two characters to run Doxygen's
 * attention to it: `/ * *` (sans spaces) or `///` and (usually but not always) containing Doxygen "commands"
 * starting with the `"@"` character: `"@param"` or `"@return"`.
 *
 * Do not Doxygen-document things just to say they don't exist, when that's already obvious.  Most notably, no
 * `"@return"` for `void` functions/methods and `void`-like functional macros.  There are probably others but just
 * follow the principle.
 *
 * Do not copy/paste documentation, unless some other constraint forces you to do so.  Instead, just refer
 * to the other documentation bit in all the instances except the 1 being referred to by others.
 * (This has its own maintenance problems, but we believe they are far outweighed by the
 * maintenance headache of copy/pasting explanatory sentences and paragraphs.)
 * This applies to all documentation, including inline comments inside function/method bodies.  Sadly, the
 * constraint that every "item" must have a Doxygen comment will often force you to copy/paste.  Even so,
 * do your best.  Specifically, if 4 of 5 parameters of function `f2()` use identical semantics to 4 of 7 parameters of
 * function `f1()` then -- while you ARE forced (by the "every" rule above) to document 4 of the 5 `f2()` parameters --
 * the body of the doc of each of the shared parameters can simply say "`See f1().`" instead of copy/pasting
 * its actual semantics.  This is the most prominent copy/paste situation, but the same principles can be applied
 * to others that may pop up.
 *
 * Do not use `"@brief"` explicily; use the first-sentence rule instead.  It's already clear by example
 * that we do not use `"@brief"`, but the rule that IS used is non-obvious from just looking at the code: Namely:
 * The first *sentence* of the Doxygen comment of the function/method/macro or
 * class/`struct`/union is taken as the brief description of the item.  A "sentence" is defined as everything up
 * to and including the first period (.).  (Watch out for "a.k.a." and such: use "a/k/a" instead.)
 * Everything after that first sentence is the detailed description and is separated from the brief one on the
 * page (and, in many places, the brief description is present along with item name, while detailed one is
 * not there at all; for example the Class List page).  (After the description are other sub-items such as
 * `"@param"` or `"@see"` or `"@todo"`.)
 *
 * ### Doxygen markup within a Doxygen command ###
 * This section originally existed in the class net_flow::Node doc header, and I have left it there, because that is
 * a large class which generates a Doxygen doc page (unlike the present comment) with many examples available for
 * clarity.  Please see that doc header's identically titled subsection ("Doxygen markup," etc.).
 *
 * @warning Please read it!  Also, if you intend to add to/otherwise change Doxygen-use conventions in the Flow project,
 *          please make sure you document it in that section as opposed to simply doing it and creating undocumented
 *          inconsistency (especially over time). */

// -- BEST PRACTICES: Namespaces, libraries --

// - Do: Use `namespace` to wrap *every* symbol in a namespace, whether public or internal. -

/* - Corollary: Since #define macros cannot belong to any namespace, "simulate" it with the `NAME_SPACE_` convention. -
 *
 * namespace outer { namespace inner { [...stuff...] } }
 * #define OUTER_INNER_[...stuff...]
 *
 * Example: A class about logging in Flow: `namespace flow { namespace log { class Logger; } }
 *          A macro about logging in Flow: #define FLOW_LOG_WARNING([...]) [...] */

// - Do: Liberally use `using a::b::thing;` inside function { bodies }. -
// - Do: Use `namespace convenient_alias = a::b;` inside function { bodies }. -
// - Corollary: Avoid: heavy explicit namespace-qualifying in function { bodies }; prefer `using` 90%+ of the time -

// - Do not: Use file-level `using` or `using namespace`; especially never ever in headers! -

/* - Do: Use `boost::` (Boost) or `std::` (STL) over other libraries or self-written facilities when possible -
 *
 * STL facilities by definition have a high expectation of reliability and performance, though the latter should be
 * verified for each feature when performance is paramount.  Such code is also much more portable and maintainable
 * than many of the alternatives.  Also, cppreference.com documentation is *fantastic*.
 *
 * Most STL facilities originate in Boost and then gain entry to the C++ standard and actual STL.
 * Hence Boost is considered to have equivalently high expectations of reliability, perf, *and* documentation.
 * Moreover, Boost has tons of features (in particular entire modules) not (yet) in STL. */

/* - Corollary: Slight preference to boost:: over std:: when a given feature is equivalently in both.
 *
 * It's very common that std::...::X has boost::...::X counterpart.  In this case they're either exactly equal, or
 * the one in boost:: is a ~100% compatible super-set (equal, *plus* certain added APIs).  Either facility can be used,
 * unless you need one of the added features (e.g., boost::chrono:: equals std::chrono:: but has a thread-time clock
 * class added, among others).
 *
 * When you can use either facility, Flow leans to Boost.  A vague exception are C++03 interfaces
 * like std::string, std::vector, std::map.
 *
 * [Rationale: Honestly the latter "lean" is mostly a historical thing: In 2012 we were limited to C++03, so STL
 * was *much* more limited.  Since then there has been no reason to switch to std::, and several instances where
 * Boost-only features proved useful in their availability, we've conservatively stayed with this principle.
 * Possibly if Flow were developed in 2019, we might have started off differently and would lean to std::.
 * It wasn't, so for now it is prudent to stay the course (don't fix it if it ain't broken).] */

/* - Corollary: Use, e.g., `using Thread = <std or boost>::thread` for commonly used things from STL/Boost features. -
 *
 * [Rationale: 2 benefits.  One, if it's commonly used then this increases pithiness and cosmetic consistency.
 * Two, particularly for items in both STL and Boost, it makes it easy to switch the underlying implementation
 * easily.] */

/* - Do: In particular, familiarize self with these boost. modules: -
 *
 *   Basic tools (should be familiar at minimum):
 *     any, array, chrono^, core, dynamic_bitset, lexical_cast, random*, unordered#,
 *     smart_ptr (+movelib::unique_ptr), system^, thread^&, tuple, unordered, utility
 *   More advanced but similarly uncontroversial:
 *     atomic$, date_time, filesystem, heap, ratio, stacktrace, timer, tribool
 *   Advanced and should be decided on with care; but institutionally known to be solid:
 *     asio%, format, interprocess, intrusive, io, iostreams, program_options, regex, algorithms, string_algo@
 *   No personal or institutional experience; advanced; should be looked into opportunistically:
 *     context, couroutine2, fiber - Relates to "micro-thread" concept.
 *     exception - Also see guidelines re. exceptions elsewhere in this doc.
 *     beast - Write HTTP-WebSocket-... servers, on top of boost.asio.
 *     pool
 *     log - Flow has its own flow::log framework.  A top-level doc header discusses why using that and not boost.log.
 *   Do not use:
 *     function!
 *
 * -%- First see flow::async::..., which provides the most convenient way to work in the boost.asio style.
 * -*- See also Flow's random.hpp.
 * -#- See also flow::util::Linked_hash_{map|set}.
 * -^- Check out the flow:: public `using` aliases for stuff inside these modules.
 * -&- Also: There's a long-standing ticket about the slowness of boost::shared_mutex (sans contention) even as of
 *     Boost-1.80.  It might be better to use std::shared_mutex.  However consider that experts say a regular
 *     mutex is blindingly fast to lock/unlock, and thus any `shared_mutex` is only "worth it" with long read
 *     critical sections and their very frequent invocation; this is apparently quite rare.
 * -@- Be very careful about performance.  Lib is highly generic and might be too slow in some contexts (<-experience).
 * -$- Performance-sensitive; perf bake-off vs. std:: might be relevant in some cases.
 *     Update: Current (8/2023) wisdom appears to be to possibly prefer std::atomic to boost::atomic; suspecting
 *     better or at least not-worse perf due to native nature of it.  It can go either way; but current preference
 *     in Flow has switched to std::atomic.  (A perf bake-off might be nice, but std::atomic really should be safe
 *     and full-featured.)
 * -!- Boost.function is, it turns out, horrendously slow (too much capture copying).  Plus it has a 10-arg limit.
 *     std::function in gcc looks great in both regards though lacking some very minor APIs from Boost.function.
 *     Just use flow::Function: it's the best of both worlds.  Definitely do not use Boost.function! */

// -- BEST PRACTICES: Error handling --

// - Do: Use assert() for bad arg values as much as possible *unless* returning an error is worth it to user. -
// - Do: Use static_assert() for compile-time checks. -

/* - Do: Use Flow's standard error reporting convention/boiler-plate when returning errors from a public lib API. -
 *
 * This allows for returning both error codes (boost.system ones) for high perf *and* optionally wrapping that in
 * an exception if so desired.  Read more starting in flow::Error_error code doc header.  See also some notes on error
 * reporting in common.hpp. */

/* - Do: ...follow the following thoughts when considering using exceptions or not. -
 *
 * Question 1 is whether to optionally throw an exception on error at the *top* level of a given *public* *libary* API.
 * That's covered in the previous guideline.  Here we talk about whether to throw exceptions *throughout* the
 * implementation, which is a different question.  So, suppose an error has been detected.  Then:
 *
 * Basic fact: Fastest is to assert().  Sometimes that's not sufficient for obvious reasons.  If so:
 * Basic fact: Next fastest is, on error, to return an Error_code (or just a `bool` or similar, if truly sufficient).
 *   This is very fast; it's just like returning an `int` (it's a couple of `int`s really internally but close enough).
 * Basic fact: Finally, you can throw an exception.  This should be considered slow, and I leave it at that for
 *   simplicity.
 * HUGELY KEY FACT: *Not* throwing an exception (meaning no error was detected) is *just* as fast as returning
 *   a success code / false / zero / etc.  A tiny-tiny bit faster even.  A *potential* exception -- or 500 potential
 *   exceptions in a given call tree -- has zero perf cost, period.
 *
 * Therefore this somewhat vague guideline: Avoid exceptions in internal implementations, as then there's no perf
 * question.  However, it may be very convenient to throw an exception sometimes; the stack unwinds and can make
 * some code so much simpler.  Then, consider using an exception... and do so if you can very confidently state this
 * will *not* cause a high rate of exceptions thrown in reality.  If there's any question about that, lean to avoiding
 * exception code paths after all.  If there isn't, feel free.  For example, as of this writing there are only a
 * small handful of such places in Flow, but they do exist.
 *
 * See also some notes on internally thrown exceptions in common.hpp. */

// -- BEST PRACTICES: Logging --

/* - Log as much as possible and practical.  If verbosity+perf is a concern, just use the appropriate log::Sev. -
 *
 * Do have some concern for the visual experience of reading the logs (shouldn't be TOO redundant and wordy).
 * [Rationale: Debuggers are great... but not always available when you need them.  Logging is superior to
 * relying on debuggers in almost every way; in many cases it even avoids the need to reproduce problem/bug, as it
 * can often be solved by looking at the logs.  Even if not, it's much easier to turn up verbosity and reproduce than
 * to attach a debugger or examine a core file.  The famous K&R C book makes this wonderful, correct point.] */

// -- BEST PRACTICES: Types and type safety --

/* - Do: Always ensure total const-correctness. -
 *
 * [Rationale: Probably most/all coders are familiar with this and likely agree.  But this is a const-correct shop,
 * no exceptions, outside of working with ill-behaved outside libraries which may not be const-correct.] */

/* - Corollary: Do not: use C-style casts; const|reinterpret_cast<> are to be rare and always justified w/ comment. -
 *
 * [Rationale: C-style cast, like `(T)x`, is always bad, as it allows type-unsafety and const-discarding without
 * even clearly doing so.  Instead, use `static|dynamic|reinterpret|const_cast`, with the latter 2 used rarely
 * when it is required to be type-unsafe and const-incorrect respectively.  Hence explain why you're doing it in those
 * cases.] */

/* - Do: Learn lambdas ASAP.  Use them extensively.  There should be NO reason to use bind() or a functor. -
 *
 * [Rationale: Can wax poetic here, but it's best understood by experience.  One sees the advantages quickly.
 * Basically, lambdas are pithy, fast, and flexible ways to create function objects which is a constant need in
 * modern C++.  bind() was SPECTACULAR in C++03, but with C++11 lambdas its use cases are mainly subsumed, adding
 * not only clarity but performance (compiler is much more likely to inline/optimize cleverly, as it's a built-in
 * language feature and not a crazy feature built in the library via C++ generic trickery).] */
// - Corollary: Async code flows can be expressed readably, helping resolve the biggest prob with async code flows. -

// - Do not: Use #define macros for constants.  Use typed `const`s. - [Rationale: Better for debugging, type safety.]
// - Corollary: Non-functional macros (#define without `()`) must be very rare, such as for an efficient logging API. -

/* - Avoid: functional macros, period. -
 *
 * See if it can be done with a function with minimal loss of syntactic sugar.  [Rationale: Debugging, type safety.]
 * If there is syntactic sugar lost that is absolutely essential, write as much as possible in a function; then wrap
 * that in a functional #define to add the necessary syntactic sugar.  (Example: __FILE__, __LINE__, __FUNCTION__
 * are often auto-logged in logging APIs; a macro is needed to add these automatically at log call sites without
 * coder having to do so every time.) */

// - Corollary: Functional macros used in a local chunk of one file should be #undef`ed. -

// - Do: Use `const T&` for performance (instead of `T`) when relevant. -

/* - Avoid: Using non-const `T&` function args (use T* instead). -
 *
 * Hence if you have an out-arg of type T, take a pointer to T, not a reference to T.  (You can easily alias it to
 * a reference T& inside the function, if you like it for syntactic sugar.)  If it can be null, you can use that as
 * a semantic tidbit meaning "turn off the feature to do with this arg"; if there's no need then just assert() it's not
 * null and move on.
 *
 * Hence, when calling a function and passing an out-arg, it'll commonly look like: cool_func(&modified_obj);
 * That's instead of: cool_func(modified_obj);
 *
 * [Rationale: This might be surprising, as vast majority of coders (including STL, Boost) use non-const ref out-args.
 * This guideline may seem controversial (and it is, after all, optional though strongly encouraged) but consider this:
 *   - Using a pointer is no slower.  assert() (if applicable) costs little to nothing.
 *   - Using a pointer makes the call site *much* more expressive: You can see a thing may be modified.
 *     Makes algorithm easier to understand at a glance.
 *   - Using a pointer provides an easy way to "turn off" the arg which is commonly needed though of course not
 *     universal.  When so used, the API is usually pithier than the alternative (requiring another arg, or a special
 *     value, or something).  For example, see how Flow's lib-API-error-returning convention works:
 *     if you pass in `Error_code* ec = null`, it'll throw exception on error; else it'll return normally and set
 *     *ec accordingly (including setting it no-error on success).
 * Try it.  You might like it.]
 *
 * Not a hard rule.  In particular, `ostream& os` is a common arg form by convention from STL and Boost.  Use your
 * judgment. */

// -- BEST PRACTICES: General design do's and don'ts --

/* - Do: Use inheritance for "is-a" interfaces, as in Java interfaces (no, or very little, non-pure-virtual content). -
 *
 * Class A = interface; describes how to use it as *only* 1+ pure virtual methods.
 * Classes B, C = concrete types: Each B or C "is-an" A, so they use public inheritance B->A<-C. */
/* - Corollary: With wide-spread use of such interfaces, multiple inheritance becomes wide-spread... yet simple, as
 *   one inherits only pure virtual method signatures and nothing else. - */

/* - Do: Use inheritance when adding capabilities to an existing concrete class, a/k/a "is-a" inheritance. -
 *
 * Class A = concrete class with capabilies X.
 * Class B `public`ly inherits B->A and adds capabilities Y.  Therefore, B "is-an" A, plus it adds more capabilities
 * on top.  (This can of course continue indefinitely.  C can subclass B and add more stuff; etc. */

/* - Avoid: Using inheritance to provide common capabilities to 2+ subclasses. -
 * - Instead: Use composition. -
 *
 * Composition = put common capabilities into a separate class; then instantiate data member(s) and/or local
 * variable(s) of that type; no need to derive from that type usually.

 * Two techniques are pretty common but should be avoided.
 *   - Classes B and C need common, stateful capabilities in class A.  Therefore, use `private` inheritance B->A<-C.
 *     Instead: Instantiate A in B and C, as needed (composition).
 *   - Classes B and C implement interface A.  Therefore, use `public` B->A<-C... *but also* B and C need
 *     common, stateful capabilities.  Therefore, add those capabilities into A as well making it not a clean interface.
 *     Instead: Keep the interface class A clean; create separate class A' with the common, stateful capabilities;
 *     then instantiate A' in B and C, as needed (composition).
 *
 * *Not* a hard rule, but considering it before going ahead with non-interface-y inheritance = good design instinct.
 *
 * [Rationale: For me (ygoldfel), this lesson took 10+ years to learn.  It is much more pleasant and flexible.
 * Using inheritance for this tends to be the result of not cleanly separating the notion of "common interface" (primary
 * purpose of inheritance) from that of "common concrete code" (typically easily done with composition).
 * There's also a certain beauty to a B->A<-C relationship that tightly helps B and C reuse complex code stored in A,
 * while B and C don't care about each other.  What I've learned over a long time is that this satisfaction becomes
 * fleeting, when one has to tweak something about B->A or C->A, and suddenly one has to rejigger the whole setup.
 * Composition (where A is instantiated by each of B and C) helps make that process much simpler.
 * Among other things, A can be constructed/destroyed anytime; instantiated 2+ times simultaneously; etc.] */

/* - Corollary: `protected` non-pure non-`static` methods and `const` data members are OK, but consider composition. -

/* - Do not: use `protected` non-`const` data members (member of A, with inheritance B->A)! -
 *
 * This breaks encapsulation; A is best designed to be self-contained; giving direct mutable access to B allows it to
 * break invariants of A; so now basically A and B become inter-related and must be maintained together, tightly
 * coupled.
 *
 * Can't you just "cheat" by wrapping the access into a non-`const`-reference-returning `protected` accessor or
 * something?  Well, yes, but then you're still breaking the spirit of the guideline.  The spirit is the point.
 * Still, a protected method or accessor (if you must) is still better. */


/* - Do: Use the `_base` inheritance pattern when a T-parameterized class template C contains non-T-parameterized
 *   aliases/constants/etc. that must be usable without actually instantiating the template C<T> for some concrete
 *   `T`. -
 *
 * For example, in STL, `std::ios_base::app` ("append" file mode constant) is used without having to mention a full-on
 * `basic_ostream<char, [...]>` (a/k/a `ostream`) nor any other such basic_ostream<> type.  Note
 * basic_ostream<[...]> inherits from `ios_base`.
 *
 * Composition can also be often used.  However, think of it like this: "Really I want to put the stuff right into
 * C<T>, where it belongs, but then people can't use it easily without instantiating the template, so I'll just split
 * it off into a _base class.  I could just put the stuff entirely outside C<T>, but _base is a nicer pattern." */

/* - Avoid: Adding public `static` methods into a class just because "everything should be in a class." -
 * - Instead: Is it really firmly linked conceptually to class C?  No?  Make it a free function then. -
 *
 * This isn't a hard rule at all.  For example, factory `static` methods are very common, and that's fine.
 * But that's different, because in that case it's essentially a constructor and *is* firmly linked conceptually to
 * class C.  Plus, then this method can access C's insides (data members even), and that can be very useful.
 *
 * [Rationale: This isn't a hard rule at all.  Public `static` methods are common.  However, people tend to use classes
 * as, basically, namespaces that organize groups of public `static` methods.  This seems to be a result of the
 * pre-`namespace` era.  Now, one can just make an actual `namespace` if such grouping is desired.  Use judgment.] */

/* - Avoid: Using `friend` to "cheat" an otherwise intentional encapsulation design. -
 * - Instead: Reconsider your design. -
 *
 * This is NOT saying `friend`s shouldn't be used just because.  There are a few perfectly reasonable `friend` uses;
 * a common one is for binary operators written as free functions (`string operator+(string, string)` and such).
 * I won't try to enumerate them here.  Just saying: If it feels like you're using `friend` as an "exception" to an
 * otherwise solid relationship between 2 encapsulating entities, then you might be abusing the existing design which
 * can easily lead to more abuse and a mess over time.  Then, ask yourself if maybe that design can be amended. */

#  endif
#endif // !defined(FLOW_DOXYGEN_ONLY)
