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

#include "flow/util/util_fwd.hpp"
#include "flow/common.hpp"
#include <vector>

namespace flow::util
{

/**
 * A registry of callbacks (actions) sharing a common signature, intended for fan-out invocation
 * across statically-constructed singletons.  Each singleton self-registers a callback during its
 * static construction; the end user then invokes execute() once to fan out to all registered callbacks.
 *
 * It is possible, and fine, to register actions from places other than singleton initializers, and the
 * action itself is in no way required to do something singleton- or even global-related.  The original
 * motivation was for singleton self-registration, but conceivably Action_registry can be useful elsewhere.
 *
 * ### Motivation ###
 * Consider a framework with multiple singleton subsystems, each needing a `set_logger()` call
 * at startup and teardown.  Without Action_registry the user must know about and call each one;
 * and a new subsystem (possibly from a downstream project) cannot participate without modifying
 * the framework's central code.  With Action_registry each singleton registers itself, and
 * the user's single execute() call reaches all of them -- including ones the framework author
 * never anticipated.
 *
 * ### Usage ###
 * Define a tag type to differentiate your registry from others:
 *
 *   ~~~
 *   struct My_set_logger_tag {};
 *   using My_set_logger_registry = flow::util::Action_registry<My_set_logger_tag, flow::log::Logger*>;
 *   ~~~
 *
 * Each singleton registers during construction:
 *
 *   ~~~
 *   // In some singleton's static-init context (ctor, Static_state ctor, etc.):
 *   My_set_logger_registry::register_action([](flow::log::Logger* logger_ptr)
 *   { 
 *     Foo::static_set_logger(logger_ptr);
 *   });
 *   ~~~
 *
 * The end user simply does:
 *
 *   ~~~
 *   My_set_logger_registry::execute(&my_logger); // startup
 *   // Forwarded `&my_logger` to all registered actions: Executed them in order of registration.
 *
 *   // ... application runs ...
 *
 *   My_set_logger_registry::execute(nullptr); // teardown
 *   // Forwarded `nullptr` to all registered actions: Executed them in order of registration.
 *   ~~~
 *
 * ### Arg types ###
 * Each type in `Args` shall be a plain non-reference, non-`const` type -- typically a pointer or scalar.
 * (Pointer *to* `const` is perfectly fine: e.g., `const Logger*`.)  execute() receives
 * each arg by `const` reference and passes it, as an lvalue, to each registered
 * callback; since the callback signature takes `Args` by value, a copy is made per callback invocation.
 * For pointer/scalar types this cost is trivial.  For heavier types, prefer passing a pointer to the
 * heavy object rather than the object itself.
 *
 * ### Thread safety ###
 * All methods are thread-safe with respect to each other.  Internally a non-recursive mutex is held
 * during both register_action() and execute().  Consequently a callback invoked by execute()
 * must not call register_action() on the same Action_registry instantiation; doing so will deadlock
 * (this is intentional: such a call pattern would indicate a design problem).
 *
 * @tparam Tag_t
 *         A tag type used solely to differentiate distinct Action_registry instantiations.
 *         Each unique #Tag yields a distinct singleton registry.
 * @tparam Args
 *         The parameter types of the action.  Each registered callback has signature `void (Args...)`.
 *         See "Arg types" above.
 */
template<typename Tag_t, typename... Args>
class Action_registry
{
public:
  // Types.

  /// Alias for template parameter.
  using Tag = Tag_t;

  // Methods.

  /**
   * Registers a callback to be invoked by future execute() calls.  Intended to be called from
   * singleton constructors (static-init time or lazy-init `get_instance()` patterns).
   * Thread-safe.
   *
   * @tparam Action_func
   *         Functor type matching the signature `void (Args...)`.
   * @param action
   *        The callback to register.  Must not be empty.
   */
  template<typename Action_func>
  static void register_action(Action_func&& action);

  /**
   * Invokes every registered callback, in registration order, passing the given arguments to each.
   * Thread-safe.
   *
   * @param args
   *        Arguments forwarded to each registered callback.
   */
  static void execute(const Args&... args);

private:
  // Methods.

  /**
   * Returns the singleton instance.  Uses a function-local `static`, so it is safe to call
   * during static initialization from any translation unit.
   *
   * @return Reference to the sole instance.
   */
  static Action_registry& get_instance();

  // Data.

  /// Protects #m_actions.
  mutable Mutex_non_recursive m_mutex;

  /**
   * Registered callbacks, in registration order.
   *
   * If we wanted to add an `unregister_action()`, this might turn into a Linked_hash_set.
   */
  std::vector<Function<void (Args...)>> m_actions;
}; // class Action_registry

// Template implementations.

template<typename Tag_t, typename... Args>
template<typename Action_func>
void Action_registry<Tag_t, Args...>::register_action(Action_func&& action) // Static.
{
  auto& self = get_instance();
  Lock_guard<Mutex_non_recursive> lock{self.m_mutex};
  self.m_actions.emplace_back(std::move(action));
}

template<typename Tag_t, typename... Args>
void Action_registry<Tag_t, Args...>::execute(const Args&... args) // Static.
{
  const auto& self = get_instance();
  Lock_guard<Mutex_non_recursive> lock{self.m_mutex};
  for (const auto& action : self.m_actions)
  {
    action(args...);
  }
}

template<typename Tag_t, typename... Args>
Action_registry<Tag_t, Args...>& Action_registry<Tag_t, Args...>::get_instance() // Static.
{
  static Action_registry s_inst; // Thread-safe at least in C++17 and better.
  return s_inst;
}

} // namespace flow::util
