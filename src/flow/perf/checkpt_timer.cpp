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
#include "flow/perf/checkpt_timer.hpp"
#include <boost/algorithm/string.hpp>
#include <time.h>
#include <utility>

namespace flow::perf
{

// Checkpointing_timer::Aggregator implementations.

Checkpointing_timer::Checkpointing_timer(log::Logger* logger_ptr,
                                         std::string&& name_moved, Clock_types_subset which_clocks,
                                         size_t max_n_checkpoints) :
  log::Log_context(logger_ptr, Flow_log_component::S_PERF),
  m_name(std::move(name_moved)),
  m_which_clocks(which_clocks)
  // m_start_when and m_last_checkpoint_when are currently garbage.
{
  using flow::util::ostream_op_string;
  using boost::algorithm::join;
  using std::vector;
  using std::string;

  assert(max_n_checkpoints != 0);

  m_checkpoints.reserve(max_n_checkpoints); // Performance -- avoid reallocs.

  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, get_log_component()))
  {
    vector<string> active_clock_type_strs;
    active_clock_type_strs.reserve(m_which_clocks.size()); // Overboard but whatever....
    for (size_t clock_type_idx = 0; clock_type_idx != m_which_clocks.size(); ++clock_type_idx)
    {
      if (m_which_clocks[clock_type_idx])
      {
        active_clock_type_strs.emplace_back(ostream_op_string(Clock_type(clock_type_idx)));
      }
    }

    FLOW_LOG_TRACE_WITHOUT_CHECKING("Timer [" << m_name << "] created; "
                                    "active clock types = [" << join(active_clock_type_strs, " | ") << "].");
  }

  // Start timing at the last moment: as close to the start of the user's measured op as possible.
  m_start_when = m_last_checkpoint_when = now();
}

Checkpointing_timer::Checkpointing_timer(log::Logger* logger_ptr,
                                         std::string&& name_moved, Clock_types_subset which_clocks) :
  // (Reminder: This ctor is used for the Aggregator path (non-public).)  Initialize everything const; nothing else.
  log::Log_context(logger_ptr, Flow_log_component::S_PERF),
  m_name(std::move(name_moved)),
  m_which_clocks(which_clocks)
  // m_start_when and m_last_checkpoint_when are currently garbage.  m_checkpoints is not pre-reserved.
{
  // That's it.
}

Clock_types_subset Checkpointing_timer::real_clock_types() // Static.
{
  Clock_types_subset clocks;
  clocks.set(size_t(Clock_type::S_REAL_HI_RES));
  return clocks;
}

Clock_types_subset Checkpointing_timer::process_cpu_clock_types() // Static.
{
  Clock_types_subset clocks;
  clocks.set(size_t(Clock_type::S_CPU_USER_LO_RES));
  clocks.set(size_t(Clock_type::S_CPU_SYS_LO_RES));
  clocks.set(size_t(Clock_type::S_CPU_TOTAL_HI_RES));
  return clocks;
}

Clock_types_subset Checkpointing_timer::thread_cpu_clock_types() // Static.
{
  Clock_types_subset clocks;
  clocks.set(size_t(Clock_type::S_CPU_THREAD_TOTAL_HI_RES));
  return clocks;
}

void Checkpointing_timer::scale(uint64_t mult_scale, uint64_t div_scale)
{
  assert(m_checkpoints.size() != 0);

  /* Please see doc header for our contract.  Essentially we are to take a completed (1+ checkpoints) *this timer and
   * change it to be as if it started at the recorded time, but each of the checkpoints completed not in recorded time T
   * but time (T * mult_scale / div_scale).  This involves updating the checkpoints themselves; and
   * m_last_checkpoint_when which determines since_start(), the other key bit of user-interest info. */

  /* - Always multiply first; overflow is not happening (64-bit); meanwhile it avoids rounding error to extent possible.
   * - Reminder: all the stuff being modified aren't single values but arrays thereof (element per Clock_type). */

  for (auto& checkpoint : m_checkpoints)
  {
    checkpoint.m_since_last *= mult_scale;
    checkpoint.m_since_last /= div_scale;
  }

  /* Now let's "fix" since_start() accordingly.  Of course since_start() is not an lvalue but equals
   * (m_start_when + m_last_checkpoint_when).  m_start_when is unchanged; so we are to fix m_last_checkpoint_when.
   * An alternative way to think of it is simply that m_last_checkpoint_when == the time point(s) that resulted
   * in m_checkpoints.back().m_since_last; and we had changed all the `m_since_last`s including the final one. */
  auto scaled_since_start = since_start();
  scaled_since_start *= mult_scale;
  scaled_since_start /= div_scale;
  m_last_checkpoint_when = m_start_when;
  m_last_checkpoint_when += scaled_since_start;
  // (Could also get identical result by simply adding up all `checkpoint.m_since_last`s.)
} // Checkpointing_timer::create_scaled()

Time_pt Checkpointing_timer::now(Clock_type clock_type)
{
  using ::timespec;
  using ::clock_gettime;
#if 0 // Not necessary anyway, but also in Linux this is a #define and hence not in a namespace.
  using ::CLOCK_PROCESS_CPUTIME_ID;
#endif
  using boost::chrono::duration;
  using boost::chrono::nanoseconds;

  // Reminder: This call is extremely performance-centric.  Keep it very low-fat.

  switch (clock_type)
  {
    case Clock_type::S_REAL_HI_RES:
      return Fine_clock::now();
    case Clock_type::S_CPU_USER_LO_RES:
      return now_cpu_lo_res(now_cpu_lo_res_raw(), true);
    case Clock_type::S_CPU_SYS_LO_RES:
      return now_cpu_lo_res(now_cpu_lo_res_raw(), false);
    case Clock_type::S_CPU_TOTAL_HI_RES:
    {
      /* @todo This is all fine, but ultimately the right thing to do would be to make a boost.chrono-ish clock
       * that does the following in its now().  In fact there is a boost.chrono Trac ticket to do just that.
       * Then <that clock>::now() would be called here. */
      timespec time_spec;
#ifndef NDEBUG
    const bool ok = 0 ==
#endif
      clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time_spec);
#ifndef NDEBUG
    assert(ok);
#endif
      return Time_pt(nanoseconds(duration_rep_t(time_spec.tv_sec) * duration_rep_t(1000 * 1000 * 1000)
                                   + duration_rep_t(time_spec.tv_nsec)));
    }
    case Clock_type::S_CPU_THREAD_TOTAL_HI_RES:
    {
      // Similar to just above... keeping comments light.
      timespec time_spec;
#ifndef NDEBUG
    const bool ok = 0 ==
#endif
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time_spec);
#ifndef NDEBUG
    assert(ok);
#endif
      return Time_pt(nanoseconds(duration_rep_t(time_spec.tv_sec) * duration_rep_t(1000 * 1000 * 1000)
                                   + duration_rep_t(time_spec.tv_nsec)));
    }
    case Clock_type::S_END_SENTINEL:
      assert(false && "END_SENTINEL passed to now() -- must specify actual Clock_type.");
    // Intentional: no `default:` -- to trigger typical compiler warning if an enum value is forgotten.
  }

  assert(false && "Bug?  now() forgot to handle a Clock_type, yet compiler did not warn in switch()?");
  return Time_pt();
} // Checkpointing_timer::now(Clock_type)

Time_pt_set Checkpointing_timer::now(const Clock_types_subset& which_clocks) // Static.
{
  /* Reminder: This call is extremely performance-centric.  Keep it very low-fat.
   * Get all the clock values that were specified to get.  Others skipped for performance. */

  Time_pt_set time_pt_set; // Uninitialized garbage.
  /* Would be unnecessary except that aggregation elsewhere doesn't worry about m_which_clocks.
   * So just leave unused clock types' values at 0, and then aggregation will just add up a bunch of zeroes,
   * when performance no longer matters.  This fill() should be quite cheap.
   * Also, string/stream output will list 0 on unused clocks instead of printing potential garbage. */
  time_pt_set.m_values.fill(Time_pt());

  const bool do_cpu_user_lo = which_clocks[size_t(Clock_type::S_CPU_USER_LO_RES)];
  const bool do_cpu_sys_lo = which_clocks[size_t(Clock_type::S_CPU_SYS_LO_RES)];
  if (do_cpu_user_lo || do_cpu_sys_lo)
  {
    const auto cpu_combo_now_raw = now_cpu_lo_res_raw();
    if (do_cpu_user_lo)
    {
      time_pt_set.m_values[size_t(Clock_type::S_CPU_USER_LO_RES)] = now_cpu_lo_res(cpu_combo_now_raw, true);
      if (do_cpu_sys_lo) // Doing this within the `if (){}` is a cheesy optimization... we're trying hard here.
      {
        time_pt_set.m_values[size_t(Clock_type::S_CPU_SYS_LO_RES)] = now_cpu_lo_res(cpu_combo_now_raw, false);
      }
    }
    else // Continue the cheesy optimization started just above.
    {
      time_pt_set.m_values[size_t(Clock_type::S_CPU_SYS_LO_RES)] = now_cpu_lo_res(cpu_combo_now_raw, false);
    }
  }

  if (which_clocks[size_t(Clock_type::S_CPU_TOTAL_HI_RES)])
  {
    time_pt_set.m_values[size_t(Clock_type::S_CPU_TOTAL_HI_RES)]
      = Checkpointing_timer::now(Clock_type::S_CPU_TOTAL_HI_RES);
  }

  if (which_clocks[size_t(Clock_type::S_CPU_THREAD_TOTAL_HI_RES)])
  {
    time_pt_set.m_values[size_t(Clock_type::S_CPU_THREAD_TOTAL_HI_RES)]
      = Checkpointing_timer::now(Clock_type::S_CPU_THREAD_TOTAL_HI_RES);
  }

  if (which_clocks[size_t(Clock_type::S_REAL_HI_RES)])
  {
    time_pt_set.m_values[size_t(Clock_type::S_REAL_HI_RES)]
      = Checkpointing_timer::now(Clock_type::S_REAL_HI_RES);
  }

  return time_pt_set;
} // Checkpointing_timer::now(Clock_types_subset)

Time_pt_set Checkpointing_timer::now() const
{
  return now(m_which_clocks);
}

Checkpointing_timer::Cpu_split_clock_durs_since_epoch Checkpointing_timer::now_cpu_lo_res_raw() // Static.
{
  // This is a struct of raw integers, each of which is a tick count since some epoch ref pt.
  return Cpu_split_clock::now().time_since_epoch().count();
}

Time_pt Checkpointing_timer::now_cpu_lo_res(const Cpu_split_clock_durs_since_epoch& cpu_combo_now_raw,
                                            bool user_else_sys) // Static.
{
  using boost::chrono::nanoseconds;

  /* "Rebuild" the duration type of each *individual* component of the split-by-time-type struct stored by the
   * Cpu_split_clock's unusual duration type.  So the latter is like duration<::times, N>, where N is likely
   * nano but could be something less precise; and the following is therefore like duration<uint64, N>.
   * The key is that N needs to be the same; otherwise the below construction of a final Duration will use
   * the wrong unit.  (This is inside-baseball of this somewhat obscure boost.chrono class Cpu_split_clock.) */
  using Cpu_split_component_duration = boost::chrono::duration<nanoseconds::rep, Cpu_split_clock::period>;

  /* cpu_combo_now_raw is a struct of raw integers, each of which is a tick count for Cpu_split_component_duration,
   * hence we can construct Cpu_split_component_duration from each.  Probably type of each of the following is
   * nanoseconds; but anything with that resolution or worse will convert OK.  Otherwise it wouldn't compile below
   * when converting to Duration in the construction of Time_pt. */
  return Time_pt(Duration(Cpu_split_component_duration(user_else_sys ? cpu_combo_now_raw.user
                                                                     : cpu_combo_now_raw.system)));
} // Checkpointing_timer::now_cpu_lo_res()

const Checkpointing_timer::Checkpoint& Checkpointing_timer::checkpoint(std::string&& name_moved)
{
  // Reminder: This call is extremely performance-centric.  Keep it very low-fat.  Obviously that includes now() call:

  // Get the time ASAP, being as close to the potential end of measured user operation as possible.
  const auto checkpoint_when = now();

  // We don't want to allocate after initial reserve().  Ctor doc header (as of this writing) says undefined behavior.
  assert((m_checkpoints.size() + 1) <= m_checkpoints.capacity());
  // Save the new checkpoint.
  m_checkpoints.push_back({ std::move(name_moved),
                            checkpoint_when - m_last_checkpoint_when });
  // Update invariant.
  m_last_checkpoint_when = checkpoint_when;

  FLOW_LOG_TRACE("Timer [" << m_name << "]: Added checkpoint # [" << (m_checkpoints.size() - 1) << "]: "
                 "[" << m_checkpoints.back() << "]; total elapsed [" << since_start() << "].");

  return m_checkpoints.back();
}

Duration_set Checkpointing_timer::since_start() const
{
  assert(!m_checkpoints.empty());
  return m_last_checkpoint_when - m_start_when; // This is why m_last_checkpoint_when exists.
}

const std::vector<Checkpointing_timer::Checkpoint>& Checkpointing_timer::checkpoints() const
{
  return m_checkpoints;
}

void Checkpointing_timer::output(std::ostream* os) const
{
  // We promised to have multi-lines but no trailing newline.  Basically assume we start on column 0.
  assert(!m_checkpoints.empty());

  (*os)
    <<
    "--- Timer [" << m_name << "] ----\n"
    "Checkpoints (count = [" << m_checkpoints.size() << "]):\n";

  size_t idx = 0;
  for (const auto& checkpoint : m_checkpoints)
  {
    (*os) << "  [#" << idx << "| " << checkpoint << ']';
    if ((idx + 1) != m_checkpoints.size())
    {
      (*os) << '\n'; // Else no newline as promised.
    }
    ++idx;
  }

  if (m_checkpoints.size() == 1)
  {
    return; // No point, as total = the one checkpoint (a fairly common use case).
  }
  // else

  (*os) << "\n"
           "Total duration: [" << since_start() << "]"; // No newline as promised.
}

std::ostream& operator<<(std::ostream& os, const Checkpointing_timer::Checkpoint& checkpoint)
{
  return os << checkpoint.m_name << "<-[" << checkpoint.m_since_last << ']';
}

std::ostream& operator<<(std::ostream& os, const Checkpointing_timer& timer)
{
  timer.output(&os);
  return os;
}

// Checkpointing_timer::Aggregator implementations.

Checkpointing_timer::Aggregator::Aggregator(flow::log::Logger* logger_ptr,
                                            std::string&& name_moved, size_t max_n_samples) :
  log::Log_context(logger_ptr, Flow_log_component::S_PERF),
  m_name(std::move(name_moved))
{
  assert(max_n_samples != 0);
  m_timers.reserve(max_n_samples); // Performance -- avoid reallocs.
}

void Checkpointing_timer::Aggregator::aggregate(Checkpointing_timer_ptr timer_ptr)
{
  assert((m_timers.size() + 1) <= m_timers.capacity()); // Similarly to Checkpointing_timer::checkpoint()....
  m_timers.emplace_back(timer_ptr); // Watch out for concurrency!

  FLOW_LOG_TRACE("Aggregator [" << m_name << "]: Added Timer # [" << (m_timers.size() - 1) << "] "
                 "named [" << timer_ptr->m_name << "].");
}

void Checkpointing_timer::Aggregator::clear()
{
  m_timers.clear(); // Again, watch out for concurrency.
}

Checkpointing_timer_ptr Checkpointing_timer::Aggregator::create_aggregated_result
                          (log::Logger* alternate_logger_ptr_or_null, bool info_log_result,
                           uint64_t mean_scale_or_zero) const
{
  using std::string;
  using std::copy;

  assert(!m_timers.empty());
  auto model_timer = m_timers.front();

  // Construct the thing minimally and fill out the rest of the fields as the public ctor would, with special values.
  Checkpointing_timer_ptr agg_timer(new Checkpointing_timer(get_logger(), string(m_name), model_timer->m_which_clocks));
  agg_timer->m_checkpoints.reserve(model_timer->m_checkpoints.size());
  // m_start_when, m_last_checkpoint_when, and m_checkpoints itself are empty; we set them below.

  bool first_dur = true;
  Duration_set total_dur; // Uninitialized!

  // Add it all up, making sure each timer's sequence of checkpoints is the same (names, etc.).
  for (const auto& timer : m_timers)
  {
    if (timer != model_timer) // Sanity-check as noted -- first on the timer at large, below on checkpoints.
    {
      assert(timer->m_which_clocks == model_timer->m_which_clocks);
      assert(timer->m_checkpoints.size() == model_timer->m_checkpoints.size());
    }

    size_t idx_checkpoints = 0; // So we can walk along model_timer->m_checkpoints in parallel.
    for (const auto& checkpoint : timer->m_checkpoints)
    {
      if (timer == model_timer)
      {
        // First timer being examined.  Start up this *result* checkpt to equal it.  Later, we will += to it.
        agg_timer->m_checkpoints.emplace_back(checkpoint);
      }
      else // if (timer != model_timer)
      {
        // Sanity-check the checkpoints line up as noted.
#ifndef NDEBUG
        const auto& model_checkpoint = model_timer->m_checkpoints[idx_checkpoints];
        assert(checkpoint.m_name == model_checkpoint.m_name);
#endif
        // Name is all set (and is consistent); aggregate in the durations from the agged timer's checkpoint.
        agg_timer->m_checkpoints[idx_checkpoints].m_since_last += checkpoint.m_since_last;
      }

      // Add to or start the uber-sum.
      if (first_dur)
      {
        first_dur = false;
        total_dur = checkpoint.m_since_last; // Uninitialized right now => assign.
      }
      else
      {
        // Initialized: sum.
        total_dur += checkpoint.m_since_last;
      }

      ++idx_checkpoints;
    }
  } // for (timer : m_timers)

  /* Note it's currently an uninitialized array; set it to 0s.  Then the below calls will yield appropriate
   * agg_timer->since_start() result.  This is a bit of a hack, perhaps, but since_start() is not an lvalue, and this
   * does work given that fact. */
  agg_timer->m_start_when.m_values.fill(Time_pt());
  agg_timer->m_last_checkpoint_when = agg_timer->m_start_when;
  agg_timer->m_last_checkpoint_when += total_dur;
  // This is what we really wanted, but since_start() isn't an lvalue hence the above hackery.
  assert(agg_timer->since_start().m_values == total_dur.m_values);

  const auto n_samples = m_timers.size();
  const bool mean_aggregation = mean_scale_or_zero != 0;

  if (mean_aggregation)
  {
    FLOW_LOG_TRACE("Returning aggregated-mean result timer [" << m_name << "] "
                   "with [" << n_samples << "] samples "
                   "scaled by [" << mean_scale_or_zero << "] samples.");
    agg_timer->scale(mean_scale_or_zero, n_samples);
  }
  else
  {
    FLOW_LOG_TRACE("Returning aggregated-sum result timer [" << m_name << "] "
                   "with [" << n_samples << "] samples.");
  }

  /* This is typically the "official" way to log something user-friendly.
   * (Sometimes they may have prepared their own agg_timer; in which case they can call
   * log_aggregated_result_in_timer() [which is static] directly.  See Checkpointing_timer doc header. */
  if (info_log_result)
  {
    log_aggregated_result_in_timer(alternate_logger_ptr_or_null ? alternate_logger_ptr_or_null : get_logger(),
                                   *agg_timer, n_samples, mean_scale_or_zero);
  }

  return agg_timer;
} // Checkpointing_timer::Aggregator::create_aggregated_result()

void Checkpointing_timer::Aggregator::log_aggregated_result_in_timer
       (log::Logger* logger_ptr,
        const Checkpointing_timer& agg_timer, unsigned int n_samples,
        uint64_t mean_scale_or_zero) // Static.
{
  using std::array;
  using flow::log::Logger;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_PERF);
  const bool mean_aggregation = mean_scale_or_zero != 0;

  // Show one type (numerically speaking) of view into the aggregated results.
  if (!mean_aggregation)
  {
    FLOW_LOG_INFO("Aggregator [" << agg_timer.m_name << "]: "
                  "Collated timing result from [" << n_samples << "] samples: "
                  "SUM: [\n" << agg_timer << "\n].");
  }
  else // if (mean_aggregation)
  {
    if (mean_scale_or_zero == 1)
    {
      FLOW_LOG_INFO("Aggregator [" << agg_timer.m_name << "]: "
                    "Collated timing result from [" << n_samples << "] samples: "
                    "MEAN: [\n" << agg_timer << "\n].");
    }
    else // if (mean_scale_or_zero != 1)
    {
      assert(mean_scale_or_zero != 1);
      FLOW_LOG_INFO("Aggregator [" << agg_timer.m_name << "]: "
                    "Collated timing result from [" << n_samples << "] samples: "
                    "SCALED-MEAN per [" << mean_scale_or_zero << "] samples: [\n" << agg_timer << "\n].");
    }
  }
} // Checkpointing_timer::Aggregator::log_aggregated_result_in_timer()

void Checkpointing_timer::Aggregator::log_aggregated_results(log::Logger* alternate_logger_ptr_or_null,
                                                             bool show_sum, bool show_mean,
                                                             uint64_t mean_scale_or_zero) const
{
  FLOW_LOG_INFO("Aggregation completed earlier.  Collating aggregated timing data; logging here/elsewhere:");

  // Show each type (numerically speaking) of view into the aggregated results.

  if ((mean_scale_or_zero == 1) && show_mean)
  {
    /* If we're going to show MEAN anyway, and SCALED-MEAN's specified scale is the denegerate 1, then
     * pretend they didn't enabled SCALED-MEAN output in the first place.  It would show the same numbers anyway --
     * twice; so just do it once. */
    mean_scale_or_zero = 0;
  }

  // First `true` triggers actual logging; beyond that we ignore returned timer.
  if (show_sum)
  {
    create_aggregated_result(alternate_logger_ptr_or_null, true, 0); // Log sum.
  }
  if (show_mean)
  {
    create_aggregated_result(alternate_logger_ptr_or_null, true, 1); // Ditto for simple mean.
  }
  if (mean_scale_or_zero != 0)
  {
    create_aggregated_result(alternate_logger_ptr_or_null, true, mean_scale_or_zero); // Ditto for scaled mean.
  }
} // Checkpointing_timer::Aggregator::log_aggregated_results()

} // namespace flow::perf
