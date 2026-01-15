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
#include "flow/async/concurrent_task_loop.hpp"
#include "flow/error/error.hpp"
#ifdef FLOW_OS_MAC
#  if 0 // That code is disabled at the moment (see below).
#    include <mach/thread_policy.h>
#    include <mach/thread_act.h>
#  endif
#endif

namespace flow::async
{

// Method implementations.

Concurrent_task_loop::~Concurrent_task_loop() = default; // Virtual.

// Free function implementations.

unsigned int optimal_worker_thread_count_per_pool(log::Logger* logger_ptr,
                                                  bool est_hw_core_sharing_helps_algo)
{
  using util::Thread;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_ASYNC);

  const unsigned int n_phys_cores = Thread::physical_concurrency();
  const unsigned int n_logic_cores = Thread::hardware_concurrency();
  const bool core_sharing_supported = n_phys_cores != n_logic_cores;

  FLOW_LOG_INFO("System reports processor with [" << n_phys_cores << "] physical cores; and "
                "[" << n_logic_cores << "] hardware threads a/k/a logical cores; core sharing (a/k/a "
                "hyper-threading) is "
                "thus [" << (core_sharing_supported ? "supported" : "unsupported") << "].");

  if (!core_sharing_supported)
  {
    FLOW_LOG_INFO("Core sharing is [unsupported].  "
                  "Therefore suggested thread pool thread count is "
                  "simply the logical core count = [" << n_logic_cores << "].");
    return n_logic_cores;
  }
  // else if (n_phys_cores != n_logic_cores)
  if (est_hw_core_sharing_helps_algo)
  {
    FLOW_LOG_INFO("Application estimates this thread pool DOES benefit from 2+ hardware threads sharing physical "
                  "processor core (a/k/a hyper-threading); therefore we shall act as if there is 1 hardware thread "
                  "a/k/a logical core per physical core, even though in reality above shows it is [supported].  "
                  "Therefore suggested thread pool thread count is "
                  "simply the logical core count = [" << n_logic_cores << "].");
    return n_logic_cores;
  }
  // else

  FLOW_LOG_INFO("Application estimates this thread pool does NOT benefit from 2+ hardware threads sharing physical "
                "processor core (a/k/a hyper-threading); "
                "therefore suggested thread pool thread count is "
                "simply the physical core count = [" << n_phys_cores << "].");

  return n_phys_cores;
} // optimal_worker_thread_count_per_pool()

void optimize_pinning_in_thread_pool(log::Logger* logger_ptr,
                                     const std::vector<util::Thread*>& threads_in_pool,
                                     bool est_hw_core_sharing_helps_algo,
                                     bool est_hw_core_pinning_helps_algo,
                                     bool hw_threads_is_grouping_collated,
                                     Error_code* err_code)
{
  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code)
           { optimize_pinning_in_thread_pool(logger_ptr, threads_in_pool, est_hw_core_sharing_helps_algo,
                                             est_hw_core_pinning_helps_algo, hw_threads_is_grouping_collated,
                                             actual_err_code); },
         err_code, "flow::async::optimize_pinning_in_thread_pool()"))
  {
    return;
  }
  // else if (err_code):
  err_code->clear();

  /* There are 2 ways (known to us) to set thread-core affinity.  In reality they are mutually exclusive (one is Mac,
   * other is Linux), but conceptually they could co-exist.  With the latter in mind, note the subtlety that we choose
   * the Linux way over the Mac way, had they both been available.  The Mac way doesn't rely on specifying a hardware
   * thread index, hence it needs to make no assumptions about the semantics of which threads share which cores, and
   * for this and related reasons it's actually superior to the Linux way.  The reason we choose the inferior Linux
   * way in that case is "thin," but it's this: per
   * https://developer.apple.com/library/archive/releasenotes/Performance/RN-AffinityAPI
   * the affinity tags are not shared between separate processes (except via fork() after the first affinity API call,
   * which we probably could do if it came down to it, but it almost certainly won't).  So in the rare case where
   * it'd help performance that a "producer thread" is pinned to the same hardware core as a "consumer thread," the
   * Linux way lets one easily do this, whereas the Mac way doesn't (except via the fork() thing).  In our case,
   * the pinning is about avoiding the NEGATIVE implications of core sharing, but there could be POSITIVE
   * implications in some cases.  So in that case it's nice to pin those to the same
   * core which will indeed occur in the Linux algorithm below. */

  static_assert(FLOW_ASYNC_HW_THREAD_AFFINITY_PTHREAD_VIA_CORE_IDX || FLOW_ASYNC_HW_THREAD_AFFINITY_MACH_VIA_POLICY_TAG,
                "We only know how to deal with thread-core affinities in Darwin/Mac and Linux.");

  using boost::system::system_category;
  using std::runtime_error;
  using util::ostream_op_string;
  using util::Thread;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_ASYNC);

  if (!est_hw_core_pinning_helps_algo)
  {
    FLOW_LOG_INFO("Application estimates the logic in this thread pool would not benefit from pinning threads to "
                  "processor cores; therefore letting system control assignment of threads to processor cores.");
    return;
  }
  // else
  assert(est_hw_core_pinning_helps_algo);

  // This mode only works if we started in this mode earlier when determing # of threads in pool.  @todo assert()?
  const auto n_pool_threads = threads_in_pool.size();
  assert(n_pool_threads == optimal_worker_thread_count_per_pool(get_logger(), est_hw_core_sharing_helps_algo));

  const auto n_logic_cores_per_pool_thread = Thread::hardware_concurrency() / n_pool_threads;

  FLOW_LOG_INFO("Application estimates thread pool would benefit from pinning threads to processor cores; "
                "will set affinities as follows below.  "
                "Thread count in pool is [" << n_pool_threads << "]; "
                "at [" << n_logic_cores_per_pool_thread << "] logical processor cores each.");

  for (unsigned int thread_idx = 0; thread_idx != n_pool_threads; ++thread_idx)
  {
    Thread* thread = threads_in_pool[thread_idx];
    const auto native_pthread_thread_id = thread->native_handle();

#if FLOW_ASYNC_HW_THREAD_AFFINITY_PTHREAD_VIA_CORE_IDX
    using ::cpu_set_t;
    using ::pthread_setaffinity_np;

    cpu_set_t cpu_set_for_thread;
    CPU_ZERO(&cpu_set_for_thread);

    for (unsigned int logical_core_idx_given_thread_idx = 0;
         logical_core_idx_given_thread_idx != n_logic_cores_per_pool_thread;
         ++logical_core_idx_given_thread_idx)
    {
      /* (If you're confused, suggest first looking at doc header's explanation of hw_threads_is_grouping_collated.
       * Also consider classic example configuration with 8 hardware threads, 4 physical threads, and
       * !hw_threads_is_grouping_collated, resulting in system hardware thread indexing 01230123.
       * Or if hw_threads_is_grouping_collated, then it's 00112233.) */
      const unsigned int native_logical_core_id
        = hw_threads_is_grouping_collated ? ((thread_idx * n_logic_cores_per_pool_thread)
                                             + logical_core_idx_given_thread_idx)
                                          : ((logical_core_idx_given_thread_idx * n_pool_threads)
                                             + thread_idx);
      FLOW_LOG_INFO("Thread [" << thread_idx << "] in pool: adding affinity for "
                    "logical core/hardware thread [" << native_logical_core_id << "].");

      CPU_SET(native_logical_core_id, &cpu_set_for_thread);
    }

    const auto code = pthread_setaffinity_np(native_pthread_thread_id, sizeof(cpu_set_for_thread), &cpu_set_for_thread);
    if (code == -1)
    {
      const Error_code sys_err_code{errno, system_category()};
      FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log non-portable error.
      *err_code = sys_err_code;
      return;
    }
    // else OK!
#else // if FLOW_ASYNC_HW_THREAD_AFFINITY_MACH_VIA_POLICY_TAG

    static_assert(false, "This strongly platform-dependent function has not been properly tested and maintained "
                           "for Darwin/Mac in a long time, as Flow has been Linux-only for many years.  "
                           "There is also a likely (documented, known) bug in this impl.  Please revisit when "
                           "we re-add Mac/Darwin support.")

#  if 0
    /* Maintenance note: When/if re-enabling this Darwin/Mac section:
     *   - Resolve the likely bug noted in below @todo.
     *   - Update the code to generate an Error_code even on the Mach error (will need to make a new Error_code
     *     category, in flow.async; which is no big deal; we do that all the time; or if there is one out there
     *     for these Mach errors specifically, then use that).
     *   - Update the code assign to *err_code, not throw (the rest of the function now acts this way, per standard
     *     Flow semantics).
     *   - Test, test, test... Mac, ARM64... all that. */

    using ::pthread_mach_thread_np;
    using ::thread_affinity_policy_data_t;
    using ::thread_policy_set;
    // using ::THREAD_AFFINITY_POLICY; // Nope; it's a #define.

    const unsigned int native_affinity_tag = 1 + thread_idx;
    FLOW_LOG_INFO("Thread [" << thread_idx << "] in pool: setting Mach affinity tag [" << native_affinity_tag << "].");

    Error_code sys_err_code;
    const auto native_mach_thread_id = pthread_mach_thread_np(native_pthread_thread_id);
    if (native_pthread_thread_id == 0)
    {
      const Error_code sys_err_code{errno, system_category()}; // As above....
      FLOW_ERROR_SYS_ERROR_LOG_WARNING();
      throw error::Runtime_error{sys_err_code, "pthread_mach_thread_np() call in optimize_pinning_in_thread_pool()"};
    }
    // else
    FLOW_LOG_TRACE("pthread ID [" << native_pthread_thread_id << "] "
                   "<=> Mach thread ID [" << native_mach_thread_id << "].");

    /* @todo CAUTION! There may be a bug here.  The (Darwin-specific) documentation
     * https://developer.apple.com/library/archive/releasenotes/Performance/RN-AffinityAPI/
     * recommends one create the thread, then set its affinity, and only THEN run the thread.
     * The Boost (and the derived STL equivalent) thread lib does not allow one to create a thread in suspended state;
     * pthread_create_suspended_np() does but isn't accessible (and seemingly is a barely-documented Darwin thing
     * not available in Linux, though the present code is Mac anyway) nor possible to somehow splice into the
     * boost::thread ctor's execution.  We can't/shouldn't abandon the thread API, so we are stuck.
     * Now, when it *recommends* it, does it actually require it?  The wording seems to imply "no"... but
     * there is no guarantee.  Empirically speaking, when trying it out, it's unclear (via cpu_idx() calls elsewhere
     * in this file) whether Darwin's listening to us.  It definitely keeps migrating the threads back and forth,
     * which MIGHT suggest it's not working, as the doc claims setting an affinity tag would "tend" to reduce migration
     * (but how much, and what does "tend" mean?).  The to-do is to resolve this; but it is low-priority, because
     * in reality we care about Linux only and do the Mac thing for completeness only, in this PoC.
     *
     * There's an official @todo in our doc header, and it refers to this text and code. */

    thread_affinity_policy_data_t native_thread_policy_data{ int(native_affinity_tag) }; // Cannot be const due to API.
    const auto code = thread_policy_set(native_mach_thread_id, THREAD_AFFINITY_POLICY,
                                        // The evil cast is necessary given the Mach API design.  At least they're ptrs.
                                        reinterpret_cast<thread_policy_t>(&native_thread_policy_data), 1);
    if (code != 0)
    {
      /* I don't know/understand Mach error code system, and there are no `man` pages as such that I can find
       * (including on Internet) -- though brief kernel code perusal suggests fairly strongly it's not simply errno
       * here -- so let's just save the numeric code in a general runtime error exception string; hence do not
       * use Error_code-taking Runtime_exception as we would normally for a nice message.  @todo If we wanted
       * to we could make a whole boost.system error category for these Mach errors, etc. etc.  Maybe someone has.
       * Maybe Boost has!  Who knows?  We don't care about this corner case at the moment and doubtful if ever will.
       * @todo For sure though should use error::Runtime_error here, the ctor that takes no Error_code.
       * That ctor did not exist when the present code was written; as of this writing Flow is Linux-only.
       * Would do it right now but lack the time to verify any changes for Mac at the moment. */
      throw runtime_error{ostream_op_string("[MACH_KERN_RETURN_T:", code,
                                            "] [thread_policy_set(THREAD_AFFINITY_POLICY) failed]")};
    }
    // else OK!
#  endif // if 0
#endif
  } // for (thread_idx in [0, n_pool_threads))
} // optimize_pinning_in_thread_pool()

void reset_thread_pinning(log::Logger* logger_ptr, util::Thread* thread_else_ours, Error_code* err_code)
{
  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code) { reset_thread_pinning(logger_ptr, thread_else_ours, actual_err_code); },
         err_code, "flow::async::reset_thread_pinning()"))
  {
    return;
  }
  // else if (err_code):
  err_code->clear();

  using util::Thread;
  using boost::system::system_category;

  static_assert(FLOW_ASYNC_HW_THREAD_AFFINITY_PTHREAD_VIA_CORE_IDX,
                "For this function we only know how to deal with thread-core affinities in Linux.");

  using ::cpu_set_t;
  using ::pthread_setaffinity_np;
  using ::pthread_self;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_ASYNC);

  const auto native_pthread_thread_id = thread_else_ours ? thread_else_ours->native_handle()
                                                           // `man` -v- page says, "This function always succeeds."
                                                         : pthread_self();
  cpu_set_t cpu_set_for_thread;
  CPU_ZERO(&cpu_set_for_thread);

  const unsigned int n_logic_cores = Thread::hardware_concurrency();
  for (unsigned int logic_core_idx = 0; logic_core_idx != n_logic_cores; ++logic_core_idx)
  {
    CPU_SET(logic_core_idx, &cpu_set_for_thread);
  }

  FLOW_LOG_INFO("Thread with native ID [" << native_pthread_thread_id << "]: resetting processor-affinity, "
                "so that no particular core is preferred for this thread.  (This may have already been the case.)");

  if (pthread_setaffinity_np(native_pthread_thread_id, sizeof(cpu_set_for_thread), &cpu_set_for_thread) == -1)
  {
    const Error_code sys_err_code{errno, system_category()};
    FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log non-portable error.
    *err_code = sys_err_code;
    return;
  }
  // else OK!
} // reset_thread_pinning()

void reset_this_thread_pinning() // I know this looks odd and pointless; but see our doc header.
{
  reset_thread_pinning();
}

} // namespace flow::async
