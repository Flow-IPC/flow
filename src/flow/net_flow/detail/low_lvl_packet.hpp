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
#include "flow/net_flow/detail/seq_num.hpp"
#include "flow/net_flow/net_flow_fwd.hpp"
#include "flow/log/log.hpp"
#include "flow/util/util.hpp"
#include "flow/util/shared_ptr_alias_holder.hpp"
#include "flow/util/blob.hpp"
#include <boost/endian.hpp>
#include <limits>
#include <type_traits>

namespace flow::net_flow
{

/**
 * Internal `net_flow` `struct` that encapsulates the Flow-protocol low-level packet structure and serves as
 * the super-type for all specific packet types, represented by derived `struct`s like Ack_packet, Rst_packet,
 * etc.
 *
 * This `struct` and its sub-`struct` hierarchy are not too complex but nevertheless are somewhat unorthodox,
 * combining a few different paradigms.  So it's worth explaining these paradigms.
 *
 * ### Paradigm: Data store ###
 * This is a `struct` holding the conceptual contents of a packet.  Low_lvl_packet stores common values like
 * #m_opt_rexmit_on, #m_src_port, and a few more.  Each sub-`struct` stores further values specific to each
 * packet type; for example, as of this writing, Data_packet::m_data contains the data payload of a DATA
 * packet; this would not apply, for example, to another sub-`struct` like Rst_packet, because an RST stores
 * no binary blob like that.
 *
 * Since it's a simple data store, the applicable data members (in this `struct` and the sub-types) are
 * both public AND non-`const`.  They can be read and written at will, thus changing or reading the
 * logical contents of the packet.
 *
 * ### Paradigm: Polymorphic type ###
 * While a simple data store, it also represents a hierarchy of possible packet types, and indeed a number
 * of important polymorphic operations are included.  Some are private/internal, and some are public.
 * A public method may not appear polymorphic by its signature, but in that case usually its implementation
 * uses non-public `virtual` methods to accomplish its tasks.  Specifically, the key APIs are:
 *
 *   - `create_uninit_packet<T>()`: This creates an uninitialized sub-object of Low_lvl_packet, of actual
 *     type `T`, where `T` derives from Low_lvl_packet.  This returns a `shared_ptr<T>`, and any actual
 *     constructor used to implement this factory method is non-public, so the only way for a user to
 *     create a Low_lvl_packet is to get at a ref-counted pointer to it -- not a raw, unprotected pointer.  If storing
 *     only `shared_ptr<T>` or `Ptr` values, deletion is not something to worry about, as it will happen
 *     automatically.
 *     - Same with the other factory methods described just below.  Direct access to `Low_lvl_packet*` or
 *       `T*`, where `T` derives from Low_lvl_packet, is not allowed (but this is not enforced at compile time
 *       and, at best, via `assert()`s at runtime).
 *   - create_from_raw_data_packet(): Given a raw serialized packet, this factory method constructs
 *     a Low_lvl_packet of the appropriate polymorphic sub-type, filling it at all levels with the data
 *     deserialized for the input binary blob.  Internally, it uses some private, `virtual` action to get this
 *     to work.  At any rate, this method is used to crete a logical representation of a packet off the wire.
 *   - serialize_to_raw_data(): The opposite operation: convert existing object to raw data sendable over the wire.
 *     This uses a high-performance scatter-gather paradigm of producing a sequence of pointers and lengths to
 *     already existing data areas, avoiding copying.  This thing is `virtual` and applies to any Low_lvl_packet
 *     sub-object.
 *
 * (Reminder: As with all polymorphism and `shared_ptr`, you may need `static_pointer_cast<>` to up- and down-cast
 * between Low_lvl_packet and subclasses.  Also `Low_lvl_packet::ptr_cast()` up-casts even syntactically-sugarier-ly.)
 *
 * The two basic flows are:
 *   - Call `create_uninit_packet<T>()` to construct a sub-`struct` of Low_lvl_packet, returned as a ref-counted
 *     pointer to `T`.  Fill out all the fields manually, accessing object via aforemetioned smart pointer.
 *     Call serialize_to_raw_data().  Pass result of latter to a boost.asio network-sending routine
 *     to send over wire.  Ensure you pass a `Ptr` to the completion handler passed to that routine, so that
 *     the Low_lvl_packet survives while being sent.  The handler can let the `Ptr` go out of scope, likely
 *     `delete`ing the Low_lvl_packet.
 *   - Receive raw packet over wire.  Call create_from_raw_data_packet() to yield a Low_lvl_packet::Ptr pointing
 *     to new Low_lvl_packet sub-object with all fields filled out.  Read the fields as needed to process the
 *     packet; store the `Ptr` if necessary; eventually removing it from all data structures will drop ref-count
 *     to zero, and it will be auto-`delete`d.
 *
 * ### Other utlities ###
 * A number of (typically polymorphic) facilities exist, mainly to log these objects to `ostream`s, including
 * when logging with FLOW_LOG_TRACE() and FLOW_LOG_DATA().
 *
 *   - #m_verbose_ostream_manip and #m_concise_ostream_manip are `const` data members that can be output
 *     via the usual `<<` operator to `ostream`s in order to output verbose and concise descriptions
 *     of any Low_lvl_packet (of any sub-`struct` type, polymorphically).
 *   - #m_type_ostream_manip can be similarly used to output just the packet type of any given
 *     Low_lvl_packet (producing strings like `"ACK"` or `"RST"`).
 *
 * The implementation of these is typically some polymorphic `virtual` magic, but the use is straightforward using
 * these `ostream` manipulator data members.
 *
 * Implementation notes
 * --------------------
 *
 * ### History of approaches ###
 * Originally the implementation used the boost.serialization library for packet serialization, but
 * it turned out to consume way too much CPU time which consequently limited the maximum
 * throughput at high speeds.  The next implementation used the traditional approach based on
 * "packed" structures (structures without any padding), in one big `struct Low_lvl_packet`.
 * However, I deemed the latter setup too C-like (yet simultaneously
 * not C-like enough, in that no unions were used, hence RAM was wasted despite the C-like approach).
 * Ultimately, the current version of the implementation came about, with its polymorphic `struct` hierarchy
 * and abstract Low_lvl_packet `struct` (which, in the previous iteration, simply contained everything
 * about every type, complete with an `enum Type` instead of using C++'s built-in `typeid`, as the current
 * version uses).
 *
 * While the above evolution was more about coding style philosophy (from a single type and a type selector
 * `enum` to a polymorphic hierarchy and `typeid`), the other evolution concerned the way serialization
 * (for sending over the write) worked.  At first, as mentioned, there were the logical values (which were
 * public and non-`const`), and there were private packed `struct`s into which the logical values were
 * placed (with transformation for endianness and other things) at serialization time; finally then assembling
 * a newly allocated single binary buffer, those packed `struct`s copied into it.  Not bad -- rather verbose
 * having all those types defined, but not bad -- but it did involve a new buffer and linear-time copying into
 * that buffer.  This seemed like a shame.
 *
 * This led to the final improvement, which was to turn the serialization function into one that generates
 * a `boost::asio::const_buffer_sequence`, basically a sequence of pointers and associated lengths of areas
 * already present in memory.  In most cases (with a couple small exceptions), the stored public data members
 * (e.g., #m_src_port and Data_packet::m_data) could simply have their addresses and lengths taken and placed
 * into the `const_buffer_sequence` -- no buffer copying involved.  This should achieve a nice performance
 * improvement.  The removal of things like `struct Common_header` and `struct Ack_header` is a good bonus
 * coding-wise, too.
 *
 * (There is a caveat there.  While no issue for the true performance culprits, namely Data_packet::m_data,
 * the various multi-byte integer values like #m_src_port technically have to undergo endianness conversion
 * during serialization.  So technically simply taking an address and size is not enough; the value needs
 * to be flipped *potentially*.  In reality, though, all relevant architectures are such that we chose
 * identical network byte order as memory byte order, making no flipping necessary in practice.  Of course,
 * if we spread to an architecture where this is not true, everything will explode.  However, a compile-time
 * assertion would warn us of this, and a nearby comment will instruct on how to deal with the problem, in the
 * unlikely case it were to appear.  [Little-endian is rather universal these days.])
 *
 * ### Packed structures, stored integers, and alignment ###
 * When 2 or more contiguous data members are used in serializing (serialize_to_raw_data() or overridden
 * version), they are packed together via `"#pragma pack"`.  (Note that this affect entire `struct`s at a time
 * only; hence we need to use some anonymously-typed grouping `struct`s named `m_packed`.)
 *
 * Numeric values are stored as unsigned integers, which is the most portable choice for serializing.
 * Booleans are serialized as bytes for compactness.
 *
 * Some effort is made to keep fields aligned along word edges when serializing.  There is not too much
 * of that as of this writing, but we should remain vigilant as packet formats become more complex over time.
 *
 * @todo With C++11, some lines of code could be eliminated by using `using` for Low_lvl_packet sub-types
 * to inherit the constructor(s) of Low_lvl_packet.  (For some of the sub-types an explicit constructor
 * would still be necessary, but it would just shadow the inherited one, which is fine and still saves lines
 * in the other sub-types.)  However, for now I've left it as-is, partially to be friendly to the Doxygen
 * documentation generator, and partially to make the interface easy to read.  Still, it may be better the other
 * way.
 */
struct Low_lvl_packet :
  public util::Null_interface,
  // Endow us with shared_ptr<>s ::Ptr and ::Const_ptr (syntactic sugar).
  public util::Shared_ptr_alias_holder<boost::shared_ptr<Low_lvl_packet>>,
  public log::Log_context,
  private boost::noncopyable
{
  // Types.

  /**
   * Short-hand for boost.asio immutable buffer, which essentially is a pointer to and length of a memory area.
   *
   * @todo Consider moving alias Low_lvl_packet::Const_buffer to `util` namespace or even outside it, as it is
   * used commonly (or `boost::asio::const_buffer` is used where `Const_buffer` could be used for readability).
   */
  using Const_buffer = boost::asio::const_buffer;

  /**
   * Short-hand for sequence of immutable buffers; i.e., a sequence of 1 or more scattered areas in memory.
   * This is a model of the `ConstBufferSequence` model in boost.asio, which means a `const` reference to this
   * can be passed to boost.asio scatter-gathering send functions such as `Udp_socket::async_send_to()`;
   * the scattered buffers represented by an instance of this type will be, at high performance, gathered
   * into a single UDP datagram and sent over the wire if possible.
   */
  using Const_buffer_sequence = std::vector<Const_buffer>;

  /// Type used for `m_security_token` member of a couple different packet types.
  using security_token_t = uint64_t;

  /**
   * Type used to store the retransmission count in DATA and ACK packets.
   *
   * Why 8 bits?  This handles up 255 retransmissions, which is long after we'd kill the connection
   * anyway.
   */
  using rexmit_id_t = uint8_t;

  /// Type used to store the size of `m_rcv_wnd` member in a couple of different packet types.
  using rcv_wnd_t = uint32_t;

  /* Data: These are actual Common Header payload.
   *
   * These are common to all packet types, since we are super-`struct` to all actual packet types.
   *
   * Note the implicit data field typeid(*this) which identifies the actual type of this packet.
   * (C++ subtlety: typeid(this) will just yield typeid(Low_lvl_packet), which isn't that useful;
   * but typeid(*this) will actually get you typeid(Data_packet), typeid(Ack_packet), etc. */

  /**
   * Option indicating whether this connection is using retransmission or not.  This value must not
   * change over the course of a connection and is provided in each packet for simpler
   * deserialization (so that the latter can proceed completely without having to check the
   * connection properties).
   *
   * For a given connection handshake, the SYN receiver should either disconnect/RST or respond with
   * SYN_ACK.  If it responds with SYN_ACK, it indicates agreement to abide by this option for the
   * rest of the connection.
   */
  bool m_opt_rexmit_on;

#pragma pack(push, 1)
  /// Packed group affected by `#pragma pack`.
  struct
  {
    // Data.

    /// Flow-protocol port # of socket in sending Node.
    flow_port_t m_src_port;
    /// Flow-protocol port # of socket in receiving Node.
    flow_port_t m_dst_port;
  } m_packed;
#pragma pack(pop)

  // Type checks.
  static_assert(std::numeric_limits<flow_port_t>::is_integer
                  && (!std::numeric_limits<flow_port_t>::is_signed),
                "Ports are non-negative integers.");

  // Data: These are peripheral (not actual packet payload).

  /// `ostream` manipulator (argument to `ostream <<`) that will output packet's type ("ACK", "RST", etc.).
  const Function<std::ostream& (std::ostream&)> m_type_ostream_manip;

  /// `ostream` manipulator (argument to `ostream <<`) that will output packet info suitable for DATA log level.
  const Function<std::ostream& (std::ostream&)> m_verbose_ostream_manip;

  /// `ostream` manipulator (argument to `ostream <<`) that will output packet info suitable for TRACE log level.
  const Function<std::ostream& (std::ostream&)> m_concise_ostream_manip;

  // Methods.

  /**
   * Constructs packet with uninitialized (essentially random) values, of the Low_lvl_packet sub-type specified
   * as the template parameter (Ack_packet, Rst_packet, etc.).  Since any constructor is not public, this is
   * the only way to instantiate a blank Low_lvl_packet sub-object.
   *
   * Compiler template parameter inference should make the following work, so the template parameter can be omitted.:
   *
   *   ~~~
   *   shared_ptr<Ack_packet> = create_uninit_packet(get_logger());
   *   // ^-- Low_lvl_packet_sub was inferred to be Ack_packet based on the left hand side of the assignment.
   *   ~~~
   *
   * @tparam Low_lvl_packet_sub
   *         Any type deriving from Low_lvl_packet.
   * @param logger_ptr
   *        Logger to use subsequently in the new object.
   * @return Ref-counted pointer to newly created object of the specified type.
   *         If you need an up-cast pointer, use create_uninit_packet_base().
   */
  template<typename Low_lvl_packet_sub>
  static boost::shared_ptr<Low_lvl_packet_sub> create_uninit_packet(log::Logger* logger_ptr);

  /**
   * A simple convenience method that casts the result of create_uninit_packet() from `shared_ptr<T>`, where `T` is a
   * sub-type of Low_lvl_packet, to `shared_ptr<Low_lvl_packet>` a/k/a Ptr.
   *
   * @param logger_ptr
   *        See create_uninit_packet().
   * @return See create_uninit_packet() (but cast to a type compatible with the polymorphic base type).
   */
  template<typename Low_lvl_packet_sub>
  static Ptr create_uninit_packet_base(log::Logger* logger_ptr);

  /**
   * Constructs packet on the heap with values determined by the given raw binary data as presumably
   * received from the wire and originally serialized by a compatible serializing Node.  The returned
   * value is a reference-counted pointer with a reference count of 1 (i.e., no other references to
   * underlying object have been saved anywhere by the time method returns).
   *
   * It is the opposite of serialize_to_raw_data() but not symmetrically efficient; where the latter
   * produces a sequence of pointers/lengths of already present areas of memory (no copying), the present method
   * copies the raw data into a new structure and is thus less efficient.
   *
   * Suppose `ptr` is returned.  Then `*ptr` is of polymorphic type `Low_lvl_packet*` but actually is
   * of some specific sub-type (like `Ack_packet*`).  `typeid(*ptr)` can be used to determine the exact type.
   * In particular one can write things like: `bool is_ack = typeid(*ptr) == typeid(Ack_packet)`.
   * Of course, in many cases one may call one of a number of virtual methods of Low_lvl_packet to get
   * type-specific polymorphic behavior.
   *
   * @see m_type_ostream_manip() for easy way to output human-readable version of `typeid(*ptr).name()`, where
   *      `ptr` is the value returned.
   * @param raw_packet
   *        Pointer to entire packet raw buffer as received over the wire.
   *        Upon return, the state of `*raw_packet` is not known; and caller retains
   *        ownership of it (e.g., can read another datagram into it if desired).  As of this writing it will either
   *        remain unchanged or be emptied (via a move elsewhere) -- but ideally code without relying on either specific
   *        outcome.
   * @param prefer_no_move
   *        If `true`, the method should (if at all reasonable) not alter `raw_packet->capacity()` (in particular
   *        it should not use move semantics to send its contents to another data structure); otherwise
   *        it should be free to do so (i.e., if it considers doing so beneficial for performance reasons).
   * @param logger_ptr
   *        Will be used to log within this method as well as be saved as the
   *        `Logger*` in the new Low_lvl_packet created and returned (if any).
   * @return A null pointer on failure; otherwise pointer to an object of some sub-type of Low_lvl_packet.
   */
  static Ptr create_from_raw_data_packet(log::Logger* logger_ptr, util::Blob* raw_packet, bool prefer_no_move);

  /**
   * Serializes the current logical packet data from `*this` into the given `Const_buffer_sequence`,
   * which is a sequence of pointers and lengths of existing scattered areas in memory, presumably
   * for transmission over the wire to a compatible serializing Node.
   *
   * It is the opposite of create_from_raw_data_packet() but not symmetrically efficient; where the latter
   * copies the raw data into a new structure, the present method produces a sequence of pointers/lengths
   * of already present areas of memory (no copying) and is thus more efficient.
   *
   * The input `*raw_bufs` is appended to and is *not* cleared by this method.  Thus one may use this
   * method to produce a larger buffer sequence of which the serialization of `*this` is only a part
   * (possibly in the middle somewhere).
   *
   * Behavior is undefined if one modifies `*this` after calling the present method.  It is safer to
   * run this on a `const Low_lvl_packet` rather than a mutable Low_lvl_packet.
   *
   * @warning This efficiency comes with an important caveat: the generated additions to `*raw_bufs` will
   *          remain valid *only* while `*this` exists.  If its destructor runs, the buffers added here
   *          will become invalid, and accessing them will result in undefined behavior.
   *
   * @param raw_bufs
   *        Pointer to the sequence of `Const_buffer`s into which we will append pointers/lengths
   *        of serialized data.
   * @return Size in bytes of the data references to which have been appended to `*raw_bufs`;
   *         Already present data are not included.
   */
  virtual size_t serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const = 0;

  /**
   * Identical to serialize_to_raw_data() but adds log-level-appropriate logging after the operation.
   *
   * @param raw_bufs
   *        See serialize_to_raw_data().
   * @return See serialize_to_raw_data().
   */
  size_t serialize_to_raw_data_and_log(Const_buffer_sequence* raw_bufs) const;

  /**
   * Returns a brief (a few characters) string description of the given packet type given as
   * `type_index(typeid(p))`, where `p` is a reference to an instance of a concrete Low_lvl_packet sub-type.
   *
   * @param type_id
   *        See above.  If the `p` points to a value of some other type, behavior is undefined.
   * @return Reference to a string without newlines or other whitespace: "DATA", "ACK", "SYN_ACK_ACK", etc.
   */
  static const std::string& type_id_to_str(const std::type_index& type_id);

protected:
  // Constructors/destructor.

  /**
   * Constructs packet with uninitialized (essentially random) values.  This is not public, because
   * a Low_lvl_packet is meaningless without being of some specific sub-type referring to an actual
   * packet (e.g., Ack_packet, Rst_packet, etc.).  (The presence of pure virtual methods would probably
   * accomplish this too, but it's nice to be explicit.)
   *
   * @param logger_ptr
   *        Logger to use subsequently.
   */
  explicit Low_lvl_packet(log::Logger* logger_ptr);

  // Methods.

  /**
   * Helper for serialize_to_raw_data() implementations in sub-types that encodes the header common to all
   * packet types, starting with the packet type ID leading that header.
   *
   * @param raw_bufs
   *        See serialize_to_raw_data().  Header fields are encoded and appended to this.
   * @return See serialize_to_raw_data().  The total # of bytes encoded and appended to `*raw_bufs` by the call.
   */
  size_t serialize_common_header_to_raw_data(Const_buffer_sequence* raw_bufs) const;

  /**
   * Writes a multi-line representation of `*this` to an output stream.
   *
   * Unless the sub-type needs to add no information of its own (as of this writing, the case for Rst_packet),
   * this implementation (of this virtual method) is only present as a utility for the sub-types.
   * The sub-type is recommended to first call this implementation; then add type-specific lines
   * afterwards, ending with a newline.  Following this recommendation will result in uniform, decent-looking
   * overall output.
   *
   * @note This is protected, because it is meant only to be used in the implementation of
   *       the `ostream` manipulators #m_verbose_ostream_manip and #m_concise_ostream_manip, which
   *       are public and to be used by users of this type.
   * @param os
   *        The output stream to use.  Normally I'd make this a pointer, but this non-constant reference is
   *        fairly typical in STL and the world in general, for `ostream` in particular.
   * @param verbose
   *        If `false`, output suitable for TRACE level logging is produced; otherwise for DATA level logging.
   * @return `os`.
   */
  virtual std::ostream& to_ostream(std::ostream& os, bool verbose = false) const;

  /**
   * Returns `true`, at compile time, if and only if the native memory representation is big-endian, meaning,
   * for example, the value `uint32_t(1)` is stored as the bytes, in order, 0x00 0x00 0x00 0x01, and not the reverse.
   * Can be used in a compile-time check such as `static_assert()`.
   *
   * @return See above.
   */
  static constexpr bool native_is_big_endian();

private:
  // Types.

#pragma pack(push, 1)

  /**
   * Helper data store type for storing binary data needed by serialize_to_raw_data(), when certains bits
   * are not already represented by the public data members present in `struct` Low_lvl_packet.
   *
   * The need for this is somewhat subtle and is explained fully inside Low_lvl_packet::serialize_to_raw_data(),
   * so I won't go into it here.  Also see each data member's doc header.
   */
  struct Aux_raw_data :
    private boost::noncopyable
  {
    /**
     * This is the serialized version of the multi-byte `bool` Low_lvl_packet::m_opt_rexmit_on.
     * Since the latter is of a more civilized `bool` type, and we need a single byte version, this here
     * is set to the obvious encoding of the `bool`, when Low_lvl_packet::serialize_to_raw_data() needs to
     * serialize said `bool`.
     */
    uint8_t m_opt_rexmit_on_raw;

    /**
     * Unused space reserved for future use, ensuring correct alignment of other fields and
     * headers.  Currently it must be always set to zero.  Since Low_lvl_packet has no need of a public
     * data member like this, we keep it in here, as needed for Low_lvl_packet::serialize_to_raw_data().
     */
    const uint16_t m_reserved2;

    // Constructors/destructor.

    /// Constructs a mostly-uninitialized object, except for the `const` member(s), if any.
    explicit Aux_raw_data();
  };

#pragma pack(pop)

  /**
   * A simple, unmodifiable data store that contains the properties unique to each packet type a/k/a
   * concrete sub-type of Low_lvl_packet.
   */
  struct Packet_type_info // Cannot use boost::noncopyable, as that disables direct member initialization.
  {
    /// The type ID value, in serialized order, to be used in each serialization of all packets of this type.
    const uint8_t m_raw_type_id;

    /// The brief string representation of this packet type, suitable for Low_lvl_packet::type_id_to_str().
    const std::string m_type_id_str;
  };

  // Methods.

  /**
   * Writes a brief representation of `typeid(*this)` -- i.e., the packet type (ACK, RST, etc.) --
   * to an output stream.  See also type_id_to_str().
   *
   * @note This is private, because it is meant only to be used in the implementation of
   *       the `ostream` manipulator #m_type_ostream_manip, which is public and to be used by users of this type.
   * @param os
   *        The output stream to use.  Normally I'd make this a pointer, but this non-constant reference is
   *        fairly typical in STL and the world in general, for `ostream` in particular.
   * @return `os`.
   */
  std::ostream& type_to_ostream(std::ostream& os) const;

  /**
   * Helper that looks up the Packet_type_info::m_raw_type_id value for the given `typeid(p)`, where `p` refers
   * to an instance of a concrete sub-type of Low_lvl_packet.  Note that is returns a reference, and indeed the referred
   * to memory will remain valid and unmodified throughout the program's runtime.  Therefore, it can be used
   * for serialization without copying.
   *
   * ### Implementation note ###
   * It would be nice to make this `constexpr`, as then it can be used in `switch()` statement
   * conditionals.  Unfortunately, at least as of C++17, this is not possible with our current implementation:
   * #S_NATIVE_TYPE_ID_TO_PACKET_TYPE_INFO is an `unordered_map` (as of this writing), and that is not a "literal"
   * type (in other words, lookups in it are not done at compile time, even when for a human it would seem clearly
   * possible... iterators and such are involved, and that goes beyond what `constexpr` evaluation can do).
   *
   * @param type_id
   *        See type_id_to_str().
   * @return Reference to a constant area of memory.
   */
  static const uint8_t& type_id_native_to_raw(const std::type_info& type_id);

  /**
   * `virtual` helper for create_from_raw_data_packet() that fills out the fields of `*this` that are *not*
   * in Low_lvl_packet but rather in the sub-type.
   *
   * `*raw_buf` must represent the area of create_from_raw_data_packet()'s `*raw_buf` argument immediately
   * following the Common Header, which is the area deserialized into Low_lvl_packet proper.  Hence
   * `*raw_buf` must point to and contain the length of the rest of that input buffer.  The present method
   * must then deserialize it into the sub-object's own fields.
   *
   * @param raw_buf
   *        Pointer to immutable buffer just past the input raw packet data's Common Header area.
   *        Note that while the underlying area is immutable (hence the type #Const_buffer!),
   *        `*raw_buf` itself may be modified (i.e., the area to which it will refer on exit is undefined).
   * @param prefer_no_move
   *        See create_from_raw_data_packet().
   * @param raw_packet
   *        See create_from_raw_data_packet().  `raw_buf` must start somewhere within it and be
   *        sized to go exactly to its end.
   * @return `false` if create_from_raw_data_packet() should return `Ptr{}` (error); `true` if
   *         deserialization successful.
   */
  virtual bool deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                                   bool prefer_no_move, util::Blob* raw_packet) = 0;

  // Constants.

  /**
   * Mapping from native `typeid()`, a/k/a packet type (for all possible packet types), to the set of properties of
   * that packet type.  The keys are every possible value of `type_index(typeid(p))`, where `p` is a reference
   * to an instance of any concrete Low_lvl_packet sub-type.
   *
   * ### Implementation note ###
   * An alternative way to implement this mechanism (as of this writing, raw type IDs and brief
   * string descriptions per packet type) is to use `virtual` liberally, combined with a `switch` if needed.  (Note,
   * also, that a way to index -- such as in an `unordered` container -- by
   * packet type is necessary elsewhere.  We use `type_index(typeid()))` now, but a [worse] alternative is to have
   * `virtual` type IDs returned for each Low_lvl_packet sub-type.)  I could explain
   * at length all the reasons why the chosen way is superior, but let me just give you the conclusion: I originally
   * tried it the `virtual/switch` way.  Instead of the raw type IDs and
   * string descriptions being conveniently encapsulated in the present map (where there's little to no chance of
   * conflicting values), they were distributed across `virtual` implementations.  This was less maintainable; nor did
   * it improve OO style, since the raw type ID values (at least) were
   * still inter-dependent (were not allowed to conflict) in any case.
   * The present solution does not pretend the actual contents of these values are of no interest to Low_lvl_packet
   * itself (as opposed to the sub-types), and in not having to pretend this, all related code is quite a bit more
   * compact and a bit more maintainable as well.
   */
  static const boost::unordered_map<std::type_index, Packet_type_info> S_NATIVE_TYPE_ID_TO_PACKET_TYPE_INFO;

  // Data.

  /**
   * Auxilliary data area necessary for serialize_to_raw_data() to work.  This is explained in doc header
   * for Aux_raw_data.
   *
   * It is declared `mutable`, because the conceptual state contained within a Low_lvl_packet is expressed
   * via its public non-`const` data members.  #m_aux_raw_data is merely a helper data store for
   * the `const` method serialize_to_raw_data().  Since that method is `const`, this should be `mutable`,
   * like a performance cache of sorts, only not exactly.
   */
  mutable Aux_raw_data m_aux_raw_data;
}; // struct Low_lvl_packet

/**
 * Internal `net_flow` `struct` that encapsulates the Flow-protocol low-level SYN packet.
 * See Low_lvl_packet doc header for information common to all low-level packets including this one.
 *
 * A SYN packet is sent by the actively connecting peer to a passively listening server socket.
 * Thus a SYN arises from the Node that performs `Node::connect...()`.  In response, a Syn_ack_packet
 * is expected.
 */
struct Syn_packet : public Low_lvl_packet
{
  // Data.

  /**
   * The Initial Sequence Number (ISN) of the sequence number line that the sender of this SYN will be
   * using in `Data_packet`s over this connection, if it succeeds.  All bytes actually sent by the
   * SYN sender will start with this ISN + 1 and increment by 1 for each byte in the stream further.
   * A retransmission of a given datum starting with sequence number S will still start with sequence number
   * S.
   */
  Sequence_number m_init_seq_num;

  /**
   * Arbitrary serialized user-supplied metadata to send in SYN, where it can be deserialized by
   * the user on the other side.
   *
   * @todo Possibly eliminate this, since NetFlow is reliable; the metadata can just be sent explicitly by the
   * user once the connection is established.  However, for now reliability is optional.
   */
  util::Blob m_serialized_metadata;

  // Type checks.
  static_assert(std::numeric_limits<Sequence_number::seq_num_t>::is_integer
                  && (!std::numeric_limits<Sequence_number::seq_num_t>::is_signed),
                "Raw sequence numbers are non-negative integers.");

  /// In serialized packet, the type ID byte identifying this as a SYN packet.  Must not equal any other packet type's.
  static const uint8_t S_RAW_TYPE_ID;

  // Methods.

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param raw_bufs
   *        See Low_lvl_packet::serialize_to_raw_data().
   * @return See Low_lvl_packet::serialize_to_raw_data().
   */
  size_t serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const override;

private:
  // Friends.

  /// Friend of Syn_packet: For access to private constructor `Syn_packet(Logger*)`.
  // @todo Doxygen complains unless I make the above a Doxygen comment.  In other places it doesn't complain.  Fix...?
  friend boost::shared_ptr<Syn_packet> Low_lvl_packet::create_uninit_packet<Syn_packet>(log::Logger*);

  // Constructors/destructor.

  /**
   * The implementation of Low_lvl_packet::create_uninit_packet() for this sub-type of Low_lvl_packet.
   *
   * @param logger_ptr
   *        Logger to use subsequently.
   */
  explicit Syn_packet(log::Logger* logger_ptr);

  // Methods.

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param os
   *        See Low_lvl_packet::to_ostream().
   * @param verbose
   *        See Low_lvl_packet::to_ostream().
   * @return `os`.
   */
  std::ostream& to_ostream(std::ostream& os, bool verbose) const override;

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param raw_buf
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @param prefer_no_move
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @param raw_packet
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @return See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   */
  bool deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                           bool prefer_no_move, util::Blob* raw_packet) override;
}; // struct Syn_packet

/**
 * Internal `net_flow` `struct` that encapsulates the Flow-protocol low-level SYN_ACK packet.
 * See Low_lvl_packet doc header for information common to all low-level packets including this one.
 *
 * A SYN_ACK packet is sent by the passively listening server socket in response to a valid received SYN.
 * Thus a SYN_ACK arises from the Node that performs Node::listen() and receives SYN.  In response, a
 * Syn_ack_ack_packet is expected to complete the 3-way handshake and establish a Peer_socket to Peer_socket
 * connection.
 */
struct Syn_ack_packet : public Low_lvl_packet
{
  // Data.

  /**
   * Same meaning as Syn_packet::m_init_seq_num but applied to the essentially independent opposite
   * traffic direction of the full-duplex connection being established.
   */
  Sequence_number m_init_seq_num;

#pragma pack(push, 1)
  /// Packed group affected by `#pragma pack`.
  struct
  {
    /**
     * Random security token used during SYN_ACK-SYN_ACK_ACK.  For a given connection handshake, the SYN_ACK_ACK
     * receiver ensures that `Syn_ack_ack_packet` `m_security_token` it receives is equal to the original
     * one it had sent (this #m_security_token here).
     */
    security_token_t m_security_token;

    /**
     * Number of DATA payload bytes the sender of this packet would accept into its Receive buffer,
     * without dropping, at the moment this packet was generated to send.  This information is
     * piggybacked into ACK (and SYN_ACK/SYN_ACK_ACK) packets.
     *
     * @todo We could be similar to TCP by opportunistically sending rcv_wnd in other packet types,
     * namely Data_packet.  However this would only help in connections with heavy 2-way traffic.
     * Personally I would then prefer to create a new packet type instead, Rcv_wnd_packet, and also
     * implement some generalized "packet combo" scheme which would allow to piggy-back arbitrary
     * packet types together into a single packet; and then we'd dissociate
     * ACK/SYN_ACK/SYN_ACK_ACK from rcv_wnd.
     */
    rcv_wnd_t m_rcv_wnd;
  } m_packed;
#pragma pack(pop)

  // Type checks.
  static_assert(std::numeric_limits<Sequence_number::seq_num_t>::is_integer
                  && (!std::numeric_limits<Sequence_number::seq_num_t>::is_signed),
                "Raw sequence numbers are non-negative integers.");
  static_assert(std::numeric_limits<security_token_t>::is_integer
                  && (!std::numeric_limits<security_token_t>::is_signed),
                "Security tokens are non-negative integers.");
  static_assert(std::numeric_limits<rcv_wnd_t>::is_integer
                  && (!std::numeric_limits<rcv_wnd_t>::is_signed),
                "rcv_wnd values are non-negative integers.");

  /// In serialized packet, the type ID byte identifying this as a SYN_ACK.  Must not equal any other packet type's.
  static const uint8_t S_RAW_TYPE_ID;

  // Methods.

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param raw_bufs
   *        See Low_lvl_packet::serialize_to_raw_data().
   * @return See Low_lvl_packet::serialize_to_raw_data().
   */
  size_t serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const override;

private:
  // Friends.

  /// Friend of Syn_ack_packet: For access to private constructor `Syn_ack_packet(Logger*)`.
  // @todo Doxygen complains unless I make the above a Doxygen comment.  In other places it doesn't complain.  Fix...?
  friend boost::shared_ptr<Syn_ack_packet> Low_lvl_packet::create_uninit_packet<Syn_ack_packet>(log::Logger*);

  // Constructors/destructor.

  /**
   * The implementation of Low_lvl_packet::create_uninit_packet() for this sub-type of Low_lvl_packet.
   *
   * @param logger_ptr
   *        Logger to use subsequently.
   */
  explicit Syn_ack_packet(log::Logger* logger_ptr);

  // Methods.

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param os
   *        See Low_lvl_packet::to_ostream().
   * @param verbose
   *        See Low_lvl_packet::to_ostream().
   * @return `os`.
   */
  std::ostream& to_ostream(std::ostream& os, bool verbose) const override;

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param raw_buf
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @param prefer_no_move
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @param raw_packet
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @return See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   */
  bool deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                           bool prefer_no_move, util::Blob* raw_packet) override;
};

/**
 * Internal `net_flow` `struct` that encapsulates the Flow-protocol low-level SYN_ACK_ACK packet.
 * See Low_lvl_packet doc header for information common to all low-level packets including this one.
 *
 * A SYN_ACK_ACK packet is sent by the original SYN sender in response to a valid received SYN_ACK,
 * completing the 3-way handshake and establishing a Peer_socket to Peer_socket connection.
 */
struct Syn_ack_ack_packet : public Low_lvl_packet
{
  // Data.

#pragma pack(push, 1)
  /// Packed group affected by `#pragma pack`.
  struct
  {
    /**
     * This must equal `Syn_ack_packet` `m_security_token` received in the packet to which `*this` is
     * replying.  The other side will only proceed with the connection if the two are equal.
     */
    security_token_t m_security_token;

    /**
     * Same meaning as in `Syn_ack_packet` but applied to the essentially independent opposite
     * traffic direction of the full-duplex connection being established.
     */
    rcv_wnd_t m_rcv_wnd;
  } m_packed;
#pragma pack(pop)

  // Type checks.
  static_assert(std::numeric_limits<security_token_t>::is_integer
                  && (!std::numeric_limits<security_token_t>::is_signed),
                "Security tokens are non-negative integers.");
  static_assert(std::numeric_limits<rcv_wnd_t>::is_integer
                  && (!std::numeric_limits<rcv_wnd_t>::is_signed),
                "rcv_wnd values are non-negative integers.");

  /// In serialized packet, the type ID byte identifying this as a SYN_ACK_ACK.  Must not equal any other packet type's.
  static const uint8_t S_RAW_TYPE_ID;

  // Methods.

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param raw_bufs
   *        See Low_lvl_packet::serialize_to_raw_data().
   * @return See Low_lvl_packet::serialize_to_raw_data().
   */
  size_t serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const override;

private:
  // Friends.

  /// Friend of Syn_ack_ack_packet: For access to private constructor `Syn_ack_ack_packet(Logger*)`.
  // @todo Doxygen complains unless I make the above a Doxygen comment.  In other places it doesn't complain.  Fix...?
  friend boost::shared_ptr<Syn_ack_ack_packet> Low_lvl_packet::create_uninit_packet<Syn_ack_ack_packet>(log::Logger*);

  // Constructors/destructor.

  /**
   * The implementation of Low_lvl_packet::create_uninit_packet() for this sub-type of Low_lvl_packet.
   *
   * @param logger_ptr
   *        Logger to use subsequently.
   */
  explicit Syn_ack_ack_packet(log::Logger* logger_ptr);

  // Methods.

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param os
   *        See Low_lvl_packet::to_ostream().
   * @param verbose
   *        See Low_lvl_packet::to_ostream().
   * @return `os`.
   */
  std::ostream& to_ostream(std::ostream& os, bool verbose) const override;

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param raw_buf
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @param prefer_no_move
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @param raw_packet
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @return See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   */
  bool deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                           bool prefer_no_move, util::Blob* raw_packet) override;
}; // struct Syn_ack_ack_packet

/**
 * Internal `net_flow` `struct` that encapsulates the Flow-protocol low-level DATA packet.
 * See Low_lvl_packet doc header for information common to all low-level packets including this one.
 *
 * Each DATA packet contains actual data payload packetized from the application-provided stream
 * of bytes.  It is identified by the sequence number (encoded in this object) of the first
 * byte of that payload.  An individual acknowledgment inside an Ack_packet refers to the Data_packet
 * by that first sequence number.  If Low_lvl_packet::m_opt_rexmit_on is `true`, then #m_rexmit_id
 * identifies which attempt at sending the payload with this sequence number and payload this
 * packet represents: 0 is 1st attempt, 1 is 1st retry, 2 is 2nd retry, etc.  If that option is off,
 * then a lost packet is never retransmitted; and therefore #m_rexmit_id is always 0.
 */
struct Data_packet : public Low_lvl_packet
{
  // Types.

  /**
   * Type used to store the retransmission count in DATA and ACK packets.
   *
   * Why 8 bits?  This handles up 255 retransmissions, which is long after we'd kill the connection
   * anyway.
   */
  using rexmit_id_t = uint8_t;

  // Data.

  /**
   * The sequence number of the first byte in the payload; i.e., of `m_data.front()`, a/k/a `m_data[0]`.
   * Note that #m_data cannot be empty, so #m_seq_num is always meaningful.
   */
  Sequence_number m_seq_num;

  /**
   * Retransmit counter of the DATA packet being sent.  Identifies which attempt we are acknowledging
   * (0 = initial, 1 = first retransmit, 2 = second retransmit, ...).  Always 0 if `!m_opt_rexmit_on`.
   */
  rexmit_id_t m_rexmit_id;

  /**
   * This is the serialized version of `m_data.size()` (see #m_data).  We send this value, even though it could
   * be figured out from the overall serialized size, to ensure integrity (see the deserialization logic for details).
   * This here is set to `m_data.size()`, when Low_lvl_packet::serialize_to_raw_data() needs to
   * serialize that value.  Outside serialization `m_data.size()` is used directly, and this
   * value is meaningless.
   */
  mutable uint16_t m_data_size_raw;

  /**
   * The payload.  Cannot be `empty()`.
   *
   * As of this writing there is an important (for performance) subtlety about how this is originally filled in
   * when deserializing a wire-arrived packet.  See Data_packet::deserialize_type_specific_data_from_raw_data_packet().
   */
  util::Blob m_data;

  // Type checks.
  static_assert(std::numeric_limits<Sequence_number::seq_num_t>::is_integer
                  && (!std::numeric_limits<Sequence_number::seq_num_t>::is_signed),
                "Raw sequence numbers are non-negative integers.");
  static_assert(std::numeric_limits<rexmit_id_t>::is_integer
                  && (!std::numeric_limits<rexmit_id_t>::is_signed),
                "Retransmission IDs are non-negative integers.");

  /// In serialized packet, the type ID byte identifying this as a DATA packet.  Must not equal any other packet type's.
  static const uint8_t S_RAW_TYPE_ID;

  // Methods.

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param raw_bufs
   *        See Low_lvl_packet::serialize_to_raw_data().
   * @return See Low_lvl_packet::serialize_to_raw_data().
   */
  size_t serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const override;

private:
  // Friends.

  /// Friend of Data_packet: For access to private constructor `Data_packet(Logger*)`.
  // @todo Doxygen complains unless I make the above a Doxygen comment.  In other places it doesn't complain.  Fix...?
  friend boost::shared_ptr<Data_packet> Low_lvl_packet::create_uninit_packet<Data_packet>(log::Logger*);

  // Constructors/destructor.

  /**
   * The implementation of Low_lvl_packet::create_uninit_packet() for this sub-type of Low_lvl_packet.
   *
   * @param logger_ptr
   *        Logger to use subsequently.
   */
  explicit Data_packet(log::Logger* logger_ptr);

  // Methods.

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param os
   *        See Low_lvl_packet::to_ostream().
   * @param verbose
   *        See Low_lvl_packet::to_ostream().
   * @return `os`.
   */
  std::ostream& to_ostream(std::ostream& os, bool verbose) const override;

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param raw_buf
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @param prefer_no_move
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @param raw_packet
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @return See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   */
  bool deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                           bool prefer_no_move, util::Blob* raw_packet) override;
}; // struct Data_packet

/**
 * Internal `net_flow` `struct` that encapsulates the Flow-protocol low-level ACK packet.
 * See Low_lvl_packet doc header for information common to all low-level packets including this one.
 *
 * Each ACK packet encapsulates 0 or more individual acknowledgments, each acknowledging receipt of
 * a specific, distinct Data_packaet; and the current rcv_wnd (receive window state) on the ACK sender's
 * side.  The latter datum is always present.  If individual acks are present, then the rcv_wnd advertising
 * is merely opportunistic.  If NO individual acks are present, then the rcv_wnd advertising is intentional
 * for its own sake.  The algorithm for the latter is discussed elsewhere.
 *
 * The mechanics of individual acks are further explored in the doc header for Ack_packet::Individual_ack
 * nested `struct`.
 *
 * @todo Conceivably, since Ack_packet actually stores 1+ acknowledgments, it should become Acks_packet, of
 * type ACKS (not ACK).  Many comments and log messages would become clearer, as no one would assume an individual
 * packet's acknowledgment when reading "an ACK" or similar phrase.
 */
struct Ack_packet : public Low_lvl_packet
{
  // Types.

  /**
   * Type used to store the ACK delay for a given individual acknowledged packet.  The value
   * specifies the number of multiples of `Ack_delay_time_unit{1}` comprised by a packet's ACK delay.
   *
   * An earlier version of `net_flow` used the unit milliseconds and the encoding type uint16_t.  The
   * reasoning was that this allowed a maximum ACK delay of ~65 sec which should be plenty; and that
   * the 1-millisecond finegrainedness was acceptable.  However when implementing queue delay-based
   * congestion control (like FAST or Vegas) we realized it is important for RTTs (which use the ACK
   * delay value) to be quite precise (microsecond level or so).  Therefore, to be totally safe, we
   * choose to use the same units as #Fine_duration, which is how we compute all time periods.  As
   * for the encoding width, we use 64 bits just in case.
   *
   * @todo Reconsider the encoding width.  If `Ack_delay_time_unit{1}` is a nanosecond, then 32 bits
   * would support a maximum delay of ~4.1 seconds which is likely fine for most real-world
   * scenarios.  This would reduce the size of ACK packets quite a bit.
   */
  using ack_delay_t = uint64_t;

  /// `Ack_delay_time_unit{1}` is the duration corresponding to the #ack_delay_t value 1; and proportionally further.
  using Ack_delay_time_unit = Fine_duration;

  struct Individual_ack;
  struct Individual_ack_rexmit_off;
  struct Individual_ack_rexmit_on;

  // Data.

  /// Current receive window (remaining Receive buffer size) of the ACK sender.
  rcv_wnd_t m_rcv_wnd;

  /**
   * This is the serialized version of `m_rcv_acked_packets_rexmit_{on|off}_out.size()` and
   * `m_rcv_acked_packets.size()` (whichever is applicable in context).
   * We send this value, even though it could be figured out from the overall serialized
   * size, to ensure integrity (see the deserialization logic for details).
   * This here is set to `size()`, when Low_lvl_packet::serialize_to_raw_data() needs to
   * serialize that value.  Outside serialization `size()` is used directly, and this
   * value is meaningless.
   */
  mutable uint16_t m_rcv_acked_packets_rexmit_out_size;

  // Type checks.
  static_assert(std::numeric_limits<rcv_wnd_t>::is_integer
                  && (!std::numeric_limits<rcv_wnd_t>::is_signed),
                "rcv_wnd values are non-negative integers.");

  /**
   * List of *incoming* (post-deserialization of ACK) acknowledgments of DATA packets,
   * each identified by its first datum's sequence number as
   * provided by the other side and ordered in the chronological order they were received.
   * This may also be empty, in which case the containing ACK acknowledges no DATA packets but only
   * advertises the current receive window size (#m_rcv_wnd).  (Note that it advertises it if
   * #m_rcv_acked_packets is NOT empty as well.)
   *
   * This is used if and only if this Ack_packet is incoming.  See #m_rcv_acked_packets_rexmit_on_out
   * and #m_rcv_acked_packets_rexmit_off_out for the outgoing scenario.
   *
   * Unlike vanilla TCP from RFC 793, which features cumulative acknowledgment (wherein only the
   * latest received segment BEFORE any unreceived gap is acknowledged, thus just one total number
   * in the ACK), and unlike later enhanced TCPs, which feature both cumulative acknowledgement
   * and individual ACKs (Selective ACKs), we use something similar to Selective ACKs only.  That
   * is, we only acknowledge each packet individually (though we combine many such little
   * acknowledgments into one packet via delayed ACKs).  Moreover, we do not piggy-back ACK info
   * onto DATA packets; ACK is its own type.
   *
   * Rationale for using SACKs only: Selective ACKs help greatly in giving accurate congestion
   * control data (e.g., ACK delay per packet, dropped packets) and remove complex ambiguities of
   * trying to interpret cumulative ACKs for that purpose.  For retransmits, when enabled, Selective
   * ACKs also greatly simplify the design choices (since we know exactly what they don't have
   * [assuming no lost ACKs], we can send exactly what they want and no more).
   *
   * Rationale for keeping ACK and DATA separate: This is less clear-cut.  It is easier to think
   * about, certainly, as the two types of traffic aren't really logically related in a full-duplex
   * connection.  On the other hand it increases overhead.  On the third hand that disadvantage is
   * not a big deal assuming mostly unidirectional traffic flow (which is typical), since most of
   * the time the ACKs would be data-less anyway in that situation.  Delayed ACKs also help combat
   * overhead -- somewhat.
   *
   * Rationale for delayed ACKs a/k/a accumulating more than 1 acknowledgment into an ACK: Combat
   * overhead which can be a big deal for high bitrate streaming traffic for example (research
   * shows ACK flow can be 10% of data flow in the other direction).  The cost is that the other
   * side gets somewhat delayed congestion control information, but the delay can be tuned.  In
   * TCP implementations delayed ACKs appear to be universal since a long time ago.
   *
   * Any two packets represented by these `Individual_ack`s may be duplicates of each other (same
   * Sequence_number, possibly different delay values).  It's up to the sender (receiver of ACK)
   * to sort it out.  However, again, they MUST be ordered chronologicaly based on the time when
   * they were received; from earliest to latest.
   *
   * Storing shared pointers to avoid copying of `struct`s (however small) during internal
   * reshuffling; shared instead of raw pointers to not worry about `delete`.
   */
  std::vector<boost::shared_ptr<Individual_ack>> m_rcv_acked_packets;

  /**
   * Equivalent of #m_rcv_acked_packets but used for *outgoing* (pre-serialization of ACK) acknowledgments
   * and only if retransmission is disabled.  See notes in Individual_ack_rexmit_off doc header about how
   * this is used for efficient serialization of Ack_packet.
   *
   * This is used if and only if Ack_packet is outgoing, and retransmission is disabled.
   * See #m_rcv_acked_packets for the incoming scenario.
   */
  std::vector<Individual_ack_rexmit_off> m_rcv_acked_packets_rexmit_off_out;

  /// Equivalent of #m_rcv_acked_packets_rexmit_off_out but for retransmission enabled.
  std::vector<Individual_ack_rexmit_on> m_rcv_acked_packets_rexmit_on_out;

  /// In serialized packet, the type ID byte identifying this as an ACK packet.  Must not equal any other packet type's.
  static const uint8_t S_RAW_TYPE_ID;

  // Methods.

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param raw_bufs
   *        See Low_lvl_packet::serialize_to_raw_data().
   * @return See Low_lvl_packet::serialize_to_raw_data().
   */
  size_t serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const override;

private:
  // Friends.

  /// Friend of Ack_packet: For access to private constructor `Ack_packet(Logger*)`.
  // @todo Doxygen complains unless I make the above a Doxygen comment.  In other places it doesn't complain.  Fix...?
  friend boost::shared_ptr<Ack_packet> Low_lvl_packet::create_uninit_packet<Ack_packet>(log::Logger*);

  // Constructors/destructor.

  /**
   * The implementation of Low_lvl_packet::create_uninit_packet() for this sub-type of Low_lvl_packet.
   *
   * @param logger_ptr
   *        Logger to use subsequently.
   */
  explicit Ack_packet(log::Logger* logger_ptr);

  // Methods.

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param os
   *        See Low_lvl_packet::to_ostream().
   * @param verbose
   *        See Low_lvl_packet::to_ostream().
   * @return `os`.
   */
  std::ostream& to_ostream(std::ostream& os, bool verbose) const override;

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param raw_buf
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @param prefer_no_move
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @param raw_packet
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @return See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   */
  bool deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                           bool prefer_no_move, util::Blob* raw_packet) override;
}; // struct Ack_packet

/**
 * Specifies the *incoming* (post-deserialization) acknowledgment of a single received Data_packet.
 * It is not copyable, and moving them around by smart Individual_ack::Ptr is encouraged.
 * Construct this by direct member initialization.
 */
struct Ack_packet::Individual_ack
  // Cannot use boost::noncopyable or Shared_ptr_alias_holder, because that turns off direct initialization.
{
  // Types.

  /// Short-hand for ref-counted pointer to mutable objects of this class.
  using Ptr = boost::shared_ptr<Individual_ack>;

  /// Short-hand for ref-counted pointer to immutable objects of this class.
  using Const_ptr = boost::shared_ptr<const Individual_ack>;

  // Data.

  /**
   * Sequence number of first datum in packet that we acknowledge.
   * This is non-`const` for a technical reason: one must be able to use Sequence_number::set_metadata().
   */
  Sequence_number m_seq_num;

  /// The delay between when we received the acknowledged packet and when decided to send this ack.
  const Fine_duration m_delay;

  /**
   * Retransmit counter of the acknowledged Data_packet.  Identifies which attempt we are acknowledging
   * (0 = initial, 1 = first retransmit, 2 = second retransmit, ...).  Literally this equals the acked
   * Data_packet::m_rexmit_id.  Always 0 if retransmission is disabled for the given socket.
   */
  const unsigned int m_rexmit_id;

  /// Make us noncopyable without breaking aggregateness (direct-init).
  [[no_unique_address]] util::Noncopyable m_nc{};
}; // struct Ack_packet::Individual_ack

static_assert(std::is_aggregate_v<Ack_packet::Individual_ack>,
              "We want it to be direct-initializable.");
static_assert((!std::is_copy_constructible_v<Ack_packet::Individual_ack>)
                && (!std::is_copy_assignable_v<Ack_packet::Individual_ack>),
              "We want it to be noncopyable but rather passed-around via its ::Ptr.");

#pragma pack(push, 1)

/**
 * Specifies the *outgoing* (pre-serialization) acknowledgment of a single received Data_packet, when
 * retranmission is disabled on the socket.  Assuming network byte order equals native byte order,
 * and given that tight `struct` field packing is enabled throughout this code, one can arrange a tight
 * array of these `struct`s and transmit that entire array over the network with no need for scatter/gather
 * or any additional transformation of data post-construction and pushing onto aforementioned array.
 * These are copy-constructible, for pushing onto containers and such, but not assignable to avoid unnecessary
 * copying.  Update: A later version of clang does not like
 * this technique and warns about it; to avoid any such trouble just forget the non-assignability stuff;
 * it's internal code; we should be fine.
 *
 * @see Historical note in Ack_packet::serialize_to_raw_data() explains why this and
 *      Individual_ack_rexmit_on both exist, instead of simply using Individual_ack in both directions.
 */
struct Ack_packet::Individual_ack_rexmit_off
{
  // Data.

  /// See Individual_ack::m_seq_num and Sequence_number::raw_num_ref().
  const Sequence_number::seq_num_t m_seq_num_raw;

  /// See Individual_ack::m_delay; this is in `Ack_delay_time_unit{1}` multiples.
  const ack_delay_t m_delay;

  // Type checks.
  static_assert(std::numeric_limits<Sequence_number::seq_num_t>::is_integer
                  && (!std::numeric_limits<Sequence_number::seq_num_t>::is_signed),
                "Raw sequence numbers are non-negative integers.");
  static_assert(std::numeric_limits<ack_delay_t>::is_integer
                  && (!std::numeric_limits<ack_delay_t>::is_signed),
                "Time periods are non-negative integers.");

  // Constructors/destructor.

  /**
   * Constructs object.
   *
   * @param seq_num
   *        `seq_num.raw_num_ref()` is copied to #m_seq_num_raw.
   * @param delay
   *        #m_delay.
   */
  explicit Individual_ack_rexmit_off(const Sequence_number& seq_num, ack_delay_t delay);
};

/// Equivalent of `Individual_ack_rexmit_off` but for sockets with retransmission enabled.
struct Ack_packet::Individual_ack_rexmit_on
{
  // Data.

  /// Stores the values applicable to both Individual_ack_rexmit_off and Individual_ack_rexmit_on.
  Individual_ack_rexmit_off m_basic_ack;

  /// See Individual_ack::m_rexmit_id but note the type difference.
  Data_packet::rexmit_id_t m_rexmit_id;

  // Type checks.
  static_assert(std::numeric_limits<rexmit_id_t>::is_integer
                  && (!std::numeric_limits<rexmit_id_t>::is_signed),
                "Retransmission IDs are non-negative integers.");

  // Constructors/destructor.

  /**
   * Constructs object.
   *
   * @param seq_num
   *        Individual_ack_rexmit_off::m_seq_num.
   * @param rexmit_id
   *        #m_rexmit_id.
   * @param delay
   *        Individual_ack_rexmit_off::m_delay.
   */
  explicit Individual_ack_rexmit_on(const Sequence_number& seq_num, unsigned int rexmit_id, ack_delay_t delay);
};

#pragma pack(pop)

/**
 * Internal `net_flow` `struct` that encapsulates the Flow-protocol low-level RST packet.
 * See Low_lvl_packet doc header for information common to all low-level packets including this one.
 *
 * An RST means the sender is immediately shutting down the connection without waiting for any
 * feedback from the receiver and is recommending that the latter do the same ASAP.  This is sent in
 * a variety of error situations.
 *
 * Implemementation notes
 * ----------------------
 * The one peculiar thing about Rst_packet is it contains no data beyond the super-`struct` Low_lvl_packet,
 * other than the fact it *is* an Rst_packet (i.e., its `typeid()` identifies it as such).  Thus its
 * various `virtual` methods are typically either simple or even don't exist and defer to the Low_lvl_packet
 * implementation.
 */
struct Rst_packet : public Low_lvl_packet
{

  // Methods.

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param raw_bufs
   *        See Low_lvl_packet::serialize_to_raw_data().
   * @return See Low_lvl_packet::serialize_to_raw_data().
   */
  size_t serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const override;

  // Data.

  /// In serialized packet, the type ID byte identifying this as an RST packet.  Must not equal any other packet type's.
  static const uint8_t S_RAW_TYPE_ID;

private:
  // Friends.

  /// Friend of Rst_packet: For access to private constructor `Rst_packet(Logger*)`.
  // @todo Doxygen complains unless I make the above a Doxygen comment.  In other places it doesn't complain.  Fix...?
  friend boost::shared_ptr<Rst_packet> Low_lvl_packet::create_uninit_packet<Rst_packet>(log::Logger*);

  // Constructors/destructor.

  /**
   * The implementation of Low_lvl_packet::create_uninit_packet() for this sub-type of Low_lvl_packet.
   *
   * @param logger_ptr
   *        Logger to use subsequently.
   */
  explicit Rst_packet(log::Logger* logger_ptr);

  // Methods.

  /**
   * Implements Low_lvl_packet API.  See that super-method's doc header.
   *
   * @param raw_buf
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @param prefer_no_move
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @param raw_packet
   *        See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   * @return See Low_lvl_packet::deserialize_type_specific_data_from_raw_data_packet().
   */
  bool deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                           bool prefer_no_move, util::Blob* raw_packet) override;
}; // struct Rst_packet

// Template and constexpr implementations.

template<typename Low_lvl_packet_sub>
boost::shared_ptr<Low_lvl_packet_sub> Low_lvl_packet::create_uninit_packet(log::Logger* logger) // Static.
{
  using boost::shared_ptr;

  // Note: Low_lvl_packet_sub is not Low_lvl_packet. It is a sub-type: Syn_packet, Ack_packet, etc. We're a template.

  // `friend` relation required to be able to call this private constructor.
  return shared_ptr<Low_lvl_packet_sub>(new Low_lvl_packet_sub{logger});
}

template<typename Low_lvl_packet_sub>
Low_lvl_packet::Ptr Low_lvl_packet::create_uninit_packet_base(log::Logger* logger) // Static.
{
  return Low_lvl_packet::ptr_cast(create_uninit_packet<Low_lvl_packet_sub>(logger));
}

constexpr bool Low_lvl_packet::native_is_big_endian() // Static.
{
  using boost::endian::native_to_big;

  constexpr uint16_t TEST_VAL = 0x0102;
  return native_to_big(TEST_VAL) == TEST_VAL;
}

} // namespace flow::net_flow
