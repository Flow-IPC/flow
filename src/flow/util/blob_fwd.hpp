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
#include <memory>

namespace flow::util
{
// Types.

// Find doc headers near the bodies of these compound types.

template<typename Allocator = std::allocator<uint8_t>, bool S_SHARING_ALLOWED = false>
class Basic_blob;
template<bool S_SHARING_ALLOWED = false>
class Blob_with_log_context;

/**
 * Short-hand for a Basic_blob that allocates/deallocates in regular heap and is itself assumed to be stored
 * in heap or on stack; sharing feature compile-time-disabled (with perf boost as a result).
 *
 * @see Consider also #Blob which takes a log::Logger at construction and stores it; so it is not
 *      necessary to provide one to each individual API one wants to log.  It also adds logging where it is normally not
 *      possible (as of this writing at least the dtor).  See Basic_blob doc header "Logging" section for brief
 *      discussion of trade-offs.
 */
using Blob_sans_log_context = Basic_blob<>;

/**
 * Identical to #Blob_sans_log_context but with sharing feature compile-time-enabled.  The latter fact implies
 * a small perf hit; see details in Basic_blob doc header.
 *
 * @see Consider also #Sharing_blob; and see #Blob_sans_log_context similar "See" note for more info as to why.
 */
using Sharing_blob_sans_log_context = Basic_blob<std::allocator<uint8_t>, true>;

/**
 * A concrete Blob_with_log_context that compile-time-disables Basic_blob::share() and the sharing API derived from it.
 * It is likely the user will refer to #Blob (or #Sharing_blob) rather than Blob_with_log_context.
 *
 * @see Also consider #Blob_sans_log_context.
 */
using Blob = Blob_with_log_context<>;

/**
 * A concrete Blob_with_log_context that compile-time-enables Basic_blob::share() and the sharing API derived from it.
 * It is likely the user will refer to #Sharing_blob (or #Blob) rather than Blob_with_log_context.
 *
 * @see Also consider #Sharing_blob_sans_log_context.
 */
using Sharing_blob = Blob_with_log_context<true>;

// Free functions.

/**
 * Returns `true` if and only if both given objects are not `zero() == true`, and they either co-own a common underlying
 * buffer, or *are* the same object.  Note: by the nature of Basic_blob::share(), a `true` returned value is orthogonal
 * to whether Basic_blob::start() and Basic_blob::size() values are respectively equal; `true` may be returned even if
 * their [`begin()`, `end()`) ranges don't overlap at all -- as long as the allocated buffer is co-owned by the 2
 * `Basic_blob`s.
 *
 * If `&blob1 != &blob2`, `true` indicates `blob1` was obtained from `blob2` via a chain of Basic_blob::share() (or
 * wrapper thereof)  calls, or vice versa.
 *
 * @relatesalso Basic_blob
 * @param blob1
 *        Object.
 * @param blob2
 *        Object.
 * @return Whether `blob1` and `blob2` both operate on the same underlying buffer.
 */
template<typename Allocator, bool S_SHARING_ALLOWED>
bool blobs_sharing(const Basic_blob<Allocator, S_SHARING_ALLOWED>& blob1,
                   const Basic_blob<Allocator, S_SHARING_ALLOWED>& blob2);

/**
 * Equivalent to `blob1.swap(blob2)`.
 *
 * @relatesalso Basic_blob
 * @param blob1
 *        Object.
 * @param blob2
 *        Object.
 * @param logger_ptr
 *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
 */
template<typename Allocator, bool S_SHARING_ALLOWED>
void swap(Basic_blob<Allocator, S_SHARING_ALLOWED>& blob1,
          Basic_blob<Allocator, S_SHARING_ALLOWED>& blob2, log::Logger* logger_ptr = 0);

/**
 * On top of the similar Basic_blob related function, logs using the stored log context of `blob1`.
 *
 * @relatesalso Blob_with_log_context
 * @param blob1
 *        See super-class related API.
 * @param blob2
 *        See super-class related API.
 */
template<bool S_SHARING_ALLOWED>
void swap(Blob_with_log_context<S_SHARING_ALLOWED>& blob1, Blob_with_log_context<S_SHARING_ALLOWED>& blob2);

} // namespace flow::util
