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
#include "flow/log/config.hpp"
#include <boost/algorithm/string.hpp>

namespace flow::log
{
// Static initializations.

// By definition INFO is the most-verbose severity meant to avoid affecting performance.
const Sev Config::S_MOST_VERBOSE_SEV_DEFAULT = Sev::S_INFO;

// Implementations.

Config::Config(Sev most_verbose_sev_default) :
  m_use_human_friendly_time_stamps(true),
  m_verbosity_default(raw_sev_t(most_verbose_sev_default))
{
  // Nothing.
}

Config::Config(const Config&) = default;

Config::component_union_idx_t Config::component_to_union_idx(const Component& component) const
{
  const auto component_cfg_it = m_component_cfgs_by_payload_type.find(component.payload_type_index());
  // BTW, as of this writing, that asserts !component.empty() (non-null Component), as advertised (undefined behavior).

  if (component_cfg_it == m_component_cfgs_by_payload_type.end())
  {
    return component_union_idx_t(-1); // As advertised, this is an allowed eventuality.
  }
  // else

  // Note: Never -1 (both are non-negative).
  return component_cfg_it->second.m_enum_to_num_offset + component.payload_enum_raw_value();
}

void Config::store_severity_by_component(component_union_idx_t component_union_idx, raw_sev_t most_verbose_sev_or_none)
{
  using std::memory_order_relaxed;

  /* This is explicitly disallowed in our contract.  See doc header for m_verbosities_by_component for brief
   * discussion of why this is disallowed, but it's allowed in output_whether_should_log(). */
  assert(component_union_idx != component_union_idx_t(-1));
  assert(component_union_idx < m_verbosities_by_component.size());

  /* Store the severity, encoded as a byte for compactness and such.  Please read doc header for
   * Component_union_idx_to_sev_map at this point then come back here.
   * As noted there, the lookup by index is safe against concurrent output_whether_should_log() doing the same,
   * because by the time either we or output_whether_should_log() are called, init_component_to_union_idx_mapping()
   * calls are required to have been all completed for perpetuity.
   * That brings us to the actual assignment to the atomic.  Firstly, we could have used just a regular-looking
   * =assignment.  However that actually means basically .store(..., memory_order_seq_cst) which has spectacular
   * thread safety (total coherence across cores, and of course atomicity) -- but it is expensive.  Granted, we don't
   * care about *our* performance that much, but we highly care about output_whether_should_log()'s; and unless that
   * one also uses memory_order_seq_cst (via = or otherwise) we don't really reap the benefit.  So, we use
   * memory_order_relaxed in both places.  Its performance is the highest possible for an atomic<>, which in fact for
   * x86 means it is no worse than an assignment of a *non*-atomic (in fact the more stringent memory_order_release
   * would, on x86, been perf-equivalent to a non-atomic assignment -- and this is faster or the same by definition).
   * Basically memory_order_relaxed guarantees atomicity (so it is not corrupted, formally) and not much more.
   * So the only question remains: is the multi-thread behavior of this acceptable for our purposes?
   *
   * To tackle this, consider the actual scenario.  What's at stake?  Simple: The Sev is set to X before our call
   * and Y after; the change from X to Y would cause message M to pass the output_whether_should_log() filter
   * before but not after the change, or vice versa.  So by executing this store() op, do we get this desired effect?
   * Consider that we are called in thread 1, and there are other threads; take thread 2 as an example.
   *   - Thread 1: If M is logged in this thread, then either that occurs strictly before this call or strictly after.
   *     Even a regular assignment, no atomics involved, works in that scenario.  So this store() certainly does also.
   *     No problem.
   *   - Thread 2: If M is logged in thread 2, then there is potential controversy.  The store() is atomic, but there
   *     is no guarantee that it will reflect immediately (for some definition of immediately) in thread 2; thread 2
   *     could have loaded X into a register already and may use it strictly after in memory it has changed to Y,
   *     so to thread 2's C++ code the change to Y is not "visible" yet.  However, this does not matter in practice.
   *     It just means it will act as if M were logged a split second earlier and thus with verbosity X and not Y
   *     anyway.  Very shortly the side effect of the assignment will propagate to be visible on every core, registers
   *     and caching notwithstanding.  Now thread 2 will operate according to severity Y.  No problem.
   *
   * As you can see in atomic<> docs, memory_order_relaxed guarantees atomicity and read-write coherence (a/k/a sanity)
   * but performs no synchronization, meaning other memory writes that happen before may be reordered to after and
   * vice versa; but we absolutely do not care about that.  Even just conceivably we could only care about other
   * components' severities, and we make no promise that output_whether_should_log() will expect to work with some sort
   * of batch-atomic multi-component verbosity update.  So we are fine.
   *
   * What about `configure_component_verbosity*()` in thread 1 vs. same method in thread 2, concurrently?
   * Well, that's just a race.  Both are atomic, so either one or the other "wins" and will be propagated to be
   * visible by all threads soon enough.  I add that we explicitly say concurrent such calls are allowed and are safe,
   * but in practical terms it's ill-advised to have your program change config concurrently from multiple threads;
   * there is unlikely to be a good design that would do this. */
  m_verbosities_by_component[size_t(component_union_idx)].store(most_verbose_sev_or_none, memory_order_relaxed);
} // Config::store_severity_by_component()

void Config::configure_default_verbosity(Sev most_verbose_sev, bool reset)
{
  using std::memory_order_relaxed;

  // Straightforward (except see discussion on atomic assignment in store_severity_by_component(); applies here).
  m_verbosity_default.store(raw_sev_t(most_verbose_sev), memory_order_relaxed);

  if (reset)
  {
    for (component_union_idx_t component_union_idx = 0;
         size_t(component_union_idx) != m_verbosities_by_component.size();
         ++component_union_idx)
    {
      store_severity_by_component(component_union_idx, raw_sev_t(-1));
    }
  }
}

bool Config::configure_component_verbosity_by_name(Sev most_verbose_sev,
                                                   util::String_view component_name_unnormalized)
{
  const auto component_name(normalized_component_name(component_name_unnormalized));
  const auto component_union_idx_it = m_component_union_idxs_by_name.find(component_name);
  if (component_union_idx_it == m_component_union_idxs_by_name.end())
  {
    return false;
  }
  // else

  const component_union_idx_t component_union_idx = component_union_idx_it->second;
  assert(component_union_idx != component_union_idx_t(-1)); // We don't save `-1`s.  This is our bug if it happens.

  store_severity_by_component(component_union_idx, raw_sev_t(most_verbose_sev));
  return true;
} // Config::configure_component_verbosity_by_name()

bool Config::output_component_to_ostream(std::ostream* os_ptr, const Component& component) const
{
  assert(os_ptr);
  auto& os = *os_ptr;

  if (component.empty()) // Unspecified component at log call site (or wherever).
  {
    // Same as if unregistered component: do not show one.
    return false;
  }

  const auto component_union_idx = component_to_union_idx(component);
  if (component_union_idx == component_union_idx_t(-1))
  {
    return false; // Unregistered component.payload_type().
  }
  // else

  const auto component_name_it = m_component_names_by_union_idx.find(component_union_idx);
  if (component_name_it == m_component_names_by_union_idx.end())
  {
    os << component_union_idx; // Unknown name; or intentionally not saved, b/c they preferred numeric output.
  }
  else
  {
    auto& name = component_name_it->second;
    assert(!name.empty());
    os << name;
  }
  return true;
} // Config::output_component_to_ostream()

bool Config::output_whether_should_log(Sev sev, const Component& component) const
{
  using std::memory_order_relaxed;

  // See doc header for contract please; then come back here.

  // Check possible thread-local override in which case it's a very quick filter check.
  const auto sev_override = *(this_thread_verbosity_override());
  if (sev_override != Sev::S_END_SENTINEL)
  {
    return sev <= sev_override;
  }
  // else: Must actually check the config in `*this`.  Concurrency is a factor from this point forward.

  // See doc header for Component_union_idx_to_sev_map; then come back here.

  if (!component.empty())
  {
    const auto component_union_idx = component_to_union_idx(component);
    if (component_union_idx != component_union_idx_t(-1))
    {
      /* There are a few things going on here.
       *   - If m_verbosities_by_component[component_union_idx] == -1, then by m_verbosities_by_component semantics
       *     (see its doc header) that means it's not a key in the conceptual map, so no severity configured for
       *     component: fall through to check of m_verbosity_default.
       *   - If component_union_idx is out of bounds, we promised in our contract and in doc header for
       *     m_verbosities_by_component to treat it the same (in this context) as if it were -1 (not in the "map").
       *     The rationale for not crashing instead is briefly discussed in m_verbosities_by_component doc header,
       *     including why in store_severity_by_component() it is disallowed (crashes).
       *   - If m_verbosities_by_component[component_union_idx] != -1, then yay, that's the severity configured for
       *     component.  Read it from the atomic with memory_order_relaxed; the full reasoning is discussed in the
       *     write site, store_severity_by_component(); we just follow that strategy here. */

      const auto most_verbose_sev_raw
        = (component_union_idx < m_verbosities_by_component.size())
            ? m_verbosities_by_component[component_union_idx].load(memory_order_relaxed) // Might be -1 still.
            : raw_sev_t(-1);

      if (most_verbose_sev_raw != raw_sev_t(-1))
      {
        return sev <= Sev(most_verbose_sev_raw);
      }
      // else { Fall through.  Out of bounds, or -1. }
    }
    // else { component.payload()'s enum wasn't registered.  Fall through. }
  }
  // else if (component.empty()) { No component, so cannot look it up.  Fall through. }

  /* Fell through here: no component specified; or enum wasn't registered, so none
   * of the values therein is in the verbosity table; or the particular enum value doesn't have an individual
   * verbosty in the table.  In all cases the default catch-all verbosity therefore applies. */

  // Use the same atomic read technique as in the verbosity table for the identical reasons.
  return sev <= Sev(m_verbosity_default.load(memory_order_relaxed));
} // Config::output_whether_should_log()

std::string Config::normalized_component_name(util::String_view name) // Static.
{
  using boost::algorithm::to_upper_copy;
  using std::locale;
  using std::string;

  return to_upper_copy(string(name), locale::classic());
}

void Config::normalize_component_name(std::string* name) // Static.
{
  using boost::algorithm::to_upper;
  using std::locale;

  to_upper(*name, locale::classic());
}

Sev* Config::this_thread_verbosity_override() // Static.
{
  thread_local Sev verbosity_override = Sev::S_END_SENTINEL; // Initialized 1st time through this line in this thread.
  return &verbosity_override;

  /* Also considered using boost::thread_specific_ptr<Sev> and/or making it a class-static member/file-static variable.
   * But this seems like it has the highest chance to be optimized to the max, being as local as possible and skipping
   * any library (like Boost) that could add overhead with user-space associative lookups.  Details omitted. */
}

util::Scoped_setter<Sev> Config::this_thread_verbosity_override_auto(Sev most_verbose_sev_or_none) // Static.
{
  /* Return this RAII thingie that'll immediately set *P and in dtor restore *P, where P is the pointer to thread-local
   * storage as returned by this_thread_verbosity_override(). */
  return util::Scoped_setter<Sev>(this_thread_verbosity_override(), std::move(most_verbose_sev_or_none));
}

Config::Atomic_raw_sev::Atomic_raw_sev(raw_sev_t init_val) :
  std::atomic<raw_sev_t>(init_val)
{
  // Nothing more.
}

Config::Atomic_raw_sev::Atomic_raw_sev(const Atomic_raw_sev& src) :
  std::atomic<raw_sev_t>(src.load(std::memory_order_relaxed))
{
  /* Note the way we implement the copy construction via load(relaxed); this is why surprising stuff can technically
   * happen if src's contents haven't propagated to this thread yet.  As of this writing the 2 call sites are
   * in the Config::Config(Config)=default (copy ctor); and in init_component_to_union_idx_mapping() when the resize()
   * makes copies of a default-constructed Atomic_raw_sev(-1).  In the latter scenario we're guaranteed to call
   * this in the same thread as the default-construction.  In the former scenario we warn the user in the doc header
   * about the possibility.  If the call sites change after this writing, the spirit should stand fast, I think. */
}

} // namespace flow::log
