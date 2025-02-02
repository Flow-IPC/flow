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

#include "flow/log/log_fwd.hpp"
#include <boost/thread.hpp>
#include <boost/unordered_map.hpp>
#include <vector>
#include <map>
#include <unordered_map>
#include <typeinfo>
#include <typeindex>

namespace flow::log
{
// Types.

// Find doc headers near the bodies of these compound types.

class Serial_file_logger;
template<typename Map_to_cfg_t>
class Component_payload_type_dict_by_ptr_via_map;
template<typename Cfg_t>
class Component_payload_type_dict_by_ptr_via_array;
template<typename Cfg_t>
class Component_payload_type_dict_by_ptr_via_sorted_array;
template<typename Map_to_cfg_t>
class Component_payload_type_dict_by_val_via_map;
template<typename Cfg_t>
class Component_payload_type_dict_by_val_via_array;
template<typename Cfg_t>
class Component_payload_type_dict_by_val_via_sorted_array;
template<typename Dict_by_ptr_t, typename Dict_by_val_t>
class Component_payload_type_dict;

/// Convenience alias.
template<typename Cfg_t>
using Component_payload_type_dict_by_ptr_via_tree_map
  = Component_payload_type_dict_by_ptr_via_map<std::map<const std::type_info*, Cfg_t>>;
/// Convenience alias.
template<typename Cfg_t>
using Component_payload_type_dict_by_ptr_via_s_hash_map
  = Component_payload_type_dict_by_ptr_via_map<std::unordered_map<const std::type_info*, Cfg_t>>;
/// Convenience alias.
template<typename Cfg_t>
using Component_payload_type_dict_by_ptr_via_b_hash_map
  = Component_payload_type_dict_by_ptr_via_map<boost::unordered_map<const std::type_info*, Cfg_t>>;
/// Convenience alias.
template<typename Cfg_t>
using Component_payload_type_dict_by_val_via_tree_map
  = Component_payload_type_dict_by_val_via_map<std::map<std::type_index, Cfg_t>>;
/// Convenience alias.
template<typename Cfg_t>
using Component_payload_type_dict_by_val_via_s_hash_map
  = Component_payload_type_dict_by_val_via_map<std::unordered_map<std::type_index, Cfg_t>>;
/// Convenience alias.
template<typename Cfg_t>
using Component_payload_type_dict_by_val_via_b_hash_map
  = Component_payload_type_dict_by_val_via_map<boost::unordered_map<std::type_index, Cfg_t>>;

// Globals.

/**
 * Thread-local Msg_metadata object used by FLOW_LOG_WITHOUT_CHECKING() for an alleged perf bonus in the
 * synchronous-Logger code path.  It is allocated at the first log call site for a given thread; and auto-deallocated
 * when thread exits.
 */
extern boost::thread_specific_ptr<Msg_metadata> this_thread_sync_msg_metadata_ptr;

} // namespace flow::log
