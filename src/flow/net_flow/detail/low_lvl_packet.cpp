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
#include "flow/net_flow/detail/low_lvl_packet.hpp"

namespace flow::net_flow
{

// Note: In this file, it felt more readable to group methods/etc. by signature instead of by class/struct.

// Static initializations.

const boost::unordered_map<std::type_index, Low_lvl_packet::Packet_type_info>
        Low_lvl_packet::S_NATIVE_TYPE_ID_TO_PACKET_TYPE_INFO
          ({
             // { m_raw_type_id, m_type_id_str } Careful!  The m_raw_type_id values must not conflict.
             { std::type_index(typeid(Syn_packet)),
               { 0, "SYN" } },
             { std::type_index(typeid(Syn_ack_packet)),
               { 1, "SYN_ACK" } },
             { std::type_index(typeid(Syn_ack_ack_packet)),
               { 2, "SYN_ACK_ACK" } },
             { std::type_index(typeid(Data_packet)),
               { 3, "DATA" } },
             { std::type_index(typeid(Ack_packet)),
               { 4, "ACK" } },
             { std::type_index(typeid(Rst_packet)),
               { 5, "RST" } }
           });

// Implementations.

Low_lvl_packet::Aux_raw_data::Aux_raw_data() :
  m_reserved2(0) // This guy is const and is always to be zero.
{
  // The rest remains uninitialized, as advertised.
}

Low_lvl_packet::Low_lvl_packet(log::Logger* logger) :
  log::Log_context(logger, Flow_log_component::S_NET_FLOW),
  /* This bit of cleverness creates an ostream manipulator: if f passed to `os <<`, then f(os) is called.
   * So in this case, this->type_to_ostream(os), where os is the ostream&, will be called.
   * Caution: This is scary, because type_to_ostream() makes virtual call, which is not OK to call in constructor in
   * C++, since the vtable won't be set up yet, and the wrong method will be called; but we are not CALLING it;
   * we are taking a pointer to something that'll be filled out by the time it's used later on.  It works fine;
   * just don't try using it with an actual stream IN the constructor. */
  m_type_ostream_manip([this](std::ostream& os) -> std::ostream& { return type_to_ostream(os); }),
  m_verbose_ostream_manip([this](std::ostream& os) -> std::ostream& { return to_ostream(os, true); }),
  m_concise_ostream_manip([this](std::ostream& os) -> std::ostream& { return to_ostream(os, false); })
{
  // Note: Lots of uninitialized values, as advertised.
}

Syn_packet::Syn_packet(log::Logger* logger) :
  Low_lvl_packet(logger),
  m_serialized_metadata(logger)
{
  // Note: Lots of uninitialized values.
}

Syn_ack_packet::Syn_ack_packet(log::Logger* logger) :
  Low_lvl_packet(logger)
{
  // Note: Lots of uninitialized values.
}

Syn_ack_ack_packet::Syn_ack_ack_packet(log::Logger* logger) :
  Low_lvl_packet(logger)
{
  // Note: Lots of uninitialized values.
}

Data_packet::Data_packet(log::Logger* logger) :
  Low_lvl_packet(logger),
  m_rexmit_id(0), // If !m_opt_rexmit_on, then this will be expected to remain 0.  Initialize to avoid worrying.
  m_data(logger)
{
  // Note: Lots of uninitialized values.
}

Ack_packet::Ack_packet(log::Logger* logger) :
  Low_lvl_packet(logger)
{
  // Note: Lots of uninitialized values.
}

Rst_packet::Rst_packet(log::Logger* logger) :
  Low_lvl_packet(logger)
{
  // Nothing further.
}

const std::string& Low_lvl_packet::type_id_to_str(const std::type_index& type_id) // Static.
{
  using std::type_index;

  assert(type_id != type_index(typeid(Low_lvl_packet)));
  const auto it = S_NATIVE_TYPE_ID_TO_PACKET_TYPE_INFO.find(type_id);
  assert(it != S_NATIVE_TYPE_ID_TO_PACKET_TYPE_INFO.end());

  return it->second.m_type_id_str;
}

const uint8_t& Low_lvl_packet::type_id_native_to_raw(const std::type_info& type_id)
{
  using std::type_index;

  assert(type_id != typeid(Low_lvl_packet));
  const auto it = S_NATIVE_TYPE_ID_TO_PACKET_TYPE_INFO.find(type_index(type_id));
  assert(it != S_NATIVE_TYPE_ID_TO_PACKET_TYPE_INFO.end());

  return it->second.m_raw_type_id;
}

size_t Low_lvl_packet::serialize_to_raw_data_and_log(Const_buffer_sequence* raw_bufs) const
{
  using util::buffers_dump_string;
  using std::ostream;

  const size_t size = serialize_to_raw_data(raw_bufs);

  auto const logger_ptr = get_logger();
  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, get_log_component()))
  {
    if (logger_ptr->should_log(log::Sev::S_DATA, get_log_component()))
    {
      FLOW_LOG_DATA_WITHOUT_CHECKING
        ("Serialized Low_lvl_packet [" << this << "] of type [" << m_type_ostream_manip << "]; "
         "total added output size [" << size << "] bytes; final buffer contents including added output: "
         "[\n" << buffers_dump_string(*raw_bufs, "    ") << "].");
    }
    else
    {
      FLOW_LOG_TRACE_WITHOUT_CHECKING
        ("Serialized Low_lvl_packet [" << this << "] of type [" << m_type_ostream_manip << "]; "
         "total added output size [" << size << "] bytes.");
    }
  }

  return size;
}

size_t Low_lvl_packet::serialize_common_header_to_raw_data(Const_buffer_sequence* raw_bufs) const
{
  using boost::asio::const_buffer;
  using std::type_index;

  // Reminder: We're not encoding a full packet here.  Just the Common Header, starting with the type specifier.

  // Reminder: Keep raw_bufs->size() low, as explained near UDP async_send_to() call!

  /* Look up the statically stored constant raw type ID corresponding to the true polymorphic type of `*this`.
   * Provide the direct adress of that place in memory as part of the "scattered" serialized data. */
  const auto& raw_type_id_ref = type_id_native_to_raw(typeid(*this)); // Important subtlety: This returns a reference.
  // As long as this is true, we need not worry about byte order for, specifically, the raw type ID.
  static_assert(sizeof raw_type_id_ref == 1,
                "Raw packet type ID should be small enough for byte ordering to be a non-issue.");
  size_t size = sizeof raw_type_id_ref;
  raw_bufs->push_back(const_buffer(&raw_type_id_ref, size));

  /* Subtlety: Now we just have to write the m_opt_rexmit_on flag byte, followed by two reserved 0 bytes.
   * The subleties are:
   *   - The byte does not equal the bool m_opt_rexmit_on bit-wise and needs to be specially encoded.
   *     So the byte needs to be stored separately, so that a buffer pointer to it can be added
   *     to *raw_bufs.  We can't just refer to some transient local object memory, as that will go away,
   *     once some { block } exits!
   *     It has to be permanent (while *this exists).
   *   - Same deal with the reserved 0 bytes.
   *
   * So use m_aux_raw_data to store that stuff.  The reserved bytes are already set.  Save the flag. */
  m_aux_raw_data.m_opt_rexmit_on_raw = m_opt_rexmit_on ? 1 : 0;
  raw_bufs->push_back(const_buffer(&m_aux_raw_data.m_opt_rexmit_on_raw,
                                   sizeof m_aux_raw_data.m_opt_rexmit_on_raw + sizeof m_aux_raw_data.m_reserved2));
  size += raw_bufs->back().size();

  // Subtlety: These are stored together in packed fashion, so refer to them in one buffer.
  raw_bufs->push_back(const_buffer(&m_packed.m_src_port, sizeof m_packed.m_src_port + sizeof m_packed.m_dst_port));
  size += raw_bufs->back().size();

  return size;
}

size_t Syn_packet::serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const // Virtual.
{
  using boost::asio::const_buffer;

  /* Important: We directly copy various integers from memory into packet.  That is only OK if
   * the chosen network encoding convention equals that of this machine's endianness.
   * If not, we'll need to write considerably more complex code, since flipped versions of such
   * integers would need to be stored as well as the real versions.  (Performance-wise, using
   * the "scatter" technique would still be worthwhile, since large areas like Data_packet::m_data
   * are stored in exact order and need no flipping.  @todo Revisit but only if this assertion actually trips.
   *
   * We've intentionally chosen little-endian as the network byte order, since most platforms natively use it
   * in memory (x86, etc.).
   *
   * Note that this technique is the basis of various "zero-copy" [de]serializers such as Cap'n Proto. */
  static_assert(!native_is_big_endian(),
                "Byte ordering in this platform is not conducive to zero-copy serialization.");

  // Common header = type ID + common fields.
  size_t size = serialize_common_header_to_raw_data(raw_bufs);

  // Then our own stuff.

  const auto* raw_seq_num = &(m_init_seq_num.raw_num_ref());
  raw_bufs->push_back(const_buffer(raw_seq_num, sizeof *raw_seq_num));
  size += sizeof *raw_seq_num;

  if (!m_serialized_metadata.empty())
  {
    raw_bufs->push_back(const_buffer(m_serialized_metadata.const_data(), m_serialized_metadata.size()));
    size += m_serialized_metadata.size();
  }

  return size;
}

size_t Syn_ack_packet::serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const // Virtual.
{
  using boost::asio::const_buffer;

  // Using same techniques as in Syn_packet::serialize_to_raw_data().  Keeping comments light.

  static_assert(!native_is_big_endian(),
                "Byte ordering in this platform is not conducive to zero-copy serialization.");

  // Common header = type ID + common fields.
  size_t size = serialize_common_header_to_raw_data(raw_bufs);

  const Sequence_number::seq_num_t* raw_seq_num = &(m_init_seq_num.raw_num_ref());
  raw_bufs->push_back(const_buffer(raw_seq_num, sizeof *raw_seq_num));
  size += sizeof *raw_seq_num;

  // Subtlety: These are stored together in packed fashion, so refer to them in one buffer.
  raw_bufs->push_back(const_buffer(&m_packed.m_security_token,
                                   sizeof m_packed.m_security_token + sizeof m_packed.m_rcv_wnd));
  size += raw_bufs->back().size();

  return size;
}

size_t Syn_ack_ack_packet::serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const // Virtual.
{
  using boost::asio::const_buffer;

  // Using same techniques as in Syn_packet::serialize_to_raw_data().  Keeping comments light.

  static_assert(!native_is_big_endian(),
                "Byte ordering in this platform is not conducive to zero-copy serialization.");

  // Common header = type ID + common fields.
  size_t size = serialize_common_header_to_raw_data(raw_bufs);

  // Subtlety: These are stored together in packed fashion, so refer to them in one buffer.
  raw_bufs->push_back(const_buffer(&m_packed.m_security_token,
                                   sizeof m_packed.m_security_token + sizeof m_packed.m_rcv_wnd));
  size += raw_bufs->back().size();

  return size;
}

size_t Data_packet::serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const // Virtual.
{
  using boost::asio::const_buffer;
  using std::numeric_limits;

  // Using same techniques as in Syn_packet::serialize_to_raw_data().  Keeping comments light.

  // Reminder: Keep raw_bufs->size() low, as explained near UDP async_send_to() call!

  static_assert(!native_is_big_endian(),
                "Byte ordering in this platform is not conducive to zero-copy serialization.");

  // Common header = type ID + common fields.
  size_t size = serialize_common_header_to_raw_data(raw_bufs);

  const Sequence_number::seq_num_t* raw_seq_num = &(m_seq_num.raw_num_ref());
  raw_bufs->push_back(const_buffer(raw_seq_num, sizeof *raw_seq_num));
  size += sizeof *raw_seq_num;

  if (m_opt_rexmit_on)
  {
    raw_bufs->push_back(const_buffer(&m_rexmit_id, sizeof m_rexmit_id));
    size += sizeof m_rexmit_id;
  }

  assert(!m_data.empty());

  // Serialize this, even though it's not strictly required, for reasons explained near deserialization code.
  const size_t data_size = m_data.size();
  using data_size_raw_t = decltype(m_data_size_raw);
  assert(data_size <= numeric_limits<data_size_raw_t>::max());

  m_data_size_raw = data_size_raw_t(data_size);
  raw_bufs->push_back(const_buffer(&m_data_size_raw, sizeof m_data_size_raw));
  size += sizeof m_data_size_raw;

  raw_bufs->push_back(const_buffer(m_data.const_data(), data_size));
  size += m_data.size();

  return size;
}

size_t Ack_packet::serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const // Virtual.
{
  using boost::asio::const_buffer;
  using std::numeric_limits;

  // Using same techniques as in Syn_packet::serialize_to_raw_data().  Keeping comments light.

  // Reminder: Keep raw_bufs->size() low, as explained near UDP async_send_to() call!

  static_assert(!native_is_big_endian(),
                "Byte ordering in this platform is not conducive to zero-copy serialization.");

  // Common header = type ID + common fields.
  size_t size = serialize_common_header_to_raw_data(raw_bufs);

  raw_bufs->push_back(const_buffer(&m_rcv_wnd, sizeof m_rcv_wnd));
  size += sizeof m_rcv_wnd;

  // Serialize this, even though it's not strictly required, for reasons explained near deserialization code.
  const size_t pkts_size = m_opt_rexmit_on
                             ? m_rcv_acked_packets_rexmit_on_out.size()
                             : m_rcv_acked_packets_rexmit_off_out.size();
  using pkts_size_t = decltype(m_rcv_acked_packets_rexmit_out_size);
  assert(pkts_size <= numeric_limits<pkts_size_t>::max());

  m_rcv_acked_packets_rexmit_out_size = pkts_size_t(pkts_size);
  raw_bufs->push_back(const_buffer(&m_rcv_acked_packets_rexmit_out_size, sizeof(pkts_size_t)));
  size += sizeof(pkts_size_t);

  /* The following reaps the benefits of arranging the individual acks in a packed array or packed structures
   * that are pre-serialized.  In the past, this instead used scatter-gather semantics to cherry-pick parts of
   * each ack and, for example, simply omitting the inapplicable m_rexmit_id when rexmit_on was false.
   * There was thus no need for m_rcv_acked_packets_rexmit_*_out -- m_rcv_acked_packets was used for both
   * incoming and outgoing packets.  However, we then ran into the aforementioned UDP async_send_to() limitation
   * at least for some platforms.  This encouraged the optimization you see now -- which has the benefit of
   * being better-performing in any case.  The code, however, is arguably more complex (not in this function
   * but elsewhere, with 2 more structs being needed. */
  if (m_opt_rexmit_on)
  {
    raw_bufs->push_back(const_buffer(m_rcv_acked_packets_rexmit_on_out.data(),
                                     m_rcv_acked_packets_rexmit_on_out.size()
                                       * sizeof(decltype(m_rcv_acked_packets_rexmit_on_out)::value_type)));
  }
  else
  {
    raw_bufs->push_back(const_buffer(m_rcv_acked_packets_rexmit_off_out.data(),
                                     m_rcv_acked_packets_rexmit_off_out.size()
                                       * sizeof(decltype(m_rcv_acked_packets_rexmit_off_out)::value_type)));
  }
  size += raw_bufs->back().size();

  return size;
}

size_t Rst_packet::serialize_to_raw_data(Const_buffer_sequence* raw_bufs) const // Virtual.
{
  // Common header = type ID + common fields.  We add nothing else.
  return serialize_common_header_to_raw_data(raw_bufs);
}

Low_lvl_packet::Ptr Low_lvl_packet::create_from_raw_data_packet(log::Logger* logger_ptr, util::Blob* raw_packet,
                                                                bool prefer_no_move) // Static.
{
  using util::buffers_dump_string;
  using boost::asio::buffer;
  using boost::asio::const_buffer;
  using boost::endian::little_to_native;
  using std::ostream;

  Const_buffer raw_buf(raw_packet->const_data(), raw_packet->size());

  // Make FLOW_LOG_...() calls below use these (we are static).
  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_NET_FLOW);

  const size_t raw_buf_size = raw_buf.size();

  FLOW_LOG_DATA
    ("Deserializing Low_lvl_packet; "
     "total raw buffer size [" << raw_buf_size << "] bytes; buffer contents: "
     "[\n" << buffers_dump_string(raw_buf, "    ") << "].");

  /* The basic structure of raw_buf is as follows:
   *   - Common Header:
   *     - The raw type ID, which indicates what is after the Common Header.
   *       - We'll create the appropriate Low_lvl_packet sub-type object based on this.
   *     - Certain common fields that apply to all.
   *       - We'll fill those out in the Low_lvl_packet (super-type) area of the the just-created object.
   *   - Type-specific data:
   *     - We hand off the partially filled object and the remaining buffer to a virtual method that
   *       will appropriately fill the rest of the object. */

  // This is the expected size of the Common Header mentioned above.
  const size_t common_data_size
    = sizeof(uint8_t) + sizeof m_packed.m_src_port + sizeof m_packed.m_dst_port
    + sizeof m_aux_raw_data.m_opt_rexmit_on_raw + sizeof m_aux_raw_data.m_reserved2;

  if (raw_buf_size < common_data_size)
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level packet: The packet is too small: "
                     "[" << raw_buf_size << "] bytes.");
    return Ptr();
  }

  /* We'll advance this as we keep reading off values from raw buffer.
   * Could also use += on *raw_buf, but I don't feel like cast<>ing all the time.
   * Would rather reinterpret_cast<> due to it being the devil I know. Doesn't really matter. */

  const uint8_t* common_data = static_cast<const uint8_t*>(raw_buf.data());
  // Meanwhile, this guy can skip past the entire Common Header.  We'll check that it matches common_data at the end.
  raw_buf = raw_buf + common_data_size;

  // Get the type specifier.
  const auto& raw_type_id = *common_data;
  common_data += sizeof raw_type_id;

  /* The type specifier determines the polymorphic type we choose.  It is not actually recorded.
   * C++ keeps typeid() which performs that task for us without us having to do it explicitly. */
  Ptr packet
    = (raw_type_id == type_id_native_to_raw(typeid(Syn_packet)))
      ? create_uninit_packet_base<Syn_packet>(logger_ptr)
    : ((raw_type_id == type_id_native_to_raw(typeid(Syn_ack_packet)))
      ? create_uninit_packet_base<Syn_ack_packet>(logger_ptr)
    : ((raw_type_id == type_id_native_to_raw(typeid(Syn_ack_ack_packet)))
      ? create_uninit_packet_base<Syn_ack_ack_packet>(logger_ptr)
    : ((raw_type_id == type_id_native_to_raw(typeid(Data_packet)))
      ? create_uninit_packet_base<Data_packet>(logger_ptr)
    : ((raw_type_id == type_id_native_to_raw(typeid(Ack_packet)))
      ? create_uninit_packet_base<Ack_packet>(logger_ptr)
    : ((raw_type_id == type_id_native_to_raw(typeid(Rst_packet)))
      ? create_uninit_packet_base<Rst_packet>(logger_ptr)
    : Ptr())))));
  if (!packet)
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level packet: The packet type is invalid: "
                     "[" << int(raw_type_id) << "].");
    return packet;
  }

  using opt_rexmit_on_raw_t = decltype(Aux_raw_data::m_opt_rexmit_on_raw);
  const auto& opt_rexmit_on_raw
    = *reinterpret_cast<const opt_rexmit_on_raw_t*>(common_data);
  common_data += sizeof opt_rexmit_on_raw;
  packet->m_opt_rexmit_on = (opt_rexmit_on_raw != 0);

  using reserved2_t = decltype(Aux_raw_data::m_reserved2);
  const auto& reserved2 = *reinterpret_cast<const reserved2_t*>(common_data);
  common_data += sizeof reserved2;
  if (reserved2 != 0)
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level packet: The packet format is unknown.");
    return Ptr();
  }
  // else

  const auto& src_port_raw = *reinterpret_cast<const flow_port_t*>(common_data);
  common_data += sizeof src_port_raw;
  packet->m_packed.m_src_port = little_to_native(src_port_raw);

  const auto& dst_port_raw = *reinterpret_cast<const flow_port_t*>(common_data);
  common_data += sizeof dst_port_raw;
  packet->m_packed.m_dst_port = little_to_native(dst_port_raw);

  // Did we claculate the total size of Common Header correctly at the start?
  assert(common_data == static_cast<const uint8_t*>(raw_buf.data()));

  FLOW_LOG_TRACE("Deserialized packet [" << packet << "] common info: "
                 "NetFlow ports [" << packet->m_packed.m_src_port << "] -> [" << packet->m_packed.m_dst_port << "]; "
                 "opt_rexmit_on = [" << packet->m_opt_rexmit_on << "]; "
                 "common serialized size was [" << common_data_size << "].");

  // Low_lvl_packet part is filled out.  The sub-type part has junk.  This will fill that part out.
  if (!packet->deserialize_type_specific_data_from_raw_data_packet(&raw_buf, prefer_no_move, raw_packet))
  {
    return Ptr(); // Error.  It logged.
  }
  // else

  // raw_packet is not in valid but unknown state.  Do not use.

  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, get_log_component()))
  {
    const bool data_sev = logger_ptr->should_log(log::Sev::S_DATA, get_log_component());
    FLOW_LOG_WITHOUT_CHECKING(data_sev ? log::Sev::S_DATA : log::Sev::S_TRACE,
                              "Deserialized packet: Final version: "
                              "[\n"
                              << (data_sev ? packet->m_verbose_ostream_manip : packet->m_concise_ostream_manip)
                              << "].");
  }

  return packet;
} // Low_lvl_packet::create_from_raw_data_packet()

bool Syn_packet::deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                                     [[maybe_unused]] bool prefer_no_move,
                                                                     [[maybe_unused]] util::Blob* raw_packet)
  // Virtual.
{
  using boost::asio::buffer;
  using boost::endian::little_to_native;

  // Structure: a few required fields; then arbitrary-length metadata buffer.

  // Use same techniques as in Low_lvl_packet::create_from_raw_data_packet().  Keeping comments light.

  const size_t min_data_size = sizeof(Sequence_number::seq_num_t);
  const size_t raw_buf_size = raw_buf->size();
  if (raw_buf_size < min_data_size)
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level [" << m_type_ostream_manip << "] "
                     "packet: The packet is too small: ["<< raw_buf_size << "] bytes.");
    return false;
  }
  // else

  const uint8_t* data = static_cast<const uint8_t*>(raw_buf->data());
  (*raw_buf) = ((*raw_buf) + min_data_size);

  const auto& seq_num_raw = *reinterpret_cast<const Sequence_number::seq_num_t*>(data);
  data += sizeof seq_num_raw;
  m_init_seq_num.set_raw_num(little_to_native(seq_num_raw));
  /* Set some metadata. Subleties:
   * The 'type' we know and can set sans controversy. It's the number line for remote-originated data.
   * The multiple is, if anything, max-block-size, which we don't know, since we don't yet know socket for this,
   * (if it's even legal at all).
   * The zero point is the ISN + 1, and since we *are* the ISN, we could actually set it here.  Pragmatically,
   * however, it's nice to log the absolute raw number at this point and turn everything to nice relative values
   * after that; so leave the applying of ISN to this until `sock` is detected.  Note, also, that this is consistent
   * with what's done for Data_packet and Ack_packet (though, to be fair, for those there is no choice, as ISN is
   * not known without socket). */
  m_init_seq_num.set_metadata('R');

  assert(data == static_cast<const uint8_t*>(raw_buf->data()));

  const size_t n_copied = m_serialized_metadata.assign_copy(*raw_buf);
  assert(n_copied == m_serialized_metadata.size());

  FLOW_LOG_INFO("Deserialized low-level [" << m_type_ostream_manip << "] packet with "
                "ISN [" << m_init_seq_num << "]; "
                "metadata size ["<< n_copied << "] bytes; "
                "serialized size beyond common header was [" << raw_buf_size << "].");

  return true;
} // Syn_packet::deserialize_type_specific_data_from_raw_data_packet()

bool Syn_ack_packet::deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                                         [[maybe_unused]] bool prefer_no_move,
                                                                         [[maybe_unused]] util::Blob* raw_packet)
  // Virtual.
{
  using boost::endian::little_to_native;

  // Structure: a few required fields; and that's it.

  // Use same techniques as in Low_lvl_packet::create_from_raw_data_packet().  Keeping comments light.

  const size_t exp_data_size = sizeof(Sequence_number::seq_num_t)
                             + sizeof m_packed.m_security_token + sizeof m_packed.m_rcv_wnd;
  const size_t raw_buf_size = raw_buf->size();
  if (raw_buf_size != exp_data_size)
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level [" << m_type_ostream_manip << "] "
                     "packet: The packet is of wrong size: ["<< raw_buf_size << "] bytes.");
    return false;
  }
  // else

  const uint8_t* data = static_cast<const uint8_t*>(raw_buf->data());
  (*raw_buf) = ((*raw_buf) + exp_data_size);

  const auto& seq_num_raw = *reinterpret_cast<const Sequence_number::seq_num_t*>(data);
  data += sizeof seq_num_raw;
  m_init_seq_num.set_raw_num(little_to_native(seq_num_raw));
  // See comment in same place for Syn_packet.
  m_init_seq_num.set_metadata('R');

  const auto& security_token_raw = *reinterpret_cast<const security_token_t*>(data);
  data += sizeof security_token_raw;
  m_packed.m_security_token = little_to_native(security_token_raw);

  const auto& rcv_wnd_raw = *reinterpret_cast<const rcv_wnd_t*>(data);
  data += sizeof rcv_wnd_raw;
  m_packed.m_rcv_wnd = little_to_native(rcv_wnd_raw);

  assert(data == static_cast<const uint8_t*>(raw_buf->data()));

  FLOW_LOG_INFO("Deserialized low-level [" << m_type_ostream_manip << "] packet with "
                "ISN [" << m_init_seq_num << "]; "
                "rcv_wnd ["<< m_packed.m_rcv_wnd << "] bytes; "
                "serialized size beyond common header was [" << raw_buf_size << "].");

  return true;
} // Syn_ack_packet::deserialize_type_specific_data_from_raw_data_packet()

bool Syn_ack_ack_packet::deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                                             [[maybe_unused]] bool prefer_no_move,
                                                                             [[maybe_unused]] util::Blob* raw_packet)
  // Virtual.
{
  using boost::endian::little_to_native;

  // Structure: a few required fields; and that's it.

  // Use same techniques as in Low_lvl_packet::create_from_raw_data_packet().  Keeping comments light.

  const size_t exp_data_size = sizeof m_packed.m_security_token + sizeof m_packed.m_rcv_wnd;
  const size_t raw_buf_size = raw_buf->size();
  if (raw_buf_size != exp_data_size)
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level [" << m_type_ostream_manip << "] "
                     "packet: The packet is too small: ["<< raw_buf_size << "] bytes.");
    return false;
  }
  // else

  const uint8_t* data = static_cast<const uint8_t*>(raw_buf->data());
  (*raw_buf) = ((*raw_buf) + exp_data_size);

  const auto& security_token_raw = *reinterpret_cast<const security_token_t*>(data);
  data += sizeof security_token_raw;
  m_packed.m_security_token = little_to_native(security_token_raw);

  const auto& rcv_wnd_raw = *reinterpret_cast<const rcv_wnd_t*>(data);
  data += sizeof rcv_wnd_raw;
  m_packed.m_rcv_wnd = little_to_native(rcv_wnd_raw);

  assert(data == static_cast<const uint8_t*>(raw_buf->data()));

  FLOW_LOG_INFO("Deserialized low-level [" << m_type_ostream_manip << "] packet with "
                "rcv_wnd ["<< m_packed.m_rcv_wnd << "] bytes; "
                "serialized size beyond common header was [" << raw_buf_size << "].");

  return true;
} // Syn_ack_ack_packet::deserialize_type_specific_data_from_raw_data_packet()

bool Data_packet::deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                                      bool prefer_no_move,
                                                                      util::Blob* raw_packet) // Virtual.
{
  using boost::asio::buffer;
  using boost::endian::little_to_native;
  using std::numeric_limits;
  using std::memcpy;

  using data_size_raw_t = decltype(m_data_size_raw);

  // Structure: a few required fields; then the arbitrarily-long (but not 0-length) data payload.

  // Use same techniques as in Low_lvl_packet::create_from_raw_data_packet().  Keeping comments light.

  const size_t min_data_size
    = sizeof(Sequence_number::seq_num_t) + (m_opt_rexmit_on ? sizeof m_rexmit_id : 0) + sizeof(data_size_raw_t);
  const size_t raw_buf_size = raw_buf->size();
  if (raw_buf_size < min_data_size)
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level [" << m_type_ostream_manip << "] "
                     "packet: The packet is too small: ["<< raw_buf_size << "] bytes.");
    return false;
  }
  // else

  const uint8_t* cur_data = static_cast<const uint8_t*>(raw_buf->data());
  (*raw_buf) = ((*raw_buf) + min_data_size);

  const auto& seq_num_raw = *reinterpret_cast<const Sequence_number::seq_num_t*>(cur_data);
  cur_data += sizeof seq_num_raw;
  m_seq_num.set_raw_num(little_to_native(seq_num_raw));
  // Remote seq. # line, but we don't (yet) know for which socket, so max-block-size and ISN are (for now) unknown.
  m_seq_num.set_metadata('R');

  if (m_opt_rexmit_on)
  {
    const auto& rexmit_id_raw = *reinterpret_cast<const rexmit_id_t*>(cur_data);
    cur_data += sizeof rexmit_id_raw;
    m_rexmit_id = little_to_native(rexmit_id_raw);
  }

  /* m_data.size() is encoded separately, even though we could figure it out simply by counting the bytes not yet
   * deserialized to this point.  The reason is that I've seen, in practice, silent truncation occur; for example
   * with Boost 1.63 on Mac, at least, using more than ~64 scatter/gather buffers when invoking UDP sendto() caused
   * the over-that-silent-limit buffers to simply be ignored.  (Another reason is the existing socket options
   * mechanism in this library makes it very difficult to ensure Node_options::m_dyn_low_lvl_max_packet_size is
   * sufficiently larger than Peer_socket_options::m_st_max_block_size (because they are not both per-Node or
   * both per-socket -- trust me, I tried, but thread considerations made checking this at all appropriate times
   * either extremely hard or even impossible).  So it has to be checked outside option checking time, but it can't
   * be done at receipt time, because at that point the Peer_socket from which to obtain m_st_max_block_size is not
   * yet known; one has to deserialize packet to get at that... but at that point it's kind of too late.)  So all
   * these issues can be sanity-checked away by encoding the expected size up-front and then ensuring the actual
   * data area matches this exactly.  To be clear, there are certainly ways to avoid it -- but then one must constantly
   * be aware of it; whereas using a couple of bytes to do this eliminates that problem at a fairly low cost.
   * @todo Revisit this sometime. */

  data_size_raw_t data_size_raw;
  // Using `= *reinterpret_cast<...*>` makes UBSAN complain; so humor it by using explicit memcpy().
  memcpy(&data_size_raw, cur_data, sizeof data_size_raw);
  cur_data += sizeof data_size_raw;

  data_size_raw = little_to_native(data_size_raw);

  if (data_size_raw == 0)
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level [" << m_type_ostream_manip << "] "
                     "packet: The data area specifies empty size.");
    return false;
  }
  // else

  assert(cur_data == static_cast<const uint8_t*>(raw_buf->data()));

  // The rest of the buffer = the data payload.
  const size_t data_size = raw_buf->size();

  if ((data_size > numeric_limits<data_size_raw_t>::max()) // Don't let overflow fools us or something like that.
      || (data_size_raw_t(data_size) != data_size_raw))
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level [" << m_type_ostream_manip << "] "
                     "packet: The data area specifies size [" << data_size_raw << "] but has "
                     "actual size [" << data_size << "].  Did truncation occur somewhere?");
    return false;
  }
  // else

  /* Subtlety: m_data, to store the DATA payload itself, will consist of the entire raw packet data but with
   * its logical [begin(), end) range starting past the header and other stuff, right where the payload begins.
   * This allows us to MOVE instead of (slowly) copying (part of) raw_packet into m_data -- but then still treat m_data
   * as if it's just the payload itself and not the entire packet.  (In point of fact, as of this writing, m_data will
   * be further moved into Socket_buffer Receive buffer, again via a constant-time move.)  This way we sacrifice a small
   * memory overhead for major eliminating of copying.
   *
   * Update: The caller may now specify to make a copy instead.  Why this might be done is explained elsewhere, where
   * it is decided. */

  // First move over the entire raw packet.
  if (prefer_no_move)
  {
    m_data.assign_copy(*raw_buf);
  }
  else
  {
    m_data = std::move(*raw_packet);
    // Then shift begin() right to where the payload begins (size() adjusted down accordingly).
    m_data.start_past_prefix(cur_data - m_data.const_data());
    assert(m_data.const_begin() == cur_data);
  }
  // m_data.[begin, end()) range is now the right thing, regardless of how it got there.
  assert(m_data.size() == data_size);

  if (m_opt_rexmit_on)
  {
    FLOW_LOG_TRACE("Deserialized low-level [" << m_type_ostream_manip << "] packet with "
                   "sequence number [" << m_seq_num << "] rexmit_id [" << int(m_rexmit_id) << "]; "
                   "user data size [" << data_size << "] (" << (prefer_no_move ? "copied" : "moved") << "); "
                   "serialized size beyond common header was [" << raw_buf_size << "].");
  }
  else
  {
    FLOW_LOG_TRACE("Deserialized low-level [" << m_type_ostream_manip << "] packet with "
                   "sequence number [" << m_seq_num << "]; "
                   "user data size [" << data_size << "] (" << (prefer_no_move ? "copied" : "moved") << "); "
                   "serialized size beyond common header was [" << raw_buf_size << "].");
  }

  return true;
} // Data_packet::deserialize_type_specific_data_from_raw_data_packet()

bool Ack_packet::deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                                     [[maybe_unused]] bool prefer_no_move,
                                                                     [[maybe_unused]] util::Blob* raw_packet)
  // Virtual.
{
  using boost::endian::little_to_native;
  using std::numeric_limits;

  using pkts_size_raw_t = decltype(m_rcv_acked_packets_rexmit_out_size);

  // Structure: a few required fields; then 0 or more acknowledgments, each consisting of constant # of fields.

  // Use same techniques as in Low_lvl_packet::create_from_raw_data_packet().  Keeping comments light.

  const size_t min_data_size = sizeof m_rcv_wnd + sizeof(pkts_size_raw_t);
  const size_t raw_buf_size = raw_buf->size();
  if (raw_buf_size < min_data_size)
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level [" << m_type_ostream_manip << "] "
                     "packet: The packet is too small: ["<< raw_buf_size << "] bytes.");
    return false;
  }
  // else

  const uint8_t* data = static_cast<const uint8_t*>(raw_buf->data());
  (*raw_buf) = ((*raw_buf) + min_data_size);

  const auto& rcv_wnd_raw = *reinterpret_cast<const rcv_wnd_t*>(data);
  data += sizeof rcv_wnd_raw;
  m_rcv_wnd = little_to_native(rcv_wnd_raw);

  /* m_rcv_acked_packets.size() is encoded separately, even though we could figure it out by counting the bytes not yet
   * deserialized to this point and dividing by size of each individual ack.  The reason is explained in a similar
   * comment in Data_packet::deserialize_type_specific_data_from_raw_data_packet() -- see that comment. */
  auto pkts_size_raw = *reinterpret_cast<const pkts_size_raw_t*>(data);
  data += sizeof pkts_size_raw;
  pkts_size_raw = little_to_native(pkts_size_raw);

  assert(data == static_cast<const uint8_t*>(raw_buf->data()));

  const size_t ack_size
    = sizeof(Sequence_number::seq_num_t) + sizeof(ack_delay_t) + (m_opt_rexmit_on ? sizeof(rexmit_id_t) : 0);
  const size_t n_acks = raw_buf->size() / ack_size; // Note: rounds down.
  (*raw_buf) = ((*raw_buf) + (n_acks * ack_size));

  if ((n_acks > numeric_limits<pkts_size_raw_t>::max()) // Don't let overflow fools us or something like that.
      || (pkts_size_raw_t(n_acks) != pkts_size_raw))
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level [" << m_type_ostream_manip << "] "
                     "packet: The individual acks area specifies [" << pkts_size_raw << "] acks but has "
                     "actual ack count [" << n_acks << "].  Did truncation occur somewhere?");
    return false;
  }
  // else

  m_rcv_acked_packets.clear();
  m_rcv_acked_packets.reserve(n_acks);

  for (size_t ack_idx = 0; ack_idx != n_acks; ++ack_idx)
  {
    const auto& seq_num_raw = *reinterpret_cast<const Sequence_number::seq_num_t*>(data);
    data += sizeof seq_num_raw;
    Sequence_number seq_num;
    seq_num.set_raw_num(little_to_native(seq_num_raw));
    // Local seq. # line, but we don't (yet) know for which socket, so max-block-size and ISN are (for now) unknown.
    seq_num.set_metadata('L');

    const auto& ack_delay_raw = *reinterpret_cast<const ack_delay_t*>(data);
    data += sizeof ack_delay_raw;
    const Fine_duration ack_delay
      = Ack_delay_time_unit(little_to_native(ack_delay_raw));

    unsigned int rexmit_id;
    if (m_opt_rexmit_on)
    {
      const auto& rexmit_id_raw = *reinterpret_cast<const rexmit_id_t*>(data);
      data += sizeof rexmit_id_raw;
      rexmit_id = static_cast<unsigned int>(little_to_native(rexmit_id_raw));
    }
    else
    {
      rexmit_id = 0;
    }

    m_rcv_acked_packets.push_back(Individual_ack::Ptr(new Individual_ack{ seq_num, ack_delay, rexmit_id }));
  } // for (all acks)

  assert(data == static_cast<const uint8_t*>(raw_buf->data()));

  // If the acks area was not an exact multiple of size of each ack, then bytes will be left over (bad).

  const size_t remaining_buf_size = raw_buf->size();
  if (remaining_buf_size != 0)
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level [" << m_type_ostream_manip << "] packet: "
                     "Unexpected trailing data: [" << remaining_buf_size << "] bytes.");
    return false;
  }
  // else

  FLOW_LOG_TRACE("Deserialized low-level [" << m_type_ostream_manip << "] packet with "
                 "[" << m_rcv_acked_packets.size() << "] individual acks; "
                 "rcv_wnd [" << m_rcv_wnd << "]; "
                 "serialized size beyond common header was [" << raw_buf_size << "].");

  return true;
} // Ack_packet::deserialize_type_specific_data_from_raw_data_packet()

bool Rst_packet::deserialize_type_specific_data_from_raw_data_packet(Const_buffer* raw_buf,
                                                                     [[maybe_unused]] bool prefer_no_move,
                                                                     [[maybe_unused]] util::Blob* raw_packet)
  // Virtual.
{
  const size_t raw_buf_size = raw_buf->size();
  if (raw_buf_size != 0)
  {
    FLOW_LOG_WARNING("Unable to deserialize low-level [" << m_type_ostream_manip << "] packet: "
                     "Unexpected trailing data: [" << raw_buf_size << "] bytes.");
    return false;
  }
  // else

  FLOW_LOG_WARNING("Deserialized low-level [" << m_type_ostream_manip << "] packet.");

  return true;
}

std::ostream& Low_lvl_packet::to_ostream(std::ostream& os, [[maybe_unused]] bool verbose) const // Virtual.
{
  constexpr char INDENTATION[] = "    ";

  // Reminder: This is just a part (the leading part) of the ultimate output for the entire object of the sub-type.

  return os << "Low-level packet " << this << ":\n"
            << INDENTATION << "type: " << m_type_ostream_manip << "\n"
            << INDENTATION << "m_src_port: " << m_packed.m_src_port << "\n"
            << INDENTATION << "m_dst_port: " << m_packed.m_dst_port << "\n"
            << INDENTATION << "m_opt_rexmit_on: " << m_opt_rexmit_on << "\n";
}

std::ostream& Syn_packet::to_ostream(std::ostream& os, bool verbose) const // Virtual.
{
  using util::buffers_to_ostream;
  using boost::asio::buffer;

  constexpr char INDENTATION[] = "    ";
  constexpr char INDENTATION2[] = "        ";

  this->Low_lvl_packet::to_ostream(os, verbose); // Super-method.

  os << INDENTATION << "m_init_seq_num: " << m_init_seq_num << "\n";
  if (!m_serialized_metadata.empty())
  {
    os << INDENTATION << "m_serialized_metadata (" << m_serialized_metadata.size() << " bytes):\n";
    buffers_to_ostream(os,
                       buffer(m_serialized_metadata.const_data(), m_serialized_metadata.size()),
                       INDENTATION2);
  }

  return os;
}

std::ostream& Syn_ack_packet::to_ostream(std::ostream& os, bool verbose) const // Virtual.
{
  constexpr char INDENTATION[] = "    ";

  this->Low_lvl_packet::to_ostream(os, verbose); // Super-method.

  return os << INDENTATION << "m_init_seq_num: " << m_init_seq_num << "\n"
            << INDENTATION << "m_rcv_wnd: " << m_packed.m_rcv_wnd << "\n"
            << INDENTATION << "m_security_token: " << m_packed.m_security_token << "\n";
}

std::ostream& Syn_ack_ack_packet::to_ostream(std::ostream& os, bool verbose) const // Virtual.
{
  constexpr char INDENTATION[] = "    ";

  this->Low_lvl_packet::to_ostream(os, verbose); // Super-method.

  return os << INDENTATION << "m_rcv_wnd: " << m_packed.m_rcv_wnd << "\n"
            << INDENTATION << "m_security_token: " << m_packed.m_security_token << "\n";
}

std::ostream& Data_packet::to_ostream(std::ostream& os, bool verbose) const // Virtual.
{
  using util::buffers_to_ostream;
  using boost::asio::buffer;

  constexpr char INDENTATION[] = "    ";
  constexpr char INDENTATION2[] = "        ";

  this->Low_lvl_packet::to_ostream(os, verbose); // Super-method.

  os << INDENTATION << "m_seq_num: " << m_seq_num << "\n";
  if (m_opt_rexmit_on)
  {
    os << INDENTATION << "m_rexmit_id: " << int(m_rexmit_id) << "\n";
  }
  if (!m_data.empty())
  {
    // Subtlety: m_data_size is ensured to equal m_data.size; and it may be meaningless; so don't output it per se.
    os << INDENTATION << "m_data (m_data_size=" << m_data.size() << " bytes)" << (verbose ? ':' : '.') << "\n";
    if (verbose)
    {
      buffers_to_ostream(os, buffer(m_data.const_data(), m_data.size()), INDENTATION2);
    }
  }

  return os;
}

std::ostream& Ack_packet::to_ostream(std::ostream& os, bool verbose) const // Virtual.
{
  using boost::asio::buffer;

  constexpr char INDENTATION[] = "    ";

  this->Low_lvl_packet::to_ostream(os, verbose); // Super-method.

  os << INDENTATION << "m_rcv_wnd: " << m_rcv_wnd << "\n";
  if (!m_rcv_acked_packets.empty())
  {
    os << INDENTATION << "m_rcv_acked_packets [INCOMING] (" << m_rcv_acked_packets.size() << " items):\n";
    if (m_opt_rexmit_on)
    {
      for (auto ack : m_rcv_acked_packets)
      {
        os << INDENTATION << INDENTATION << ack->m_seq_num << '/' << ack->m_rexmit_id
           << " : [" << ack->m_delay << "]\n";
      }
    }
    else
    {
      for (auto ack : m_rcv_acked_packets)
      {
        os << INDENTATION << INDENTATION << ack->m_seq_num << " : [" << ack->m_delay << "]\n";
      }
    }
  }
  const Ack_delay_time_unit TIME_UNIT(1);
  if (!m_rcv_acked_packets_rexmit_on_out.empty())
  {
    os << INDENTATION << "m_rcv_acked_packets rexmit=[on] [OUTGOING] "
                           "(m_rcv_acked_packets_rexmit_out_size="
                        << m_rcv_acked_packets_rexmit_out_size << '='
                        << m_rcv_acked_packets_rexmit_on_out.size() << " items):\n";
    for (const auto& ack : m_rcv_acked_packets_rexmit_on_out)
    {
      os << INDENTATION << INDENTATION << ack.m_basic_ack.m_seq_num_raw << '/' << int(ack.m_rexmit_id)
         << " : " << ack.m_basic_ack.m_delay << " x [" << TIME_UNIT << "]\n";
    }
  }
  else if (!m_rcv_acked_packets_rexmit_off_out.empty()) // @todo Consider code reuse here vs. the preceding snippet.
  {
    os << INDENTATION << "m_rcv_acked_packets rexmit=[off] [OUTGOING] "
                           "(m_rcv_acked_packets_rexmit_out_size="
                        << m_rcv_acked_packets_rexmit_out_size << '='
                        << m_rcv_acked_packets_rexmit_off_out.size() << " items):\n";
    for (const auto& ack : m_rcv_acked_packets_rexmit_off_out)
    {
      os << INDENTATION << INDENTATION << ack.m_seq_num_raw
         << " : " << ack.m_delay << " x [" << TIME_UNIT << "]\n";
    }
  }

  return os;
}

std::ostream& Low_lvl_packet::type_to_ostream(std::ostream& os) const
{
  using std::type_index;

  return os << type_id_to_str(type_index(typeid(*this)));
}

Ack_packet::Individual_ack_rexmit_off::Individual_ack_rexmit_off(const Sequence_number& seq_num,
                                                                 ack_delay_t delay) :
  m_seq_num_raw(seq_num.raw_num_ref()),
  m_delay(delay)
{
  // Nothing else.
}

Ack_packet::Individual_ack_rexmit_on::Individual_ack_rexmit_on(const Sequence_number& seq_num,
                                                               unsigned int rexmit_id,
                                                               ack_delay_t delay) :
  m_basic_ack(seq_num, delay),
  m_rexmit_id(Data_packet::rexmit_id_t(rexmit_id))
{
  // Nothing else.
}

} // namespace flow::net_flow
