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
#include <type_traits>

namespace flow::log
{

// Global initializations.

thread_local Msg_metadata this_thread_sync_msg_metadata;
static_assert(std::is_trivially_destructible_v<Msg_metadata>,
              "We want this fully available even while thread_local deinit is happening, for FLOW_LOG_DO_LOG() et al.");

// Static initializations.

thread_local Fixed_string Logger::s_this_thread_nickname;
static_assert(std::is_trivially_destructible_v<Fixed_string>,
              "We want this fully available even while thread_local deinit is happening, for FLOW_LOG_DO_LOG() et al.");

// Logger implementations.

void Logger::this_thread_set_logged_nickname(util::String_view thread_nickname, Logger* logger_ptr,
                                             bool also_set_os_name) // Static.
{
  using std::string;
  using boost::system::system_category;
  using ::pthread_setname_np;
  using ::pthread_self;

  /* Either set or delete (empty means no nickname, which is the original state of things).
   * Note: This value is saved in a thread-local fashion.  This has no effect on the
   * value of s_this_thread_nickname in any thread except the one in which we currently execute.
   *
   * For reason(s) explained in its doc header, the target is a Fixed_string; hence truncate if necessary. */
  {
    const auto thread_nickname_maybe_trunc = thread_nickname.substr(0, Fixed_string::static_capacity);
    // util::String_view may or may not be supported by Fixed_string, so just explicit form, so it'll always compile.
    s_this_thread_nickname.assign(thread_nickname_maybe_trunc.data(), thread_nickname_maybe_trunc.size());
  }

  // Log about it if given an object capable of logging about itself.
  if (logger_ptr)
  {
    FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_LOG);
    FLOW_LOG_INFO("Set new thread nickname for current thread ID [" << util::this_thread::get_id() << "] / "
                  "thread-token [" << util::this_thread_unique_token() << "].");
    /* (Regarding that unique-token: While we do not generally log unique-tokens per-message <=> do not keep
     * it in Msg_metadata -- consult the latter's doc header for some discussion -- it can be a useful
     * truly-unique, within a process, thread identifier albeit one with less universal recognition.  This is
     * a cheap place to log it once, without having to store it per-message.  This can help correlate information
     * in the field despite by default only appearing here.) */
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
        const Error_code sys_err_code{errno, system_category()};
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

  /* If there's a thread nickname, output that.  Otherwise default to actual thread ID.
   * ATTN: This must be consistent with behavior in set_thread_info*().
   * We could also just use the latter here, but the following is a bit quicker.  @todo Reconsider maybe. */

  if (!s_this_thread_nickname.empty())
  {
    return os << s_this_thread_nickname;
  }
  // else
  return os << util::this_thread::get_id();
}

void Logger::set_thread_info(Fixed_string* call_thread_nickname, flow::util::Thread_id* call_thread_id) // Static.
{
  assert(call_thread_nickname);
  assert(call_thread_id);

  /* If there's a thread nickname, output that.  Otherwise default to actual thread ID.
   * ATTN: This must be consistent with behavior in this_thread_logged_name_os_manip() (and our overloads). */

  if (s_this_thread_nickname.empty())
  {
    *call_thread_id = util::this_thread::get_id();
  }
  else
  {
    *call_thread_nickname = s_this_thread_nickname; // Fixed_string => Fixed_string.
  }
}

void Logger::set_thread_info(std::string* call_thread_nickname, flow::util::Thread_id* call_thread_id) // Static.
{
  assert(call_thread_nickname);
  assert(call_thread_id);

  // Mirror overload.  @todo Maybe code reuse?  Template?  Meh.
  if (s_this_thread_nickname.empty())
  {
    *call_thread_id = util::this_thread::get_id();
  }
  else
  {
    *call_thread_nickname = s_this_thread_nickname; // Fixed_string => string.
  }
}

void Logger::set_thread_info(util::String_view* call_thread_nickname, flow::util::Thread_id* call_thread_id) // Static.
{
  assert(call_thread_nickname);
  assert(call_thread_id);

  // Mirror overload.  @todo Maybe code reuse?  Template?  Meh.
  if (s_this_thread_nickname.empty())
  {
    *call_thread_id = util::this_thread::get_id();
  }
  else
  {
    // Fixed_string => String_view.
    *call_thread_nickname = {s_this_thread_nickname.begin(), s_this_thread_nickname.size()};
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
  const auto appender_or_null = Thread_local_string_appender::this_thread_string_appender(*this);
  return appender_or_null ? appender_or_null->appender_ostream() : nullptr; // Latter can happen near thread exit.
}

// Component implementations.

Component::Component() :
  m_payload_type_or_null(nullptr), // <=> empty() == true.
  m_payload_enum_raw_value() // Should not be necessary (uninit=OK), but in some contexts at least gcc-9 warns.
{
  // That's it.
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
  if (&src != this)
  {
    operator=(static_cast<const Log_context&>(src));
    src.m_logger = nullptr;
    src.m_component = {};
  }
  return *this;
}

Logger* Log_context::get_logger() const
{
  return m_logger;
}

Logger* Log_context::set_logger(Logger* logger)
{
  const auto prev = m_logger;
  m_logger = logger;
  return prev;
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

// Log_context_mt implementations.

Log_context_mt::Log_context_mt(Logger* logger) :
  Log_context(logger)
{
  // Nothing.
}

Log_context_mt::Log_context_mt(const Log_context_mt& src) :
  Log_context() // Eliminate possible warning at tiny (if any) perf cost.
{
  // We could just do `operator=(src)`; but to avoid unnecessary locking of this->m_mutex do it manually.
  util::Lock_guard<decltype(m_mutex)> lock{src.m_mutex};
  Log_context::operator=(src);
}

Log_context_mt::Log_context_mt(Log_context_mt&& src) :
  Log_context() // Clear *this in preparation for swap.
{
  using std::swap; // This enables proper ADL.

  // We could just do `operator=(move(src))`; but to avoid unnecessary locking of this->m_mutex do it manually.
  util::Lock_guard<decltype(m_mutex)> lock{src.m_mutex};
  swap(static_cast<Log_context&>(*this),
       static_cast<Log_context&>(src));
}

Log_context_mt& Log_context_mt::operator=(const Log_context_mt& src)
{
  using Lock = util::Lock_guard<decltype(m_mutex)>;

  if (&src != this)
  {
    /* Naively we'd do something close to:
     *   Lock lock1{m_mutex};
     *   Lock lock2{src.m_mutex};
     *   Log_context::operator=(src);
     * However conceivably this could cause an obscure deadlock for reasons similar to those cited in swap().  As there:
     * Seems there's no choice but to lock things piecewise and execute the copy-assignment via a temporary
     * intermediary Log_context. */

    Log_context obj_tmp;
    {
      Lock lock{src.m_mutex};
      obj_tmp = src;
    }
    {
      Lock lock{m_mutex};
      Log_context::operator=(obj_tmp);
    }
  }
  return *this;
}

Log_context_mt& Log_context_mt::operator=(Log_context_mt&& src)
{
  using Lock = util::Lock_guard<decltype(m_mutex)>;
  using std::swap; // This enables proper ADL.

  if (&src != this)
  {
    // Same deal as in copy ctor; just have to add the clearing of `src` which we do by using swap(L_c&, L_c&).

    Log_context obj_tmp;
    {
      Lock lock{src.m_mutex};
      swap(obj_tmp, static_cast<Log_context&>(src));
    }
    {
      Lock lock{m_mutex};
      Log_context::operator=(obj_tmp);
    }
  }

  return *this;
} // Log_context_mt::operator=(&&)

Logger* Log_context_mt::set_logger(Logger* logger)
{
  util::Lock_guard<decltype(m_mutex)> lock{m_mutex};
  return Log_context::set_logger(logger);
}

void Log_context_mt::swap(Log_context_mt& other)
{
  using Lock = util::Lock_guard<decltype(m_mutex)>;
  using std::swap; // This enables proper ADL.

  /* Naively we'd do something close to:
   *   Lock lock1{m_mutex};
   *   Lock lock2{other.m_mutex};
   *   Log_context::swap(other);
   * However conceivably this could cause an obscure deadlock; e.g. at least if one concurrently tries
   *   lc_mt1.swap(lc_mt2);
   * and
   *   lc_mt2.swap(lc_mt1);
   * Strange thing to do, but it is legal, and a classic AB-BA deadlock results.
   * Seems there's no choice but to lock things in series and use a temporary intermediary.
   * (We could've also let the default std::swap() just do one move-construct and two move-assignments, but
   * perf-wise that'd do some unnecessary stuff.) */

  auto& obj1_mt = *this;
  auto& obj2_mt = other;
  auto& obj1_rw = static_cast<Log_context&>(obj1_mt);
  auto& obj2_rw = static_cast<Log_context&>(obj2_mt);

  Log_context tmp_rw;
  {
    Lock lock{obj1_mt.m_mutex};
    tmp_rw = obj1_rw;
  }
  {
    Lock lock{obj2_mt.m_mutex};
    swap(tmp_rw, obj2_rw);
  }
  {
    Lock lock{obj1_mt.m_mutex};
    obj1_rw = tmp_rw;
  }
} // Log_context_mt::swap()

void swap(Log_context_mt& val1, Log_context_mt& val2)
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
    const auto os = logger_ptr->this_thread_ostream();
    if (os)
    {
      beautify_chrono_ostream(os);
    }
    // else { Nothing left to beautify (near thread exit).  It is fine. }
  }
}

size_t deep_size(const Msg_metadata&)
{
  /* We're following the loose pattern explained at the end of Async_file_logger::mem_cost()...
   * ...and since we (now) lack any non-shallow memory use in any members, the right answer is just zero.
   * (Historically there was an std::string, so we had return deep_size(<that thing>); it is now Fixed_string.) */
  return 0;
}

} // namespace flow::log
