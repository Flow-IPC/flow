# Flow
# Copyright 2023 Akamai Technologies, Inc.
#
# Licensed under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in
# compliance with the License.  You may obtain a copy
# of the License at
#
#   https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in
# writing, software distributed under the License is
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing
# permissions and limitations under the License.

# See this guy; it'll explain inside.  It mandates the following procedure and documents details.
set(LIB_CMAKE_SCRIPT "../tools/cmake/FlowLikeLib.cmake")

# Flow needs these Boost libs.
# (Our dependents should do similar for any additional such dependencies; or omit this if none.)
# (By the way no need to worry about find_package(Threads), as Boost::thread should take care of that.)
set(BOOST_LIBS thread chrono timer program_options filesystem)

find_package(fmt REQUIRED)
list(APPEND DEP_LIBS fmt::fmt)
list(APPEND DEP_LIBS_PKG_ARG_LISTS fmt::fmt)

# (See above if wondering why we're not auto-searching for these.  There's a good reason.)
# Ideally keep this in `find -s` order.
set(SRCS
    flow/async/concurrent_task_loop.cpp
    flow/async/detail/async.cpp
    flow/async/detail/task_qing_thread.cpp
    flow/async/op.cpp
    flow/async/segregated_thread_task_loop.cpp
    flow/async/single_thread_task_loop.cpp
    flow/async/timed_concurrent_task_loop.cpp
    flow/async/x_thread_task_loop.cpp
    flow/cfg/cfg_manager.cpp
    flow/cfg/option_set.cpp
    flow/common.cpp
    # flow/detail/doc/doc-coding_style.cpp <-- Not an actual source file.  .cpp only for syntax highlighting.
    flow/error/error.cpp
    flow/log/async_file_logger.cpp
    flow/log/buffer_logger.cpp
    flow/log/config.cpp
    flow/log/detail/serial_file_logger.cpp
    flow/log/detail/thread_lcl_str_appender.cpp
    flow/log/log.cpp
    flow/log/ostream_log_msg_writer.cpp
    flow/log/simple_ostream_logger.cpp
    flow/log/verbosity_config.cpp
    flow/net_flow/asio/node.cpp
    flow/net_flow/asio/peer_socket.cpp
    flow/net_flow/asio/server_socket.cpp
    flow/net_flow/detail/cong_ctl/cong_ctl_classic.cpp
    flow/net_flow/detail/cong_ctl/cong_ctl_classic_bw.cpp
    flow/net_flow/detail/cong_ctl/cong_ctl_util.cpp
    flow/net_flow/detail/cong_ctl.cpp
    flow/net_flow/detail/drop_timer.cpp
    flow/net_flow/detail/low_lvl_io.cpp
    flow/net_flow/detail/low_lvl_packet.cpp
    flow/net_flow/detail/net_flow_util.cpp
    flow/net_flow/detail/port_space.cpp
    flow/net_flow/detail/seq_num.cpp
    flow/net_flow/detail/socket_buffer.cpp
    flow/net_flow/detail/stats/bandwidth.cpp
    flow/net_flow/detail/stats/socket_stats.cpp
    flow/net_flow/endpoint.cpp
    flow/net_flow/error/error.cpp
    flow/net_flow/event_set.cpp
    flow/net_flow/info.cpp
    flow/net_flow/net_env_simulator.cpp
    flow/net_flow/node.cpp
    flow/net_flow/options.cpp
    flow/net_flow/peer_socket.cpp
    flow/net_flow/server_socket.cpp
    flow/perf/checkpt_timer.cpp
    flow/perf/clock_type.cpp
    flow/util/detail/sched_task_handle_state.cpp
    flow/util/detail/util.cpp
    flow/util/sched_task.cpp
    flow/util/string_ostream.cpp
    flow/util/uniq_id_holder.cpp
    flow/util/util.cpp)
set(HDRS
    flow/async/async_fwd.hpp
    flow/async/concurrent_task_loop.hpp
    flow/async/detail/async_fwd.hpp
    flow/async/detail/task_qing_thread.hpp
    flow/async/op.hpp
    flow/async/segregated_thread_task_loop.hpp
    flow/async/single_thread_task_loop.hpp
    flow/async/timed_concurrent_task_loop.hpp
    flow/async/util.hpp
    flow/async/x_thread_task_loop.hpp
    flow/cfg/cfg_fwd.hpp
    flow/cfg/cfg_manager.hpp
    flow/cfg/cfg_manager_fwd.hpp
    flow/cfg/detail/cfg_fwd.hpp
    flow/cfg/dynamic_cfg_context.hpp
    flow/cfg/option_set.hpp
    flow/cfg/static_cfg_manager.hpp
    flow/common.hpp
    flow/detail/common.hpp
    flow/detail/macros/log_component_enum_declare.macros.hpp
    flow/error/error.hpp
    flow/error/error_fwd.hpp
    flow/log/async_file_logger.hpp
    flow/log/buffer_logger.hpp
    flow/log/config.hpp
    flow/log/detail/log_fwd.hpp
    flow/log/detail/serial_file_logger.hpp
    flow/log/detail/thread_lcl_str_appender.hpp
    flow/log/log.hpp
    flow/log/log_fwd.hpp
    flow/log/macros/config_enum_end_cpp.macros.hpp
    flow/log/macros/config_enum_end_hdr.macros.hpp
    flow/log/macros/config_enum_start_cpp.macros.hpp
    flow/log/macros/config_enum_start_hdr.macros.hpp
    flow/log/ostream_log_msg_writer.hpp
    flow/log/simple_ostream_logger.hpp
    flow/log/verbosity_config.hpp
    flow/log/verbosity_config_fwd.hpp
    flow/net_flow/asio/node.hpp
    flow/net_flow/asio/peer_socket.hpp
    flow/net_flow/asio/server_socket.hpp
    flow/net_flow/detail/cong_ctl/cong_ctl_classic.hpp
    flow/net_flow/detail/cong_ctl/cong_ctl_classic_bw.hpp
    flow/net_flow/detail/cong_ctl/cong_ctl_util.hpp
    flow/net_flow/detail/cong_ctl.hpp
    flow/net_flow/detail/drop_timer.hpp
    flow/net_flow/detail/low_lvl_io.hpp
    flow/net_flow/detail/low_lvl_packet.hpp
    flow/net_flow/detail/net_flow_fwd.hpp
    flow/net_flow/detail/port_space.hpp
    flow/net_flow/detail/seq_num.hpp
    flow/net_flow/detail/socket_buffer.hpp
    flow/net_flow/detail/stats/bandwidth.hpp
    flow/net_flow/detail/stats/socket_stats.hpp
    flow/net_flow/endpoint.hpp
    flow/net_flow/error/error.hpp
    flow/net_flow/event_set.hpp
    flow/net_flow/info.hpp
    flow/net_flow/net_env_simulator.hpp
    flow/net_flow/net_flow_fwd.hpp
    flow/net_flow/node.hpp
    flow/net_flow/options.hpp
    flow/net_flow/peer_socket.hpp
    flow/net_flow/server_socket.hpp
    flow/perf/checkpt_timer.hpp
    flow/perf/clock_type_fwd.hpp
    flow/perf/perf_fwd.hpp
    flow/perf/timed_function.hpp
    flow/perf/timed_handler.hpp
    flow/util/basic_blob.hpp
    flow/util/blob.hpp
    flow/util/blob_fwd.hpp
    flow/util/detail/sched_task_handle_state.hpp
    flow/util/detail/util.hpp
    flow/util/detail/util_fwd.hpp
    flow/util/linked_hash_map.hpp
    flow/util/linked_hash_set.hpp
    flow/util/random.hpp
    flow/util/sched_task.hpp
    flow/util/sched_task_fwd.hpp
    flow/util/shared_ptr_alias_holder.hpp
    flow/util/string_ostream.hpp
    flow/util/string_view.hpp
    flow/util/traits.hpp
    flow/util/uniq_id_holder.hpp
    flow/util/util.hpp
    flow/util/util_fwd.hpp)

include(${LIB_CMAKE_SCRIPT})

message(STATUS "Install target: Exports FlowLikeLib.cmake: utility for dependents' CMake scripts.")

# Last but not least... export that same guy, so dependents can use it!  Natually dependents need *not* do the same;
# they should instead:
#   find_package(Flow CONFIG REQUIRED)
#   include("${Flow_DIR}/../../../share/flow/cmake/FlowLikeLib.cmake")
install(FILES ${LIB_CMAKE_SCRIPT} DESTINATION share/${PROJ}/cmake)
