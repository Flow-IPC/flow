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

#include "flow/util/util_fwd.hpp"
#include "flow/common.hpp"

/**
 * Flow module that facilitates working with error codes and exceptions; essentially comprised of niceties on top
 * boost.system's error facility.  As of this writing, we feel the latter facility is fantastic and quite complete;
 * so as of this writing flow::error is intended to contain only minor syntactic-sugary items.
 *
 * @note Flow uses the convention wherein error-related facilities of a particular Flow module X (in
 *       `namespace flow::X`) -- especially in particular X's dedicated error code set, if any -- are to reside
 *       in `namespace flow::X::error`, mirroring the naming of up-one-level-from-that `flow::error`.
 *
 * @see flow::Error_code whose doc header introduces the error-emission conventions used in all Flow modules.
 */
namespace flow::error
{
// Types.

// Find doc headers near the bodies of these compound types.

class Runtime_error;

// Free functions.

/**
 * Helper for FLOW_ERROR_EXEC_AND_THROW_ON_ERROR() macro that does everything in the latter not needing a preprocessor.
 * Probably not to be called except by that macro.
 *
 * This function is the meat of it.  I omit details, since they are explained in FLOW_ERROR_EXEC_AND_THROW_ON_ERROR()
 * doc header.  The present function does what is described therein -- except for preprocessor-requiring actions:
 *
 * Preprocessor is needed to (1) supply halfway-decent "context" (file/line #/etc.) info to the
 * exception object; and (2) to trigger the invoker of the macro (the actual user-facing API or very close to it)
 * to return XYZ to *its* caller (user code or very close to it).  So this function cannot do that, but instead it
 * respectively (1) takes the context string as an argument; and (2) returns `true` if and only if the invoker of the
 * macro should in fact immediately return the value `*ret`, where `ret` is an "out" argument
 * to the present function.
 *
 * @note ATTENTION!  This is a (rare) case where documentation is more complicated than just looking at the
 *       implementation.  This embarrassingly verbose doc header is sort of a pedantic formality in this particular
 *       (rare) case and is probably not worth reading for most people.
 * @tparam Func
 *         Any type such that given an instance `Func f`, the expression `r = f(&e_c)` is valid,
 *         assuming `e_c` is an #Error_code, and `r` is a `Ret`.  In practice, this is usually the insane
 *         type of whatever concoction lambda or `bind()` conjures out of a plain, hard-working method like
 *         Peer_socket::send() and a bunch of innocent, API user-originated arguments thereto.
 * @tparam Ret
 *         The return type of the operation `func()`.  Should equal `ARG_ret_type` of
 *         FLOW_ERROR_EXEC_AND_THROW_ON_ERROR().
 * @param func
 *        The operation, with the return type and argument as described above in `Func` template argument doc,
 *        that performs whatever possibly error-generating actions are being wrapped by all this; and returns
 *        whatever value is intended for the ultimate API caller.  The `Error_code*` passed into `func()` will NOT be
 *        null; thus `func()` must NOT throw Runtime_error on error.
 * @param ret
 *        Non-null pointer to a value into which the present function will place the return value of `func()`, if
 *        indeed it executes the latter (and thus `true` is returned).
 * @param err_code
 *        Null; or non-null pointer to an error code in memory; probably originating from the API caller.
 *        `*err_code` is set if and only if `err_code` was non-null; and `func()` was executed (thus `true` returned);
 *        and `func()` encountered an error.
 * @param context
 *        Value suitable for the `context` argument of Runtime_error constructor.
 * @return `true` if and only if `err_code` is null; and therefore `func()` was called.
 *         `false` otherwise (i.e., `err_code` is not null; nothing was done; and the caller should perform the
 *         equivalent of `func()` while safely assuming `*err_code` may be assigned to upon error).
 */
template<typename Func, typename Ret>
bool exec_and_throw_on_error(const Func& func, Ret* ret,
                             Error_code* err_code, util::String_view context);

/**
 * Equivalent of exec_and_throw_on_error() for operations with `void` return type.  Unlike the latter function,
 * however, the present function is to be used directly as opposed to via macro.
 *
 * @tparam Func
 *         Any type such that given an instance `Func f`, the expression `f(&e_c)` is valid,
 *         assuming `e_c` is an #Error_code.
 * @param func
 *        The operation, with the return type `void` and argument as described above in `Func` template argument doc,
 *        that performs whatever possibly error-generating actions are being wrapped by all this.
 *        The `Error_code*` passed into `func()` will NOT be null; thus `func()` must NOT throw Runtime_error on error.
 * @param err_code
 *        See exec_and_throw_on_error().
 * @param context
 *        See exec_and_throw_on_error().
 * @return See exec_and_throw_on_error().
 */
template<typename Func>
bool exec_void_and_throw_on_error(const Func& func, Error_code* err_code, util::String_view context);

} // namespace flow::error
