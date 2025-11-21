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
#include "flow/net_flow/detail/socket_buffer.hpp"

namespace flow::net_flow
{

// Implementations.

Socket_buffer::Socket_buffer(log::Logger* logger_ptr, size_t block_size_hint) :
  log::Log_context(logger_ptr, Flow_log_component::S_NET_FLOW),
  m_block_size_hint(block_size_hint),
  m_data_size(0)
{
  FLOW_LOG_TRACE("Socket_buffer [" << this << "] created; block_size_hint = [" << block_size_hint << "].");
}

size_t Socket_buffer::data_size() const
{
  return m_data_size;
}

size_t Socket_buffer::feed_buf_move(util::Blob* data, size_t max_data_size)
{
  using util::Blob;
  using boost::asio::const_buffer;
  using boost::asio::buffer;

  const size_t orig_data_size = m_data_size;

  if (m_block_size_hint == 0)
  {
    /* In this mode, we postpone all re-packetizing until -- possibly -- the data are consume*()d.
     * In other words, since we have advertised we will clear any data that we append to *this from
     * *data, we can simply create a new buffer in m_q, and move (constant-time operation) *data
     * into that buffer.  Only case where we cannot do that is if that would exceed max_data_size
     * data_size(); in that case we have to move only part of *data and therefore will have to copy. */

    if (data->empty() || (m_data_size >= max_data_size))
    {
      return 0; // Get rid of these pathological cases right away.
    }
    const size_t target_space_left = max_data_size - m_data_size; // At least 1 (byte).

    const size_t src_data_size = data->size();
    assert(src_data_size > 0);
    if (target_space_left < src_data_size)
    {
      // Unfortunately we'll have to move only part of *data; so we'll have to do a linear-time thing.
      Blob* bytes = new Blob{get_logger()};
      // (All operations are max-performance:)  Allocate N bytes; copy N bytes: (*data)[0, 1, ...].
      bytes->assign_copy(const_buffer(data->const_data(), target_space_left));
      data->erase(data->begin(), data->begin() + target_space_left);

      m_q.push_back(Blob_ptr{bytes});
      m_data_size = max_data_size;
    }
    else
    {
      // Enough space for all of *data -- so just use a constant-time swap.
      Blob_ptr bytes_ptr{new Blob{std::move(*data)}}; // Move inner representation of *data into *bytes_ptr.
      // *data empty now.

      m_q.push_back(bytes_ptr);
      m_data_size += src_data_size;
    }

    FLOW_LOG_TRACE("Socket_buffer/rcv [" << this << "]: data_size [" << m_data_size << "]; "
                   "buf_count [" << m_q.size() << "]; fast-fed (if not truncated) buffer "
                   "original/truncated size = [" << src_data_size << '/' << m_q.back()->size() << "].");
    // Very verbose and CPU-intensive!
    FLOW_LOG_DATA("Buffer data [" << util::buffers_dump_string(m_q.back()->const_buffer(), "", size_t(-1)) << "].");
  }
  else // if (m_block_size_hint != 0)
  {
    /* This is basically a special case of feed_bufs_copy() but with only one buffer in the buffer
     * sequence and with the copied data then removed from that source buffer.  This is the unlikely method to be
     * used anyway, but we provide it for completeness. */

    const size_t num_copied = feed_bufs_copy(buffer(data->const_data(), data->size()), max_data_size);
    data->erase(data->begin(), data->begin() + num_copied);
  }

  return m_data_size - orig_data_size;
} // Socket_buffer::feed_buf_move()

void Socket_buffer::consume_buf_move(util::Blob* target_buf, size_t max_data_size)
{
  using util::Blob;

  if (m_data_size == 0)
  {
    target_buf->clear(); // Pathological case.
    return;
  }
  // else

  /* This is the use case for which m_block_size_hint != 0 attempts to optimize.  Since when feed*() is
   * called in that mode, we always maintain the invariant that the m_q contains blocks of
   * exactly m_block_size_hint bytes, except for the last block.  And if one only ever calls
   * consume_buf_move() with max_data_size == m_block_size_hint, then that invariant is always maintained.
   * Therefore, the following if condition will always hold, which will enable us to perform
   * an O(1) operation. */

  Blob& front_buf = *m_q.front();
  const size_t front_buf_size = front_buf.size();
  if ((front_buf_size == max_data_size)
      || ((front_buf_size < max_data_size) && (m_q.size() == 1)))
  {
    // Either the first block is perfectly sized, or it fits AND is the only one in the sequence.

    *target_buf = std::move(front_buf); // Constant-time operation (swap internal buffer representations).
    m_q.pop_front();
    // Whatever they had in *target_buf is junk.  Deleted!
    m_data_size -= target_buf->size();

    FLOW_LOG_TRACE("Socket_buffer [" << this << "]: data_size [" << m_data_size << "]; "
                   "buf_count [" << m_q.size() << "]; fast-consumed buffer of size [" << front_buf_size << "].");
    // Very verbose and CPU-intensive!
    FLOW_LOG_DATA("Buffer data [" << util::buffers_dump_string(target_buf->const_buffer(), "", size_t(-1)) << "].");
  }
  else
  {
    using boost::asio::buffer;

    /* Either the front buffer is too big for max_data_size and thus has to be split up (left part
     * moves, right part stays); or the front buffer is smaller than max_data_size, and there are more
     * data in the following buffers, so we can only optimize the copying of the front buffer but not
     * later.  Thus the use case above must not be in effect, and we can just use the general
     * consume_bufs_copy to copy/erase stuff in O(n) time. */

    // consume_bufs_copy() uses Boost buffer semantics: size() of vector indicates max # bytes.

    if ((!target_buf->zero()) && (target_buf->capacity() < max_data_size))
    {
      // *target_buf must be enlarged; but to enlarge it we must explicitly deallocate it first.
      target_buf->make_zero();
    }
    target_buf->resize(max_data_size);

    // Move up to max_data_size bytes into *target_buf.
    const size_t num_copied = consume_bufs_copy(buffer(target_buf->data(), target_buf->size()));

    // As advertised, fit the vector to the bytes placed (keep size() same or decrease it).
    target_buf->resize(num_copied);
  }
} // Socket_buffer::consume_buf_move()

bool Socket_buffer::empty() const
{
  return m_data_size == 0;
}

void Socket_buffer::clear()
{
  m_q.clear();
  m_data_size = 0;
}

std::ostream& operator<<(std::ostream& os, const Socket_buffer& sock_buf)
{
  for (const auto& buf_ptr : sock_buf.m_q)
  {
    // Serialization of block will contain no newlines or unprintable characters.
    os << '[' << util::buffers_dump_string(buf_ptr->const_buffer(), "", size_t(-1)) << "]\n";
  }
  return os;
}

} // namespace flow::net_flow
