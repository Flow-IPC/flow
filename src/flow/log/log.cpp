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
#include "flow/log/log.hpp"
#include "flow/error/error.hpp"
#include "flow/util/util_fwd.hpp"

namespace flow::log
{

// Global initializations.

boost::thread_specific_ptr<Msg_metadata> this_thread_sync_msg_metadata_ptr;

// Static initializations.

boost::thread_specific_ptr<std::string> Logger::s_this_thread_nickname_ptr;

// Logger implementations.

void Logger::this_thread_set_logged_nickname(util::String_view thread_nickname, Logger* logger_ptr,
                                             bool also_set_os_name) // Static.
{
  using std::string;
  using boost::system::system_category;
  using ::pthread_setname_np;
  using ::pthread_self;

  /* Either set or delete (0 means no nickname, which is the original state of things).
   * Note: This value is saved in a thread-local fashion. This has no effect on the
   * value of s_this_thread_nickname.get() or dereference thereof in any thread except
   * the one in which we currently execute. */
  s_this_thread_nickname_ptr.reset(thread_nickname.empty()
                                     ? 0
                                     : new string(thread_nickname));

  // Log about it if given an object capable of logging about itself.
  if (logger_ptr)
  {
    FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_LOG);
    FLOW_LOG_INFO("Set new thread nickname for current thread ID "
                  "[" << util::this_thread::get_id() << "].");
  }

  if (also_set_os_name)
  {
#ifndef FLOW_OS_LINUX
    static_assert(false, "this_thread_set_logged_nickname() also_set_os_name implementation is for Linux only "
                           "for now.");
    /* `man pthread_setname_np` works in Darwin/Mac but is very short, and there is no `man pthread_getname_np`.
     * It might work fine for Mac, but it's untested, and I (ygoldfel) didn't want to deal with it yet.
     * In particular it's unclear if the MAX_PTHREAD_NAME_SZ would apply (it's not in `man` for Mac)... etc.
     * @todo Look into it. */
#endif
    string os_name = thread_nickname.empty() ? util::ostream_op_string(util::this_thread::get_id())
                                             : string(thread_nickname);

    // See `man pthread_setname_np`.  There is a hard limit on the length of the name, and it is:
    constexpr size_t MAX_PTHREAD_NAME_SZ = 15;
    if (os_name.size() > MAX_PTHREAD_NAME_SZ)
    {
      // As advertised: Truncate.  `man` indicates not doing so shall lead to ERANGE error.
      os_name.erase(MAX_PTHREAD_NAME_SZ);
    }

    const auto result_code = pthread_setname_np(pthread_self(), os_name.c_str());

    // Log about it if given an object capable of logging about itself.
    if (logger_ptr)
    {
      FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_LOG);
      if (result_code == -1)
      {
        const Error_code sys_err_code(errno, system_category());
        FLOW_LOG_WARNING("Unable to set OS thread name to [" << os_name << "], possibly truncated "
                         "to [" << MAX_PTHREAD_NAME_SZ << "] characters, via pthread_setname_np().  "
                         "This should only occur due to an overlong name, which we guard against, so this is "
                         "highly unexpected.  Details follow.");
        FLOW_ERROR_SYS_ERROR_LOG_WARNING();
      }
      else
      {
        FLOW_LOG_INFO("OS thread name has been set to [" << os_name << "], possibly truncated "
                      "to [" << MAX_PTHREAD_NAME_SZ << "] characters.");
      }
    }

    // We could continue, but it's indicative of a newly-broken API or misunderstanding of `man`: better to be alerted.
    assert(result_code != -1);
  } // if (also_set_os_name)
} // Logger::this_thread_set_logged_nickname()

std::ostream& Logger::this_thread_logged_name_os_manip(std::ostream& os) // Static.
{
  // Reminder: we are an ostream manipulator, invoked like flush or endl: os << flush;

  /* If there's a thread nickname, output that. Otherwise default to actual thread ID.
   * ATTN: This must be consistent with behavior in set_thread_info*().
   * We could also just use the latter here, but the following is a bit quicker.  @todo Reconsider maybe. */

  auto const this_thread_nickname_ptr = s_this_thread_nickname_ptr.get();
  if (this_thread_nickname_ptr)
  {
    return os << (*this_thread_nickname_ptr);
  }
  // else
  return os << util::this_thread::get_id();
}

void Logger::set_thread_info(std::string* call_thread_nickname, flow::util::Thread_id* call_thread_id) // Static.
{
  assert(call_thread_nickname);
  assert(call_thread_id);

  /* If there's a thread nickname, output that. Otherwise default to actual thread ID.
   * ATTN: This must be consistent with behavior in this_thread_logged_name_os_manip(). */

  auto const this_thread_nickname_ptr = s_this_thread_nickname_ptr.get();
  if (this_thread_nickname_ptr)
  {
    *call_thread_nickname = (*this_thread_nickname_ptr);
  }
  else
  {
    *call_thread_id = util::this_thread::get_id();
  }
}

void Logger::set_thread_info_in_msg_metadata(Msg_metadata* msg_metadata) // Static.
{
  assert(msg_metadata);
  set_thread_info(&msg_metadata->m_call_thread_nickname, &msg_metadata->m_call_thread_id);
}

std::ostream* Logger::this_thread_ostream() const
{
  // Grab the stream used for the current thread by this particular Logger.
  return Thread_local_string_appender::get_this_thread_string_appender(*this)->appender_ostream();
}

// Component implementations.

Component::Component() :
  m_payload_type_or_null(0) // <=> empty() == true.
{
  // That's it.  m_payload_enum_raw_value is uninitialized.
}

Component::Component(const Component& src) = default;
Component::Component(Component&& src_moved) = default; // Note it doesn't empty()-ify src_moved.
Component& Component::operator=(const Component& src) = default;
Component& Component::operator=(Component&& src_moved) = default; // Note it doesn't empty()-ify src_moved.

bool Component::empty() const
{
  return !m_payload_type_or_null;
}

const std::type_info& Component::payload_type() const
{
  assert(!empty()); // We advertised undefined behavior in this case.
  return *m_payload_type_or_null;
}

Component::enum_raw_t Component::payload_enum_raw_value() const
{
  assert(!empty()); // We advertised undefined behavior in this case.
  return m_payload_enum_raw_value;
}

// Log_context implementations.

Log_context::Log_context(Logger* logger) :
  m_logger(logger)
{
  // Nothing.
}

Log_context::Log_context(const Log_context& src) = default;

Log_context::Log_context(Log_context&& src)
{
  operator=(std::move(src));
}

Log_context& Log_context::operator=(const Log_context& src) = default;

Log_context& Log_context::operator=(Log_context&& src)
{
  using std::swap;

  if (&src != this)
  {
    m_logger = 0;
    m_component = Component();

    swap(*this, src);
  }
  return *this;
}

Logger* Log_context::get_logger() const
{
  return m_logger;
}

const Component& Log_context::get_log_component() const
{
  return m_component;
}

void Log_context::swap(Log_context& other)
{
  using std::swap;

  swap(m_logger, other.m_logger);
  swap(m_component, other.m_component);
}

void swap(Log_context& val1, Log_context& val2)
{
  val1.swap(val2);
}

// Sev implementations.

std::ostream& operator<<(std::ostream& os, Sev val)
{
  // Note: Must satisfy istream_to_enum() requirements.
  switch (val)
  {
    case Sev::S_NONE: return os << "NONE";
    case Sev::S_FATAL: return os << "FATAL";
    case Sev::S_ERROR: return os << "ERROR";
    case Sev::S_WARNING: return os << "WARNING";
    case Sev::S_INFO: return os << "INFO";
    case Sev::S_DEBUG: return os << "DEBUG";
    case Sev::S_TRACE: return os << "TRACE";
    case Sev::S_DATA: return os << "DATA";
    case Sev::S_END_SENTINEL: assert(false && "Should not be printing sentinel.");
  }

  assert(false && "Looks like a corrupt/sentinel log::Sev value.  gcc would've caught an incomplete switch().");
  return os;
}

std::istream& operator>>(std::istream& is, Sev& val)
{
  // Range [NONE, END_SENTINEL); no match => NONE; allow for number instead of ostream<< string; case-insensitive.
  val = util::istream_to_enum(&is, Sev::S_NONE, Sev::S_END_SENTINEL);
  return is;
}

// Free function implementations.

void beautify_chrono_logger_this_thread(Logger* logger_ptr)
{
  using util::beautify_chrono_ostream;

  if (logger_ptr)
  {
    beautify_chrono_ostream(logger_ptr->this_thread_ostream());
  }
}

size_t deep_size(const Msg_metadata& val)
{
  // We're following the loose pattern explained at the end of Async_file_logger::mem_cost().

  using util::deep_size;

  /* Reminder: exclude sizeof(val); include only non-shallow memory used on val's behalf; so
   * sum of deep_size(X), for each X that is (1) a member of Msg_metadata; and (2) has a deep_size(X) free function.
   * As of this writing there is just one: */
  return deep_size(val.m_call_thread_nickname);
}

} // namespace flow::log
