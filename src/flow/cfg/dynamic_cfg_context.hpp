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

#include "flow/cfg/cfg_fwd.hpp"
#include <boost/function.hpp>
#include <algorithm>
#include <type_traits>

namespace flow::cfg
{

/**
 * Class which facilitates managing access to a dynamic configuration.  Another class can make use of this one by means
 * of a composition relationship, either through inheritance (probably `private` or `protected`) or by containing one or
 * more `Dynamic_cfg_context` members.  If using inheritance, the class will gain methods which can be used to access
 * the dynamic configuration.  If using composition by members, more than one dynamic configuration can be used, each of
 * which can be accessed separately by calling the access methods of the associated member.
 *
 * A "configuration," here, is a data object (probably a `struct`), which is referred to as the "root", containing an
 * internal data object, which is referred to as the "target".  The target is the configuration object which is expected
 * to be normally accessed.  The root and the target can be the same.
 *
 * @tparam Root
 *         Type for the root configuration.  This should meet the requirements of the template argument for
 *         `Option_set`.  A `Root::Const_ptr` type for a ref-counted pointer to an immutable `Root` must at least be
 *         defined (which can be provided by deriving from `util::Shared_ptr_alias_holder`).  See Option_set.
 * @tparam Target
 *         Type for the target configuration.
 * @tparam Target_ptr
 *         Please leave this at its default.  Background: This would not have been a template parameter in the
 *         first place, had the authors known of `pointer_traits::rebind` at that the time.  To preserve
 *         backwards compatibility this parameter remains for now (albeit deprecated).
 *
 * @todo flow::cfg::Dynamic_cfg_context `Target_ptr` is deprecated and shall be always left at its default
 *       value in future code; eventually remove it entirely and hard-code the default value internally.
 */
template<typename Root, typename Target,
         typename Target_ptr> // See default in _fwd.hpp.
class Dynamic_cfg_context
{
  static_assert(std::is_same_v<typename Target_ptr::element_type, std::add_const_t<Target>>,
                "The `element_type` of `Target_ptr` must be `const Target`.");

public:
  // Types.

  /// Type alias for `Target_ptr`.
  using Target_ptr_type = Target_ptr;

  /// Type for a function object which returns a ref-counted pointer to an immutable root configuration object.
  using Get_root_func = Function<typename Root::Const_ptr ()>;

  /// Type for a function object which translates a `Root` object to a contained `Target` object.
  using Root_to_target_func = Function<const Target& (const Root&)>;

  // Constructors/destructor.

  /**
   * Constructor.
   *
   * @param get_root_func_moved
   *        Returns a ref-counted pointer to the (immutable) root configuration object.
   * @param root_to_target_func_moved
   *        Translates a root configuration object to a contained target configuration object.
   */
  Dynamic_cfg_context(Get_root_func&& get_root_func_moved, Root_to_target_func&& root_to_target_func_moved);

  /**
   * Constructor.
   *
   * This produces a `Dynamic_cfg_context` which will obtain its configuration from a `Config_manager`.
   *
   * @param config_manager
   *        A `Config_manager` which is currently managing the desired dynamic configuration.
   * @param root_to_target_func_moved
   *        Translates a root configuration object to a contained target configuration object.
   * @param d_value_set_idx
   *        The dynamic config slot index of `config_manager` which corresponds to the desired dynamic configuration.
   *        @see `Config_manager`.
   */
  template<typename... S_d_value_set>
  Dynamic_cfg_context(const Config_manager<S_d_value_set...>& config_manager,
                      Root_to_target_func&& root_to_target_func_moved, size_t d_value_set_idx = 0);

  // Methods.

  /**
   * Obtain the root configuration.
   *
   * @return a ref-counted pointer to the (immutable) root configuration object.
   */
  typename Root::Const_ptr root_dynamic_cfg() const;

  /**
   * Obtain the target configuration.
   *
   * This method provides the key mechanism of the class.  The returned pointer object can be used to easily access the
   * target object, but will additionally cause the containing root object to be held valid in memory.
   *
   * @return a ref-counted pointer to the (immutable) target configuration object, but which shares ownership of the
   *         containing root configuration object.
   */
  Target_ptr dynamic_cfg() const;

private:
  // Data.

  /// Called to obtain the root configuration.
  const Get_root_func m_get_root_func;

  /// Translates a root configuration object to a contained target configuration object.
  const Root_to_target_func m_root_to_target_func;
}; // class Dynamic_cfg_context

// Template implementations.

template<typename Root, typename Target, typename Target_ptr>
Dynamic_cfg_context<Root, Target, Target_ptr>::Dynamic_cfg_context
  (Get_root_func&& get_root_func_moved, Root_to_target_func&& root_to_target_func_moved) :
  m_get_root_func(std::move(get_root_func_moved)),
  m_root_to_target_func(std::move(root_to_target_func_moved))
{
}

template<typename Root, typename Target, typename Target_ptr>
template<typename... S_d_value_set>
Dynamic_cfg_context<Root, Target, Target_ptr>::Dynamic_cfg_context
  (const Config_manager<S_d_value_set...>& config_manager, Root_to_target_func&& root_to_target_func_moved,
   size_t d_value_set_idx) :
  m_get_root_func([&config_manager, d_value_set_idx]()
  {
    return config_manager.template dynamic_values<Root>(d_value_set_idx);
  }),
  m_root_to_target_func(std::move(root_to_target_func_moved))
{
}

template<typename Root, typename Target, typename Target_ptr>
typename Root::Const_ptr Dynamic_cfg_context<Root, Target, Target_ptr>::root_dynamic_cfg() const
{
  return m_get_root_func();
}

template<typename Root, typename Target, typename Target_ptr>
Target_ptr Dynamic_cfg_context<Root, Target, Target_ptr>::dynamic_cfg() const
{
  auto root = root_dynamic_cfg();
  /* This aliasing constructor provides the key mechanism: the constructed pointer object *shares ownership* of `root`,
   * but *stores* (and operates on) a pointer to the target object (contained by and translated from `root`). */
  return Target_ptr(root, &(m_root_to_target_func(*root)));
}

} // namespace flow::cfg
