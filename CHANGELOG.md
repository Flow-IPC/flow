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
* Dependency bump: Boost v1.84 to 1.87. Results in slight breaking API changes in flow::error.
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
    * If you were using this, simply change it to `FLOW_ERROR_EXEC_AND_THROW_ON_ERROR` which now supports free functions.
