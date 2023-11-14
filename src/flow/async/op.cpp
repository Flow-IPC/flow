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
#include "flow/async/op.hpp"

namespace flow::async
{

// Implementations.

Op_list::Op_list(log::Logger* logger_ptr, size_t n_ops, const Create_op_func& create_op_func) :
  log::Log_context(logger_ptr, Flow_log_component::S_ASYNC),
  m_ops(n_ops), // This many empty boost::any objects.  Must still set each one.
  m_rnd_op_idx(0, n_ops - 1) // Random indices generated in [0, n_ops).  Seed from time; it's fine.
{
  assert(n_ops >= 1);

  FLOW_LOG_INFO("Op_list [" << static_cast<const void*>(this) << "]: will store [" << n_ops << "] ops.");

  for (size_t idx = 0; idx != n_ops; ++idx)
  {
    m_ops[idx] = create_op_func(idx);
  }
}

size_t Op_list::size() const
{
  return m_ops.size();
}

const Op& Op_list::operator[](size_t idx) const
{
  return m_ops[idx];
}

const Op& Op_list::random_op(size_t* chosen_idx) const
{
  const auto idx = random_idx();
  if (chosen_idx)
  {
    *chosen_idx = idx;
  }

  FLOW_LOG_TRACE("Op_list [" << this << "]: Randomly selected "
                 "op index [" << idx << "] from [0, " << size() << ").");

  return operator[](idx);
}

size_t Op_list::random_idx() const
{
  return m_rnd_op_idx();
}

const std::vector<Op>& Op_list::ops_sequence() const
{
  return m_ops;
}

} // namespace flow::async
