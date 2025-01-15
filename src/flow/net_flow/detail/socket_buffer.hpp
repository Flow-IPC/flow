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

#include "flow/net_flow/detail/net_flow_fwd.hpp"
#include "flow/log/log.hpp"
#include "flow/util/blob.hpp"
#include "flow/util/util.hpp"
#include <boost/asio.hpp>
#include <boost/shared_ptr.hpp>
#include <vector>
#include <deque>
#include <ostream>

namespace flow::net_flow
{

/**
 * Internal `net_flow` class that implements a socket buffer, as used by Peer_socket for Send and
 * Receive buffers.  Users do not get direct access to such objects, but instead they access them
 * indirectly through things like Peer_socket::send() and Peer_socket::receive().  Meanwhile
 * Node modifies them directly when data are received or sent over the network.
 *
 * In terms of the API semantics, the object represents a sequence of bytes.  User can enqueue bytes
 * at the back -- from either a `Const_buffer_sequence` (boost.asio style gather operation) or a
 * vector of bytes.  User can dequeue bytes from the front -- to either a `Mutable_buffer_sequence`
 * (boost.asio style scatter operation) or a vector of bytes.
 *
 * That's all quite simple, but the complexity comes from performance aspects.  The goal is to avoid
 * copying arrays (linear-time) and encourage moving them (constant-time) whenever the user enqueues
 * or dequeues some bytes.  To that end, the class can operate in one of two modes (permanently
 * specified at construction), tailored to high performance in the two practical use cases of the
 * class.
 *
 * #### Mode 1: `block_size_hint == 0` ####
 * Intended for use as a Receive buffer.  In this case, packetized
 * blocks of data come from the network and can be of arbitrary sizes (though typically they'll be
 * max-block-size in length if possible).  They are then converted to a logical byte stream, and the
 * user (via receive()) will dequeue them using a scatter operation.  Since the size(s) of the
 * buffer(s) the user will choose in the scatter receive() are entirely unpredictable, copying of
 * the bytes during dequeuing is unavoidable.  However, copying the packet blocks while enqueuing a
 * packet upon receipt IS avoidable; since the Node no longer needs the packet data having given it
 * to the Receive buffer, we can take ownership of the block's data without copying.
 *
 * In mode 1, it is expected the class user will call feed_buf_move() for a constant-time
 * enqueueing; and the class user will call consume_bufs_copy() for a linear-time dequeuing into the
 * end user's (`receive()` caller's) data structure.
 *
 * #### Mode 2: `block_size_hint > 0` ####
 * Intended for use as a Send buffer.  In this case, a stream of bytes
 * comes from the end user (via `send()`), in the form of a sequence of buffers of arbitrary
 * length/structure, which is enqueued into this object.  Node, when it is ready to send a packet,
 * dequeues max-block-size bytes (or fewer, if fewer are available in Socket_buffer), packetizes
 * them, and sends them off.  Node will never ask for fewer than max-block-size bytes (in
 * particular, it will not send anything until congestion window [CWND] has at least that much free
 * space).  Since the size(s) of the enqueued buffers are entirely unpredictable, copying of the
 * bytes during enqueueing is unavoidable.  However, copying the packets while dequeueing a packet
 * to send IS avoidable.  How?  During enqueuing, we internally pack bytes into groups of
 * `block_size_hint` == max-block-size bytes.  Then when a packet is dequeued, we can provide a
 * ready-made front group of bytes with a constant-time operation.
 *
 * In mode 2, it is expected the class user will call feed_bufs_copy() for a linear-time enqueueing
 * from the end user's (`send()` caller's) data structure; and the class user will call
 * consume_buf_move() for a constant-time dequeuing into a low-level packet.
 *
 * This narrow design for performance is the reason for the kind-of-odd asymmetrical set of methods
 * for feeding and consuming bytes.  The two modes differ only in performance, and technically you
 * may mix and match the recommended calls for the 2 modes after construction.  However that defeats
 * the point of the class, so I wouldn't recommend that.
 *
 * ### Thread safety ###
 * Separate objects: All functionality safe for simultaneous execution from multiple
 * threads.  Same object: Not safe to execute a non-const operation simultaneously with any other
 * operation; otherwise safe.  Rationale: While locking of Socket_buffer objects is essential, typically
 * they must be locked together with various other external (to Socket_buffer) pieces of data (such
 * as a given Peer_socket::m_disconnect_cause), and therefore the locking is left to the user of
 * the class.
 */
class Socket_buffer :
  public log::Log_context,
  private boost::noncopyable // Definitely discourage copying.  If we make copyable, it'll be conscious design choice.
{
public:
  // Constructors/destructor.

  /**
   * Initialize empty buffer.  The block_size_hint argument is an important parameter that
   * determines the algorithm used for internally storing the bytes supplied via `feed*()`.  While
   * this has NO effect on the semantics of the output of `consume*()`, it has an effect on internal
   * performance.
   *
   * See Socket_buffer class doc header for explanation of `block_size_hint`.
   *
   * @param logger_ptr
   *        Logger to use for subsequent logging.
   * @param block_size_hint
   *        See above.
   */
  explicit Socket_buffer(log::Logger* logger_ptr, size_t block_size_hint);

  // Methods.

  /**
   * The total number of bytes of application-layer data stored in this object.  Intended use case:
   * estimate of memory usage of this object.
   *
   * Note: this only counts the actual user bytes fed into the buffer -- not other internal
   * book-keeping.  For example, if the internal storage is a doubly-linked list of byte arrays,
   * this will not count the memory used by the previous and next pointers for each node.
   * Rationale: if we give control over the maximum size to the `net_flow` user, with these semantics
   * it is easier to express and implement what that limit means (even if some side data are not
   * counted as a result).
   *
   * @return See above.
   */
  size_t data_size() const;

  /**
   * Returns true if and only if data_size() == 0.
   *
   * @return Ditto.
   */
  bool empty() const;

  /**
   * Feeds (adds to the back of the byte buffer) the contents of the byte stream composed of the
   * bytes in the given `Const_buffer_sequence`, in the order in which they are given -- up to an
   * internal buffer size.  The data are copied (O(n)) and not modified.
   *
   * As many bytes as possible are copied, subject to the constraint `data_size() <= max_data_size`.
   * In particular if this is already untrue, no bytes are copied.
   *
   * @tparam Const_buffer_sequence
   *         Type that models the boost.asio `ConstBufferSequence` concept (see Boost docs).
   *         Basically, it's any container with elements convertible to `boost::asio::const_buffer`;
   *         and bidirectional iterator support.  Examples: `vector<const_buffer>`, `list<const_buffer>`.
   *         Why allow `const_buffer` instead of, say, Sequence of bytes?  Same reason as boost.asio's
   *         send functions: it allows a great amount of flexibility without sacrificing performance,
   *         since `boost::asio::buffer()` function can adapt lots of different objects (arrays,
   *         vectors, strings, and more of bytes, integers, and more).
   * @param data
   *        Bytes will be copied from this buffer sequence until exhausted or data_size() equals
   *        `max_data_size`.
   * @param max_data_size
   *        See above.
   * @return Number of bytes (possibly zero) by which data_size() increased.
   */
  template<typename Const_buffer_sequence>
  size_t feed_bufs_copy(const Const_buffer_sequence& data, size_t max_data_size);

  /**
   * Feeds (adds to the back of the byte buffer) the byte sequence equal to the given byte sequence
   * `*data`, up to an internal buffer size.  Any bytes thus fed are cleared from the given buffer.
   * Internally, as much as possible while following the described `block_size_hint` constraints and
   * the `max_data_size` constraint, move semantics are used, attempting to keep time complexity at
   * @em O(1).
   *
   * As many bytes as possible are taken, subject to the constraint `data_size() <= max_data_size`.
   * In particular if this is already untrue, no bytes are taken.  If `block_size_hint == 0`, and
   * `data_size() + data->size() <= max_data_size`, then time complexity is constant.  Otherwise it is
   * linear in `data->size()`.
   *
   * @param data
   *        Bytes will be moved from this byte byffer until exhausted or data_size() equals
   *        `max_data_size`; the bytes thus moved are `erase()`d from `*data`.
   *        Why make this a `vector` of bytes, and not a `const_buffer`?  Because this allows us to use
   *        move semantics to avoid a copy.
   * @param max_data_size
   *        See above.
   * @return Number of bytes (possibly zero) by which data_size() increased, which equals # of bytes
   *         by which `data->size()` decreased.
   */
  size_t feed_buf_move(util::Blob* data, size_t max_data_size);

  /**
   * Consumes (removes from the front of the internal byte buffer and returns them to the caller) a
   * byte sequence of length data_size() or the available space in `target_bufs` -- whichever is
   * smaller.  The bytes are removed from the front of the internal byte buffer and are written in
   * the same order, starting from the first byte in `target_bufs`.  The operation involves a copy and
   * has @em O(n) time complexity.
   *
   * @tparam Mutable_buffer_sequence
   *         Type that models the boost.asio `MutableBufferSequence` concept (see Boost docs).
   *         Basically, it's any container with elements convertible to `boost::asio::mutable_buffer`;
   *         and bidirectional iterator support.  Examples: `vector<mutable_buffer>`,
   *         l`ist<mutable_buffer>.` Why allow `mutable_buffer` instead of, say, Sequence of bytes?
   *         Same reason as boost.asio's receive functions: it allows a great amount of flexibility
   *         without sacrificing performance, since `boost::asio::buffer()` function can adapt lots of
   *         different objects (arrays, `vector`s, `string`s, and more of bytes, integers, and more).
   * @param target_bufs
   *        Buffer sequence into which to copy bytes.
   * @return Number of bytes (possibly zero) by which data_size() decreased, which equals the # of
   *         bytes copied into `target_bufs`.  This is quite important -- without it there is
   *         absolutely no way to know how much of `target_bufs` we've used up.
   */
  template<typename Mutable_buffer_sequence>
  size_t consume_bufs_copy(const Mutable_buffer_sequence& target_bufs);

  /**
   * Consumes (removes from the front of the internal byte buffer and returns them to the caller) a
   * byte sequence of length data_size() or `max_data_size` -- whichever is smaller. The bytes are
   * removed from the front of the internal byte buffer and are written in the same order, starting
   * from the first byte in `target_buf`.  `*target_buf` is resized to the number of bytes thus
   * consumed.  If possible based on certain constraints, the operation avoids copying and has @em O(1)
   * time complexity.
   *
   * If `block_size_hint != 0`, and consume_buf_move() is always called with `max_data_size ==
   * block_size_hint`, then the time complexity is @em O(1) whenever this is called.  In other uses cases
   * this is not guaranteed, and the time complexity is @em O(n).
   *
   * @param target_buf
   *        Byte buffer which will be cleared and replaced with the bytes consumed from internal
   *        buffer.  Therefore `target_buf->size()` can be used to know how many bytes were consumed.
   *        Performance note: `*target_buf` internal buffer may be reallocated, though this is avoided if reasonably
   *        possible (if it already has enough `capacity()`, it is not reallocated).
   * @param max_data_size
   *        data_size() will not grow beyond this.
   */
  void consume_buf_move(util::Blob* target_buf, size_t max_data_size);

  /// Destroys all stored data.
  void clear();

private:
  // Types.

  /// Short-hand for byte sequence on the heap.  (Using ref-counted pointer purely for convenience.)
  using Blob_ptr = boost::shared_ptr<util::Blob>;

  /// Short-hand for byte sequence on the heap.  (Using ref-counted pointer purely for convenience.)
  using Blob_const_ptr = boost::shared_ptr<const util::Blob>;

  /**
   * FIFO of byte sequences, together comprising an overall byte sequence.  Why not just use a
   * `deque<uint8_t>`?  We easily could, and it would make the code far simpler.  Answer: this enables
   * us to reduce or eliminate copying of data in certain likely use cases, chief among them when
   * using Socket_buffer as the Send buffer.  See #m_q.
   *
   * Why `deque` instead of `list`?  Both offer ~O(1) `push_back()` and `pop_front()`.  `deque` is
   * implemented, in practice, as a set of arrays and a master array with pointers to those arrays.
   * `list` is a doubly-linked list.  `deque` is therefore probably more compact and local in memory.
   * `list` may perform less reallocating/copying, depending on how the `deque` implementation works.
   * Using `deque` on a hunch, however.  Note, also, that at least gcc 4+ defaults `queue<T>` to
   * `queue<T, deque>`, so I guess they also find it better on average.
   *
   * Using container instead of adapter `queue` due to need to iterate over the whole thing in some
   * situations.
   *
   * @todo Investigate pros and cons of deque vs. list empirically.
   */
  using Queue = std::deque<Blob_ptr>;

  // Friends.

  // Friend of Socket_buffer: For access to our internals.
  friend std::ostream& operator<<(std::ostream& os, const Socket_buffer& sock_buf);

  // Methods.

  /**
   * Helper that copies, to a given raw memory buffer, a given number of bytes from a given
   * `Const_buffer_sequence`, starting at a given offset in a given member of that buffer sequence.
   * It uses `memcpy()` or similar for copying and makes as few calls to it as possible.  It is similar to the
   * `boost::asio::buffer_copy(ConstBufferSequence -> buffer)` function but also returns the logical
   * pointers just past the last byte copied, so that this can be resumed into some other target
   * buffer.  Of course there's a `buffer_copy(ConstBufferSequence -> MutableBufferSequence)` that
   * would do this, but it would be inefficient for us to have to create a `MutableBufferSequence`.
   *
   * @todo It would not be crazy to move copy_bytes_from_buf_seq() and copy_bytes_to_buf_seq() into util::Blob to make
   * it more widely available, though as of this writing there is no demand for this: perhaps a buffer-sequence
   * version of `util::Blob::emplace_copy()` and in the other direction.  In that case `util::Blob::assign_copy()`
   * and all single-buffer util::Blob methods should probably be similarly generalized (overloaded).
   *
   * @tparam Const_buffer_sequence
   *         See user-facing APIs.
   * @param cur_buf_it
   *        Pointer to iterator to the buffer in the buffer sequence from where to begin copying.
   *        When function returns, `*cur_buf_it` will point to the buffer containing the last byte
   *        that was copied.  This can be passed to this method again to resume copying where we
   *        left off.
   * @param pos_in_buf
   *        Pointer to offset, in bytes, from the start of `**cur_buf_it` from where to resume
   *        copying.  This must be `<= (*cur_buf_it)->size()`.  If they are equal (i.e., it points
   *        just past the last byte of `**cur_buf_it`), we will properly copy from the start of the
   *        next buffer.  (This can happen if the last call to the same method copied exactly
   *        through the end of a buffer in the buffer sequence, and then you call this method again
   *        with the resulting `*cur_buf_it` and `*pos_in_buf`.)
   * @param to_copy
   *        The number of bytes to copy.  The remainder of the buffer sequence (from
   *        `cur_buf_it`/`pos_in_buf` position) MUST contain at least that many bytes.  This means you
   *        MUST determine the total byte size of the overall buffer sequence before calling this
   *        function.
   * @param dest_buf
   *        (Pointer to) buffer containing `dest` location; data will be written into it.  Must be of
   *        sufficient size, or behavior is undefined.
   * @param dest
   *        Pointer to the location where bytes will be copied.
   */
  template<typename Const_buffer_sequence>
  static void copy_bytes_from_buf_seq(typename Const_buffer_sequence::const_iterator* cur_buf_it, size_t* pos_in_buf,
                                      size_t to_copy,
                                      util::Blob* dest_buf, util::Blob::Iterator dest);

  /**
   * Helper that copies, to a given raw memory buffer, a given number of bytes to a given
   * `Mutable_buffer_sequence`, starting at a given offset in a given member of that buffer sequence.
   * It uses `memcpy()` or similar for copying and makes as few calls to it as possible.  It is the opposite of
   * copy_bytes_from_buf_seq().
   *
   * @tparam Mutable_buffer_sequence
   *         See user-facing APIs.
   * @param cur_buf_it
   *        See copy_bytes_from_buf_seq().
   * @param pos_in_buf
   *        See copy_bytes_from_buf_seq().
   * @param to_copy
   *        See copy_bytes_from_buf_seq().
   * @param src_buf
   *        Buffer containing `src` location  Must be of sufficient size, or behavior is undefined.
   * @param src
   *        Pointer to the location from where bytes will be copied.
   */
  template<typename Mutable_buffer_sequence>
  static void copy_bytes_to_buf_seq(typename Mutable_buffer_sequence::const_iterator* cur_buf_it, size_t* pos_in_buf,
                                    size_t to_copy,
                                    const util::Blob& src_buf, util::Blob::Const_iterator src);
  // Data.

  /**
   * The max_data_size argument value that the user is predicting to use when calling
   * consume_buf_move(); or 0 if the user intends to instead use consume_bufs_copy().
   * This is explored in detail in the class doc header.  Basically this should be 0, when this
   * Socket_buffer is used as a Receive buffer; and it should be set to max-block-size, when this
   * Socket_buffer is used as a Send buffer.
   */
  const size_t m_block_size_hint;

  /**
   * The data in the buffer.  The logical byte sequence, as visible to the outside user of the
   * class, is obtained as follows: iterate through range [`m_q[0]->begin()`, `m_q[0]->end()`);
   * iterate through range [`m_q[1]->begin()`, `m_q[1]->end()`); ...; etc.  `shared_ptr`s, instead
   * of raw pointers, are used as elements just to avoid having to delete.  Pointers are stored
   * instead of direct vector objects to avoid any wholesale copying by the `deque` machinery.
   *
   * Why not just use a queue of bytes then?  The code would be far simpler.  Answer: the chief use
   * case is if Socket_buffer is Send buffer, so they'll set `block_size_hint == N` and use
   * feed_bufs_copy() whenever Peer_socket::send() is called.  Then we will always `reserve()` `N` bytes
   * in each `Blob` in #m_q; as `feed*()` is called, we will fill up the last `Blob` sequence until
   * all `N` bytes are exhausted, in which case we'll `push_back()` another `N`-capacity `Blob` sequence
   * and fill that one as needed, etc.  Since `send()` always copies bytes from user's buffer(s)
   * anyway, we're not adding any extra copying (only the allocation behavior is different than if
   * we'd used a simple byte buffer instead).  Now, when the Node needs to packetize a packet to
   * send, with `max-block-size == N`, it will use consume_buf_move() directly into the packet data
   * structure.  Since Node will avoid sending unless congestion window allows at least `N` bytes to
   * be sent, in consume_buf_move() it will use `max_data_size == N`.  Therefore, consume_buf_move()
   * will be constant-time due to performing a constant-time vector swap.  Thus exactly one data
   * copy is performed including the original `send()` call through sending bytes on the wire.
   */
  Queue m_q;

  /**
   * The total amount of data stored, in bytes, stored in this object.  @see data_size() for more
   * discussion.
   *
   * Invariant: `m_data_size` = sum(`m->size()`: for all `m` in `m_q`).
   */
  size_t m_data_size;
}; // class Socket_buffer

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Const_buffer_sequence>
size_t Socket_buffer::feed_bufs_copy(const Const_buffer_sequence& data, size_t max_data_size)
{
  using util::Blob;
  using bost::asio::buffer_sequence_begin;
  using bost::asio::buffer_sequence_end;
  using boost::asio::const_buffer;
  using boost::asio::buffer_size;
  using std::min;

  const size_t orig_data_size = m_data_size;

  if (m_block_size_hint == 0)
  {
    /* In this mode, we postpone all re-packetizing until -- possibly -- the data are consume*()d.
     * In other words we just allocate a new buffer for each buffer that comes in and make a
     * direct copy.  They're not even likely to use this method in this mode (probably they'll use
     * feed_buf_move() with a Receive buffer instead), but we provide this for completeness.  For
     * more info on the use of this mode, see class doc header or feed_buf_move(). */
    for (auto buf_data_ptr = buffer_sequence_begin(data),
              but_data_end_ptr = buffer_sequence_end(data);
         buf_data_ptr != buf_data_end_ptr; ++buf_data_ptr)
    {
      const auto& buf_data = *buf_data_ptr;
      if (m_data_size >= max_data_size)
      {
        return m_data_size - orig_data_size;
      }
      // else there is space in our buffer.

      // Copy entire buffer if possible, but don't exceed max_data_size bytes in total.
      const size_t to_copy = min(buf_data.size(), // Could be zero.  This is in BYTES.
                                 max_data_size - m_data_size); // Definitely >= 1 (checked above).
      if (to_copy == 0)
      {
        continue; // I guess this buffer was empty... nothing to do for it.
      }
      // else source buffer has data to copy.

      // Get the raw data pointer.
      const auto buf_start = static_cast<Blob::value_type const *>(buf_data.data());

      const Blob_ptr buf_copy(new Blob(get_logger()));
      // Make a byte blob copy from that raw memory.  Performance is highest possible (allocate, copy).
      buf_copy->assign_copy(const_buffer(buf_start, to_copy));
      m_q.push_back(buf_copy);

      // Accounting.
      m_data_size += to_copy;

      FLOW_LOG_TRACE("Socket_buffer/rcv [" << this << "]: data_size [" << m_data_size << "]; "
                     "buf_count [" << m_q.size() << "]; fed buffer "
                     "original/truncated size = [" << buf_data.size() << "/" << to_copy << "].");
      // Very verbose and CPU-intensive!
      FLOW_LOG_DATA("Buffer data [" << util::buffers_dump_string(buf_copy->const_buffer(), "", size_t(-1)) << "].");
    } // for (buf_data in data)
  }
  else // if (m_block_size_hint != 0)
  {
    /* In this, the more likely, use of this method we are attempting to maintain the following
     * invariant:
     *   - Each buffer in m_q is allocated to m_block_size_hint bytes and never reallocated
     *     (so one reserve() call).
     *   - Each buffer in m_q is at capacity, except m_q.back().
     *
     * If the user cooperates by only consuming via consume_buf_move(..., m_block_size_hint), then
     * we will be able to perform no copies in those consume_buf_move() calls, as we can always just
     * swap our byte vector m_q.front() with their empty byte vector (a constant time
     * operation).  This is the likely behavior if *this is used as a Send buffer, as Node will not
     * consume from *this unless it has at least m_block_size_hint (max-block-size in its terms) of
     * congestion window space.
     *
     * Of course we still have to copy here, but in the Send buffer use case that is an advertised
     * given: we must copy the buffer from user data structure as opposed to messing with it. */

    if (m_data_size >= max_data_size)
    {
      return 0; // Get rid of this pathological case now.
    }
    // else some buffer space left.
    size_t target_space_left = max_data_size - m_data_size;
    // This will hold how many bytes *this can still still add before exceeding max_data_size.

    size_t src_size_left = buffer_size(data);
    /* @todo That was a bit inefficient (has to traverse buffer sequence to add up individual sizes).  To avoid
     * it, copy_bytes_from_buf_seq() can be improved at the cost of complexity of its interface.
     * To put it in perspective, the # of buffers is probably ~0.1% of the number of bytes. */
    if (src_size_left == 0)
    {
      return 0; // Get rid of this pathological case now.
    }
    // else: src_size_left will hold how many bytes we still haven't copied from data.

    // Starting source location in memory is first buffer in `data`, byte 0.
    typename Const_buffer_sequence::const_iterator cur_buf_it = data.begin();
    size_t pos_in_buf = 0;
    do
    {
      /* The following invariants must hold at the start of this loop iteration:
       *
       *   -1- src_size_left != 0 (guaranteed initially above; and by while condition below).
       *   -2- target_space_left != 0 (ditto).
       *   -3- m_q.back().size() < m_block_size_hint (i.e., still space to write into it).
       *
       * Let us now guarantee -3-. */

      // Ensure there is some space in the trailing buffer (space up to m_block_size_hint bytes).
      if (m_q.empty() || (m_q.back()->size() == m_block_size_hint))
      {
        // Either the trailing buffer in queue is filled to capacity; or no trailing buffer exists. Make an all-new one.
        m_q.push_back(Blob_ptr(new Blob(get_logger())));

        // Reserve exactly N bytes of capacity (should be the only allocation for this member).
        m_q.back()->reserve(m_block_size_hint);
      }
      Blob& target_buf = *m_q.back();

      /* Now, decide how many bytes to copy into target_buf. This must be:
       *   - at most target_buf.capacity() - target_buf.size() (maintain exactly m_block_size_hint
       *     bytes allocated in target_buf);
       *   - at most target_space_left (do not exceed m_data_size bytes in *this total);
       *   - at most src_size_left (cannot copy more bytes than available in the source buffer sequence. */
      const size_t to_copy = min(m_block_size_hint - target_buf.size(), min(target_space_left, src_size_left));

      // Due to invariant mentioned above:
      assert(to_copy != 0);

      /* Do it.  Expand the buffer to receive to_copy bytes.  This is maximally performant.
       *
       * Then, walk along the buffer sequence, memcpy()-ing (etc.) maximally large chunks of data until we
       * got to_copy bytes total.  cur_buf_it and pos_in_buf keep track of where the next copy (if
       * any) will resume. */
      target_buf.resize(target_buf.size() + to_copy);
      copy_bytes_from_buf_seq<Const_buffer_sequence>(&cur_buf_it, &pos_in_buf, to_copy,
                                                     &target_buf, target_buf.end() - to_copy);

      target_space_left -= to_copy;
      src_size_left -= to_copy;
      m_data_size += to_copy;

      FLOW_LOG_TRACE("Socket_buffer/" << m_block_size_hint << " [" << this << "]: data_size [" << m_data_size << "]; "
                     "buf_count [" << m_q.size() << "]; "
                     "fed/total buffer size = [" << to_copy << '/' << target_buf.size() << "].");
      // Very verbose and CPU-intensive!
      FLOW_LOG_DATA("Buffer data post-append: "
                    "[" << util::buffers_dump_string(target_buf.const_buffer(), "", size_t(-1)) << "].");
    }
    while ((src_size_left != 0) && (target_space_left != 0));
  } // else if (m_block_size_hint != 0)

  return m_data_size - orig_data_size;
} // Socket_buffer::feed_bufs_copy()

template<typename Mutable_buffer_sequence>
size_t Socket_buffer::consume_bufs_copy(const Mutable_buffer_sequence& target_bufs)
{
  using util::Blob;
  using boost::asio::buffer_size;
  using boost::asio::const_buffer;
  using std::min;

  if (m_data_size == 0)
  {
    return 0; // Get rid of this pathological case right away.
  }
  // else

  const size_t orig_data_size = m_data_size;

  // @todo This is a bit inefficient: O(n) time, where n is the number of buffers in target_bufs.
  size_t target_size_left = buffer_size(target_bufs);

  if (target_size_left == 0)
  {
    return 0; // Another pathological case.
  }
  // else

  /* Now go through each buffer in m_q, until either we run out bytes or fill up the target
   * buffer.  For each buffer in m_q, copy it over into target_bufs. */
  Queue::iterator cur_src_it = m_q.begin();
  // Starting destination location in memory is first buffer in "target_bufs," byte 0.
  typename Mutable_buffer_sequence::const_iterator cur_buf_it = target_bufs.begin();
  size_t pos_in_buf = 0;
  do
  {
    Blob& src_bytes = **cur_src_it;

    // Normally copy all of current buffer -- unless that would overflow the target Mutable_buffer_sequence.
    const size_t to_copy = min(src_bytes.size(), target_size_left);

    /* Do it.  Walk along the buffer sequence, memcpy-ing (etc.) maximally large chunks of data into it
     * until we copy to_copy bytes total.  cur_buf_it and pos_in_buf keep track of where the next
     * copy (if any) will resume. */
    copy_bytes_to_buf_seq<Mutable_buffer_sequence>
      (&cur_buf_it, &pos_in_buf, to_copy, src_bytes, src_bytes.const_begin());

    m_data_size -= to_copy;
    target_size_left -= to_copy;

    FLOW_LOG_TRACE("Socket_buffer [" << this << "]: data_size [" << m_data_size << "]; "
                   "slow-consumed buffer of size/total [" << to_copy << '/' << src_bytes.size() << "].");
    // Very verbose and CPU-intensive!
    FLOW_LOG_DATA("Buffer data "
                  "[" << util::buffers_dump_string(const_buffer(src_bytes.const_begin(), to_copy),
                                                   "", size_t(-1))
                  << "].");

    if (to_copy == src_bytes.size())
    {
      // Copied entire source buffer -- move to the next one; this one will be erase()ed outside the loop.
      ++cur_src_it;
    }
    else
    {
      /* Was limited by the target buffer sequence size; copied only part of source buffer.
       * Therefore the loop will not execute again.  Erase the bytes that were just consumed, but
       * leave the others alone.  erase() outside the loop will not erase the buffer, as we will not
       * ++cur_src_it. */
      src_bytes.erase(src_bytes.begin(), src_bytes.begin() + to_copy);
    }
  }
  while ((target_size_left != 0) && (m_data_size != 0));

  // All buffers in this range were entirely copied, so now they should be deallocated/removed.
  m_q.erase(m_q.begin(), cur_src_it);

  FLOW_LOG_TRACE("Socket_buffer [" << this << "] consume finished: data_size [" << m_data_size << "]; "
                 "buf_count [" << m_q.size() << "].");

  return orig_data_size - m_data_size;
} // Socket_buffer::consume_bufs_copy()

template<typename Const_buffer_sequence>
void Socket_buffer::copy_bytes_from_buf_seq(typename Const_buffer_sequence::const_iterator* cur_buf_it,
                                            size_t* pos_in_buf, size_t to_copy,
                                            util::Blob* dest_buf,
                                            util::Blob::Iterator dest) // Static.
{
  using util::Blob;
  using boost::asio::const_buffer;
  using std::min;

  /* A pre-condition is that *pos_in_buf <= (**cur_buf_it).size().  If it's strictly less, then
   * we will copy into some position in **cur_buf_it.  If it is equal, then we need to go to the
   * next buffer -- actually the next non-empty buffer.  Why do we allow the == case?  Because if we
   * end the copying in an earlier invocation by copying the very last byte in a buffer, then the
   * final *pos_in_buf will indeed equal .size() of that buffer.  So basically, we leave
   * *pos_in_buf at the value it is after a copy, regardless of whether that's in the middle or end
   * of the buffer.  Then if we *need* to, we will go to the next non-empty buffer. */

  // Keep decreasing to_copy until all copied.  A pre-condition is that the buffer sequence has enough bytes total.
  while (to_copy != 0)
  {
    // At this point *pos_in_buf <= .size().  We must copy, so if they're equal, find next non-empty buffer.
    size_t cur_buf_size;
    while (*pos_in_buf == (cur_buf_size = (*cur_buf_it)->size()))
    {
      ++*cur_buf_it;
      *pos_in_buf = 0; // If the next buffer is empty, this will equal .size() (0).
    }
    // Since we are guaranteed buffer sequence has enough bytes to satisfy to_copy, **cur_buf_it must be a buffer.

    // Destination buffer may be larger than what we still have to copy.
    const size_t to_copy_in_buf = min(to_copy, cur_buf_size - *pos_in_buf);
    /* This is the reason for using this function instead of buffer_iterator (which would've been much easier -- but
     * this is way faster and probably uses memcpy() or similar). */
    dest = dest_buf->emplace_copy(dest,
                                  const_buffer(static_cast<Blob::Const_iterator>((*cur_buf_it)->data()) + *pos_in_buf,
                                               to_copy_in_buf));

    to_copy -= to_copy_in_buf;
    *pos_in_buf += to_copy_in_buf;
    // Note that if to_copy != 0 right now, then *pos_in_buf points just past end of current buffer.
  }
} // Socket_buffer::copy_bytes_from_buf_seq()

template<typename Mutable_buffer_sequence>
void Socket_buffer::copy_bytes_to_buf_seq(typename Mutable_buffer_sequence::const_iterator* cur_buf_it,
                                          size_t* pos_in_buf, size_t to_copy,
                                          const util::Blob& src_buf,
                                          util::Blob::Const_iterator src) // Static.
{
  /* This is almost exactly the same as copy_bytes_from_buf_seq() -- just in the other direction.
   * Basically memcpy() (or similar) arguments are switched.  So I'll keep comments minimal -- see the other
   * method.
   * @todo Code reuse somehow.  It's possible but may not be worth it given the small volume of code
   * involved. */

  using util::Blob;
  using boost::asio::mutable_buffer;
  using std::min;

  while (to_copy != 0)
  {
    size_t cur_buf_size;
    while (*pos_in_buf == (cur_buf_size = (*cur_buf_it)->size()))
    {
      ++*cur_buf_it;
      *pos_in_buf = 0;
    }

    const size_t to_copy_in_buf = min(to_copy, cur_buf_size - *pos_in_buf);
    src = src_buf.sub_copy(src,
                           mutable_buffer(static_cast<Blob::Iterator>((*cur_buf_it)->data()) + *pos_in_buf,
                                          to_copy_in_buf));

    to_copy -= to_copy_in_buf;
    *pos_in_buf += to_copy_in_buf;
  }
} // Socket_buffer::copy_bytes_to_buf_seq()

} // namespace flow::net_flow
