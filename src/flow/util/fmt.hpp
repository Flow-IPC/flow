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

/* #include this to use fmt's full API.  Reason: With some `gcc`s there's a gcc bug which causes
 * false warnings when heavy auto-inlining is turned on.  #include me, and you won't have to repeat
 * the below to avoid issues with such `gcc`s.
 *
 * @todo Eliminate when/if possible.  Some tickets about it:
 *   https://github.com/fmtlib/fmt/issues/2708
 *   https://github.com/fmtlib/fmt/issues/3334
 *   https://github.com/gabime/spdlog/issues/2761
 *
 * Also we had to deal with similar in our own code; as of this writing in util/basic_blob.hpp at least. */

#if defined(__GNUC__) && !defined(__clang__)
#  define GCC_COMPILER
#endif

#ifdef GCC_COMPILER
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif

#include <fmt/format.h>
#include <fmt/chrono.h>

#ifdef GCC_COMPILER
#  pragma GCC diagnostic pop
#  undef GCC_COMPILER
#endif
