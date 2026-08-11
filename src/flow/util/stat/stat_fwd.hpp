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

#include <boost/type_traits/type_identity.hpp>
#include <atomic>
#include <iosfwd>
#include <string>
#include <vector>

/**
 * Sub-module of flow::util providing lightweight facilities for working with simple `struct`s of
 * stat counters/gauges/etc.  Histogram_counter is available as a stand-alone utility as well.
 * That aside, the rest of this doc header discusses the aforementioned simple-`struct`s-of-stats facilities:
 * a/k/a `Stat_set` wrangling.
 *
 * ### Premise of `Stat_set` wrangling ###
 * A `Stat_set` stands for a `struct` not actually named that, used by you to track stats for some unified
 * purpose.  An application would have potentially many different `Stat_set`s.
 *
 * At a core level, working with such `struct`s requires little to no
 * scaffolding: define a `struct` with some per-stat members, initialized right there inline; then just
 * `++` an individual counter member to increment it (example); read it when interested; that's about it.
 *
 * If (and this is certainly not always the case) multiple threads might make updates to one such `struct`, it
 * is common to declare stat data members as `std::atomic<T_numeric> m_...;`.  Then updating and reading these
 * is reasonably straightforward (though often over-assumed to be completely straightforward).
 *
 * However on-demand operations covering every member -- pretty-printing
 * the `struct`'s contents being a simple example -- become tedious to write and error-prone to maintain.
 * A more complex example use-case is aggregating the stats from N threads' thread-local `struct`s of the same type:
 * listing each member is tedious enough; and then there's the fact that adding N accumulators is one thing
 * (just a sum), while adding N gauges is quite different (perhaps a mean).  Then there are `Histogram_counter`s
 * which in many ways are equivalent to arrays of `atomic` counters, but there's also bucket-structure metadata
 * contained therein, and resetting the counters is syntactically different from assigning zero; so there's more
 * potential tedium there.
 *
 * This sub-module, without changing the typical stat-holding `struct` at all, makes that stuff simple(r).
 *
 * The key feature is the ability to write, once, a *declare-stats function* for
 * a given stats `struct` (referred to as `Stat_set` by convention in docs and such) -- listing its members
 * via the FLOW_UTIL_STAT_DECLARE() macro -- and then pass that `struct` to various utility functions (such as
 * stats_to_ostream<Stat_set>() which provides output of a `Stat_set` to an `ostream`; better yet use
 * `<< print(stats)` for syntactic sugar) that iterate the members generically.
 *
 * This eliminates the boiler-plate of manually maintaining `operator<<`, aggregation, resetting, ..., for stats
 * structures.  Only one bit of duplication is required: Declare the option in C++ (as normal); *and* add a line
 * `FLOW_UTIL_STAT_DECLARE*()`ing it as well.  Failure to do the latter will merely mean that data member
 * will be ignored when one calls the ops supported such as stats_to_ostream() or stats_reset(); other than
 * that it's still perfectly usable.
 *
 * Even ignoring the preceding 2 paragraphs, some small utilities are available to simplify per-stat update
 * operations themselves:
 *   - For `atomic<T_numeric>` stats:
 *     load(), store(), fetch_add(), fetch_sub(), exchange() are, essentially, respectively:
 *     read value, assign value, `+=` (and `++`), `-=` (and `--`), `atomic::exchange()`.  load() returns the
 *     value; the mutators except store() return the *pre-change* value (matching
 *     `atomic::fetch_add()`/`fetch_sub()`/`exchange()` semantics; note the non-`atomic` overloads of
 *     fetch_add()/fetch_sub() return `void` instead -- see their doc headers).
 *     - Note: Often simply reading, assigning, and incrementing `atomic`s can be done the usual-looking way
 *       (e.g., `atomic<int>` can be `++`ed).  However this uses strict memory-ordering and may be slower than
 *       one prefers.  The supplied element-level primitives (`load()`, `store()`, `fetch_add()`, et al.)
 *       are, from your PoV, syntactic sugar to standardize
 *       making updates and loads of `atomic` stat members consistently with best practices in the context of
 *       individual stats.
 *   - For high-water-mark gauges (`atomic<T_numeric>` or otherwise): update_hi_wmark() updates a target
 *     stat with a new value, if and only if the latter exceeds the former.
 *
 * ### Design ###
 * This is a vastly simplified analog of flow::cfg::Option_set which uses a similar macro-based
 * declaration pattern (FLOW_CFG_OPTION_SET_DECLARE_OPTION()) to do many things with config `struct`s.
 * As in that case, there is a number of fairly diverse operations, all of which require iterating members
 * of a user-supplied `Stat_set` (not natively possible due to lack of reflection in C++).
 * In our case the mechanism is *far* simpler, however:
 *   - You make `Stat_set` updates and reads without our help (modulo `atomic` and high-water-mark
 *     helpers (`load()`, `store()`, `fetch_add()`, et al.) which are *highly* recommended but basically
 *     straightforward syntactic sugar).
 *   - All the fanciness occurs strictly on a transactional basis, when and only when you need to do something
 *     fancy.  A `stats_*()` function is called, and it'll do what it must inside.  You're still only dealing
 *     with your simple `Stat_set`.
 *     - Want to print your `Stat_set`?  Use `os << stat::print(stats)` (this will invoke stats_to_ostream()).
 *     - Want to reset your `Stat_set`, without messing up your gauges (which should *not* be reset to 0/etc.)
 *       and `Histogram_counter`s (which should be `.clear()`ed, or equivalent, while keeping bucket structure)?
 *       Use `stats_reset(&stats, ...)`.
 *     - Want to aggregate N `Stat_set`s into a result-`Stat_set`, which might have counters (including
 *       bunches-of-counters such as `Histogram_counter` or a custom type of your own) and gauges?
 *       Use `stats_aggregate(&target_stats, input_stats_vec.begin(), input_stats_vec.end())`.
 *     - Want to aggregate N *sharded* (perhaps thread-locally-sharded) `Stat_set`s into a result-`Stat_set`?
 *       Use `stats_aggregate_shards(&target_stats, input_stats_vec.begin(), input_stats_vec.end(), ...)`
 *     - ....
 *
 * @note For those familiar with flow::cfg: In that context there is complex state that persists beyond any
 *       particular op (like "output the values").  So there is `Option_set<Value_set>` each of which manages
 *       a `Value_set` (the latter being the managed `struct` like our `Stat_set`), with full-on aux state such
 *       as hash table(s) representing current config setting values et al.  In *our* context, there is no such
 *       persistent data structure.  Any op beyond just updating stats and reading them (<-- directly via
 *       `Stat_set` itself, lightning-quick) shall mount any required state on-demand, likely on the stack, and let it
 *       disappear once done with the op.  We do not care (within reason) about the perf cost of each such op,
 *       as the assumption is that it is rarely invoked.  Hence you will need to avoid invoking these ops willy-nilly
 *       many times per second/etc.
 *
 * ### Pattern (ignoring `atomic`s) ###
 * Define a `struct` or equivalent.  In it declare (in addition to whatever else you might keep there that's not tracked
 * by util::stat, though informally we recommend to keep it spare of such for simplicity) each public stat member.
 * By convention we recommend each member that can be default-initialized to be default-initialized (and then
 * avoid initializing them in any ctor).  (These recommendations are for readability/consistency.)
 * Those that aren't default-initialized must be initialized (via ctor), meaning we formally do not support
 * handling `Stat_set` members with garbage values.
 *
 * @note Tip: We recommend segregating stat-related items -- most prominently/at a minimum `Stat_set`s and their
 *       `declare_stats()` free functions -- into sub-namespaces named `stat`.  We won't mention this again, and it
 *       is not mandatory; but it is good organizationally and may over time help avoid ADL name-collision
 *       annoyances.
 *
 *   ~~~
 *   namespace my::cool::module::stat
 *   {
 *
 *   struct My_stats
 *   {
 *     uint64_t m_msg_count = 0;
 *     uint64_t m_byte_count = 0;
 *     size_t m_cur_q_size = 0;
 *     Histogram_counter m_histo_pkt_sizes{...};
 *     float m_weird_weight_lb;
 *     float m_weird_weight_lb_hi_wmark; // Highest m_weird_weight_lb observed so far.  Recommended naming conv shown.
 *
 *     // If unavoidable, can do this too.  Tip: If you can design a no-args ctor, your life *will* be easier.
 *     My_stats(float weird_weight_lb);
 *   };
 *   My_stats::My_stats(float weird_weight_lb): m_weird_weight_lb(weird_weight_lb) {}
 *
 *   void declare_stats(...) { ... } // See below.
 *
 *   } // namespace my::cool::module::stat
 *   ~~~
 *
 * Then write a declare-stats function using FLOW_UTIL_STAT_DECLARE() (ADL-visible; same namespace as the `struct`).
 * It must have this exact signature including all the names.
 *
 *   ~~~
 *   template<typename Visitor>
 *   void declare_stats(std::string name_prefix, const My_stats* src_stats, My_stats* target_stats,
 *                      Visitor&& visitor)
 *   {
 *     FLOW_UTIL_STAT_DECLARE(m_msg_count, ACCUMULATOR);
 *     FLOW_UTIL_STAT_DECLARE(m_byte_count, ACCUMULATOR);
 *     FLOW_UTIL_STAT_DECLARE(m_cur_q_size, GAUGE);
 *     // Example of a non-numeric type; acts as ACCUMULATOR: it has += which does the right thing internally.
 *     FLOW_UTIL_STAT_DECLARE(m_histo_pkt_sizes, ACCUMULATOR);
 *     FLOW_UTIL_STAT_DECLARE(m_weird_weight_lb, GAUGE);
 *     // Declares a special stat of type Stat_type::S_HI_WMARK that gauges preceding S_GAUGE m_weird_weight_lb.
 *     FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_weird_weight_lb_hi_wmark, m_weird_weight_lb);
 *   }
 *   ~~~
 *
 * Now stats_to_ostream() (more conveniently via `os << print()`), stats_reset(), stats_assign(),
 * stats_aggregate(), stats_aggregate_one(), stats_aggregate_shards() (and other utilities that might exist)
 * will find the above via ADL on `My_stats` and iterate the declared members as needed.
 *
 * @note How does it work internally?  For context/curiosity: It is done by calling ADL-located
 *       `declare_stats()`, passing it an appropriate `visitor()` that will -- in the stats_to_ostream()
 *       case -- print the given option.  The last piece of the puzzle is that the FLOW_UTIL_STAT_DECLARE()
 *       macro cooperates by calling `visitor()`, passing it relevant things about the individual stat member
 *       including its macro-stringified name (`#m_byte_count` => `"byte_count"`).
 *
 * ### Types (including `atomic`s) ###
 * The above informally (without specifying requirements for each managed-stat-member type) shows how to do it.
 * Plus it does not discuss `atomic<>` such members, when concurrent stat-updates (and, rarely -- it is assumed --
 * reads) may occur.  Let's get into that.  Take a given `T m_v` in `struct Stat_set`.
 *
 * `T = atomic<T_numeric>` is supported.  (As of this writing `T_numeric = float` and other floating-point types
 * are, due to a C++17 technicality -- the lack of `atomic<...>::fetch_add()` -- not supported.  If there is demand
 * for this, it would be easy to add it.  In C++20, when/if we move to that, it'll just work as-is.)
 * However, in that case, when making updates and reads:
 *   - It is *recommended* that reads, stores, and additions/subtractions/increments/decrements are done via
 *     load(), store(), fetch_add(), fetch_sub() respectively.
 *   - It is *required* (formally against the risk of undefined behavior, we do declare!) to make high-water-mark
 *     updates via update_hi_wmark().  (Anyway why not?  Do you really want to write that atomic-logic yourself
 *     every time/your own utility?  Use ours.)
 *
 * `atomic<T>`, where `T` is not numeric, is formally unsupported.  We haven't defined "numeric" but it includes
 * the native numeric types; and also anything whose representation is identical to that of a native numeric type,
 * including `chrono::duration` (with its `"::rep"` being a native numeric type) and specifically #Fine_duration
 * (alias for, probably, `chrono::duration<int64_t, nano>`).  `atomic<Fine_duration>` accumulators are cool
 * and superior to storing `.count()`s and doing unit-conversion manually!
 *
 * Now let's assume `T` is not `atomic<>`.  In that case:
 *   - Firstly, any `T_numeric` described above (plus floating-point ones) is supported; period.
 *     So to repeat, that's: native numeric types and types whose representation is identical to that of a native
 *     numeric type (including `chrono::duration` with `"::rep = T_numeric"`; and especially `Fine_duration`).
 *   - Other `T`s can also work.  Whether that is so is determined by which `stats_*<Stat_set>()` functions are
 *     instantiated in your application.  If none then there are no further requirements; store whatever you want.
 *     Else:
 *     - `T` must be copyable.
 *     - For stats_to_ostream<Stat_set>() (or via `os << print<Stat_set>(...)`): `T` must be `ostream<<`able.
 *     - For stats_aggregate_one<Stat_set>():
 *       `ACCUMULATOR`- or `GAUGE`-typed `T` must have `+=`.
 *       `HI_WMARK`-typed `T` must have `<`.
 *     - For stats_aggregate_shards<Stat_set>(): Same.
 *     - For stats_aggregate<Stat_set>(): Same requirements as `stats_aggregate_one()`; plus:
 *       `GAUGE`-typed `T` must have `/=`.
 *     - For stats_since_reset_state<Stat_set>():
 *       `ACCUMULATOR`-typed `T` must have `-=`.
 *       `HI_WMARK`-typed `T` must have `<`.
 *     - For stats_reset<Stat_set>():
 *       `ACCUMULATOR`-typed `T`: No additional requirements.  However, you may optionally define the
 *       following, in the same namespace as `Stat_set`, *if* you prefer/require that a reset of an individual
 *       `T* target_val`  does not merely perform `store(target_val, load(fresh_val))`, where `fresh_val` is
 *        supplied within the `Stat_set fresh_stats` to stats_reset().  The optional ADL-findable function is:
 *       - `void reset(T* target_val, const T& fresh_val)`.  It may ignore `fresh_val` or use it.
 *
 * @note `T` = Histogram_counter satisfies all of the above.  Therefore it can be used with any op mentioned.
 *       You may use it as a model, as source code is available nearby.
 *       In particular, there is a `stat::reset(Histogram_counter* target_val, const Histogram& fresh_val)` which
 *       performs `target_val->clear()` instead of the aforementioned store-load from `fresh_val`.  Thus, it is
 *       not necessary to supply a `fresh_val` with the exact correct bucket structure.
 *
 * ### Assigning ###
 * Naturally, `Stat_set a = b` (and similar copying) can be useful.  It is often auto-generated, or can be by using
 * `= default` (on copy ctor + assignment of your `Stat_set`).  However, if you have at least 1 `atomic<>`, then
 * this cannot be done without providing some kind of `Copyable_atomic<>` adapter and using that for such members.
 *
 * We recommend using stats_assign() instead.  Your `Stat_set` no longer need be copyable by default.  Even if
 * you lack `atomic<>`s doing so may prevent some headaches later.
 *
 * ### Resetting (vanilla) ###
 * This is another common operation.  Three things to note on this.
 *   - It is important to have a crisp conceptual understanding of what *reset* means in this context (stat-keeping).
 *     Stat-keeping here means to record information about what has happened over a particular period of time in the
 *     past.  Without resetting, that period of time begins when `Stat_set` is initialized (usually: simply
 *     constructed).  A reset means: *starting a new measurement period*.
 *     - Most stats are `ACCUMULATOR`s.  A reset means, usually, assignment of such a stat to zero (or zeroes,
 *       for conceptual compounds-of-counters including Histogram_counter).
 *     - For `GAUGE`s and derived `HI_WMARK`s that is not the case.  E.g., if you're measuring current temperature,
 *       then starting a new measurement period does not change the current temperature... that only happens if the
 *       weather changes.
 *   - You may use stats_reset() to reset a `Stat_set`.  Assuming you've classified your individual stats
 *     properly (via `FLOW_UTIL_STAT_DECLARE*()`), it will simply work.
 *   - You may be tempted to not use stats_reset() and instead do something like: `stats = {}` or
 *     `stats = Stat_set{some, ctor, args}` -- depending on what it takes to originally init your `Stat_set`.
 *     We recommend against this:
 *     - This will only work for `ACCUMULATOR`s.  It is easy to forget that `GAUGE`s et al look quite similar
 *       inside the `struct {}` body and not handle them specially, thus you've got a bug.  (Etc.)
 *     - It may be perfectly accurate now (e.g., you have *have* `ACCUMULATOR`s anyway), but the moment that changes,
 *       you have to change-over -- and it is very easy to forget at that point.  A maintainer might just not even
 *       know about these subtleties.
 *
 * That said, consult stats_reset() doc header for further details about how to use it.
 *
 * ### Composition (`struct Stat_set` contains `struct Set2`) ###
 * It is pretty common to, firstly, group items within a `Stat_set` into nested `struct`s, if only just for the
 * sake of organization.  For example:
 *
 *   ~~~
 *   struct Stat_set
 *   {
 *     struct Rcv
 *     {
 *       size_t m_n_bytes_transmitted; // Bytes received.
 *       unsigned int m_would_blocks; // # of times EAGAIN encountered.
 *     } m_rcv;
 *     struct Snd
 *     {
 *       size_t m_n_bytes_transmitted; // Bytes sent.
 *       size_t m_buf_sz; // Send buffer adaptive size (gauge).
 *     } m_snd;
 *   };
 *   ~~~
 *
 * Here the situation is not-different from just a basic un-nested `Stat_set`.  If that's all that's happening
 * simply declare each stat like any other:
 *
 *   ~~~
 *   template<typename Visitor>
 *   void declare_stats(std::string name_prefix, const Stat_set* src_stats, Stat_set* target_stats,
 *                      Visitor&& visitor)
 *   {
 *     // `os << print(stats);` would output "rcv.n_bytes_transmitted=..., ..., snd.buf_sz=..." -- note the prefixes.
 *     FLOW_UTIL_STAT_DECLARE(m_rcv.m_n_bytes_transmitted, ACCUMULATOR);
 *     FLOW_UTIL_STAT_DECLARE(m_rcv.m_would_blocks, ACCUMULATOR);
 *     FLOW_UTIL_STAT_DECLARE(m_snd.m_n_bytes_transmitted, ACCUMULATOR);
 *     FLOW_UTIL_STAT_DECLARE(m_snd.m_buf_sz, GAUGE);
 *   }
 *   ~~~
 *
 * However, it is also possible to use *composition*, wherein perhaps a `struct Set2` is
 *   - sometimes used by itself (maybe some object A maintains a `Set2` that it updates and at times `stats_reset()`s
 *     if so instructed); but also
 *   - sometimes used as part of a containing `struct Stat_set` (maybe some object B maintains an object A, plus
 *     its own object-B-only stats that it updates, and when asked for the full object-B stats it creates
 *     a `Stat_set`, copying its own B-items + A's `Set2` into that `Stat_set` and returns the latter by value).
 *     - The full `Stat_set` might then be printed via `os << print(...Stat_set...)` via
 *       stats_to_ostream<Stat_set>().
 *
 * We say "sometimes" and "maybe," as it's just one possibility.  What's relevant generally is that `Set` is
 * at times `stats_*()`ed by itself and at other times a `Stat_set` *directly containing* it is
 * `stats_*()`ed as a whole.
 *
 * In this case use the following pattern.  Note that it may be tempting to take shortcuts to a line here or there,
 * so as to skip maybe `declare_stats()`ing `Set2` on its own: In our experience this will not work out for long,
 * and eventually you'll probably be forced to do the following after all.  The good news is it involves *no*
 * copy-pasting of stat-lists; if done as shown, each stat `m_...` shall be declared exactly once
 * (well, twice: inside its immediate `struct` and in that `struct`'s `declare_stats()`).
 *
 *   ~~~
 *   struct Set2
 *   {
 *     type1_t m_stat1;
 *     type2_t m_stat2;
 *   };
 *
 *   struct Stat_set
 *   {
 *     Set2 m_set2; // Nested stat-set.
 *
 *     // ...other items...
 *   };
 *
 *   template<typename Visitor>
 *   void declare_stats(std::string name_prefix, const Set2* src_stats, Set2* target_stats,
 *                      Visitor&& visitor)
 *   {
 *     FLOW_UTIL_STAT_DECLARE(m_stat1, ACCUMULATOR);
 *     FLOW_UTIL_STAT_DECLARE(m_stat2, GAUGE);
 *   }
 *
 *   template<typename Visitor>
 *   void declare_stats(std::string name_prefix, const Stat_set* src_stats, Stat_set* target_stats,
 *                      Visitor&& visitor)
 *   {
 *     // The crux: Forward to the Set2 declare_stats. Add-on a prefix -- if desired -- so that
 *     // at a minimum the print() output would look like:
 *     //   set2.stat1=[...] set2.stat2=[...] ...<Stat_set's own stuff>...
 *     // It is recursively possible for some other Stat_superset to contain Stat_set and, for output,
 *     // furnish us with our own "prefix.", which would be in `name_prefix`, so here we'd tack-on ours
 *     // to that, resulting in Stat_superset pretty-print to look like maybe
 *     //   stat_set.set2.stat1=[...] stat_set.set2.stat2=[...] stat_set.<...> stat_set.<...>
 *     declare_stats(name_prefix + "set2.",
 *                   src_stats ? &src_stats->m_set2 : nullptr, // Attn: must do the null-check.
 *                   target_stats ? &target_stats->m_set2 : nullptr,
 *                   visitor); // Attn: No need to forward(visitor).
 *
 *     // ...Declare more here....
 *   }
 *   ~~~
 *
 * A few points for clarification:
 *   - It is not mandatory to add to the prefix when forwarding to an "inner" `declare_stats()`.
 *     It is an aesthetic/clarity decision, as of this writing affecting pretty-print only; if no further
 *     qualification is desired then just forward `name_prefix` with tacking on anything else.
 *   - util::stat machinery will always pass the prefix "" to the top-level thing being `stats_*()`ed.
 *     However that should be none of any `declare_stats()`'s concern: if not composing then ignore
 *     `name_prefix`; if composing then (at least) forward it to inner `declare_stats()`(s).
 *   - The order of the `FLOW_UTIL_STAT_DECLARE*()` invocations *is* meaningful, deliberately: the
 *     `stats_*()` ops by convention respect it -- most visibly stats_to_ostream() (pretty-print) and
 *     stats_field_names(), which emit in exactly that order.  I.e., the default presentation order is
 *     under your (`declare_stats()`-author's) control.  Any future `stats_*()`-like extensions should --
 *     all else being equal -- do the same.  (Nothing stops a *consumer* from then post-processing however
 *     they like: sort by name, whatever.)  Gentle recommendation: keep the member-declaration order and
 *     the `declare_stats()` order as similar as possible, so the `struct` reads like its printout.
 *
 * One last tip: If you are composing with an inner `Set2` as shown, then probably it is best to *also*
 * formally compose with the remaining options as opposed to `FLOW_UTIL_STAT_DECLARE*()`ing the remaining
 * stat-members inline.  Otherwise you'll likely run into trouble if trying to, say, stats_reset() one's own direct
 * stuff.  This may seem like a burden, but at worst it just adds a little boiler-plate in the form of
 * additional `struct`s and corresponding `declare_stats()`s.  What's important is you should never have to
 * copy/paste `FLOW_UTIL_STAT_DECLARE*()`s themselves -- which is where maintenance errors would crop up.
 * Composing with everything -- or with nothing! -- is how one avoids such unpleasant things.
 *
 * Alternative stat-collection methods
 * -----------------------------------
 * So far we've described the basic *vanilla* stat-collection system.  There are some other systems supported.
 * To talk about these let's slightly formalize what's involved regardless of specifics.
 *
 * "Stat collection" as a whole basically consists of the following aspects.
 *   -# *Stat-updates*: As events occur, we save things into `Stat_set`s.  In vanilla stat-collection as described
 *      so far this just means the init operation (construction usually); conceptual increments for `ACCUMULATOR`s;
 *      and either increments, decrements, or stores for `GAUGE`s.  `HI_WMARK`s are updates along with their `GAUGE`s.
 *   -# *Stat-consumption*: This means obtaining a reference to or a copy of the currently-accurate `Stat_set`,
 *      whenever the user wishes to do something with this result: print, query in detail, whatever.
 *      For vanilla stat-collection it is simply making a copy (`stats_assign()`) of the current `Stat_set`,
 *      or using it in-place.  Straightforward: even if we add basic concurrency (`atomic`s), it's just a matter
 *      of being aware that individual stat-members can change at any time.
 *   -# *Stat-resets*: To restate, a stat-reset conceptually means making it so that subsequent stat-consumptions
 *      represent measurements taken from the current point in time forward.  In short: `ACCUMULATOR`s reset to
 *      their init-values (basically zeroed), `GAUGE`s undisturbed, and `HI_WMARK`s assigned to the current
 *      values of their respective `GAUGE`s.  With vanilla stat-consumption one simply calls stats_reset() which
 *      does just that.  Adding basic concurrency does not change this.
 *
 * Now we can list the other specifically-supported available techniques.  Some are described in-depth in below
 * sections, but we'll list them all here.
 *
 * ### Stat-collection system: Vanilla ###
 * Described already; recapped just above.  One can also think of this as/call this *continuous sampling*.
 *
 * ### Stat-collection system: Vanilla with basic concurrency ###
 * We'll get into it more below, but basically it's simple.  ~All stat-members are `atomic<T_numeric>` or
 * Histogram_counter (which is conceptually and in fact a set of `atomic<T_numeric>`s).  Multiple threads make
 * updates via fetch_add(), fetch_sub(), store(), and custom-type equivalents (Histogram_counter::record_value()
 * et al being the key example of a custom action -- in reality reducing to `fetch_add()`).  Stat-consumption
 * is the same -- with the added knowledge individual fields can change concurrently.  (Make a copy via
 * stats_assign() as convenient.)  Resetting is also the same: stats_reset().
 *
 * Under heavy load with many multi-threaded updates, thread contention can affect performance.  That brings us
 * to:
 *
 * ### Stat-collection system: TL-sharding under concurrency ###
 * This is the most complex stat-collection system.  It is described in-depth below.  It achieves better performance
 * by keeping thread-local `Stat_set` *shards*, one per stat-updating thread (created on-demand for such a thread
 * the first time it observes a stat-relevant event).  Stat-consumption *reconstructs* a result `Stat_set` by
 * summing each field's shard-values, the total for that stat-member being recorded as the current value.
 * If done properly, `ACC`s and `GAUGE`s are equally accurate as with vanilla stat-collection.  `HI_WMARK`s
 * are not, as it is impossible to maintain a continuously sampled high-water-mark -- a stat-consumption-like
 * operation would be necessary after any thread updates a shard, defeating the point of the thing.  We suggest
 * a mitigation technique to still maintain useful HWM results.
 *
 * Stat-resets are conceptually a bit subtle, but in practice it is simply this: stats_aggregate_shards() for
 * stat-consumption, stats_reset_shard_aggregate() for stat-resets; the signatures and how they're to be invoked
 * are quite similar for both.
 *
 * Additionally you'll need flow::util::Thread_local_state_registry or equivalent to be able to loop
 * through the shards at stat-consumption/reset points.  Lastly you'll need a thoughtful strategy for dealing
 * with exiting threads-with-shards.  Again we get into all of that below.
 *
 * ### Stat-collection system: Query another stats-source ###
 * This is described in full in the doc header for stats_since_reset_state() and its buddy stats_mark_reset_state()
 * which together make up our support for this stat-collection system.
 *
 * In short, it just means getting stats from another stat-system or *data source*.  For example: the memory
 * manager jemalloc has a rich stat-keeping API.  If building something -- our sister project Flow-IPC, say --
 * that uses jemalloc in a central way, it would make sense to pluck out certain values from this jemalloc API
 * and package them in a `util::stat`-friendly way.  Then they too can be `print()`ed, perhaps `stats_aggregate()`d,
 * and so on.
 *
 * At its core this does not require any special API: define your `Stat_set` in a compatible way, then store
 * values in it after querying them from the data source; store() + assignment and custom ops like
 * Histogram_counter::overwrite_count_for_bucket() are straightforward.  The addition of stat-resetting complicates
 * the situation slightly.  That's where stats_mark_reset_state() and stats_since_reset_state() complete the
 * picture.
 *
 * Aggregation; TL-sharding; `atomic`-design recommendations
 * ---------------------------------------------------------
 * ### Cost of `atomic<T_numeric>` ###
 * This section isn't meant to be a complete tutorial, by any means, but it should be pretty good when it comes to
 * stat-keeping.  It is also presented without context -- that comes in the next section -- so as to pin down some
 * core assumptions.  Lastly: we assume x86-64 (the most salient assumption -- but other modern architectures like
 * ARM64 should be in-the-ballpark, with `memory_order_relaxed`; YMMV); and gcc STL `std::atomic` circa
 * 2026 + modern Linux.
 *
 * For stats, we make the full assumption that all ops shall use the most performant memory-ordering:
 * `memory_order_relaxed`.  (All our syntactic-sugary helpers, and code internal to this facility, use this:
 * load(), store(), fetch_add(), fetch_sub(), and update_hi_wmark().)  The temporary loss of
 * cross-thread coherence from using this ordering is generally considered completely reasonable in the context
 * of consuming stats (whether for logging or reporting/monitoring or ...).
 *
 * @note Here we fully assume that it is unacceptable to use a naked variable, if it can be concurrently
 *       modified.  So whenever we speak of a non-`atomic`, assume it can't be changed simultaneously.
 *       It is officially undefined-behavior in standard C++, so let's just not.
 *       (We aren't saying this will destroy the world or result in memory corruption.  We're only saying we're
 *       not going to try to even analyze what-all this entails.)
 *
 * What are the ops that matter?  There are three; everything else basically reduces to these.
 *   - Load (`a = load()` for `atomic`s, a simple memory-access of a variable otherwise: `a`): Obtain the number,
 *     which may be changing concurrently (if `atomic`), and copy it into a variable/return it/whatever.
 *     - A high-water-mark update, for the purposes of this discussion, reduces -- most of the time -- to a
 *       load: load A; => is B > A? => no (usually) => we're done.  So we don't discuss the HWM-update as its own
 *       op, for simplicity, from here on.
 *   - Store (`store(&a, b)` for `atomic`s, else simple assignment `a = b`): Take a number and store it into
 *     a variable (which may be changing concurrently if `atomic`; so then either the store happens first, or the other
 *     change happens first).
 *     - Typically, as potentially-concurrent updates go: stores compete exclusively with stores.
 *   - Add (similarly subtract) (`fetch_add(&a, b)` or `a += b`): Kinda like storing, but it's addition;
 *     in the `atomic` case either the destructive-addition -- as 1 op taken together -- happens first, or the other
 *     change happens first.
 *     - Typically, as potentially-concurrent updates go: add/sub ops compete exclusively with add/sub ops.
 *
 * Now then: Given the stated assumptions about the environment:
 *   - First assume there is *no contention*: Nothing is touching the same possibly-`atomic` value in another thread.
 *     Then:
 *     - Load and store take the same # of cycles, whether acting on an `atomic` or not.
 *     - Add/sub is not quite identical.  fetch_add() is ~5-25 cycles more than naked `a += b`.  However, in
 *       and of itself, this difference -- which amounts (in 2026) to low-single-digit nanoseconds -- is
 *       typically considered negligible.
 *   - Now assume there *is contention*: Other thread(s) are touching the thing in another thread right then.
 *     - Under heavy contention, cache-line ping-pong between cores can add hundreds to thousands of cycles to the
 *       cost of (among other things) an add/sub.  How likely is heavy contention to occur?  That depends and is
 *       really up to you to analyze.  *Usually* though for stat-keeping to hit heavy contention, it would have to
 *       be during a truly frequent operation -- for example every `malloc()` in a memory-manager library +
 *       an application that uses the heap quite a lot from many threads.
 *
 * Bottom line: relaxed-atomic ops on stats (word-sized in our context) cost essentially nothing for load/store and
 * low-single-digit ns for add/subtract (more generally, read-modify-write) *when uncontended*.
 *
 * Keep these things in mind for the following discussion, particularly starting from the section following the
 * next one.  Not everything applies, depending on the design you choose for a particular `Stat_set`/stat!
 * E.g., if you choose the thread-local approach, then there will be no contention; and although it is still
 * (as you'll see) prudent to use `atomic`s, the only perf-pessimistic factoid above that's actually relevant
 * becomes the low-single-digit-ns cost of an add/sub operation.
 *
 * ### Aggregation (no concurrency) ###
 * Okay: temporarily forget concurrency.  Assume everything happens in series: updates, consumption of stats.
 * (To be clear -- that is quite common.  Either you do stuff in one thread, or you already have mutexes
 * synchronizing things + adding stat-updates is a drop in the bucket in those critical sections.)
 *
 * stats_aggregate() can be useful in this scenario.  Perhaps you keep N `Stat_set`s independently for whatever
 * reason, and perhaps they're useful in their own right, but also perhaps at times you want to see a total view
 * of them all.  stats_aggregate() will sum `ACCUMULATOR`s, take the mean of `GAUGE`s, and
 * take the max of `HI_WMARK`s.  That's convenient and beats doing it by hand by a mile.
 *
 * Along the same lines stats_sum() is available.  It may be appropriate for post-processing instead of or
 * as a complement to stats_aggregate().  One can also develop custom operations inspired by these.
 *
 * ### Concurrency: Intro / Centralized stat-keeping ###
 * Aggregation can also be helpful, or even at times essentially mandatory, when there *is* concurrency.
 * So let's discuss concurrency w/r/t stat-keeping, in and of itself; then in the next section show *if* and
 * how *sharded aggregation* (stats_aggregate_shards()) helps.
 *
 * Suppose you have N threads.  Some mechanism acts, and therefore records stats about it, concurrently in those N
 * threads.  One viable approach is to maintain a single `Stat_set` full of `atomic<T_numeric>`s.  Each update,
 * using fetch_add(), fetch_sub(), store(), update_hi_wmark(), updates an `atomic`.
 * Then, when one is interested in consuming (to print, or publish for monitoring, or...), in some thread you
 * load() each stat of interest.  (By and large everything in this paragraph reduces to the aforementioned 3
 * basics ops: load, store, add/sub.)
 *   - Pros: it's quite easy, both the updating and the consuming!  "Just" don't forget to use the
 *     `load()`/`store()`/`fetch_add()`/... element-level ops,
 *     or equivalent, as naked ops like `+=` will use the unnecessarily-slower memory-ordering mode.
 *   - Cons: under *heavy contention* (see above about how likely that is to occur), all the concurrent ops can
 *     be much slower than one imagines a read/assign/`+=` should be.  Again, the big enemy is the cache-line
 *     ping-pong among cores.  Again though... that's under *heavy contention*.
 *
 * To recap: `atomic<>` fields and `Histogram_counter`s; store(), fetch_add(), fetch_sub(), update_hi_wmark()
 * from multiple threads to update; load() to read individual `atomic`s.  Heavy contention can reduce perf.
 * This is *centralized stat-keeping*.
 *
 * So should you do it?  Depends.  It really is easy, and in many situations heavy contention will not happen.
 * That aside, everything is a matter of trade-offs; so the decision is not necessarily an absolute one but depends
 * on the alternative(s) too.  We now present the alternative, where another form of aggregation
 * (stats_aggregate_shards()) is essential: The TL-sharding design.
 *
 * ### Aggregation (with concurrency): TL-sharding ###
 * This technique, *TL-sharding* (thread-local sharding), is the alternative to centralized stat-keeping, when
 * many threads bring possibility of perf-killing contention.  In summary: It eliminates contention entirely by
 * making stat-updates thread-local.  The price paid is complexity and a reduced fidelity for `HI_WMARK` fields.
 * (We suggest mitigations for both of these below.)
 *
 * It works as follows.  You still will make your `Stat_set` full of `atomic<>s` (low, brief contention is possible
 * at stat-consumption and stats-reset time, as we'll soon show).  Now consider potentially-stat-updating threads:
 * T1, T2, ....  Each thread shall keep a thread-local (TL) `Stat_set`; each such `Stat_set` is called
 * a *shard*.  Each thread, while regularly executing, shall only update its shard.
 *   - `ACCUMULATOR`: This is the most straightforward.  In each thread, the given stat-member `atomic`s is
 *      `fetch_add()`-ed-to, never subtracted or otherwise touched over the course of stat-updating.
 *      (Other `ACCUMULATOR` types, including Histogram_counter, are supported.  Details omitted here, but it's
 *      simple.)
 *   - `GAUGE`: (We are concerned with numeric `GAUGE`s in this context.)  Roughly speaking there are two basic
 *     possibilities.
 *     - (Simple) Imagine a `GAUGE` of "juggled balls in the air": `B`.  In thread T1, if I throw up a ball, it is `++B`
 *       in that thread (`fetch_add(&B, 1)`, but you get the point).  If I catch a ball, it is `--B`.  Let's
 *       stipulate, though, that if I throw up a ball in thread Tx, I can only catch it also in Tx.  So, I might
 *       do `++B, ++B, --B, ++B, --B, --B,`; but since one cannot catch an unthrown ball, there's no
 *       `++B, --B, --B` (for example).  So: This is simple; `fetch_add()`, `fetch_sub()`, all good.  An
 *       `unsigned` stat-member `B` in the `Stat_set` works great.
 *     - (Less simple) Now suppose in thread T1 I can throw up a ball but can catch it in another thread like T2;
 *       or in T1 again is also possible.  This is not really so different in the end.  The only change:
 *       a given thread's shard *can legitimately dip into negative values*.  Therefore the `Stat_set` member `B`
 *       in such cases must be a `signed` type.
 *       - Generally we recommend using `int64_t`.  It can go negative; but the range on either side of zero
 *         is sufficiently enormous for most applications.
 *   - `HI_WMARK`: Put a pin in this one for a moment.  For now just a recipe: *Do not update* `HI_WMARK`s
 *     in shard-`Stat_set`s.  `HI_WMARK`s are determined during stat-consumption (discussed below).
 *
 * As noted, stat-updating is simple enough; not really any different than in centralized stat-keeping.  Just
 * remember:
 *   - Some `GAUGE` members may need to be of a signed type, namely if a given shard's value may go negative.
 *     Usually use `int64_t`.
 *   - Do not update `HI_WMARK` members at all.
 *
 * What about stat-consumption?  It works as follows conceptually:
 *   - `ACCUMULATOR`: At stat-consumption: Simply sum across all shards.
 *   - `GAUGE`: Ditto.  (Even if a shard can show a negative value, upon summing the true non-negative `GAUGE`-value
 *     shall be reconstructed.)
 *   - `HI_WMARK`: Unfortunately it's impossible to compute the true high-water-mark: Since the gauged-value
 *     is composed of different shards contributing differently at various points in time, without our summing
 *     across the shards at all of those times, we cannot know anything about those times between stat-consumption
 *     points.  Therefore we can only determine the max-value *from across the stat-consumption points* themselves.
 *     This is not as good (we'll discuss mitigations below), but the algorithm is simple: At each stat-consumption
 *     check whether the gauged-value is greater-than the max recorded last time.  If so, overwrite that record
 *     with the new max.  This is the high-water-mark at this point.
 *
 * The above describes (1) a recipe for updating shards-`Stat_set`s and (2) conceptually how a resulting
 * "reconstructed" `Stat_set` is computed out of the shards at stat-consumption points.  Now we shall concretely
 * describe the recipe for (2).
 *   - Firstly -- entirely due to the (assumed in general case) presence of `HI_WMARK`s in the `Stat_set` under
 *     discussion -- it is required that you keep a `Stat_set` across stat-consumption requests; let's call it `S`.
 *     `S` shall start off as a fresh (essentially zeroed) object.  At stat-consumption, which we'll describe
 *     shortly, it will be filled-out with the current reconstructed-from-shards `ACCUMULATOR`s and `GAUGE`s --
 *     as well as the HWM values.  A copy of this `S` shall be the stat-consumed result.  Simply put: don't discard
 *     `S` but rather keep it for next time.
 *   - Secondly: In order to be able, at stat-consumption time, to loop-through extant threads' shards, you
 *     will require a *registry of extant threads/shards*.  Hence a mere `thread_local` or `thread_specific_ptr` or
 *     flow::util::Thread_local_ptr is insufficient.  flow::util::Thread_local_state_registry is, essentially,
 *     an extension of `Thread_local_ptr`/`thread_specific_ptr` that adds the ability to lock central lock, cycle
 *     through all currently extant TL objects (in this case: `Stat_set`s) to do something (in this case: aggregate
 *     stats), and unlock.  Therefore: Use `Thread_local_state_registry` (or equivalent) to maintain your
 *     `Stat_set`s.
 *     - If you already have a thread-locality-based setup (before considering stat-keeping) for your algorithm and
 *       data, then either (a) you are using `Thread_local_state_registry` already, or (b) you can upgrade to it
 *       (from `thread_local` or `thread_specific_ptr` or `Thread_local_ptr`).
 *   - At stat-consumption: You are in some thread -- either one of T1, T2, ..., or some other thread entirely.
 *     Call stats_aggregate_shards().  In and of itself it is straightforward:
 *     Give it `&S` -- both as an in-arg (storing HWM values so far) and out-arg (it'll be filled with the
 *     reconstructed-from-shards current stats); and give it an iterator-range through the extant shard `Stat_set`s;
 *     plus a fresh (init-valued) `Stat_set` for its last arg -- consulted only if the shard-range is empty
 *     (e.g., stats consumed before any stat-updating thread has yet existed; HWMs survive such stretches).
 *     - To obtain this range, use Thread_local_state_registry::while_locked() (or equivalent) to grab a stable
 *       list of extant `Stat_set`s.  This list is supplied to the `F()` in `.while_locked(F)`.
 *     - See stats_aggregate_shards() doc header for tips about range mechanics to set this up.
 *       In short, Boost or C++2x range functionality makes it simple and efficient; or one can just dump the
 *       shard-copies into a temporary `deque` first.
 *     - Here it is usually best to make a copy of `S` (via stats_assign()).  This copy is your stat-consumption
 *       result.  (`S` itself can be modified by another stat-consumption or stat-reset.  Make sure a
 *       stat-consumption or stat-reset targeting `S` never executes concurrently with another such op.)
 *
 * Resetting of stats that are collected via TL-sharding is not achieved by stats_reset().  Instead use
 * stats_reset_shard_aggregate().  The input parameters are essentially the same as for stats_aggregate_shards():
 * `&S` and the range of TL-shard `Stat_set`s.  This will (1) zero out the per-shard `ACCUMULATOR`s and (2)
 * compute (as during stat-consumption) the `GAUGE`s by summing the per-shard values and save the result
 * into each relevant `HI_WMARK` (if any).
 *
 * @warning Pay special attention to your design w/r/t what happens when a thread (with an active shard `Stat_set`)
 *          exits.  (If it is possible to design your algorithm so that this never occurs, it may significantly
 *          simplify things.)  What -- if anything -- to do depends on your mix of `ACC`s and `GAUGE`s.  *Usually*
 *          a `GAUGE` shard-value in a newly-dead thread is to be treated the same as being zero.  If so: nothing
 *          special to do yet.  If not -- meaning perhaps the dead thread's `GAUGE`'s shard-value's proper
 *          contribution to the total is its last value at thread exit -- then treat it as an `ACC` in this
 *          context.  Which brings us to `ACC`s.  Logically, an `ACC` counts some event's occurrence, so once
 *          its value exceeds zero, that's forever.  Therefore, you must set up a mechanism such that, at thread
 *          exit time, that thread's `Stat_set` shard is saved in some central store of "finalized" shards.
 *          For stats_aggregate_shards() (consumption) and stats_reset_shard_aggregate() (reset) all such shard
 *          `Stat_set`s must be part of the range given to those functions.  Note also that
 *          flow::util::Thread_local_state_registry provides a straightforward mechanism for executing code
 *          at thread exit.
 *
 * So that's how it works.  Complexity aside, it has really just one functional drawback: The HWM (`HI_WMARK`)
 * fidelity.  As explained above, it can never be perfect, but the HWM quality is basically proportional to
 * how frequently and regularly one consumes stats.  Hence, if HWM quality is a concern (which, obviously, is
 * contingent on your `Stat_set` featuring `HI_WMARK`s in the first place), one can simply stat-consume at regular
 * intervals (e.g., every 5 seconds).  `S` can be quietly updated; it is not necessary to log it or use it or copy
 * it or anything.  Then a "real" stat-consumption, whenever desired, can count on pretty well-sampled HWMs.
 *
 * At the risk of verbosity and redundancy, a recap of pros and cons of TL-sharding:
 *   - Con: Introducing a `Thread_local_state_registry`, or equivalent thing that tracks extant TL-objects,
 *     undoubtedly adds complexity.  The utility helps a ton, but that is still the case.  Adding it *on account of
 *     stat-keeping specifically* is definitely not a light-weight decision.
 *     - In many cases, though, there is already a `Thread_local_state_registry` (or similar) tracking other
 *       TL-objects.  In that case adding a `Stat_set` into the TL-state structure is a no-brainer; it is ~free.
 *     - If you have `thread_specific_ptr` or `Thread_local_ptr` (better version of `t_s_p` in Flow), then
 *       upgrading to `Thread_local_state_registry` is not hard, albeit no longer ~free.  From a naked `thread_local`
 *       it is a hair more work still.
 *     - Don't forget the above warning regarding thread exit.
 *   - Con: `HI_WMARK`s lose resolution.  A `HI_WMARK` no longer tracks the entire time range since init or reset;
 *     instead it samples only at aggregation-times.
 *     - This is a built-in limitation of the TL-sharding approach.
 *     - A pretty good mitigation -- regular quiet stat-aggregating -- is explained above.
 *   - Con: There are some coherence caveats.  These are discussed below.  In short these are well within reason.
 *   - Pros: Risk of slow stat-updates, on account of contention, is eliminated.  Hence, that part of the trade-off
 *     can just be forgotten => reduced hand-wringing.  The thought of mere stat-keeping slowing down the stuff that's
 *     being stat-tracked (likely to assess perf at least in part!) is very unpleasant.
 *
 * ### Stat-coherence caveats under concurrency and aggregation ###
 * We omit here a detailed survey of surface-level strangeness that can at times be observed under concurrency
 * and arguably increased strangeness if using TL-sharding as well.  It would be voluminous and arguably out of
 * place; more importantly it's not difficult to figure out specifics, once the source of issues is clear:
 *
 * When there is just one given observed quantity, which can concurrently change, things are simple: We observe
 * `m_x == A`, and `m_x` is an `atomic` that is routinely modified by other thread(s), then the `A` result is
 * correct at that instant but not necessarily in the next instant.
 *
 * The *coherence* caveats begin when 2+ quantities are related in some way, meaning one is computed from another
 * (at least partially).  Examples:
 *   - `GAUGE m_x`; `HI_WMARK m_x_hi_wmark` based off `m_x`.
 *     - Due to concurrency one might see `m_x > m_x_hi_wmark`: a surface-level contradiction/malfunction.
 *   - You've designed your `Stat_set` so that, e.g., value A plus value B should equal value C.
 *     - Due to concurrency one might see A plus B being almost equal to C but not quite.
 *
 * Under TL-sharded aggregation the same basic dynamics are at play, but there is also looping through N
 * shard `Stat_set`s, each of which is concurrently modifiable, *and* it takes (some) time to loop through them.
 * In short, there are more interrelationships at play.
 *
 * The bottom line: These possibilities are not worth analyzing deeply.  One must simply remember a basic
 * principle *when reading* result `Stat_set` values at stat-consumption time:
 *   - Treat each stat-member in `Stat_set` as being separately computed from all others.
 *   - Due to concurrency, expected relationships between 2+ stat-members will generally hold but might
 *     be *slightly* off in a particular `Stat_set` snapshot.
 *
 * @note Tip: Regarding the situation wherein stat C is defined, by you, as equal to A+B, where A and B is each
 *       also a stat: We recommend against this.  Without concurrency it is redundant albeit potentially does provide
 *       a bit of sanity-encouragement.  Still, though, it's more state -- without conveying more information.  *With*
 *       concurrency, it can make reading consumed `Stat_set`s or aggregations
 *       thereof still more confusing.  Instead: all else being equal it is better to define only A and C, or A and B,
 *       as actual declared-stats.  (The remaining quantity -- if significant -- can be explained in a comment;
 *       or a helper accessor can compute it.)
 */
namespace flow::util::stat
{

// Types.

// Find doc header near the body of this class.
class Histogram_counter;

/// Describes an individual member of a `Stat_set`, as decided by the user (you) via FLOW_UTIL_STAT_DECLARE().
enum class Stat_type
{
  /**
   * Value (typically numeric, or conceptually a collection of numerics) that counts/tallies (typically
   * via `+= N`, with `N > 0`, or equivalent) some event occurring over a past period of
   * time, always in one (typically increasing) direction, starting at some *init* value (typically `{}` which is
   * often zero).  For example a count of packets received; or a tally of total bytes received in packets; or
   * a total `float` weight processed by a plant.
   *   - Cf. `S_GAUGE` which covers values that over time can go in any direction; e.g., the length of a queue.
   *     This requires some care, as superficially they often look similar and also `+=`ed (but the key is -- also
   *     `-=`ed).
   *
   * Recommend, for numeric integer values, to use an unsigned type.
   *
   * `atomic<T_numeric>` is supported.  Again, recommend using an unsigned `T_numeric`.
   *
   * Histogram_counter is supported, as it satisfies various requirements below.
   *
   * ### Reset ###
   * A *reset* of an `S_ACCUMULATOR` `stats.m_v` is formally defined as:
   *   -# Input: `const Stat_set& fresh_stats`, where every stat including `fresh_stats.m_v` is at its init state.
   *   -# Make it so that `stats.m_v` is as-if one performed:
   *      `store(&stats.m_v, load(fresh_stats.m_v));`.
   *
   * The default reset() does exactly that.  However, this behavior may be overridden -- for all `ACCUMULATOR`s of
   * type `T` -- by defining an ADL-findable reset() with a signature compatible with stat::reset().  An example
   * of this is `T` = Histogram_counter.
   *
   * Informally, a reset of an `ACCUMULATOR` means, basically, zeroing/reinitializing it; the formal definition
   * is so-written above due the mechanism we find most convenient for this when operating on `Stat_set` `struct`s.
   * Namely it is convenient for you to declare a `struct` data member and right-there comment it and
   * shows its init value, e.g: `unsigned int m_cool_counter = 0;`.
   *
   * ### Aggregation ###
   * An aggregation into an `S_ACCUMULATOR` `stats.m_v` from `src_stats.m_v` is formally defined as:
   * `fetch_add(&stats.m_v, load(src_stats.m_v))`.  Therefore, for non-`atomic`s, a reasonable
   * `+=` operation must be defined for its type, if aggregation is ever compiled for the containing `Stat_set`.
   *
   * @see stats_aggregate_one().  Or repeated ~N times: stats_aggregate(), stats_aggregate_shards().
   *
   * ### Delta ###
   * The delta between `S_ACCUMULATOR` `stats.m_v` from `reset_stats.m_v` is formally defined as:
   * `fetch_sub(&stats.m_v, load(reset_stats.m_v))`.  Therefore, for non-`atomic`s, a reasonable
   * `-=` operation must be defined for its type, if aggregation is ever compiled for the containing `Stat_set`.
   *
   * @see stats_since_reset_state() which relies on the delta operation for `ACCUMULATOR`s.
   */
  S_ACCUMULATOR,

  /**
   * Value (often numeric) that represents the state of something at some point in time.  When numeric it
   * can go up and down, for example; but really it can be almost anything (an `enum`! a temperature!) and thus
   * isn't even a "stat" in the classic sense but more like simply a variable.
   *   - Cf. `S_ACCUMULATOR`.  For example, suppose you've got a queue Q.  A `GAUGE` might be the *current* length
   *     of Q.  An `ACCUMULATOR` might be a count of how many times a value was pushed onto Q.
   *
   * While a `GAUGE`'s init value is sometimes meaningless until a measurement is taken, it must still be
   * set (not left uninitialized) when creating the containing `Stat_set`.  For example, one might set a temperature
   * gauge to absolute zero.
   *
   * `atomic<T_numeric>` is supported.
   *
   * ### Reset ###
   * A *reset* of an `S_GAUGE` `stats.m_v` is formally defined as a no-op.  Informally, the reset of a
   * `Stat_set` means starting a new time period of stat-keeping; but doing so has no effect on the current state
   * of whatever a `GAUGE` tracks.  So it is to be left alone.
   *
   * ### Aggregation ###
   * An aggregation into an `S_GAUGE` `stats.m_v` from `src_stats.m_v` is formally defined the same as for
   * `S_ACCUMULATOR`.
   *
   * @see stats_aggregate_one(), stats_aggregate_shards().
   *
   * However: Aggregation of `.m_v` from `N Stat_set`s is defined as (1) aggregating them as-if it is `S_ACCUMULATOR`
   * (i.e., summing via `+=` or `atomic` variant) and (2) performing a scaling by `N` as follows: `result /= N`.
   * Therefore, for non-`atomic`s, a reasonable `/=` operation must be defined for its type, if `N`-aggregation is
   * ever compiled for the containing `Stat_set`.  Thus aggregating `N GAUGE`s is achieved via taking the mean
   * thereof, via successive `+=`s and a `/=` (or their `atomic` variations).
   *
   * @see stats_aggregate().  The preceding paragraph does not apply to stats_aggregate_shards() however.
   */
  S_GAUGE,

  /**
   * Value of the same type as a specific other value `stats.m_v` in the same `Stat_set` with `Stat_type` `S_GAUGE`
   * that represents the highest value that `stats.m_v` has reached over a past period of time.
   * For example, suppose you've got a queue Q.  A `GAUGE stat.m_q_size` might be the *current* length of Q.
   * Then a `HI_WMARK stat.m_q_size_hi_wmark` might be gauging `stat.m_q_size`; each time the latter is updated,
   * the former is updated to the same value if and only if the old value `<` the new value.
   *
   * When the gauged-stat is initialized (or reset) to `X`, its high-water mark stat must be set to the same value
   * `X`.
   *
   * @warning When declaring a `HI_WMARK stat.m_v` as gauging `GAUGE stats.m_y`, the `FLOW_UTIL_STAT_DECLARE*()`
   *          invocations must appear in order `m_y`, `m_v`.
   *
   * ### Reset ###
   * A *reset* of an `S_HI_WMARK` `stats.m_v` is formally defined as:
   * `store(&stats.m_v, load(stats.m_y));`.  (Recall that, as a `GAUGE`, `stats.m_y` would not have been
   * touched by the reset.)
   *
   * ### Aggregation ###
   * An aggregation into an `S_GAUGE` `stats.m_v` from `src_stats.m_v` is formally defined the same as:
   * `update_hi_wmark(&stats.m_v, load(src_stats.m_v))`.  Therefore, for non-`atomic`s, a reasonable
   * `<` operation must be defined for its type, if aggregation is ever compiled for the containing `Stat_set`.
   *
   * @see stats_aggregate_one().  Or repeated ~N times: stats_aggregate().
   *
   * However: by nature of sharding, stats_aggregate_shards() works differently.  See its doc header for
   * details.  Nevertheless, still, for non-`atomic`s a reasonable `<` operation must be defined for its type,
   * if one ever invokes stats_aggregate_shards<Stat_set>() for the containing `Stat_set`.
   *
   * @see stats_since_reset_state() is yet another high-water-mark technique (that again requires `<` in
   *      similar ways).
   */
  S_HI_WMARK
}; // enum class Stat_type

// Find doc header near the body of these compound types.

template<typename Stat_set>
struct Stat_set_printable;
template<typename Stat_set_t, size_t N>
class Stat_set_list;
template<typename Tag_t, typename Stat_set_t, size_t N = 1>
class Global_stats;

// Free functions.

/**
 * Default `load(stats.m_val)` that simply return `stats.m_val`.
 * Used by the generic `stat::stats_*()` algorithms when a more specific overload, such as/particularly for
 * `atomic<>`s, is not found via ADL.
 *
 * @note In order to performantly (minimizing copying) support large `T` when used in `stat::stats_*()`
 *       constructions like `store(&x, load(y))`, we must take and return `T` by ref, not by value.
 *       For small `T`, any decent optimizer will inline this template instantiation, reducing it to
 *       copy-equivalent perf after all.
 *
 * @tparam T
 *         Value type.
 * @param val
 *        Value; usually a member of a `Stat_set`.
 * @return `val`.
 */
template<typename T>
const T& load(const T& val);

/**
 * `load(stats.m_val)` that loads a copy of the value stored inside an `atomic<T> stats.m_val`.
 * Proper way for you to read an `atomic` stat; and used by the generic `stat::stats_*()` algorithms when
 * needing to load a stat, and its type happens to be `atomic<T>`.
 *
 * May be useful for general `atomic` loading with `relaxed` memory-order, outside any particular `Stat_set`.
 *
 * @param val
 *        Ref to value; usually a member of a `Stat_set`.
 * @return Copy of `val`.
 */
template<typename T>
T load(const std::atomic<T>& val);

/**
 * Default `store(&target_stats.m_val, val)` that simply assigns `*target_val = val`.
 * Used by the generic `stat::stats_*()` algorithms when a more specific overload, such as/particularly for
 * `atomic<>`s, is not found via ADL.
 *
 * @note `T` is deduced from `target_val` only; `val` will be implicitly converted to `T`.  This means
 *       you can write `store(&stats.m_val, 1)` instead of `store(&stats.m_val, size_t(1))`
 *       (or whatever the actual type is).  Same applies to the other similar element-level primitives.
 *       It is also possible to make it, like, `template<typename T, typename U> void store(T*, U)`
 *       and inside `{}` do a `static_cast<T>`.  However in that case one wouldn't get range-narrowing
 *       warnings and the like (which is less safe).
 *
 * @note In order to performantly (minimizing copying) support large `T` when used in `stat::stats_*()`
 *       constructions like `store(&x, load(y))`, we must take `T` by ref, not by value.
 *       For small `T`, any decent optimizer will inline this template instantiation, reducing it to
 *       copy-equivalent perf after all.
 *
 * @tparam T
 *         Value type.  Must be copy-assignable.
 * @param target_val
 *        Ptr to mutable value to replace; usually a member of a `Stat_set`.
 * @param val
 *        Value to store.
 */
template<typename T>
void store(T* target_val, const boost::type_identity_t<T>& val);

/**
 * `store(&target_stats.m_val, val)` that stores a copy of `val` into an `atomic<T> target_stats.m_val`.
 * Proper way for you to store to an `atomic` stat; and used by the generic `stat::stats_*()` algorithms when
 * needing to assign-to a stat, and its type happens to be `atomic<T>`.
 *
 * May be useful for general `atomic` storing with `relaxed` memory-order, outside any particular `Stat_set`.
 *
 * @param target_val
 *        Ptr to mutable value to replace; usually a member of a `Stat_set`.
 * @param val
 *        Value to store.
 */
template<typename T>
void store(std::atomic<T>* target_val, boost::type_identity_t<T> val);

/**
 * Default `fetch_add(&target_stats.m_val, addend_val)` that simply applies `*target_val += addend_val`.
 * Used by the generic `stat::stats_*()` algorithms when a more specific overload,
 * such as/particularly for `atomic<>`s, is not found via ADL.
 *
 * @note In order to performantly (minimizing copying) support large `T` when used in `stat::stats_*()`
 *       constructions like `fetch_add(&x, load(y))`, we must take `T` by ref, not by value.
 *       For small `T`, any decent optimizer will inline this template instantiation, reducing it to
 *       copy-equivalent perf after all.
 *
 * @note Corollary: We cannot return `T`, the pre-mod value of `*target_val`, the way the `atomic`-taking
 *       overload (or indeed `std::atomic::fetch_add()`) does.  Doing so would require pre-copying `T` -- at least
 *       in the abstract machine (optimizers may elide this in practice, when the caller does not use the
 *       return value; but we would rather not rely on that).
 *
 * @internal
 * @note Corollary of corollary: `stat_*()` algorithms shall not rely on `fetch_add()` returning anything.
 *       Same for `fetch_sub()`.
 * @endinternal
 *
 * @tparam T
 *         Value type.  Must have `+=`.
 * @param target_val
 *        Ptr to mutable value to increment; usually a member of a `Stat_set`.
 * @param addend_val
 *        Value to add-to `*target_val`.
 */
template<typename T>
void fetch_add(T* target_val, const boost::type_identity_t<T>& addend_val);

/**
 * `fetch_add(&target_stats.m_val, addend_val)` that increments by `addend_val` value inside
 * `atomic<T> target_stats.m_val` and returns the pre-change value.  Proper way to destructively-increase
 * an `atomic` stat (`ACCUMULATOR`s, `GAUGE`s); and used by the generic `stat::stats_*()` algorithms when needing
 * to aggregate a stat, and its type happens to be `atomic<T>`.
 *
 * May be useful for general `atomic` add with `relaxed` memory-order, outside any particular `Stat_set`.
 *
 * @see To destructively-decrease an `atomic` stat (or just numeric value) consider fetch_sub().
 *
 * @note Subtlety: If `T` is signed, then it is possible to *decrease* `*target_val` by passing a negative
 *       `addend_val`.  However, both for maintainability (what if `T` becomes unsigned down the line?) and
 *       code expressiveness, we recommend in that case to use fetch_sub() instead.  Alternatively just
 *       use the regular `atomic` API (`target_val->fetch_{add|sub}(..., memory_order_relaxed)`).
 * @note If `T` is unsigned, then you *cannot* use fetch_add() for that purpose.  You must use
 *       fetch_sub() or the regular `atomic` API `fetch_sub()` per previous note.
 *
 * @param target_val
 *        Ptr to mutable value to replace; usually a member of a `Stat_set`.
 * @param addend_val
 *        Value to add-to `*target_val`.  If `< 0` consider calling `fetch_sub(target_val, -addend_val)` instead.
 * @return Pre-change `*target_val` (matches `atomic::fetch_add()` semantics).
 */
template<typename T>
T fetch_add(std::atomic<T>* target_val, boost::type_identity_t<T> addend_val);

/**
 * Default `fetch_sub(&target_stats.m_val, _val)` that simply applies `*target_val -= subtrahend_val`.
 * Used by the generic `stat::stats_*()` algorithms when a more specific overload,
 * such as/particularly for `atomic<>`s, is not found via ADL.
 *
 * Notes for fetch_add() equivalent, regarding `const T&` (versus `T`-by-value) and returning no `T`, apply
 * here equally.  See that doc header please.
 *
 * @tparam T
 *         Value type.  Must have `-=`.
 * @param target_val
 *        Ptr to mutable value to decrement; usually a member of a `Stat_set`.
 * @param subtrahend_val
 *        Value to subtract-from `*target_val`.
 */
template<typename T>
void fetch_sub(T* target_val, const boost::type_identity_t<T>& subtrahend_val);

/**
 * `fetch_sub(&target_stats.m_val, subtrahend_val)` that decrements by `subtrahend_val` value inside
 * `atomic<T> target_stats.m_val` and returns the pre-change value.  Proper way to destructively-decrease
 * an `atomic` stat (`GAUGE`s); and used by the generic `stat::stats_*()` algorithms (when needing
 * to, at least, find the delta against an earlier `ACCUMULATOR` value -- stats_since_reset_state()), and its type
 * happens to be `atomic<T>`.
 *
 * May be useful for general `atomic` subtract with `relaxed` memory-order, outside any particular `Stat_set`.
 *
 * @param target_val
 *        Ptr to mutable value to replace; usually a member of a `Stat_set`.
 * @param subtrahend_val
 *        Value to subtract-from `*target_val`.
 * @return Pre-change `*target_val` (matches `atomic::fetch_sub()` semantics).
 */
template<typename T>
T fetch_sub(std::atomic<T>* target_val, boost::type_identity_t<T> subtrahend_val);

/**
 * Equivalent to `target_val->exchange(val, memory_order_relaxed)`; sets `*target_val` to `val` and returns
 * the pre-change value.  Useful at least for atomically reading-and-modifying a gauge that can change arbitrarily
 * as opposed to via fetch_add() and fetch_sub().
 *
 * May be useful for general `atomic` exchange with `relaxed` memory-order, outside any particular `Stat_set`.
 *
 * @note Atomic-only.  There is intentionally no non-`atomic` overload: for plain numeric stats the ~equivalent
 *       `swap()` or a couple assignments are fine.  The atomic-only nature mirrors fetch_sub().
 *
 * @param target_val
 *        Ptr to mutable value to replace; usually a member of a `Stat_set`.
 * @param val
 *        Value to store into `*target_val`.
 * @return Pre-change `*target_val` (matches `atomic::exchange()` semantics).
 */
template<typename T>
T exchange(std::atomic<T>* target_val, boost::type_identity_t<T> val);

/**
 * Default `update_hi_wmark(&target_stats.m_val, val)` that updates `*target_val` to `val` if and only if the
 * latter exceeds the former.  Used by the generic `stat::stats_*()` algorithms when a more specific overload,
 * such as/particularly for `atomic<>`s, is not found via ADL.
 *
 * @note In order to performantly (minimizing copying) support large `T` when used in `stat::stats_*()`
 *       constructions like `update_hi_wmark(&x, load(y))`, we must take `T` by ref, not by value.
 *       For small `T`, any decent optimizer will inline this template instantiation, reducing it to
 *       copy-equivalent perf after all.
 *
 * @tparam T
 *         Value type.  Must have `<`.
 * @param target_val
 *        Ptr to mutable value to potentially replace; usually a member of a `Stat_set`.
 * @param val
 *        Value to potentially store in `*target_val`.
 */
template<typename T>
void update_hi_wmark(T* target_val, const boost::type_identity_t<T>& val);

/**
 * `update_hi_wmark(&target_stats.m_val, val)` that stores copy of `val` value inside
 * `atomic<T> target_stats.m_val` if and only if the latter is less-than the former.
 * Proper way to update a Stat_type::S_HI_WMARK `atomic` stat (though usually skipped if implementing sharding);
 * and used by the generic `stat::stats_*()` algorithms when needing to aggregate a stat, and its type happens
 * to be `atomic<T>`.
 *
 * May be useful for general `atomic` update-if-new-max with `relaxed` memory-order, outside any particular `Stat_set`.
 * Writing it manually is somewhat annoying, involving a loop and weak-CAS and the like.
 *
 * @param target_val
 *        Ptr to mutable value to potentially replace; usually a member of a `Stat_set`.
 * @param val
 *        Value to potentially store in `*target_val`.
 */
template<typename T>
void update_hi_wmark(std::atomic<T>* target_val, boost::type_identity_t<T> val);

/**
 * Executes `store(*target_val, load(fresh_val))`.  Used by the generic stat::stats_reset() algorithm when a more
 * specific resetter, such as for Histogram_counter, is not found via ADL.
 *
 * There is not usually any particular reason to call this directly.
 *
 * @tparam T
 *         Value type.  Must be copy-assignable.
 * @param target_val
 *        See above.
 * @param fresh_val
 *        See above.
 */
template<typename T>
void reset(T* target_val, const T& fresh_val);

/**
 * Executes `target_val->clear()`, where `*target_val` is a Histogram_counter.
 * This overrides the default `reset()` behavior.  Note that `fresh_val` is ignored in this impl.
 *
 * There is not usually any particular reason to call this directly.
 *
 * @param target_val
 *        See above.
 * @param fresh_val
 *        See above.
 */
void reset(Histogram_counter* target_val, const Histogram_counter& fresh_val);

/**
 * Outputs a human-readable representation of all declared stats in `stats` to the given `ostream`.
 * Each stat is printed with name and value; no line-breaks inserted.
 *
 * @see util::stat namespace doc header for background about the pattern and detailed requirements
 *      on `Stat_set`.
 *
 * @tparam Stat_set
 *         See above.
 * @param os
 *        Stream to which to serialize.
 * @param stats
 *        Value to serialize.
 */
template<typename Stat_set>
void stats_to_ostream(std::ostream& os, const Stat_set& stats);

/**
 * Returns the names of all declared stats of the given `Stat_set` type, in declaration order: the same
 * names, in the same order, as in stats_to_ostream() output.  No live `Stat_set` is required: this is
 * reflection off the type's `declare_stats()`; a throwaway `Stat_set` is constructed internally from
 * `ctor_args` (typically none -- default-construction) purely to have something to walk.  Its values are
 * irrelevant.
 *
 * Precipitating use-case: test code enforcing that every stat-field is accounted for (asserted, or deliberately
 * skipped) in a coverage manifest -- so that adding a field fails a test until it is classified; or a
 * monitoring/aggregation layer enumerating the available stats generically.
 *
 * @see util::stat namespace doc header for background about the pattern and detailed requirements
 *      on `Stat_set`.
 *
 * @tparam Stat_set
 *         See above.  Unlike for the `stats_*()` APIs proper, some public constructor is required -- but
 *         only by this function, and any one will do (see `ctor_args`).
 * @tparam Ctor_args
 *         See `ctor_args`.
 * @param ctor_args
 *        Args for constructing the internal throwaway `Stat_set` (values irrelevant); typically none.
 * @return See above.
 */
template<typename Stat_set, typename... Ctor_args>
std::vector<std::string> stats_field_names(Ctor_args&&... ctor_args);

/**
 * With the help of a freshly-initialized `Stat_set`, resets the target `Stat_set` so as to immediately
 * begin a fresh measurement period.  Only those members that were properly `FLOW_UTIL_STAT_DECLARE*()`d
 * shall be affected.
 *
 * Note that `fresh_stats` is not simply used as a source for all individual values in `*target_stats`;
 * nor does `Stat_set` need to be copyable as a whole.  In particular as of this writing:
 *   - Each Stat_type::S_ACCUMULATOR is indeed loaded from `fresh_stats` and stored into `*target_stats`.
 *     Rationale: fresh measurement period => start-over from init stat.
 *     - If it is a Histogram_counter, the right thing will happen: the equivalent of Histogram_counter::clear()
 *       which does not affect bucket structure.
 *   - Each Stat_type::S_GAUGE is untouched.
 *     Rationale: fresh measurement period does not make the last measurement taken invalid.
 *   - Each Stat_type::S_HI_WMARK is loaded from its gauged-member already in `*target_stats` and stored into
 *     the `HI_WMARK`-typed stat member in question.
 *     Rationale: fresh measurement period => the gauged measurement, recorded earlier in this same
 *     stats_reset() call, is the only and therefore highest value seen so far.
 *
 * ### Thread safety versus concurrent stats_reset() call on same target ###
 * Pragmatically speaking: each field in in `*target_stats` is updated, in `FLOW_UTIL_STAT_DECLARE*()` order,
 * in one such pass; some of the fields are copied from `fresh_stats`.  You could draw conclusions from this
 * description.  Or you can try following this ~formal description:
 *   - If the concurrent stats_reset() has a differently-valued `fresh_stats`, then behavior is formally
 *     undefined.  (Really in conceivable non-abusive use cases `fresh_stats` should probably always have the
 *     same value, for a given `Stat_set` type, informally speaking.  It's the fresh-stats template by definition.
 *     If you disregard this for some reason then: In practice, depending on field types, one might end up with a
 *     mix of (1) correctly untouched/updated values (`GAUGE`s/`HI_WMARK`s respectively), (2) some values from
 *     "our" `fresh_stats` (`ACCUMULATOR`s), (3) some value from "their" `fresh_stats` (ditto).)
 *   - If the concurrent stats_reset() has the same-valued `fresh_stats`, then it is safe/OK as of this
 *     writing.
 *
 * @tparam Stat_set
 *         See util::stat namespace doc header for background about the pattern and detailed requirements
 *         on `Stat_set`.
 * @param target_stats
 *        The `Stat_set` to modify (reset).
 * @param fresh_stats
 *        A `Stat_set` whose each declared-stat is equal to what that declared-stat in `*target_stats` equalled
 *        at initial construction thereof.  (This is often `Stat_set{}` but not always; for some `Stat_set`s in
 *        some contexts it is necessary to initialize it via non-default ctor and/or some other steps.)
 */
template<typename Stat_set>
void stats_reset(Stat_set* target_stats, const Stat_set& fresh_stats);

/**
 * Given a source `Stat_set`, makes the target `Stat_set` equal to the former.  Only those members that were
 * properly `FLOW_UTIL_STAT_DECLARE*()`d shall be affected.
 *
 * Note that `src_stats` need not be copyable as a whole.
 *
 * ### Rationale ###
 * This can be useful in a few ways.  In general making a copy of a `Stat_set` is useful, for example
 * as step 1 of stats_aggregate(): one does not want to (e.g.) destructively aggregate into `v[0]`, where
 * `v` is (e.g.) a `vector<Stat_set>`.  Why not use an auto-generated copy-ctor/copy-assignment?  Answer:
 *
 * The precipitating use case was a `Stat_set` containing 1+ `atomic<T>`s.  `atomic<T>` is not copy-constructible
 * or copy-assignable; and each such member has to be explicitly atomic-loaded from the source and then
 * atomic-stored onto the target.  An alternative approach would have been to write a `Copyable_atomic<T>`
 * wrapper around `atomic<T>` and require you, if needing stat-set-copying or anything (like stats_aggregate())
 * that needs it in turn, to declare `Copyable_atomic<T>`s instead of vanilla `atomic<T>`s.
 *
 * @tparam Stat_set
 *         See util::stat namespace doc header for background about the pattern and detailed requirements
 *         on `Stat_set`.
 * @param target_stats
 *        The `Stat_set` to modify (replace).
 * @param src_stats
 *        Source of values to store-to `*target_stats`.
 */
template<typename Stat_set>
void stats_assign(Stat_set* target_stats, const Stat_set& src_stats);

/**
 * Aggregates the values from `src_stats` into `*target_stats`, in-place.  Only those members that were
 * properly `FLOW_UTIL_STAT_DECLARE*()`d shall be affected.  The aggregation rule for each declared stat
 * member is determined by its Stat_type:
 *   - Stat_type::S_ACCUMULATOR, Stat_type::S_GAUGE: `fetch_add()`-style add (essentially `a += b`).
 *     - If it is a Histogram_counter: that reduces to bucket-by-bucket `+=`.
 *   - Stat_type::S_HI_WMARK: take the higher of the two values, via `update_hi_wmark()`.
 *
 * (The above is informal; see each Stat_type's doc header for the formal rules.)
 *
 * @warning Doing (essentially) `*target_val += src_val` for each `ACCUMULATOR` makes perfect sense, but
 *          for `GAUGE` it's a subtle question.  For example: if a `GAUGE` is a temperature, does it
 *          really make sense to add 30 Celsius to 40 Celsius to get an "aggregated" 70 Celsius?  That really
 *          depends on what "aggregate one `Stat_set` onto another" really *means* practically speaking.
 *          To be direct in answering: Primarily it is a stepping stone for the generalized N-`Stat_set`
 *          stats_aggregate().  That one would perform stats_aggregate_one() N times -- and then for the
 *          `GAUGE`s `/=` each one by N, obtaining a mean... which actually *does* make sense.  However pre-applying
 *          something like that here would not make much sense.  So we're going with the pragmatic answer that
 *          works in that context; and also it might (arguably) be useful for some future other operations
 *          on-top-of stats_aggregate_one().
 *
 * @see stats_aggregate() for aggregating N `Stat_set`s into one.
 * @see stats_aggregate_shards() for aggregating N `Stat_set`s into one, when each `Stat_set` represents a
 *      shard.  See util::stat doc header for discussion of the sharding (including thread-local sharding)
 *      approach.
 *
 * @tparam Stat_set
 *         See util::stat namespace doc header for background about the pattern and detailed requirements
 *         on `Stat_set`.
 * @param target_stats
 *        The `Stat_set` to modify (aggregate-into).
 * @param src_stats
 *        Source of values to aggregate-into `*target_stats`.
 */
template<typename Stat_set>
void stats_aggregate_one(Stat_set* target_stats, const Stat_set& src_stats);

/**
 * Aggregates N source `Stat_set`s -- drawn from the iterator range `[src_stats_begin, src_stats_end)` --
 * into `*target_stats`.  Only those members that were properly `FLOW_UTIL_STAT_DECLARE*()`d shall be affected.
 * Equivalent to:
 *   -# `stats_assign(target_stats, *src_stats_begin);`
 *   -# `stats_aggregate_one(target_stats, *it);` for each `it` in `[src_stats_begin + 1, src_stats_end)`.
 *   -# Each Stat_type::S_GAUGE-typed member of `*target_stats` is then divided by `N` (yielding the mean).
 *
 * Informally summarized per Stat_type (see each Stat_type's doc header for the formal rules):
 *   - Stat_type::S_ACCUMULATOR (including Histogram_counter): sum via successive `+=`.
 *   - Stat_type::S_HI_WMARK: max via successive `<`.
 *   - Stat_type::S_GAUGE: arithmetic mean (sum-then-`/= N`; note that for integer types that is
 *     integer division, hence truncation toward zero).
 *
 * `[src_stats_begin, src_stats_end)` must be a non-empty range; else undefined behavior (assertion might trip).
 *
 * @see stats_aggregate_shards() for aggregating N `Stat_set`s into one, when each `Stat_set` represents a
 *      shard.  See util::stat doc header for discussion of the sharding (including thread-local sharding)
 *      approach.
 *
 * @tparam Stat_set
 *         See util::stat namespace doc header for background about the pattern and detailed requirements
 *         on `Stat_set`.
 * @tparam It
 *         An iterator type dereferencing to `Stat_set` (or `const Stat_set`; it is read-only-accessed
 *         regardless), typically a container's `const_iterator` or equivalent.
 * @param target_stats
 *        The `Stat_set` to modify (aggregate-into).
 * @param src_stats_begin
 *        Iterator to the first source `Stat_set`; must not equal `src_stats_end`.
 * @param src_stats_end
 *        One-past-end iterator.
 */
template<typename Stat_set, typename It>
void stats_aggregate(Stat_set* target_stats, const It& src_stats_begin, const It& src_stats_end);

/**
 * Equivalent to stats_aggregate() as-if every stat, regardless of its actual stat::Stat_type, is an
 * ACCUMULATOR; that is simply computes the sum via `+=` or equivalent.
 *
 * ### Rationale / use case ###
 * stats_aggregate() and stats_sum() are identical for Stat_type::S_ACCUMULATOR; so the different behavior
 * is for Stat_type::S_GAUGE and Stat_type::S_HI_WMARK.  Taking these one at a time:
 *
 * A `GAUGE` can be something like a temperature; in which case an "aggregated" `GAUGE` would be a mean
 * (the sum divided by N = range-size).  Instead, though, it could be something where a sum does make sense.
 * For example, say you've got N memory-arenas, and a stat-member `Stat_type::m_alloc_sz`, the currently
 * allocated memory: things that have been allocated and not yet deallocated.  If you'd like aggregation to
 * mean the average per arena, then stats_aggregate() is appropriate.  If you'd like it to mean the total
 * allocated in the system at the moment, a simply sum is instead appropraite; hence stats_sum().
 *
 * A `HI_WMARK` is a trickier proposition.  In any case, however, by definition it tracks another `GAUGE`.
 * So if a mean makes sense for the `GAUGE` (stats_aggregate()), and each `HI_WMARK` truly tracks the
 * highest value so far per `Stat_set`, then the correct aggregated `HI_WMARK` would be simply the max
 * (according to `<`) of the N `HI_WMARK`s (stats_aggregate()).  If the `GAUGE` is something you want
 * summed, like the allocated-size example above, then it's difficult to say what `HI_WMARK` should be.
 * Choosing the max is probably wrong; you'd get the high-looking `GAUGE` and a low-looking per-arena
 * corresponding `HI_WMARK`.  Summing the `HI_WMARK`s is not exactly right, either; as we lack the
 * information about the highest-sum value over the measurement period; summing the `HI_WMARK`s gives
 * the answer as-if all the max `GAUGE` values occurred always at the same time.  Nevertheless this is
 * stats_sum(), so we sum them.
 *
 * @note Your `Stat_set` might lack any `HI_WMARK`s.  Then that dilemma does not matter.
 *
 * ### What if some of my `GAUGE`s are `stats_aggregate()`able while others are `stats_sum()`able? ###
 * There are a few things you can do; it really depends on what exactly is best for you.  Worst-case,
 * you can always stats_sum() and then manually `/=` the mean-appropriate `GAUGE`s.
 * Though, then you've got the `HI_WMARK` dillema still (if applicable).
 *
 * It also perfectly possible to write your own `stats_*()`-like function which would act in the way
 * you'd prefer.  While the available `Stat_type`s are as they are (cannot be simply extended), you
 * can make compile-time or run-time choices like the following.
 *   - Use the type `T* = decltype(target_val)` (in the `Visitor`) combined with `if constexpr()`.
 *   - Possibly even draw inferences from the stat *name* (`String_view name` in the `Visitor`), perhaps
 *     based on some naming convention of your own.
 *
 * @tparam Stat_set
 *         See util::stat namespace doc header for background about the pattern and detailed requirements
 *         on `Stat_set`.  However, in this case, the requirements for the non-`ACCUMULATOR` stat-members
 *         of `Stat_set` shall be as-if they are also `ACCUMULATOR`s.  In short: each must have `+=`;
 *         no `<` or `/=` is required.
 * @tparam It
 *         An iterator type dereferencing to `Stat_set` (or `const Stat_set`; it is read-only-accessed
 *         regardless), typically a container's `const_iterator` or equivalent.
 * @param target_stats
 *        The `Stat_set` to modify (sum-into).
 * @param src_stats_begin
 *        Iterator to the first source `Stat_set`; must not equal `src_stats_end`.
 * @param src_stats_end
 *        One-past-end iterator.
 */
template<typename Stat_set, typename It>
void stats_sum(Stat_set* target_stats, const It& src_stats_begin, const It& src_stats_end);

/**
 * Sharded-stats consumption:
 * Aggregates N source `Stat_set`s, interpreted as shards of the aggregated whole -- drawn from the iterator
 * range `[src_stats_begin, src_stats_end)` -- into `*target_stats`.  For purposes of Stat_type::S_HI_WMARK
 * computation, `target_stats->m_x` for each `HI_WMARK`-typed `m_x`, shall be taken as an input-value (and also
 * as an out-arg); see below for explanation.  In particular, as explained below, `HI_WMARK` values carry a
 * special meaning.
 *
 * `[src_stats_begin, src_stats_end)` may be empty, meaning at the moment no shards are contributing to the
 * reconstructed `Stat_set`, hence the result `*target_stats` should be essentially zero-filled (w/r/t
 * `ACCUMULATOR`s and `GAUGE`s).  In this case you must supply non-null `fresh_stats_from_0_shards`; the pointee shall
 * have the same semantics as `fresh_stats` to stats_reset().  (In other words it is used as the source of
 * zero-values for all `ACCUMULATOR`s and `GAUGE`s.)
 *
 * @note Grounding example: N threads contribute stats such as some gauge
 *       `uint Stat_set::m_gauge` whose value in `*target_stats` is the sum of `m_gauge` across the N `Stat_set`s.
 *       What if there are no threads yet -- N is 0?  The proper value of `target_stats->m_gauge` at this time
 *       is `0`.  (The case where there are N>0 threads, and then they all exit: That may or may not be the same
 *       situation.  Explaining that case is out of scope here, but we implore you to read the TL-sharding
 *       section of the util::stat namespace doc header.)
 *
 * @see To execute a reset of `*target_stats`: Use stats_reset_shard_aggregate().  You'll note its signature
 *      is quite similar to the present function's.
 *
 * @note Tip: `boost::adaptors::transformed` (or older `make_transform_iterator`) can help walk things that aren't
 *       directly storing `Stat_set`s (e.g., util::Thread_local_state_registry::State_per_thread_map to
 *       util::Thread_local_state_registry::while_locked()).
 *       In C++2x there is a built-in equivalent.  Adaptors like `filtered`, `map_keys`, `map_values` tend to
 *       be also very helpful.
 *
 * @note The in-range is walked *exactly once*.   Combined with `transformed` (or equivalent) that fact may be
 *       helpful if one needs to pre-massage each input `Stat_set` just ahead of aggregation.
 *
 * @note Tip: It may be required to walk 2+ ranges.  In C++2x or with Boost.range this can be done easily by
 *       concatenating ranges (`boost::range::join()`).  Before C++2x / without Boost.range: honestly: don't
 *       bother.  Most `range`-less solution involve a temp container of shard copies, which is fine -- but it
 *       would not work for stats_reset_shard_aggregate() (which modifies the shards).
 *
 * ### Semantics ###
 * Cf. stats_aggregate(): that function is a true aggregator, in the sense that `*target_stats` is fully
 * an out-arg; its current stat-relevant contents are overwritten and not themselves taken into account; and
 * in the conceptual sense that each input-`Stat_set` in the range is meaningful in and of itself, so
 * `*target_stats`, upon return, represents an aggregation of the inputs into a recap/summary/combo.
 * Namely: `ACCUMULATOR`s (including `Histogram_counter`s) are simply summed; and (attn!) `GAUGE`s are *averaged*,
 * while each `HI_WMARK` is simply the highest of input `HI_WMARK`s for that respective stat.
 *
 * In our case, by contrast: Each input is a shard that is in itself not necessarily meaningful; while the
 * output combines the shards to come out with a meaningful total state.  Informally speaking, the aggregated
 * result is the goal and the only generally meaningful `Stat_set` for human consumption.
 *
 * In particular, this is essential for thread-local (TL) sharding as described in util::stat doc header,
 * section "Aggregation (with concurrency)."
 *
 * Specifically, then:
 *   - `ACCUMULATOR`: For such a stat, the output is still simply the sum.  E.g.: if in thread T we counted
 *     N widgets, and in thread U M widgets, in total at the moment there are N plus M widgets.
 *     - If it is a Histogram_counter: Same, repeated for each bucket.
 *   - `GAUGE`: (Attn!) The output is the sum (not the mean).  E.g., suppose we're gauging an overall ref-count,
 *     but it can go up or down in multiple threads, each with a `Stat_set` in the input range.  If in thread
 *     T we've seen 5 `++`es and 7 `--`es, in some order; and in thread U it was 13 and 4 of those respectively,
 *     then the input `Stat_set`s will report -2 and 9.  In and of itself these say little about the actual
 *     cross-thread ref-count.  However stats_aggregate_shards() shall sum these, resulting in: 7.
 *     (stats_aggregate() would instead come out with 7/2, or 3; clearly nonsensical.)
 *   - `HI_WMARK`: (Attn!) This one is particularly special in how it is handled.  The actual action is:
 *     (1) take the gauged-stat `GAUGE`'s aggregated value (preceding bullet); (2) update `target_stat->m_x`
 *     via: `update_hi_wmark(&target_stat->m_x, gauged_value)`.  E.g., if the ref-count recorded in
 *     `*target_stats` was 5, then it would now be updated to 7; if it were 8 then left alone.
 *     - Rationale: It is not possible to obtain the actual high-water mark in the sense of being the highest
 *       value for the (e.g.) total ref-count since the last reset/init.  So we do the best we can:
 *       report the highest (e.g.) total ref-count as sampled across all such stats_aggregate_shards() calls
 *       including this one.  If an even-higher (would-be -- but not in fact -- aggregated) value occurred at
 *       some point between such aggregations, we will not capture it.
 *
 * ### Thread safety versus concurrent stats_aggregate_shards() call on same target ###
 * Pragmatically speaking: we mention this, specifically, because we expect it to be a common pattern (when
 * using sharding) for a user API to have, e.g., an internally stored -- but publicly accessible, by
 * `const` ref or by value -- member `m_current_stats` containing the last shard-aggregation result.  This
 * accessor would first, typically, perform a shard-aggregation using
 * `stats_aggregate_shards(&m_current_stats, ...)`.  So can such a user guarantee that this accessor is
 * thread-safe against concurrent calls to itself?  That would be a salient question.
 *
 * @note Why would one would maintain `m_current_stats` instead of simply generating a fresh
 *       stat-set `struct` and returning it by value? There's exactly one reason: Stat_type::S_HI_WMARK
 *       members (if there are 1+ such stats in the `struct`).  With sharding, these only work at
 *       sampling times (see above); and information about previous sampling times is provided to
 *       stats_aggregate_shards() by providing the last-sampled `*target_stats` in `*target_stats`
 *       (or a fresh `struct` the first time).
 *
 * So that's why we ask this question specifically.  What's the answer?  It is: Calling
 * stats_aggregate_shards() on the same `*target_stats` concurrently => undefined behavior.
 * Pragmatically speaking: the sharding is a multi-step procedure, and we don't want to think about
 * how 2+ such procedures running concurrently would interact.  (Cf. stats_reset() or stats_assign(), where
 * it is reasonably easy.)
 *
 * ### What is the arg `fresh_stats_from_0_shards`? ###
 * (This assumes `src_stats_begin == src_stats_end`; otherwise it is unneeded/ignored.)
 *
 * Usually it carries the basic meaning as `fresh_stats` for stats_reset().  There are no shards in this case
 * (or for stats_reset_shard_aggregate() the premise is we're resetting stuff, same implication in this context).
 * Therefore we need to get the zero-value from somewhere (we cannot assume it is `0` or `{}`, as we support
 * custom types and lack this requirement for individual fields).  Usually this is no problem: `Stat_set{}` or
 * some equivalent will give a clean `struct` full of zeroes/equivalents.
 *
 * The exact requirement here, though, is subtly different: the `Stat_set` value to use
 * when *there are no shards to sum*.  For `ACCUMULATOR`s, same.  For `HI_WMARK`s, irrelevant (they are ignored
 * in the `fresh_stats_from_0_shards` context).  For `GAUGE`s, though, in some advanced situations... read on.
 *
 * The subtlety, if there is one applicable, is that for certain advanced tricks, one *may* desire that
 * when a new *shard* is created -- in TL-sharding, the first time a thread desires to update any TL-sharded
 * stat member -- its value is *not* equal to `fresh_stats_from_0_shards`.  (We want to be abundantly clear
 * here: *This is a special trick; normally this will not come to pass.*)  Here we explain by example, which
 * as of this writing is the only one the authors have come across; it is also in some sense the simplest
 * and most concept-illustrative use-case.  `Stat_set` might contain a field -- a `GAUGE` --
 * `uint m_n_shards = 1` with default ctor `Stat_set{}` indeed initializing it to `1`.  No stat-update code
 * ever needs to modify it; any shard will contain `1` forever.  Upon aggregation (stats_aggregate_shards()), the
 * ones get summed: so one can readily see how many shards composed that stat-consumption result, a nice pithy
 * diagnostic.  Kinda cool!  Now: consider the case where there are no shards: `src_stats_begin == src_stats_end`;
 * the function will grab the `GAUGE m_n_shards = 1` from `fresh_stats_from_0_shards`; but that is wrong.
 * Thus the subtlety: `Stat_set{}` (typically used for shard init -- though you're free to use any other technique,
 * this one is just easiest when possible) should have `= 1`; but for aggregation (this function) and the
 * quite-similar reset (stats_reset_shard_aggregate()) it should have `= 0`.  This does not break any other formal
 * rules or invariants, and it'll get the desired result.  To achieve this, either use a 2nd ctor or just assign
 * `= 0` when preparing non-null `*fresh_stats_from_0_shards` (and similarly for stats_reset_shard_aggregate()).
 *
 * @note In that use-case, it is reasonable to also have a `HI_WMARK m_n_shards_hi_wmark = 0`.  This does not
 *       require any further steps for correctness.  The initial HWM value is `0`, which is accurate -- no shards
 *       have yet been aggregated; and from that point on it'll survive as zero until the first aggregation, if any,
 *       with 1+ shards involved.
 *
 * @tparam Stat_set
 *         See util::stat namespace doc header for background about the pattern and detailed requirements
 *         on `Stat_set`.
 * @tparam It
 *         An iterator type dereferencing to `const Stat_set&` with `++` available.
 * @param target_stats
 *        The `Stat_set` to modify (aggregate-into).  `HI_WMARK`s' current values are taken into account
 *        as inputs.  All other current values are disregarded.
 * @param src_stats_begin
 *        Iterator to the first source `const Stat_set`.
 * @param src_stats_end
 *        One-past-end iterator.  May equal `src_stats_begin`; in which case `fresh_stats_from_0_shards` comes
 *        into play.
 * @param fresh_stats_from_0_shards
 *        If `src_stats_begin != src_stats_end`: ignored; suggest passing-in null.
 *        Else: see above.
 */
template<typename Stat_set, typename It>
void stats_aggregate_shards(Stat_set* target_stats, const It& src_stats_begin, const It& src_stats_end,
                            const Stat_set* fresh_stats_from_0_shards);

/**
 * Reset-stats companion to stats_aggregate_shards(); together the two functions provide support for the
 * (TL-)sharded stat-collection pattern.
 *   - Use stats_aggregate_shards() for stat-consumption.  A copy of out-`*target_stats` = the current `Stat_set`.
 *     Keep `*target_stats` for next time.
 *   - Use stats_reset_shard_aggregate() for stat-resets.  Keep `*target_stats` for next time.
 *
 * The signature is very similar to that of stats_aggregate_shards(), but there is a key difference that is
 * not immediately obvious from the signature alone:
 *   - stats_reset_shard_aggregate() can *modify the iterator pointee* shard `Stat_set`s (as well as
 *     modify `*target_stats`).  Namely it zeroes the `ACCUMULATOR`s therein.
 *     - I.e.: `*src_stats_begin` is `Stat_set&`.
 *   - stats_aggregate_shards() does not modify the iterator pointees.
 *     - I.e.: `*src_stats_begin` is `const Stat_set&`.
 *
 * That said, you can treat it as a black box.  Just start with a `*target_stats` equal to `fresh_stats`, and
 * keep the `*target_stats` for all subsequent stats_aggregate_shards() and stats_reset_shard_aggregate() calls.
 *
 * @note It would be a mistake to return `*target_stats` (or a copy) for consumption after this op: its
 *       `ACCUMULATOR`s are, on purpose, not made coherent (see above), so it is not "the current stats" --
 *       it merely carries what the *next* consumption needs.  Reiterating: To emit actual current stats, that (or at
 *       least that part of) stat-consumption must follow stats_aggregate_shards() (*the* stat-consumption API for
 *       TL-sharded stats).
 *
 * ### What is the arg `fresh_stats_from_0_shards`? ###
 * See stats_aggregate_shards().  Other than this arg always being required, the semantics are the same.
 * There is a possible subtlety involved either way, though, so we encourage you to read the eponymous section of
 * the stats_aggregate_shards() doc header.
 *
 * @tparam Stat_set
 *         See stats_aggregate_shards().
 * @tparam It
 *         An iterator type dereferencing to `Stat_set&` with `++` available.
 * @param target_stats
 *        The `Stat_set` to modify (`HI_WMARK`s and `GAUGE`s only).  `HI_WMARK`s' current values are taken into account
 *        as inputs.  All other current values are disregarded.
 * @param src_stats_begin
 *        Iterator to the first source `Stat_set`.  Range elements are potentially modified (`ACCUMULATOR`s only).
 * @param src_stats_end
 *        One-past-end iterator.  May equal `src_stats_begin`.
 * @param fresh_stats_from_0_shards
 *        See above.
 */
template<typename Stat_set, typename It>
void stats_reset_shard_aggregate(Stat_set* target_stats, const It& src_stats_begin, const It& src_stats_end,
                                 const Stat_set& fresh_stats_from_0_shards);

/**
 * Intended for situations where the source of stat readings -- including `ACCUMULATOR`s and not
 * just `GAUGE`s -- is not by continuous sampling but rather by querying a data source (such as a library with its
 * own stats outputs), this obtains a view of `*target_stats` (also placed back into `*target_stats` destructively)
 * relative to a past reference point, `*reset_state`.  This requires methodical explanation.
 *
 * ### The premise: *raw stats* from a data source ###
 * In this mode of working with a `Stat_set`, the basic assumptions are different from the mainstream described
 * in util::stat doc header.  To wit:
 *   - A given Stat_set::S_ACCUMULATOR stat is not manually updated via fetch_add() or equivalents
 *     each time the underlying measured-count increases.  Instead, *at stat consumption time* (when one would
 *     `load()`-or-equivalent ~all stat-members) a *raw reading* of the actual value of the `ACCUMULATOR` at that
 *     time is obtained from some data-source (e.g.: a memory manager's stats-facility, in this case tracking
 *     something accumulator-like, such as the # of `malloc()`s to have been called to date).  One then
 *     directly saves it into the stat-member.
 *     - If it is a Histogram_counter: same (use Histogram_counter::overwrite_count_for_bucket()).
 *   - A given Stat_set::S_GAUGE is handled similarly (but that's normal anyway); a `GAUGE` can be set to anything
 *     anytime unless sharded.
 *   - A Stat_set::S_HI_WMARK is never manually recorded: stats_since_reset_state() will compute it automatically.
 *     By the nature of this mode, however, the `HI_WMARK` is therefore the max of readings taken from the
 *     data-source at stat consumption times.  (In this way it is similar to how
 *     stats_aggregate_shards() + sharding works.)
 *
 * If that were all, there would be no need for any special function like stats_since_reset_state().  Indeed,
 * if one is happy with using `Stat_set` simply to represent the readings from a stats data-source -- then that's
 * it.  E.g., one can use print() for printing them nicely.  However, it is often useful to also be able to
 * perform some equivalent of stats_reset(): Particularly for `ACCUMULATOR`s (including histograms) one might
 * want to know how many widgets have accumulated since a particular point in time one had earlier *reset-marked*.
 * Along the same lines, `HI_WMARK`s would track the time period since that marked point in time.  (`GAUGE`s by
 * definition are independent of their preceding states.)  For this, one uses:
 *   - stats_since_reset_state() (this function) to compute *relative-to-marked-state* accumulated values (by
 *     subtraction) and high-water marks;
 *   - stats_mark_reset_state() to mark the latest/base reset-state;
 *   - and a simple induction-based algorithm we now describe.
 *
 * We reiterate that that if the reset/stats-since-mark functionality is not needed, then the following is
 * irrelevant.
 *
 * ### The algorithm ###
 * The algorithm requires a single persistent `Stat_set`; we call it `*reset_state`.  You'll need to first
 * set it to a *base state*.  Often by nature of the observed data-source values, it is sufficient to zero
 * (or in the case of exotic `HI_WMARK`s, set those to their lowest-possible values).  Sometimes that is not the
 * case for certain `ACCUMULATOR`s; for example, suppose the data-source value V represents some
 * event's count since 1970).  In that case obtain the base state as follows:
 *   - Read (raw readings) from data-source into `*reset_state`.  (Omit `HI_WMARK` members.)
 *   - `stats_mark_reset_state(reset_state)`.  (Sets all `HI_WMARK`s, if any, from their `GAUGE`s.)
 *
 * Now, there are just two possible triggers: stat-consumption and reset-marking.
 *
 * To consume stats:
 *   - Suppose `*target_stats` is where you want the current relative stats.
 *   - Read (raw readings) from data-source into `*target_stats`.  (Omit `HI_WMARK` members.)
 *   - `stats_since_reset_state(target_stats, reset_state)`.
 *     - `*reset_state` might be modified due to `HI_WMARK` mechanics.  You don't care: you care about `*target_stats`.
 *   - `*target_stats` contains the current relative stats which were your object.
 *     - Reminder: `HI_WMARK` = max of the `GAUGE`d stat's values starting with the last
 *       reset-marked (or base) state and compared against any stats_since_reset_state() calls since.
 *       We lack access to the data-source's values at any time points in-between.
 *
 * To reset-mark:
 *   - Read (raw readings) from data-source into `*reset_state`.  (Omit `HI_WMARK` members.)
 *   - `stats_mark_reset_state(reset_state)`.
 *
 * @note Recall that to save (raw readings) directly into a Histogram_counter stat, use
 *       a series of Histogram_counter::overwrite_count_for_bucket().
 *
 * ### Thread safety ###
 * Formally behavior is undefined if `*target_stats` or `*reset_state` is/are modified concurrently to this
 * function.  (fetch_sub(), store(), and load() are still used for/within all stat-members, for
 * conceptual consistency and a bit of perf; but we make no guarantee as to reasonable behavior under
 * concurrent modification.)
 *
 * @tparam Stat_set
 *         See util::stat namespace doc header for background about the pattern and detailed requirements
 *         on `Stat_set`.
 * @param target_stats
 *        In-out arg: As input: Contains raw (straight from data-source) readings for all non-`HI_WMARK` stats
 *        (the latter are disregarded).
 *        Post-condition: Contains stats relative to last reset-marked (or base) state.
 * @param reset_state
 *        The last reset-marked (or base) state.  May be modified (`HI_WMARK`s only).
 */
template<typename Stat_set>
void stats_since_reset_state(Stat_set* target_stats, Stat_set* reset_state);

/**
 * Sets each `HI_WMARK`-declared stat-member in `*target_stats` to its gauged-stat's current value in
 * `*target_stats`; required for stats_since_reset_state() algorithm; may be useful otherwise as well.
 *
 * @see stats_since_reset_state() which forms the basis of an algorithm for using `Stat_set`s to store
 *      values from outside stats data-sources, with available reset states via the present function.
 *
 * @tparam Stat_set
 *         See util::stat namespace doc header for background about the pattern and detailed requirements
 *         on `Stat_set`.
 * @param target_stats
 *        In-out arg: `GAUGE`s are used as input; `HI_WMARK`s are overwritten; all others are ignored.
 */
template<typename Stat_set>
void stats_mark_reset_state(Stat_set* target_stats);

/**
 * Returns a lightweight proxy object that, when streamed via `operator<<`, pretty-prints the given
 * `Stat_set` using stats_to_ostream().  This enables idiomatic chained-`<<` usage:
 *
 *   ~~~
 *   os << "Stats: [" << stat::print(my_stats) << "]; done.\n";
 *   ~~~
 *
 * @tparam Stat_set
 *         See stats_to_ostream() for requirements.
 * @param stats
 *        The stats values to print.  The returned proxy holds a `const` reference; `stats` must remain
 *        alive through the `<<` expression.
 * @return Proxy suitable for `ostream <<`.
 */
template<typename Stat_set>
Stat_set_printable<Stat_set> print(const Stat_set& stats);

/**
 * Prints string representation of the given `printable.m_stats` to the given `ostream` via stats_to_ostream().
 *
 * @tparam Stat_set
 *         See stats_to_ostream() for requirements.
 * @param os
 *        Stream to which to serialize.
 * @param printable
 *        Proxy obtained via print(); contains object to serialize.
 * @return `os`.
 */
template<typename Stat_set>
std::ostream& operator<<(std::ostream& os, const Stat_set_printable<Stat_set>& printable);

/**
 * Prints string representation of the given `Histogram_counter` to the given `ostream`.
 *
 * @relatesalso Histogram_counter
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Histogram_counter& val);

} // namespace flow::util::stat
