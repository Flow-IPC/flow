# Version 1.0.0 (Mar 2024)
* Initial open-source release.

# Version 1.0.1 (Mar 2024)
* Comment and/or doc changes.
* Removed certain minor internal dead code related to macOS.

# Version 1.0.2 (Mar 2024)
* Comment and/or doc changes.
* Style refactor (small): Encouraging `static_assert(false)` over `#error`. Encouraging `if constexpr` over SFINAE.

# Version 2.0.0 (Feb 2025)
* flow.log: Significantly improve performance of `Logger::should_log()`, invoked at ~every log call site. To enable, requires small code change when invoking `flow::log::Config::init_component_to_union_idx_mapping()`.
* C++20 mode build and GitHub-CI-pipeline support. C++17 remains the main/minimal mode, but we now ensure user can build/use in C++20 mode also.
* flow.perf: Minor perf improvements.
* Dependency bump: Boost v1.84 to 1.87. Results in slight breaking API changes in `flow::error`.
* Add a unit-test suite to Flow. (Flow now gains GTest dependency if built with test-suite enabled.)
* GitHub CI pipeline: Tweak to get gcc-13 builds to work again (GitHub changed preinstalled packages).
* Comment and/or doc changes.

## API notes
* New APIs:
  * `flow::log::Config::init_component_to_union_idx_mapping()` new optional arg `component_payload_type_info_ptr_is_uniq = false`; set to `true` for potentially significant performance gains within Flow and flow.log log call sites in your application.
    * Two optional preprocessor defines to further tweak perf-related internal behavior: `FLOW_LOG_CONFIG_COMPONENT_PAYLOAD_TYPE_DICT_BY_PTR`, `FLOW_LOG_CONFIG_COMPONENT_PAYLOAD_TYPE_DICT_BY_VAL`.
* Breaking changes:
  * A mostly, but not fully, backward-compatible improvement in the API for macro `FLOW_ERROR_EXEC_AND_THROW_ON_ERROR()`:
    * Remove any `cref()` wrappers for args 3+ to this macro; this is no longer necessary and will likely not compile.
    * Ideally remove now-unnecessary method `Qualifiers::` in arg 2 (function name).
  * Removal of macro `FLOW_ERROR_EXEC_FUNC_AND_THROW_ON_ERROR()`.
    * If you were using this, simply change it to `FLOW_ERROR_EXEC_AND_THROW_ON_ERROR()` which now supports free functions.

# Version 3.0.0 (Nov 2026)
* flow.util:
  * Add `flow::util::stat` sub-namespace: Facilities for working with simple stat-holding `struct`s ("stat-sets"). Each data member ("stat-member") is classified as an accumulator (counter), gauge, or high-water mark of a particular gauge.
    * Add `Histogram_counter`: Usable as accumulator stat-member and standalone.
    * Stat-consuming API: Direct-read stat-members from stat-sets; reset and pretty-print stat-sets; more.
    * Stat-collecting API: Collect under concurrency (optionally with thread-local sharding) or single-threaded; or integrate with third-party stat sources.
  * Add thread-local-related utilities:
    * `flow::util::Thread_local_state_registry<T>`: A thread-local `T` per thread, plus access to all extant threads' `T`s. Companions `Poll_flag` and `Polled_shared_state<Shared_state>` support signaling a rare cross-thread request that each thread notices cheaply on its fast-path.
    * `Thread_local_ptr<T>` (fast `boost::thread_specific_ptr<T>` equivalent).
    * `Thread_token`, `this_thread_unique_token()` (actually-unique thread IDs).
    * `Thread_local_obj_deinit_safe<T, Tag>`.
  * `flow::util::Basic_blob` and `Blob_with_log_context`:
    * Bug fix: Containers of blobs (e.g., `vector<Blob>`) silently copied, instead of moving, blobs when relocating them, due to missing `noexcept`s.
    * Support stateful `Allocator`s (such as `boost::interprocess::allocator`); previously this did not compile.
    * Add optional allocate-and-zero in one step (`CLEAR_ON_ALLOC` overloads), which can be significantly faster than allocating and then zeroing.
    * Smaller `sizeof()`.
  * `flow::util::Linked_hash_map` and `Linked_hash_set`:
    * Add move-semantics insertion (key need not be copyable).
    * Perf and memory-use improvements: Key not stored twice per entry; open-addressed set instead of closed-addressed map.
    * Remove `[pop_]front()`, `[pop_]back()` methods (stylistically inappropriate for associative containers).
  * Add misc utilities: `flow::util::Action_registry`, `Locked_proxy<Target, Mutex>`, `Default_init_allocator`, `construct_at()`, `default_init_at()`, `Span` (alias), and arithmetic and pointer-math/alignment free functions.
* flow.log:
  * Perf improvements at every log call site that passes the verbosity filter; and when writing message strings in ~all `flow::log::Logger`s. (From internal use of thread-locality and faster lookups.)
  * Bug fix: Logging near thread death -- such as from the destructor of a `thread_local` object -- could hit undefined behavior, because certain thread-local flow.log facilities may by then be deinitialized.
  * Add `flow::log::Log_context::set_logger()`; and `Log_context_mt`, a variant whose `set_logger()` is thread-safe.
  * `flow::log::Logger::this_thread_set_logged_nickname()` (used also by much of flow.async): When the input nickname exceeds the Linux thread-name length limit (15 characters), and the function is to also affect the OS thread name (as for `top` et al), that name is now truncated from the back, not the front.
    * The `Async_file_logger` worker thread's nickname now fits that limit.
  * `Async_file_logger` worker thread resets its core-affinity (see flow.async below), so it no longer inherits the creating thread's.
* flow.async:
  * Add `flow::async::reset_thread_pinning()` and `reset_this_thread_pinning()`, which undo (on-demand) the default OS behavior of a thread inheriting its parent's setting.
  * `flow::async::optimize_pinning_in_thread_pool()` takes an optional `Error_code*`.
* NetFlow (`flow::net_flow`):
  * Hardening: Use a CSPRNG for ephemeral ports, initial sequence numbers, and security tokens. Reported by Sebastián Alba Vives (@Sebasteuo).
  * Hardening: Add configurable listen backlog (default 64) and limit on DATA cached in SYN_RCVD state (default 64Ki bytes). Reported by Sebastián Alba Vives (@Sebasteuo).
  * Minor bug fixes (~no splash radius).
* Build/dependencies:
  * Dependency bump: fmt v10.0.0 to 11.0.2.
  * Boost.Random is now a required Boost library.
  * Flow and Flow-like projects now fail the build if an export-worthy header is not actually exported.
* Significantly expand unit-test coverage: Many new tests (including for ~all new APIs above) and expand existing ones.
* GitHub CI pipeline:
  * Conan 1 => 2 migration.
  * Reduce pipeline run time: Dependency caching; parallelized core build step.
  * Add "early-warning" jobs against newest (as opposed to only min-supported) dependency versions.
  * Core-dump stack traces; LSAN suppressions; misc.
* Coding guide: Several additions.
* Comment and/or doc changes.

## API notes
* New/improved APIs:
  * In `flow::util`:
    * `stat` sub-namespace:
      * Operations on a stat-set (a `struct` of stat-members with an ADL-associated `declare_stats()` function): `stats_to_ostream()`, `stats_reset()`, `stats_assign()`, `stats_aggregate[_one]()`, `stats_aggregate_shards()`, `stats_reset_shard_aggregate()`, `stats_sum()`, `stats_field_names()`, `stats_mark_reset_state()`, `stats_since_reset_state()`.
      * Per-stat helpers that work the same on plain and `atomic` members: `load()`, `store()`, `fetch_add()`, `fetch_sub()`, `update_hi_wmark()`.
      * `Histogram_counter`; `Stat_set_list`; `Global_stats`; `Stat_name`.
    * As pertains to thread-local state:
      * `Thread_local_state_registry<T>`: Maintains a thread-local `T` per thread, created on-demand via `this_thread_state()` and deleted when its thread exits or the registry is destroyed, whichever happens first. To access all extant threads' `T`s, call `while_locked(task)`; `task()` receives the registry's map of extant `T`s.
        * At its core this is similar to `boost::thread_specific_ptr`, except that utility deliberately holds no registry of extant threads. Use the registry functionality with care; the doc headers give advice.
      * `Poll_flag`: A single boolean flag, armed via `arm_next_poll()` (typically from another thread) and both checked and disarmed via `poll_armed()`, cheaply enough for the hottest of paths.
      * `Polled_shared_state<Shared_state>`: Optional companion to `Thread_local_state_registry`, for the pattern wherein one embeds a `Poll_flag` in each thread's `T` and arms them all to signal a rare cross-thread request, which each thread checks cheaply along its fast-path. `Shared_state` carries any state about the overall request, accessed via `Polled_shared_state::while_locked()`.
      * `Thread_local_ptr<T>`: Semantically identical to `boost::thread_specific_ptr<T>` but with significantly faster `get()`.
      * `Thread_local_obj_deinit_safe<T, Tag>`: Equivalent to a naked `thread_local T`, but for non-trivially-destructible `T` guards access to the object, yielding null if the object has been destroyed near thread exit.
      * `this_thread_unique_token()` (and type `Thread_token`): Similar to `this_thread::get_id()` but guaranteed never to recycle values.
    * As pertains to `Basic_blob<Allocator>` and `Blob_with_log_context`:
      * Stateful `Allocator` (e.g., `boost::interprocess::allocator`) support for `Basic_blob`.
      * New overloads that zero-initialize memory during buffer allocation: `reserve()`, `resize()`, and `resize()`ing ctor, taking tag-typed value `CLEAR_ON_ALLOC`.
      * Add `noexcept` to move ctor and move assignment.
    * As pertains to `Linked_hash_map` and `Linked_hash_set`:
      * `Linked_hash_map::insert()` and `operator[]()` overloads with move-semantics for keys and, where applicable, mapped-values; `Linked_hash_set::insert()` overload with move-semantics for keys.
      * Type alias members such as `Key` and `Hash`.
    * `Action_registry`: A simple singleton template (with a tag parameter separating registries); code sites (particularly singleton initializers) can register actions (functors); other code can execute all actions in registration order, passing the same args to each.
    * `Locked_proxy<Target, Mutex>`: Proxy-pointer class template for the execute-around-pointer idiom.
    * `Default_init_allocator`: Allocator for STL-compliant containers that avoids zeroing things like PoDs and `array<>`s.
    * `construct_at()`, mimicking the C++20 STL facility of the same name; `default_init_at()`, reminiscent of C++23 `std::start_lifetime_as()`.
    * Arithmetic: New `constexpr` `round_to_multiple()`; `ceil_div()` is now `constexpr` and slightly more flexible.
    * Pointer-math/alignment free function templates: `is_power_of_two()`, `max_align_sz()`, `aligned_sz_of()`, `aligned_prefix_before()`, `after_aligned_prefix()`.
    * `Span` and `DYNAMIC_EXTENT`: Aliases for `std::span`-like ops.
    * `String_view::starts_with()` and `ends_with()` are now `constexpr`.
    * `setup_auto_cleanup()` now takes its functor via move semantics.
  * In `flow::log`:
    * `Log_context::set_logger()`: Change the deriver's `Logger*` (not thread-safe).
    * `Log_context_mt`: Variant of `Log_context` whose `Logger*` can be changed thread-safely via `set_logger()`; the mutex also protects copy/move and `swap()`.
      * Mutex-locked logging: New macros `FLOW_LOG_..._LOCKED()` (lock, log, unlock); method `log_while_locked()` for a custom operation. New macro `FLOW_LOG_SET_LOCKED_CONTEXT()` is to `FLOW_LOG_SET_CONTEXT()` what `FLOW_LOG_..._LOCKED()` is to `FLOW_LOG_...()`.
      * Unlike `Log_context`, it has no `get_logger()` or `get_log_component()` (an unlocked accessor would defeat the purpose).
      * Usage/perf point: Each such log call site locks a mutex, even when the message does not pass the verbosity filter. Prefer to avoid such logging along fast-paths; the doc header gives advice.
  * In `flow::async`:
    * `reset_thread_pinning()` and `reset_this_thread_pinning()`: Reset the core-affinity (in particular as inherited from parent thread) of the given/calling thread to no preference, leaving migration decisions to the OS.
    * `optimize_pinning_in_thread_pool()`: New optional arg `Error_code*`, making it compliant with the standard Flow error-emission pattern.
  * In `flow::cfg`:
    * `value_set_member_id_to_opt_name()`: New optional arg `sep_replacement`.
  * In `flow::net_flow`:
    * New `Node`-level socket-option `m_dyn_accept_backlog_limit` (default 64): A `Server_socket` rejects (with RST) any SYN once its count of half-opened plus not-yet-accepted connections reaches this limit, akin to the `backlog` arg to Berkeley-socket `listen()`. Changing it affects subsequently created `Server_socket`s only.
    * New socket-option `m_st_rcv_syn_rcvd_data_q_cumulative_max_size` (default 64Ki bytes) limiting the cumulative size of DATA payload cached for a half-opened (SYN_RCVD-state) connection. Previously these were limited only by the more general (and typically much more generous) receive-buffer limit.
* Breaking changes:
  * Remove `flow::util::Linked_hash_{map|set}::front()` and `back()` as well as `Linked_hash_set::pop_{front|back}()`.
  * `flow::util::Basic_blob<Allocator_t>::Allocator_raw` alias renamed to `Allocator`.
  * `flow::log::Msg_metadata::m_call_thread_nickname` is now of type `log::Fixed_string` (new alias for `boost::static_string<128>`) instead of `std::string`; logged thread nicknames are truncated to 128 characters. (It is a potential breaking change only for custom `Logger` implementations. Change needed so that logging works even during `thread_local` deinit.)
  * Remove `flow::util::bind_ns` namespace alias.
  * Build/dependencies:
    * Boost.Random library is now required.
    * Flow-like projects (such as Flow-IPC) using Flow's core build script(s): Build now fails if an export-worthy `#include`ee is not exported. Doc-only headers (such as concept docs) must be listed in new list `DOC_ONLY_HDRS`.

## Impl notes
* `flow::util::Thread_local_ptr<T>::get()` faster than `boost::thread_specific_ptr<T>::get()`:
  * It wraps a `thread_specific_ptr` but also maintains its own per-thread index: a `thread_local boost::unordered_flat_map` keyed by a unique 64-bit per-instance ID. So `get()` touches only `thread_local`s plus one open-addressed probe, versus `thread_specific_ptr::get()`'s OS-TLS-API call plus an `std::map` lookup keyed by `this`.
  * Trade-off: `reset()` is slower, as it updates both. (Microbenchmark: `get()` ~1.4x faster with 1 live instance, ~2x with 16 -- ours stays flat as instance count grows, while `thread_specific_ptr`'s degrades; `reset()` ~1.8x slower.)
  * Near thread exit, once `thread_local`s may be deinitialized, it falls back to the `thread_specific_ptr` path.
* flow.log perf:
  * Three uses of `boost::thread_specific_ptr` replaced by C++ built-in `thread_local`; all are exercised at every log call site that passes the verbosity filter.
  * Per-call-site lookup in the thread-local map keyed by `Logger*` uses a faster data structure.
  * `Config::output_component_to_ostream()` (used by ~all `Logger`s when writing the message) uses a faster data structure for component lookups.
