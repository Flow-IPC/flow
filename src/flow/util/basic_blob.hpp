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

#include "flow/util/blob_fwd.hpp"
#include "flow/log/log.hpp"
#include <boost/interprocess/smart_ptr/shared_ptr.hpp>
#include <boost/move/make_unique.hpp>
#include <optional>
#include <limits>

namespace flow::util
{

/**
 * A hand-optimized and API-tweaked replacement for `vector<uint8_t>`, i.e., buffer of bytes inside an allocated area
 * of equal or larger size; also optionally supports limited garbage-collected memory pool functionality and
 * SHM-friendly custom-allocator support.
 *
 * @see Blob_with_log_context (and especially aliases #Blob and #Sharing_blob), our non-polymorphic sub-class which
 *      adds some ease of use in exchange for a small perf trade-off.  (More info below under "Logging.")
 * @see #Blob_sans_log_context + #Sharing_blob_sans_log_context, each simply an alias to
 *      `Basic_blob<std::allocator, B>` (with `B = false` or `true` respectively), in a fashion vaguely similar to
 *      what `string` is to `basic_string` (a little).  This is much like #Blob/#Sharing_blob, in that it is a
 *      non-template concrete type; but does not take or store a `Logger*`.
 *
 * The rationale for its existence mirrors its essential differences from `vector<uint8_t>` which are as follows.
 * To summarize, though, it exists to guarantee specific performance by reducing implementation uncertainty via
 * lower-level operations; and force user to explicitly authorize any allocation to ensure thoughtfully performant use.
 * Update: Plus, it adds non-prefix-sub-buffer feature, which can be useful for zero-copy deserialization.
 * Update: Plus, it adds a simple form of garbage-collected memory pools, useful for operating multiple `Basic_blob`s
 * that share a common over-arching memory area (buffer).
 * Update: Plus, it adds SHM-friendly custom allocator support.  (While all `vector` impls support custom allocators,
 * only some later versions of gcc `std::vector` work with shared-memory (SHM) allocators and imperfectly at that.
 * `boost::container::vector` a/k/a `boost::interprocess::vector` is fully SHM-friendly.)
 *
 * - It adds a feature over `vector<uint8_t>`: The logical contents `[begin(), end())` can optionally begin
 *   not at the start of the internally allocated buffer but somewhere past it.  In other words, the logical buffer
 *   is not necessarily a prefix of the internal allocated buffer.  This feature is critical when one wants
 *   to use some sub-buffer of a buffer without reallocating a smaller buffer and copying the sub-buffer into it.
 *   For example, if we read a DATA packet the majority of which is the payload, which begins a few bytes
 *   from the start -- past a short header -- it may be faster to keep passing around the whole thing with move
 *   semantics but use only the payload part, after logically
 *   deserializing it (a/k/a zero-copy deserialization semantics).  Of course
 *   one can do this with `vector` as well; but one would need to always remember the prefix length even after
 *   deserializing, at which point such details would be ideally forgotten instead.  So this API is significantly
 *   more pleasant in that case.  Moreover it can then be used generically more easily, alongside other containers.
 * - Its performance is guaranteed by internally executing low-level operations such as `memcpy()` directly instead of
 *   hoping that using a higher-level abstraction will ultimately do the same.
 *   - In particular, the iterator types exposed by the API *are* pointers instead of introducing any performance
 *     uncertainty by possibly using wrapper/proxy iterator class.
 *   - In particular, no element or memory area is *ever* initialized to zero(es) or any other particular filler
 *     value(s).  (This is surprisingly difficult to avoid with STL containers!  Google it.  Though, e.g.,
 *     boost.container does provide a `default_init_t` extension to various APIs like `.resize()`.)  If an allocation
 *     does occur, the area is left as-is unless user specifies a source memory area from which to copy data.
 *   - Note that I am making no assertion about `vector` being slow; the idea is to guarantee *we* aren't by removing
 *     any *question* about it; it's entirely possible a given `vector` is equally fast, but it cannot be guaranteed by
 *     standard except in terms of complexity guarantees (which is usually pretty good but not everything).
 *     - That said a quick story about `std::vector<uint8_t>` (in gcc-8.3 anyway): I (ygoldfel) once used it with
 *       a custom allocator (which worked in shared memory) and stored a megabytes-long buffer in one.  Its
 *       destructor, I noticed, spent milliseconds (with 2022 hardware) -- outside the actual dealloc call.
 *       Reason: It was iterating over every (1-byte) element and invoking its (non-existent/trivial) destructor.  It
 *       did not specialize to avoid this, intentionally so according to a comment, when using a custom allocator.
 *       `boost::container::vector<uint8_t>` lacked this problem; but nevertheless it shows generally written
 *       containers can have hidden such perf quirks.
 * - To help achieve the previous bullet point, as well as to keep the code simple, the class does not parameterize
 *   on element type; it stores unsigned bytes, period (though Basic_blob::value_type is good to use if you need to
 *   refer to that type in code generically).
 *   Perhaps the same could be achieved by specialization, but we don't need the parameterization in the first place.
 * - Unlike `vector`, it has an explicit state where there is no underlying buffer; in this case zero() is `true`.
 *   Also in that case, `capacity() == 0` and `size() == 0` (and `start() == 0`).  `zero() == true` is the case on
 *   default-constructed object of this class.  The reason for this is I am never sure, at least, what a
 *   default-constructed `vector` looks like internally; a null buffer always seemed like a reasonable starting point
 *   worth guaranteeing explicitly.
 * - If `!zero()`:
 *   - make_zero() deallocates any allocated buffer and ensures zero() is `true`, as if upon default construction.
 *   - Like `vector`, it keeps an allocated memory chunk of size M, at the start of which is the
 *     logical buffer of size `N <= M`, where `N == size()`, and `M == capacity()`.  However, `M >= 1` always.
 *     - There is the aforementioned added feature wherein the logical buffer begins to the right of the allocated
 *       buffer, namely at index `start()`.  In this case `M >= start() + size()`, and the buffer range
 *       is in indices `[start(), start() + size())` of the allocated buffer.  By default `start() == 0`, as
 *       in `vector`, but this can be changed via the 2nd, optional, argument to resize().
 *   - Like `vector`, `reserve(Mnew)`, with `Mnew <= M`, does nothing.  However, unlike `vector`, the same call is
 *     *illegal* when `Mnew > M >= 1`.  However, any reserve() call *is* allowed when zero() is `true`.  Therefore,
 *     if the user is intentionally okay with the performance implications of a reallocation, they can call make_zero()
 *     and *then* force the reallocating reserve() call.
 *   - Like `vector`, `resize(Nnew)` merely guarantees post-condition `size() == Nnew`; which means that
 *     it is essentially equivalent to `reserve(Nnew)` followed by setting internal N member to Nnew.
 *     However, remember that resize() therefore keeps all the behaviors of reserve(), including that it cannot
 *     grow the buffer (only allocate it when zero() is `true`).
 *     - If changing `start()` from default, then: `resize(Nnew, Snew)` means `reserve(Nnew + Snew)`, plus saving
 *       internal N and S members.
 * - The *only* way to allocate is to (directly or indirectly) call `reserve(Mnew)` when `zero() == true`.
 *   Moreover, *exactly* Mnew bytes elements are allocated and no more (unlike with `vector`, where the policy used is
 *   not known).  Moreover, if `reserve(Mnew)` is called indirectly (by another method of the class), `Mnew` arg
 *   is set to no greater than size necessary to complete the operation (again, by contrast, it is unknown what `vector`
 *   does w/r/t capacity policy).
 * - The rest of the API is common-sense but generally kept to only what has been necessary to date,
 *   in on-demand fashion.
 *
 * ### Optional, simple garbage-collected shared ownership functionality ###
 * The following feature was added quite some time after `Blob` was first introduced and matured.  However it seamlessly
 * subsumes all of the above basic functionality with full backwards compatibility.  It can also be disabled
 * (and is by default) by setting #S_SHARING to `false` at compile-time.  (This gains back a little bit of perf
 * namely by turning an internal `shared_ptr` to `unique_ptr`.)
 *
 * The feature itself is simple: Suppose one has a blob A, constructed or otherwise `resize()`d or `reserve()`d
 * so as to have `zero() == false`; meaning `capacity() >= 1`.  Now suppose one calls the core method of this *pool*
 * feature: share() which returns a new blob B.  B will have the same exact start(), size(), capacity() -- and,
 * in fact, the pointer `data() - start()` (i.e., the underlying buffer start pointer, buffer being capacity() long).
 * That is, B now shares the underlying memory buffer with A.  Normally, that underlying buffer would be deallocated
 * when either `A.make_zero()` is called, or A is destructed.  Now that it's shared by A and B, however,
 * the buffer is deallocated only once make_zero() or destruction occurs for *both* A and B.  That is, there is an
 * internal (thread-safe) ref-count that must reach 0.
 *
 * Both A and B may now again be share()d into further sharing `Basic_blob`s.  This further increments the ref-count of
 * original buffer; all such `Basic_blob`s C, D, ... must now either make_zero() or destruct, at which point the dealloc
 * occurs.
 *
 * In that way the buffer -- or *pool* -- is *garbage-collected* as a whole, with reserve() (and APIs like resize()
 * and ctors that call it) initially allocating and setting internal ref-count to 1, share() incrementing it, and
 * make_zero() and ~Basic_blob() decrementing it (and deallocating when ref-count=0).
 *
 * ### Application of shared ownership: Simple pool-of-`Basic_blob`s functionality ###
 * The other aspect of this feature is its pool-of-`Basic_blob`s application.  All of the sharing `Basic_blob`s A, B,
 * ... retain all the aforementioned features including the ability to use resize(), start_past_prefix_inc(), etc.,
 * to control the location of the logical sub-range [begin(), end()) within the underlying buffer (pool).
 * E.g., suppose A was 10 bytes, with `start() = 0` and `size() = capacity() = 10`; then share() B is also that way.
 * Now `B.start_past_prefix_inc(5); A.resize(5);` makes it so that A = the 1st 5 bytes of the pool,
 * B the last 5 bytes (and they don't overlap -- can even be concurrently modified safely).  In that way A and B
 * are now independent `Basic_blob`s -- potentially passed, say, to independent TCP-receive calls, each of which reads
 * up to 5 bytes -- that share an over-arching pool.
 *
 * The API share_after_split_left() is a convenience operation that splits a `Basic_blob`'s [begin(), end()) area into
 * 2 areas of specified length, then returns a new Basic_blob representing the first area in the split and
 * modifies `*this` to represent the remainder (the 2nd area).  This simply performs the op described in the preceding
 * paragraph.  share_after_split_right() is similar but acts symmetrically from the right.  Lastly
 * `share_after_split_equally*()` splits a Basic_blob into several equally-sized (except the last one potentially)
 * sub-`Basic_blob`s of size N, where N is an arg.  (It can be thought of as just calling `share_after_split_left(N)`
 * repeatedly, then returning a sequence of the resulting post-split `Basic_blob`s.)
 *
 * To summarize: The `share_after_split*()` APIs are useful to divide (potentially progressively) a pool into
 * non-overlapping `Basic_blob`s within a pool while ensuring the pool continues to exist while `Basic_blob`s refer
 * to any part of it (but no later).  Meanwhile direct use of share() with resize() and `start_past_prefix*()` allows
 * for overlapping such sharing `Basic_blob`s.
 *
 * Note that deallocation occurs regardless of which areas of that pool the relevant `Basic_blob`s represent,
 * and whether they overlap or not (and, for that matter, whether they even together comprise the entire pool or
 * leave "gaps" in-between).  The whole pool is deallocated the moment the last of the co-owning `Basic_blob`s
 * performs either make_zero() or ~Basic_blob() -- the values of start() and size() at the time are not relevant.
 *
 * ### Custom allocator (and SHared Memory) support ###
 * Like STL containers this one optionally takes a custom allocator type (#Allocator_raw) as a compile-time parameter
 * instead of using the regular heap (`std::allocator`).  Unlike many STL container implementations, including
 * at least older `std::vector`, it supports SHM-storing allocators without a constant cross-process vaddr scheme.
 * (Some do support this but with surprising perf flaws when storing raw integers/bytes.  boost.container `vector`
 * has solid support but lacks various other properties of Basic_blob.)  While a detailed discussion is outside
 * our scope here, the main point is internally `*this` stores no raw `value_type*` but rather
 * `Allocator_raw::pointer` -- which in many cases *is* `value_type*`; but for advanced applications like SHM
 * it might be a fancy-pointer like `boost::interprocess::offset_ptr<value_type>`.  For general education
 * check out boost.interprocess docs covering storage of STL containers in SHM.  (However note that the
 * allocators provided by that library are only one option even for SHM storage alone; e.g., they are stateful,
 * and often one would like a stateless -- zero-size -- allocator.  Plus there are other limitations to
 * boost.interprocess SHM support, robust though it is.)
 *
 * ### Logging ###
 * When and if `*this` logs, it is with log::Sev::S_TRACE severity or more verbose.
 *
 * Unlike many other Flow API classes this one does not derive from log::Log_context nor take a `Logger*` in
 * ctor (and store it).  Instead each API method/ctor/function capable of logging takes an optional
 * (possibly null) log::Logger pointer.  If supplied it's used by that API alone (with some minor async exceptions).
 * If you would like more typical Flow-style logging API then use our non-polymorphic sub-class Blob_with_log_context
 * (more likely aliases #Blob, #Sharing_blob).  However consider the following first.
 *
 * Why this design?  Answer:
 *   - Basic_blob is meant to be lean, both in terms of RAM used and processor cycles spent.  Storing a `Logger*`
 *     takes some space; and storing it, copying/moving it, etc., takes a little compute.  In a low-level
 *     API like Basic_blob this is potentially nice to avoid when not actively needed.  (That said the logging
 *     can be extremely useful when debugging and/or profiling RAM use + allocations.)
 *     - This isn't a killer.  The original `Blob` (before Basic_blob existed) stored a `Logger*`, and it was fine.
 *       However:
 *   - Storing a `Logger*` is always okay when `*this` itself is stored in regular heap or on the stack.
 *     However, `*this` itself may be stored in SHM; #Allocator_raw parameterization (see above regarding
 *     "Custom allocator") suggests as much (i.e., if the buffer is stored in SHM, we might be too).
 *     In that case `Logger*` does not, usually, make sense.  As of this writing `Logger` in process 1
 *     has no relationship with any `Logger` in process 2; and even if the `Logger` were stored in SHM itself,
 *     `Logger` would need to be supplied via an in-SHM fancy-pointer, not `Logger*`, typically.  The latter is
 *     a major can of worms and not supported by flow::log in any case as of this writing.
 *     - Therefore, even if we don't care about RAM/perf implications of storing `Logger*` with the blob, at least
 *       in some real applications it makes no sense.
 *
 * #Blob/#Sharing_blob provides this support while ensuring #Allocator_raw (no longer a template parameter in its case)
 * is the vanilla `std::allocator`.  The trade-off is as noted just above.
 *
 * ### Thread safety ###
 * Before share() (or `share_*()`) is called: Essentially: Thread safety is the same as for `vector<uint8_t>`.
 *
 * Without `share*()` any two Basic_blob objects refer to separate areas in memory; hence it is safe to access
 * Basic_blob A concurrently with accessing Basic_blob B in any fashion (read, write).
 *
 * However: If 2 `Basic_blob`s A and B co-own a pool, via a `share*()` chain, then concurrent write and read/write
 * to A and B respectively are thread-safe if and only if their [begin(), end()) ranges don't overlap.  Otherwise,
 * naturally, one would be writing to an area while it is being read simultaneously -- not safe.
 *
 * Tip: When working in `share*()` mode, exclusive use of `share_after_split*()` is a great way to guarantee no 2
 * `Basic_blob`s ever overlap.  Meanwhile one must be careful when using share() directly and/or subsequently sliding
 * the range around via resize(), `start_past_prefix*()`: `A.share()` and A not only (originally) overlap but
 * simply represent the same area of memory; and resize() and co. can turn a non-overlapping range into an overlapping
 * one (encroaching on someone else's "territory" within the pool).
 *
 * @todo Write a class template, perhaps `Tight_blob<Allocator, bool>`, which would be identical to Basic_blob
 * but forego the framing features, namely size() and start(), thus storing only the RAII array pointer data()
 * and capacity(); rewrite Basic_blob in terms of this `Tight_blob`.  This simple container type has had some demand
 * in practice, and Basic_blob can and should be cleanly built on top of it (perhaps even as an IS-A subclass).
 *
 * @tparam Allocator
 *         An allocator, with `value_type` equal to our #value_type, per the standard C++1x `Allocator` concept.
 *         In most uses this shall be left at the default `std::allocator<value_type>` which allocates in
 *         standard heap (`new[]`, `delete[]`).  A custom allocator may be used instead.  SHM-storing allocators,
 *         and generally allocators for which `pointer` is not simply `value_type*` but rather a fancy-pointer
 *         (see cppreference.com) are correctly supported.  (Note this may not be the case for your compiler's
 *         `std::vector`.)
 * @tparam S_SHARING_ALLOWED
 *         If `true`, share() and all derived methods, plus blobs_sharing(), can be instantiated (invoked in compiled
 *         code).  If `false` they cannot (`static_assert()` will trip), but the resulting Basic_blob concrete
 *         class will be slightly more performant (internally, a `shared_ptr` becomes instead a `unique_ptr` which
 *         means smaller allocations and no ref-count logic invoked).
 */
template<typename Allocator, bool S_SHARING_ALLOWED>
class Basic_blob
{
public:
  // Types.

  /// Short-hand for values, which in this case are unsigned bytes.
  using value_type = uint8_t;

  /// Type for index into blob or length of blob or sub-blob.
  using size_type = std::size_t;

  /// Type for difference of `size_type`s.
  using difference_type = std::ptrdiff_t;

  /// Type for iterator pointing into a mutable structure of this type.
  using Iterator = value_type*;

  /// Type for iterator pointing into an immutable structure of this type.
  using Const_iterator = value_type const *;

  /// Short-hand for the allocator type specified at compile-time.  Its element type is our #value_type.
  using Allocator_raw = Allocator;
  static_assert(std::is_same_v<typename Allocator_raw::value_type, value_type>,
                "Allocator template param must be of form A<V> where V is our value_type.");

  /// For container compliance (hence the irregular capitalization): pointer to element.
  using pointer = Iterator;
  /// For container compliance (hence the irregular capitalization): pointer to `const` element.
  using const_pointer = Const_iterator;
  /// For container compliance (hence the irregular capitalization): reference to element.
  using reference = value_type&;
  /// For container compliance (hence the irregular capitalization): reference to `const` element.
  using const_reference = const value_type&;
  /// For container compliance (hence the irregular capitalization): #Iterator type.
  using iterator = Iterator;
  /// For container compliance (hence the irregular capitalization): #Const_iterator type.
  using const_iterator = Const_iterator;

  // Constants.

  /// Value of template parameter `S_SHARING_ALLOWED` (for generic programming).
  static constexpr bool S_SHARING = S_SHARING_ALLOWED;

  /// Special value indicating an unchanged `size_type` value; such as in resize().
  static constexpr size_type S_UNCHANGED = size_type(-1); // Same trick as std::string::npos.

  /**
   * `true` if #Allocator_raw underlying allocator template is simply `std::allocator`; `false`
   * otherwise.
   *
   * Note that if this is `true`, it may be worth using #Blob/#Sharing_blob, instead of its `Basic_blob<std::allocator>`
   * super-class; at the cost of a marginally larger RAM footprint (an added `Logger*`) you'll get a more convenient
   * set of logging API knobs (namely `Logger*` stored permanently from construction; and there will be no need to
   * supply it as arg to subsequent APIs when logging is desired).
   *
   * ### Implications of #S_IS_VANILLA_ALLOC being `false` ###
   * This is introduced in our class doc header.  Briefly however:
   *   - The underlying buffer, if any, and possibly some small aux data shall be allocated
   *     via #Allocator_raw, not simply the regular heap's `new[]` and/or `new`.
   *     - They shall be deallocated, if needed, via #Allocator_raw, not simply the regular heap's
   *       `delete[]` and/or `delete`.
   *   - Because storing a pointer to log::Logger may be meaningless when storing in an area allocated
   *     by some custom allocators (particularly SHM-heap ones), we shall not auto-TRACE-log on dealloc.
   *     - This caveat applies only if #S_SHARING is `true`.
   *
   * @internal
   *   - (If #S_SHARING)
   *     Accordingly the ref-counted buffer pointer #m_buf_ptr shall be a `boost::interprocess::shared_ptr`
   *     instead of a vanilla `shared_ptr`; the latter may be faster and more full-featured, but it is likely
   *     to internally store a raw `T*`; we need one that stores an `Allocator_raw::pointer` instead;
   *     e.g., a fancy-pointer type (like `boost::interprocess::offset_ptr`) when dealing with
   *     SHM-heaps (typically).
   *     - If #S_IS_VANILLA_ALLOC is `true`, then we revert to the faster/more-mature/full-featured
   *       `shared_ptr`.  In particular it is faster (if used with `make_shared()` and similar) by storing
   *       the user buffer and aux data/ref-count in one contiguously-allocated buffer.
   *   - (If #S_SHARING is `false`)
   *     It's a typical `unique_ptr` template either way (because it supports non-raw-pointer storage out of the
   *     box) but:
   *       - A custom deleter is necessary similarly to the above.
   *         - Its `pointer` member alias crucially causes the `unique_ptr` to store
   *           an `Allocator_raw::pointer` instead of a `value_type*`.
   *
   * See #Buf_ptr doc header regarding the latter two bullet points.
   */
  static constexpr bool S_IS_VANILLA_ALLOC = std::is_same_v<Allocator_raw, std::allocator<value_type>>;

  // Constructors/destructor.

  /**
   * Constructs blob with `zero() == true`.  Note this means no buffer is allocated.
   *
   * @param alloc_raw
   *        Allocator to copy and store in `*this` for all buffer allocations/deallocations.
   *        If #Allocator_raw is stateless, then this has size zero, so nothing is copied at runtime,
   *        and by definition it is to equal `Allocator_raw()`.
   */
  Basic_blob(const Allocator_raw& alloc_raw = Allocator_raw{});

  /**
   * Constructs blob with size() and capacity() equal to the given `size`, and `start() == 0`.  Performance note:
   * elements are not initialized to zero or any other value.  A new over-arching buffer (pool) is therefore allocated.
   *
   * Corner case note: a post-condition is `zero() == (size() == 0)`.  Note, also, that the latter is *not* a universal
   * invariant (see zero() doc header).
   *
   * Formally: If `size >= 1`, then a buffer is allocated; and the internal ownership ref-count is set to 1.
   *
   * @param size
   *        A non-negative desired size.
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) or asynchronously when TRACE-logging
   *        in the event of buffer dealloc.  Null allowed.
   * @param alloc_raw
   *        Allocator to copy and store in `*this` for all buffer allocations/deallocations.
   *        If #Allocator_raw is stateless, then this has size zero, so nothing is copied at runtime,
   *        and by definition it is to equal `Allocator_raw()`.
   */
  explicit Basic_blob(size_type size, log::Logger* logger_ptr = nullptr,
                      const Allocator_raw& alloc_raw = Allocator_raw{});

  /**
   * Move constructor, constructing a blob exactly internally equal to pre-call `moved_src`, while the latter is
   * made to be exactly as if it were just constructed as `Basic_blob(nullptr)` (allocator subtleties aside).
   * Performance: constant-time, at most copying a few scalars.
   *
   * @param moved_src
   *        The object whose internals to move to `*this` and replace with a blank-constructed object's internals.
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   */
  Basic_blob(Basic_blob&& moved_src, log::Logger* logger_ptr = nullptr);

  /**
   * Copy constructor, constructing a blob logically equal to `src`.  More formally, guarantees post-condition wherein
   * `[this->begin(), this->end())` range is equal by value (including length) to `src` equivalent range but no memory
   * overlap.  A post-condition is `capacity() == size()`, and `start() == 0`.  Performance: see copying assignment
   * operator.
   *
   * Corner case note: the range equality guarantee includes the degenerate case where that range is empty, meaning we
   * simply guarantee post-condition `src.empty() == this->empty()`.
   *
   * Corner case note 2: post-condition: `this->zero() == this->empty()`
   * (note `src.zero()` state is not necessarily preserved in `*this`).
   *
   * Note: This is `explicit`, which is atypical for a copy constructor, to generate compile errors in hard-to-see
   * (and often unintentional) instances of copying.  Copies of Basic_blob should be quite intentional and explicit.
   * (One example where one might forget about a copy would be when using a Basic_blob argument without `cref` or
   * `ref` in a `bind()`; or when capturing by value, not by ref, in a lambda.)
   *
   * Formally: If `src.size() >= 1`, then a buffer is allocated; and the internal ownership ref-count is set to 1.
   *
   * @param src
   *        Object whose range of bytes of length `src.size()` starting at `src.begin()` is copied into `*this`.
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) or asynchronously when TRACE-logging
   *        in the event of buffer dealloc.  Null allowed.
   */
  explicit Basic_blob(const Basic_blob& src, log::Logger* logger_ptr = nullptr);

  /**
   * Destructor that drops `*this` ownership of the allocated internal buffer if any, as by make_zero();
   * if no other Basic_blob holds ownership of that buffer, then that buffer is deallocated also.
   * Recall that other `Basic_blob`s can only gain co-ownership via `share*()`; hence if one does not use that
   * feature, the destructor will in fact deallocate the buffer (if any).
   *
   * Formally: If `!zero()`, then the internal ownership ref-count is decremented by 1, and if it reaches
   * 0, then a buffer is deallocated.
   *
   * ### Logging ###
   * This will not log, as it is not possible to pass a `Logger*` to a dtor without storing it (which we avoid
   * for reasons outlined in class doc header).  Use #Blob/#Sharing_blob if it is important to log in this situation
   * (although there are some minor trade-offs).
   */
  ~Basic_blob();

  // Methods.

  /**
   * Move assignment.  Allocator subtleties aside and assuming `this != &moved_src` it is equivalent to:
   * `make_zero(); this->swap(moved_src, logger_ptr)`.  (If `this == &moved_src`, this is a no-op.)
   *
   * @param moved_src
   *        See swap().
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   * @return `*this`.
   */
  Basic_blob& assign(Basic_blob&& moved_src, log::Logger* logger_ptr = nullptr);

  /**
   * Move assignment operator (no logging): equivalent to `assign(std::move(moved_src), nullptr)`.
   *
   * @param moved_src
   *        See assign() (move overload).
   * @return `*this`.
   */
  Basic_blob& operator=(Basic_blob&& moved_src);

  /**
   * Copy assignment: assuming `(this != &src) && (!blobs_sharing(*this, src))`,
   * makes `*this` logically equal to `src`; but behavior undefined if a reallocation would be necessary to do this.
   * (If `this == &src`, this is a no-op.  If not but `blobs_sharing(*this, src) == true`, see "Sharing blobs" below.
   * This is assumed to not be the case in further discussion.)
   *
   * More formally:
   * no-op if `this == &src`; "Sharing blobs" behavior if not so, but `src` shares buffer with `*this`; otherwise:
   * Guarantees post-condition wherein `[this->begin(), this->end())` range is equal
   * by value (including length) to `src` equivalent range but no memory overlap.  Post-condition: `start() == 0`;
   * capacity() either does not change or equals size().  capacity() growth is not allowed: behavior is undefined if
   * `src.size()` exceeds pre-call `this->capacity()`, unless `this->zero() == true` pre-call.  Performance: at most a
   * memory area of size `src.size()` is copied and some scalars updated; a memory area of that size is allocated only
   * if required; no ownership drop or deallocation occurs.
   *
   * Corner case note: the range equality guarantee includes the degenerate case where that range is empty, meaning we
   * simply guarantee post-condition `src.empty() == this->empty()`.
   *
   * Corner case note 2: post-condition: if `this->empty() == true` then `this.zero()` has the same value as at entry
   * to this call.  In other words, no deallocation occurs, even if
   * `this->empty() == true` post-condition holds; at most internally a scalar storing size is assigned 0.
   * (You may force deallocation in that case via make_zero() post-call, but this means you'll have to intentionally
   * perform that relatively slow op.)
   *
   * As with reserve(), IF pre-condition `zero() == false`, THEN pre-condition `src.size() <= this->capacity()`
   * must hold, or behavior is undefined (i.e., as noted above, capacity() growth is not allowed except from 0).
   * Therefore, NO REallocation occurs!  However, also as with reserve(), if you want to intentionally allow such a
   * REallocation, then simply first call make_zero(); then execute the
   * `assign()` copy as planned.  This is an intentional restriction forcing caller to explicitly allow a relatively
   * slow reallocation op.
   *
   * Formally: If `src.size() >= 1`, and `zero() == true`, then a buffer is allocated; and the internal ownership
   * ref-count is set to 1.
   *
   * ### Sharing blobs ###
   * If `blobs_sharing(*this, src) == true`, meaning the target and source are operating on the same buffer, then
   * behavior is undefined (assertion may trip).  Rationale for this design is as follows.  The possibilities were:
   *   -# Undefined behavior/assertion.
   *   -# Just adjust `this->start()` and `this->size()` to match `src`; continue co-owning the underlying buffer;
   *      copy no data.
   *   -# `this->make_zero()` -- losing `*this` ownership, while `src` keeps it -- and then allocate a new buffer
   *      and copy `src` data into it.
   *
   * Choosing between these is tough, as this is an odd corner case.  3 is not criminal, but generally no method
   * ever forces make_zero() behavior, always leaving it to the user to consciously do, so it seems prudent to keep
   * to that practice (even though this case is a bit different from, say, resize() -- since make_zero() here has
   * no chance to deallocate anything, only decrement ref-count).  2 is performant and slick but suggests a special
   * behavior in a corner case; this *feels* slightly ill-advised in a standard copy assignment operator.  Therefore
   * it seems better to crash-and-burn (choice 1), in the same way an attempt to resize()-higher a non-zero() blob would
   * crash and burn, forcing the user to explicitly execute what they want.  After all, 3 is done by simply calling
   * make_zero() first; and 2 is possible with a simple resize() call; and the blobs_sharing() check is both easy
   * and performant.
   *
   * @warning A post-condition is `start() == 0`; meaning `start()` at entry is ignored and reset to 0; the entire
   *          (co-)owned buffer -- if any -- is potentially used to store the copied values.  In particular, if one
   *          plans to work on a sub-blob of a shared pool (see class doc header), then using this assignment op is
   *          not advised.  Use emplace_copy() instead; or perform your own copy onto
   *          mutable_buffer().
   *
   * @param src
   *        Object whose range of bytes of length `src.size()` starting at `src.begin()` is copied into `*this`.
   *        Behavior is undefined if pre-condition is `!zero()`, and this memory area overlaps at any point with the
   *        memory area of same size in `*this` (unless that size is zero -- a degenerate case).
   *        (This can occur only via the use of `share*()` -- otherwise `Basic_blob`s always refer to separate areas.)
   *        Also behavior undefined if pre-condition is `!zero()`, and `*this` (co-)owned buffer is too short to
   *        accomodate all `src.size()` bytes (assertion may trip).
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   * @return `*this`.
   */
  Basic_blob& assign(const Basic_blob& src, log::Logger* logger_ptr = nullptr);

  /**
   * Copy assignment operator (no logging): equivalent to `assign(src, nullptr)`.
   *
   * @param src
   *        See assign() (copy overload).
   * @return `*this`.
   */
  Basic_blob& operator=(const Basic_blob& src);

  /**
   * Swaps the contents of this structure and `other`, or no-op if `this == &other`.  Performance: at most this
   * involves swapping a few scalars which is constant-time.
   *
   * @param other
   *        The other structure.
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   */
  void swap(Basic_blob& other, log::Logger* logger_ptr = nullptr);

  /**
   * Applicable to `!zero()` blobs, this returns an identical Basic_blob that shares (co-owns) `*this` allocated buffer
   * along with `*this` and any other `Basic_blob`s also sharing it.  Behavior is undefined (assertion may trip) if
   * `zero() == true`: it is nonsensical to co-own nothing; just use the default ctor then.
   *
   * The returned Basic_blob is identical in that not only does it share the same memory area (hence same capacity())
   * but has identical start(), size() (and hence begin() and end()).  If you'd like to work on a different
   * part of the allocated buffer, please consider `share_after_split*()` instead; the pool-of-sub-`Basic_blob`s
   * paradigm suggested in the class doc header is probably best accomplished using those methods and not share().
   *
   * You can also adjust various sharing `Basic_blob`s via resize(), start_past_prefix_inc(), etc., directly -- after
   * share() returns.
   *
   * Formally: Before this returns, the internal ownership ref-count (shared among `*this` and the returned
   * Basic_blob) is incremented.
   *
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   * @return An identical Basic_blob to `*this` that shares the underlying allocated buffer.  See above.
   */
  Basic_blob share(log::Logger* logger_ptr = nullptr) const;

  /**
   * Applicable to `!zero()` blobs, this shifts `this->begin()` by `size` to the right without changing
   * end(); and returns a Basic_blob containing the shifted-past values that shares (co-owns) `*this` allocated buffer
   * along with `*this` and any other `Basic_blob`s also sharing it.
   *
   * More formally, this is identical to simply `auto b = share(); b.resize(size); start_past_prefix_inc(size);`.
   *
   * This is useful when working in the pool-of-sub-`Basic_blob`s paradigm.  This and other `share_after_split*()`
   * methods are usually better to use rather than share() directly (for that paradigm).
   *
   * Behavior is undefined (assertion may trip) if `zero() == true`.
   *
   * Corner case: If `size > size()`, then it is taken to equal size().
   *
   * Degenerate case: If `size` (or size(), whichever is smaller) is 0, then this method is identical to
   * share().  Probably you don't mean to call share_after_split_left() in that case, but it's your decision.
   *
   * Degenerate case: If `size == size()` (and not 0), then `this->empty()` becomes `true` -- though
   * `*this` continues to share the underlying buffer despite [begin(), end()) becoming empty.  Typically this would
   * only be done as, perhaps, the last iteration of some progressively-splitting loop; but it's your decision.
   *
   * Formally: Before this returns, the internal ownership ref-count (shared among `*this` and the returned
   * Basic_blob) is incremented.
   *
   * @param size
   *        Desired size() of the returned Basic_blob; and the number of elements by which `this->begin()` is
   *        shifted right (hence start() is incremented).  Any value exceeding size() is taken to equal it.
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   * @return The split-off-on-the-left Basic_blob that shares the underlying allocated buffer with `*this`.  See above.
   */
  Basic_blob share_after_split_left(size_type size, log::Logger* logger_ptr = nullptr);

  /**
   * Identical to share_after_split_left(), except `this->end()` shifts by `size` to the left (instead of
   * `this->begin() to the right), and the split-off Basic_blob contains the *right-most* `size` elements
   * (instead of the left-most).
   *
   * More formally, this is identical to simply
   * `auto lt_size = size() - size; auto b = share(); resize(lt_size); b.start_past_prefix_inc(lt_size);`.
   * Cf. share_after_split_left() formal definition and note the symmetry.
   *
   * All other characteristics are as written for share_after_split_left().
   *
   * @param size
   *        Desired size() of the returned Basic_blob; and the number of elements by which `this->end()` is
   *        shifted left (hence size() is decremented).  Any value exceeding size() is taken to equal it.
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   * @return The split-off-on-the-right Basic_blob that shares the underlying allocated buffer with `*this`.  See above.
   */
  Basic_blob share_after_split_right(size_type size, log::Logger* logger_ptr = nullptr);

  /**
   * Identical to successively performing `share_after_split_left(size)` until `this->empty() == true`;
   * the resultings `Basic_blob`s are emitted via `emit_blob_func()` callback in the order they're split off from
   * the left.  In other words this partitions a non-zero() `Basic_blob` -- perhaps typically used as a pool --
   * into equally-sized (except possibly the last one) adjacent sub-`Basic_blob`s.
   *
   * A post-condition is that `empty() == true` (`size() == 0`).  In addition, if `headless_pool == true`,
   * then `zero() == true` is also a post-condition; i.e., the pool is "headless": it disappears once all the
   * resulting sub-`Basic_blob`s drop their ownership (as well as any other co-owning `Basic_blob`s).
   * Otherwise, `*this` will continue to share the pool despite size() becoming 0.  (Of course, even then, one is
   * free to make_zero() or destroy `*this` -- the former, before returning, is all that `headless_pool == true`
   * really adds.)
   *
   * Behavior is undefined (assertion may trip) if `empty() == true` (including if `zero() == true`, but even if not)
   * or if `size == 0`.
   *
   * @see share_after_split_equally_emit_seq() for a convenience wrapper to emit to, say, `vector<Basic_blob>`.
   * @see share_after_split_equally_emit_ptr_seq() for a convenience wrapper to emit to, say,
   *      `vector<unique_ptr<Basic_blob>>`.
   *
   * @tparam Emit_blob_func
   *         A callback compatible with signature `void F(Basic_blob&& blob_moved)`.
   * @param size
   *        Desired size() of each successive out-Basic_blob, except the last one.  Behavior undefined (assertion may
   *        trip) if not positive.
   * @param headless_pool
   *        Whether to perform `this->make_zero()` just before returning.  See above.
   * @param emit_blob_func
   *        `F` such that `F(std::move(blob))` shall be called with each successive sub-Basic_blob.
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   */
  template<typename Emit_blob_func>
  void share_after_split_equally(size_type size, bool headless_pool, Emit_blob_func&& emit_blob_func,
                                 log::Logger* logger_ptr = nullptr);

  /**
   * share_after_split_equally() wrapper that places `Basic_blob`s into the given container via
   * `push_back()`.
   *
   * @tparam Blob_container
   *         Something with method compatible with `push_back(Basic_blob&& blob_moved)`.
   * @param size
   *        See share_after_split_equally().
   * @param headless_pool
   *        See share_after_split_equally().
   * @param out_blobs
   *        `out_blobs->push_back()` shall be executed 1+ times.
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   */
  template<typename Blob_container>
  void share_after_split_equally_emit_seq(size_type size, bool headless_pool, Blob_container* out_blobs,
                                          log::Logger* logger_ptr = nullptr);

  /**
   * share_after_split_equally() wrapper that places `Ptr<Basic_blob>`s into the given container via
   * `push_back()`, where the type `Ptr<>` is determined via `Blob_ptr_container::value_type`.
   *
   * @tparam Blob_ptr_container
   *         Something with method compatible with `push_back(Ptr&& blob_ptr_moved)`,
   *         where `Ptr` is `Blob_ptr_container::value_type`, and `Ptr(new Basic_blob)` can be created.
   *         `Ptr` is to be a smart pointer type such as `unique_ptr<Basic_blob>` or `shared_ptr<Basic_blob>`.
   * @param size
   *        See share_after_split_equally().
   * @param headless_pool
   *        See share_after_split_equally().
   * @param out_blobs
   *        `out_blobs->push_back()` shall be executed 1+ times.
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   */
  template<typename Blob_ptr_container>
  void share_after_split_equally_emit_ptr_seq(size_type size, bool headless_pool, Blob_ptr_container* out_blobs,
                                              log::Logger* logger_ptr = nullptr);

  /**
   * Replaces logical contents with a copy of the given non-overlapping area anywhere in memory.  More formally:
   * This is exactly equivalent to copy-assignment (`*this = b`), where `const Basic_blob b` owns exactly
   * the memory area given by `src`.  However, note the newly relevant restriction documented for `src` parameter below
   * (no overlap allowed).
   *
   * All characteristics are as written for the copy assignment operator, including "Formally" and the warning.
   *
   * @param src
   *        Source memory area.  Behavior is undefined if pre-condition is `!zero()`, and this memory area overlaps
   *        at any point with the memory area of same size at `begin()`.  Otherwise it can be anywhere at all.
   *        Also behavior undefined if pre-condition is `!zero()`, and `*this` (co-)owned buffer is too short to
   *        accomodate all `src.size()` bytes (assertion may trip).
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   * @return Number of elements copied, namely `src.size()`, or simply size().
   */
  size_type assign_copy(const boost::asio::const_buffer& src, log::Logger* logger_ptr = nullptr);

  /**
   * Copies `src` buffer directly onto equally sized area within `*this` at location `dest`; `*this` must have
   * sufficient size() to accomodate all of the data copied.  Performance: The only operation performed is a copy from
   * `src` to `dest` using the fastest reasonably available technique.
   *
   * None of the following changes: zero(), empty(), size(), capacity(), begin(), end(); nor the location (or size) of
   * internally stored buffer.
   *
   * @param dest
   *        Destination location within this blob.  This must be in `[begin(), end()]`; and,
   *        unless `src.size() == 0`, must not equal end() either.
   * @param src
   *        Source memory area.  Behavior is undefined if this memory area overlaps
   *        at any point with the memory area of same size at `dest` (unless that size is zero -- a degenerate
   *        case).  Otherwise it can be anywhere at all, even partially or fully within `*this`.
   *        Also behavior undefined if `*this` blob is too short to accomodate all `src.size()` bytes
   *        (assertion may trip).
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   * @return Location in this blob just past the last element copied; `dest` if none copied; in particular end() is a
   *         possible value.
   */
  Iterator emplace_copy(Const_iterator dest, const boost::asio::const_buffer& src, log::Logger* logger_ptr = nullptr);

  /**
   * The opposite of emplace_copy() in every way, copying a sub-blob onto a target memory area.  Note that the size
   * of that target buffer (`dest.size()`) determines how much of `*this` is copied.
   *
   * @param src
   *        Source location within this blob.  This must be in `[begin(), end()]`; and,
   *        unless `dest.size() == 0`, must not equal end() either.
   * @param dest
   *        Destination memory area.  Behavior is undefined if this memory area overlaps
   *        at any point with the memory area of same size at `src` (unless that size is zero -- a degenerate
   *        case).  Otherwise it can be anywhere at all, even partially or fully within `*this`.
   *        Also behavior undefined if `src + dest.size()` is past end of `*this` blob (assertion may trip).
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   * @return Location in this blob just past the last element copied; `src` if none copied; end() is a possible value.
   */
  Const_iterator sub_copy(Const_iterator src, const boost::asio::mutable_buffer& dest,
                          log::Logger* logger_ptr = nullptr) const;

  /**
   * Returns number of elements stored, namely `end() - begin()`.  If zero(), this is 0; but if this is 0, then
   * zero() may or may not be `true`.
   *
   * @return See above.
   */
  size_type size() const;

  /**
   * Returns the offset between `begin()` and the start of the internally allocated buffer.  If zero(), this is 0; but
   * if this is 0, then zero() may or may not be `true`.
   *
   * @return See above.
   */
  size_type start() const;

  /**
   * Returns `size() == 0`.  If zero(), this is `true`; but if this is `true`, then
   * zero() may or may not be `true`.
   *
   * @return See above.
   */
  bool empty() const;

  /**
   * Returns the number of elements in the internally allocated buffer, which is 1 or more; or 0 if no buffer
   * is internally allocated.  Some formal invariants: `(capacity() == 0) == zero()`; `start() + size() <= capacity()`.
   *
   * See important notes on capacity() policy in the class doc header.
   *
   * @return See above.
   */
  size_type capacity() const;

  /**
   * Returns `false` if a buffer is allocated and owned; `true` otherwise.  See important notes on how this relates
   * to empty() and capacity() in those methods' doc headers.  See also other important notes in class doc header.
   *
   * Note that zero() is `true` for any default-constructed Basic_blob.
   *
   * @return See above.
   */
  bool zero() const;

  /**
   * Ensures that an internal buffer of at least `capacity` elements is allocated and owned; disallows growing an
   * existing buffer; never shrinks an existing buffer; if a buffer is allocated, it is no larger than `capacity`.
   *
   * reserve() may be called directly but should be formally understood to be called by resize(), assign_copy(),
   * copy assignment operator, copy constructor.  In all cases, the value passed to reserve() is exactly the size
   * needed to perform the particular task -- no more (and no less).  As such, reserve() policy is key to knowing
   * how the class behaves elsewhere.  See class doc header for discussion in larger context.
   *
   * Performance/behavior: If zero() is true pre-call, `capacity` sized buffer is allocated.  Otherwise,
   * no-op if `capacity <= capacity()` pre-call.  Behavior is undefined if `capacity > capacity()` pre-call
   * (again, unless zero(), meaning `capacity() == 0`).  In other words, no deallocation occurs, and an allocation
   * occurs only if necessary.  Growing an existing buffer is disallowed.  However, if you want to intentionally
   * REallocate, then simply first check for `zero() == false` and call make_zero() if that holds; then execute the
   * `reserve()` as planned.  This is an intentional restriction forcing caller to explicitly allow a relatively slow
   * reallocation op.  You'll note a similar suggestion for the other reserve()-using methods/operators.
   *
   * Formally: If `capacity >= 1`, and `zero() == true`, then a buffer is allocated; and the internal ownership
   * ref-count is set to 1.
   *
   * @param capacity
   *        Non-negative desired minimum capacity.
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) or asynchronously when TRACE-logging
   *        in the event of buffer dealloc.  Null allowed.
   */
  void reserve(size_type capacity, log::Logger* logger_ptr = nullptr);

  /**
   * Guarantees post-condition `zero() == true` by dropping `*this` ownership of the allocated internal buffer if any;
   * if no other Basic_blob holds ownership of that buffer, then that buffer is deallocated also.  Recall that
   * other `Basic_blob`s can only gain co-ownership via `share*()`; hence if one does not use that feature, make_zero()
   * will in fact deallocate the buffer (if any).
   *
   * That post-condition can also be thought of as `*this` becoming indistinguishable from a default-constructed
   * Basic_blob.
   *
   * Performance/behavior: Assuming zero() is not already `true`, this will deallocate capacity() sized buffer
   * and save a null pointer.
   *
   * The many operations that involve reserve() in their doc headers will explain importance of this method:
   * As a rule, no method except make_zero() allows one to request an ownership-drop or deallocation of the existing
   * buffer, even if this would be necessary for a larger buffer to be allocated.  Therefore, if you intentionally want
   * to allow such an operation, you CAN, but then you MUST explicitly call make_zero() first.
   *
   * Formally: If `!zero()`, then the internal ownership ref-count is decremented by 1, and if it reaches
   * 0, then a buffer is deallocated.
   *
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) only.  Null allowed.
   */
  void make_zero(log::Logger* logger_ptr = nullptr);

  /**
   * Guarantees post-condition `size() == size` and `start() == start`; no values in pre-call range `[begin(), end())`
   * are changed; any values *added* to that range by the call are not initialized to zero or otherwise.
   *
   * From other invariants and behaviors described, you'll realize
   * this essentially means `reserve(size + start)` followed by saving `size` and `start` into internal size members.
   * The various implications of this can be deduced by reading the related methods' doc headers.  The key is to
   * understand how reserve() works, including what it disallows (growth in size of an existing buffer).
   *
   * Formally: If `size >= 1`, and `zero() == true`, then a buffer is allocated; and the internal ownership
   * ref-count is set to 1.
   *
   * ### Leaving start() unmodified ###
   * `start` is taken to be the value of arg `start_or_unchanged`; unless the latter is set to special value
   * #S_UNCHANGED; in which case `start` is taken to equal start().  Since the default is indeed #S_UNCHANGED,
   * the oft-encountered expression `resize(N)` will adjust only size() and leave start() unmodified -- often the
   * desired behavior.
   *
   * @param size
   *        Non-negative desired value for size().
   * @param start_or_unchanged
   *        Non-negative desired value for start(); or special value #S_UNCHANGED.  See above.
   * @param logger_ptr
   *        The Logger implementation to use in *this* routine (synchronously) or asynchronously when TRACE-logging
   *        in the event of buffer dealloc.  Null allowed.
   */
  void resize(size_type size, size_type start_or_unchanged = S_UNCHANGED, log::Logger* logger_ptr = nullptr);

  /**
   * Restructures blob to consist of an internally allocated buffer and a `[begin(), end)` range starting at
   * offset `prefix_size` within that buffer.  More formally, it is a simple resize() wrapper that ensures
   * the internally allocated buffer remains unchanged or, if none is currently large enough to
   * store `prefix_size` elements, is allocated to be of size `prefix_size`; and that `start() == prefix_size`.
   *
   * All of resize()'s behavior, particularly any restrictions about capacity() growth, applies, so in particular
   * remember you may need to first make_zero() if the internal buffer would need to be REallocated to satisfy the
   * above requirements.
   *
   * In practice, with current reserve() (and thus resize()) restrictions -- which are intentional -- this method is
   * most useful if you already have a Basic_blob with internally allocated buffer of size *at least*
   * `n == size() + start()` (and `start() == 0` for simplicity), and you'd like to treat this buffer as containing
   * no-longer-relevant prefix of length S (which becomes new value for start()) and have size() be readjusted down
   * accordingly, while `start() + size() == n` remains unchaged.  If the buffer also contains irrelevant data
   * *past* a certain offset N, you can first make it irrelevant via `resize(N)` (then call `start_past_prefix(S)`
   * as just described):
   *
   *   ~~~
   *   Basic_blob b;
   *   // ...
   *   // b now has start() == 0, size() == M.
   *   // We want all elements outside [S, N] to be irrelevant, where S > 0, N < M.
   *   // (E.g., first S are a frame prefix, while all bytes past N are a frame postfix, and we want just the frame
   *   // without any reallocating or copying.)
   *   b.resize(N);
   *   b.start_past_prefix(S);
   *   // Now, [b.begin(), b.end()) are the frame bytes, and no copying/allocation/deallocation has occurred.
   *   ~~~
   *
   * @param prefix_size
   *        Desired prefix length.  `prefix_size == 0` is allowed and is a degenerate case equivalent to:
   *        `resize(start() + size(), 0)`.
   */
  void start_past_prefix(size_type prefix_size);

  /**
   * Like start_past_prefix() but shifts the *current* prefix position by the given *incremental* value
   * (positive or negative).  Identical to `start_past_prefix(start() + prefix_size_inc)`.
   *
   * Behavior is undefined for negative `prefix_size_inc` whose magnitue exceeds start() (assertion may trip).
   *
   * Behavior is undefined in case of positive `prefix_size_inc` that results in overflow.
   *
   * @param prefix_size_inc
   *        Positive, negative (or zero) increment, so that start() is changed to `start() + prefix_size_inc`.
   */
  void start_past_prefix_inc(difference_type prefix_size_inc);

  /**
   * Equivalent to `resize(0, start())`.
   *
   * Note that the value returned by start() will *not* change due to this call.  Only size() (and the corresponding
   * internally stored datum) may change.  If one desires to reset start(), use resize() directly (but if one
   * plans to work on a sub-Basic_blob of a shared pool -- see class doc header -- please think twice first).
   */
  void clear();

  /**
   * Performs the minimal number of operations to make range `[begin(), end())` unchanged except for lacking
   * sub-range `[first, past_last)`.
   *
   * Performance/behavior: At most, this copies the range `[past_last, end())` to area starting at `first`;
   * and then adjusts internally stored size member.
   *
   * @param first
   *        Pointer to first element to erase.  It must be dereferenceable, or behavior is undefined (assertion may
   *        trip).
   * @param past_last
   *        Pointer to one past the last element to erase.  If `past_last <= first`, call is a no-op.
   * @return Iterator equal to `first`.  (This matches standard expectation for container `erase()` return value:
   *         iterator to element past the last one erased.  In this contiguous sequence that simply equals `first`,
   *         since everything starting with `past_last` slides left onto `first`.  In particular:
   *         If `past_last()` equaled `end()` at entry, then the new end() is returned: everything starting with
   *         `first` was erased and thus `first == end()` now.  If nothing is erased `first` is still returned.)
   */
  Iterator erase(Const_iterator first, Const_iterator past_last);

  /**
   * Returns pointer to mutable first element; or end() if empty().  Null is a possible value in the latter case.
   *
   * @return Pointer, possibly null.
   */
  Iterator begin();

  /**
   * Returns pointer to immutable first element; or end() if empty().  Null is a possible value in the latter case.
   *
   * @return Pointer, possibly null.
   */
  Const_iterator const_begin() const;

  /**
   * Equivalent to const_begin().
   *
   * @return Pointer, possibly null.
   */
  Const_iterator begin() const;

  /**
   * Returns pointer one past mutable last element; empty() is possible.  Null is a possible value in the latter case.
   *
   * @return Pointer, possibly null.
   */
  Iterator end();

  /**
   * Returns pointer one past immutable last element; empty() is possible.  Null is a possible value in the latter case.
   *
   * @return Pointer, possibly null.
   */
  Const_iterator const_end() const;

  /**
   * Equivalent to const_end().
   *
   * @return Pointer, possibly null.
   */
  Const_iterator end() const;

  /**
   * Returns reference to immutable first element.  Behavior is undefined if empty().
   *
   * @return See above.
   */
  const value_type& const_front() const;

  /**
   * Returns reference to immutable last element.  Behavior is undefined if empty().
   *
   * @return See above.
   */
  const value_type& const_back() const;

  /**
   * Equivalent to const_front().
   *
   * @return See above.
   */
  const value_type& front() const;

  /**
   * Equivalent to const_back().
   *
   * @return See above.
   */
  const value_type& back() const;

  /**
   * Returns reference to mutable first element.  Behavior is undefined if empty().
   *
   * @return See above.
   */
  value_type& front();

  /**
   * Returns reference to mutable last element.  Behavior is undefined if empty().
   *
   * @return See above.
   */
  value_type& back();

  /**
   * Equivalent to const_begin().
   *
   * @return Pointer, possibly null.
   */
  value_type const * const_data() const;

  /**
   * Equivalent to begin().
   *
   * @return Pointer, possibly null.
   */
  value_type* data();

  /**
   * Synonym of const_begin().  Exists as standard container method (hence the odd formatting).
   *
   * @return See const_begin().
   */
  Const_iterator cbegin() const;

  /**
   * Synonym of const_end().  Exists as standard container method (hence the odd formatting).
   *
   * @return See const_end().
   */
  Const_iterator cend() const;

  /**
   * Returns `true` if and only if: `this->derefable_iterator(it) || (it == this->const_end())`.
   *
   * @param it
   *        Iterator/pointer to check.
   * @return See above.
   */
  bool valid_iterator(Const_iterator it) const;

  /**
   * Returns `true` if and only if the given iterator points to an element within this blob's size() elements.
   * In particular, this is always `false` if empty(); and also when `it == this->const_end()`.
   *
   * @param it
   *        Iterator/pointer to check.
   * @return See above.
   */
  bool derefable_iterator(Const_iterator it) const;

  /**
   * Convenience accessor returning an immutable boost.asio buffer "view" into the entirety of the blob.
   * Equivalent to `const_buffer(const_data(), size())`.
   *
   * @return See above.
   */
  boost::asio::const_buffer const_buffer() const;

  /**
   * Same as const_buffer() but the returned view is mutable.
   * Equivalent to `mutable_buffer(data(), size())`.
   *
   * @return See above.
   */
  boost::asio::mutable_buffer mutable_buffer();

  /**
   * Returns a copy of the internally cached #Allocator_raw as set by a constructor or assign() or
   * assignment-operator, whichever happened last.
   *
   * @return See above.
   */
  Allocator_raw get_allocator() const;

protected:
  // Constants.

  /// Our flow::log::Component.
  static constexpr Flow_log_component S_LOG_COMPONENT = Flow_log_component::S_UTIL;

  // Methods.

  /**
   * Impl of share_after_split_equally() but capable of emitting not just `*this` type (`Basic_blob<...>`)
   * but any sub-class (such as `Blob`/`Sharing_blob`) provided a functor like share_after_split_left() but returning
   * an object of that appropriate type.
   *
   * @tparam Emit_blob_func
   *         See share_after_split_equally(); however it is to take the type to emit which can
   *         be `*this` Basic_blob or a sub-class.
   * @tparam Share_after_split_left_func
   *         A callback with signature identical to share_after_split_left() but returning
   *         the same type emitted by `Emit_blob_func`.
   * @param size
   *        See share_after_split_equally().
   * @param headless_pool
   *        See share_after_split_equally().
   * @param emit_blob_func
   *        See `Emit_blob_func`.
   * @param logger_ptr
   *        See share_after_split_equally().
   * @param share_after_split_left_func
   *        See `Share_after_split_left_func`.
   */
  template<typename Emit_blob_func, typename Share_after_split_left_func>
  void share_after_split_equally_impl(size_type size, bool headless_pool,
                                      Emit_blob_func&& emit_blob_func,
                                      log::Logger* logger_ptr,
                                      Share_after_split_left_func&& share_after_split_left_func);

private:

  // Types.

  /**
   * Internal deleter functor used if and only if #S_IS_VANILLA_ALLOC is `false` and therefore only with
   * #Buf_ptr being `boost::interprocess::shared_ptr` or
   * deleter-parameterized `unique_ptr`.  Basically its task is to undo the
   * `m_alloc_raw.allocate()` call made when allocating a buffer in reserve(); the result of that call is
   * passed-to `shared_ptr::reset()` or `unique_ptr::reset()`; as is #m_alloc_raw (for any aux allocation needed,
   * but only for `shared_ptr` -- `unique_ptr` needs no aux data); as is
   * an instance of this Deleter_raw (to specifically dealloc the buffer when the ref-count reaches 0).
   * (In the `unique_ptr` case there is no ref-count per se; or one can think of it as a ref-count that equals 1.)
   *
   * Note that Deleter_raw is used only to dealloc the buffer actually controlled by the `shared_ptr` group
   * or `unique_ptr`.  `shared_ptr` will use the #Allocator_raw directly to dealloc aux data.  (We guess Deleter_raw
   * is a separate argument to `shared_ptr` to support array deletion; `boost::interprocess:shared_ptr` does not
   * provide built-in support for `U[]` as the pointee type; but the deleter can do whatever it wants/needs.)
   *
   * Note: this is not used except with custom #Allocator_raw.  With `std::allocator` the usual default `delete[]`
   * behavior is fine.
   */
  class Deleter_raw
  {
  public:
    // Types.

    /**
     * Short-hand for the allocator's pointer type, pointing to Basic_blob::value_type.
     * This may or may not simply be `value_type*`; in cases including SHM-storing allocators without
     * a constant cross-process vaddr scheme it needs to be a fancy-pointer type instead (e.g.,
     * `boost::interprocess::offset_ptr<value_type>`).
     */
    using Pointer_raw = typename std::allocator_traits<Allocator_raw>::pointer;

    /// For `boost::interprocess::shared_ptr` and `unique_ptr` compliance (hence the irregular capitalization).
    using pointer = Pointer_raw;

    // Constructors/destructor.

    /**
     * Default ctor: Must never be invoked; suitable only for a null smart-pointer.
     * Without this a `unique_ptr<..., Deleter_Raw>` cannot be default-cted.
     */
    Deleter_raw();

    /**
     * Constructs deleter by memorizing the allocator (of zero size if #Allocator_raw is stateless, usually)
     * used to allocate whatever shall be passed-to `operator()()`; and the size (in # of `value_type`s)
     * of the buffer allocated there.  The latter is required, at least technically, because
     * `Allocator_raw::deallocate()` requires the value count, equal to that when `allocate()` was called,
     * to be passed-in.  Many allocators probably don't really need this, as array size is typically recorded
     * invisibly near the array itself, but formally this is not guaranteed for all allocators.
     *
     * @param alloc_raw
     *        Allocator to copy and store.
     * @param buf_sz
     *        See above.
     */
    explicit Deleter_raw(const Allocator_raw& alloc_raw, size_type buf_sz);

    // Methods.

    /**
     * Deallocates using `Allocator_raw::deallocate()`, passing-in the supplied pointer (to `value_type`) `to_delete`
     * and the number of `value_type`s to delete (from ctor).
     *
     * @param to_delete
     *        Pointer to buffer to delete as supplied by `shared_ptr` or `unique_ptr` internals.
     */
    void operator()(Pointer_raw to_delete);

  private:
    // Data.

    /**
     * See ctor: the allocator that `operator()()` shall use to deallocate.  For stateless allocators this
     * typically has size zero.
     *
     * ### What's with `optional<>`? ###
     * ...Okay, so actually this has size (whatever `optional` adds, probably a `bool`) + `sizeof(Allocator_raw)`,
     * the latter being indeed zero for stateless allocators.  Why use `optional<>` though?  Well, we only do
     * to support stateful allocators which cannot be default-cted; and our own default ctor requires that
     * #m_alloc_raw is initialized to *something*... even though it (by default ctor contract) will never be accessed.
     *
     * It is slightly annoying that we waste the extra space for `optional` internals even when `Allocator_raw`
     * is stateless (and it is often stateless!).  Plus, when #Buf_ptr is `shared_ptr` instead of `unique_ptr`
     * our default ctor is not even needed.  Probably some meta-programming thing could be done to avoid even this
     * overhead, but in my (ygoldfel) opinion the overhead is so minor, it does not even rise to the level of a to-do.
     */
    std::optional<Allocator_raw> m_alloc_raw;

    /// See ctor and operator()(): the size of the buffer to deallocate.
    size_type m_buf_sz;
  }; // class Deleter_raw

  /**
   * The smart-pointer type used for #m_buf_ptr; a custom-allocator-and-SHM-friendly impl and parameterization is
   * used if necessary; otherwise a more typical concrete type is used.
   *
   * The following discussion assumes the more complex case wherein #S_SHARING is `true`.  We discuss the simpler
   * converse case below that.
   *
   * Two things affect how #m_buf_ptr shall behave:
   *   - Which type this resolves-to depending on #S_IS_VANILLA_ALLOC (ultimately #Allocator_raw).  This affects
   *     many key things but most relevantly how it is dereferenced.  Namely:
   *     - Typical `shared_ptr` (used with vanilla allocator) will internally store simply a raw `value_type*`
   *       and dereference trivially.  This, however, will not work with some custom allocators, particularly
   *       SHM-heap ones (without a constant cross-process vaddr scheme), wherein a raw `T*` meaningful in
   *       the original process is meaningless in another.
   *       - `boost::shared_ptr` and `std::shared_ptr` both have custom-allocator support via
   *         `allocate_shared()` and co.  However, as of this writing, they are not SHM-friendly; or another
   *         way of putting it is they don't support custom allocators fully: `Allocator::pointer` is ignored;
   *         it is assumed to essentially be raw `value_type*`, in that the `shared_ptr` internally stores
   *         a raw pointer.  boost.interprocess refers to this as the impetus for implementing the following:
   *     - `boost::interprocess::shared_ptr` (used with custom allocator) will internally store an
   *       instance of `Allocator_raw::pointer` (to `value_type`) instead.  To dereference it, its operators
   *       such as `*` and `->` (etc.) will execute to properly translate to a raw `T*`.
   *       The aforementioned `pointer` may simply be `value_type*` again; in which case there is no difference
   *       to the standard `shared_ptr` situation; but it can instead be a fancy-pointer (actual technical term, yes,
   *       in cppreference.com et al), in which case some custom code will run to translate some internal
   *       data members (which have process-agnostic values) inside the fancy-pointer to a raw `T*`.
   *       For example `boost::interprocess::offset_ptr<value_type>` does this by adding a stored offset to its
   *       own `this`.
   *   - How it is reset to a newly allocated buffer in reserve() (when needed).
   *     - Typical `shared_ptr` is efficiently assigned using a `make_shared()` variant.  However, here we store
   *       a pointer to an array, not a single value (hence `<value_type[]>`); and we specifically want to avoid
   *       any 0-initialization of the elements (per one of Basic_blob's promises).  See reserve() which uses a
   *       `make_shared()` variant that accomplishes all this.
   *     - `boost::interprocess::shared_ptr` is reset differently due to a couple of restrictions, as it is made
   *       to be usable in SHM (SHared Memory), specifically, plus it seems to refrain from tacking on every
   *       normal `shared_ptr` feature.  To wit: 1, `virtual` cannot be used; therefore the deleter type must be
   *       declared at compile-time.  2, it has no special support for a native-array element-type (`value_type[]`).
   *       Therefore it leaves that part up to the user: the buffer must be pre-allocated by the user
   *       (and passed to `.reset()`); there is no `make_shared()` equivalent (which also means somewhat lower
   *       perf, as aux data and user buffer are separately allocated and stored).  Accordingly deletion is left
   *       to the user, as there is no default deleter; one must be supplied.  Thus:
   *       - See reserve(); it calls `.reset()` as explained here, including using #m_alloc_raw to pre-allocate.
   *       - See Deleter_raw, the deleter functor type an instance of which is saved by the `shared_ptr` to
   *         invoke when ref-count reaches 0.
   *
   * Other than that, it's a `shared_ptr`; it works as usual.
   *
   * ### Why use typical `shared_ptr` at all?  Won't the fancy-allocator-supporting one work for the vanilla case? ###
   * Yes, it would work.  And there would be less code without this dichotomy (although the differences are,
   * per above, local to this alias definition; and reserve() where it allocates buffer).  There are however reasons
   * why typical `shared_ptr` (we choose `boost::shared_ptr` over `std::shared_ptr`; that discussion is elsewhere,
   * but basically `boost::shared_ptr` is solid and full-featured/mature, though either choice would've worked).
   * They are at least:
   *   - It is much more frequently used, preceding and anticipating its acceptance into the STL standard, so
   *     maturity and performance are likelier.
   *   - Specifically it supports a perf-enhancing use mode: using `make_shared()` (and similar) instead of
   *     `.reset(<raw ptr>)` (or similar ctor) replaces 2 allocs (1 for user data, 1 for aux data/ref-count)
   *     with 1 (for both).
   *   - If verbose logging in the deleter is desired, its `virtual`-based type-erased deleter semantics make that
   *     quite easy to achieve.
   *
   * ### The case where #S_SHARING is `false` ###
   * Firstly: if so then the method -- share() -- that would *ever* increment `Buf_ptr::use_count()` beyond 1
   * is simply not compiled.  Therefore using any type of `shared_ptr` is a waste of RAM (on the ref-count)
   * and cycles (on aux memory allocation and ref-count math), albeit a minor one.  Hence we use `unique_ptr`
   * in that case instead.  Even so, the above #S_IS_VANILLA_ALLOC dichotomy still applies but is quite a bit
   * simpler to handle; it's a degenerate case in a way.
   *   - Typical `unique_ptr` already stores `Deleter::pointer` instead of `value_ptr*`.  Therefore
   *     We can use it for both cases; in the vanilla case supplying no `Deleter` template param
   *     (the default `Deleter` has `pointer = value_ptr*`); otherwise supplying Deleter_raw whose
   *     Deleter_raw::pointer comes from `Allocator_raw::pointer`.  This also, same as with
   *     `boost::interprocess::shared_ptr`, takes care of the dealloc upon being nullified or destroyed.
   *   - As for initialization:
   *     - With #S_IS_VANILLA_ALLOC at `true`: Similarly to using a special array-friendly `make_shared()` variant,
   *       we use a special array-friendly `make_unique()` variant.
   *     - Otherwise: As with `boost::interprocess::shared_ptr` we cannot `make_*()` -- though AFAIK without
   *       any perf penalty (there is no aux data) -- but reserve() must be quite careful to also
   *       replace `m_buf_ptr`'s deleter (which `.reset()` does not do... while `boost::interprocess::shared_ptr`
   *       does).
   */
  using Buf_ptr = std::conditional_t<S_IS_VANILLA_ALLOC,
                                     std::conditional_t<S_SHARING,
                                                        boost::shared_ptr<value_type[]>,
                                                        boost::movelib::unique_ptr<value_type[]>>,
                                     std::conditional_t<S_SHARING,
                                                        boost::interprocess::shared_ptr
                                                          <value_type, Allocator_raw, Deleter_raw>,
                                                        boost::movelib::unique_ptr<value_type, Deleter_raw>>>;

  // Methods.

  /**
   * The body of swap(), except for the part that swaps (or decides not to swap) #m_alloc_raw.  As of this writing
   * used by swap() and assign() (move overload) which perform mutually different steps w/r/t #m_alloc_raw.
   *
   * @param other
   *        See swap().
   * @param logger_ptr
   *        See swap().
   */
  void swap_impl(Basic_blob& other, log::Logger* logger_ptr);

  /**
   * Returns iterator-to-mutable equivalent to given iterator-to-immutable.
   *
   * @param it
   *        Self-explanatory.  No assumptions are made about valid_iterator() or derefable_iterator() status.
   * @return Iterator to same location as `it`.
   */
  Iterator iterator_sans_const(Const_iterator it);

  // Data.

  /**
   * See get_allocator(): copy of the allocator supplied by the user (though, if #Allocator_raw is stateless,
   * it is typically defaulted to `Allocator_raw()`), as set by a constructor or assign() or
   * assignment-operator, whichever happened last.  Used exclusively when allocating and deallocating
   * #m_buf_ptr in the *next* reserve() (potentially).
   *
   * By the rules of `Allocator_aware_container` (see cppreference.com):
   *   - If `*this` is move-cted: member move-cted from source member counterpart.
   *   - If `*this` is move-assigned: member move-assigned from source member counterpart if
   *     `std::allocator_traits<Allocator_raw>::propagate_on_container_move_assignment::value == true` (else untouched).
   *   - If `*this` is copy-cted: member set to
   *     `std::allocator_traits<Allocator_raw>::select_on_container_copy_construction()` (pass-in source member
   *     counterpart).
   *   - If `*this` is copy-assigned: member copy-assigned if
   *     `std::allocator_traits<Allocator_raw>::propagate_on_container_copy_assignment::value == true` (else untouched).
   *   - If `*this` is `swap()`ed: member ADL-`swap()`ed with source member counterpart if
   *     `std::allocator_traits<Allocator_raw>::propagate_on_container_swap::value == true` (else untouched).
   *   - Otherwise this is supplied via a non-copy/move ctor arg by user.
   *
   * ### Specially treated value ###
   * If #Allocator_raw is `std::allocator<value_type>` (as supposed to `something_else<value_type>`), then
   * #m_alloc_raw (while guaranteed set to the zero-sized copy of `std::allocator<value_type>()`) is never
   * in practice touched (outside of the above-mentioned moves/copies/swaps, though they also do nothing in reality
   * for this stateless allocator).  This value by definition means we are to allocate on the regular heap;
   * and as of this writing for perf/other reasons we choose to use a vanilla
   * `*_ptr` with its default alloc-dealloc APIs (which perform `new[]`-`delete[]` respectively); we do not pass-in
   * #m_alloc_raw anywhere.  See #Buf_ptr doc header for more.  If we did pass it in to
   * `allocate_shared*()` or `boost::interprocess::shared_ptr::reset` the end result would be functionally
   * the same (`std::allocator::[de]allocate()` would get called; these call `new[]`/`delete[]`).
   *
   * ### Relationship between #m_alloc_raw and the allocator/deleter in #m_buf_ptr ###
   * (This is only applicable if #S_IS_VANILLA_ALLOC is `false`.)
   * #m_buf_ptr caches #m_alloc_raw internally in its centrally linked data.  Ordinarily, then, they compare as equal.
   * In the corner case where (1) move-assign or copy-assign or swap() was used on `*this`, *and*
   * (2) #Allocator_raw is stateful and *can* compare unequal (e.g., `boost::interprocess::allocator`):
   * they may come to compare as unequal.  It is, however, not (in our case) particularly important:
   * #m_alloc_raw affects the *next* reserve() (potentially); the thing stored in #m_buf_ptr affects the logic when
   * the underlying buffer is next deallocated.  The two don't depend on each other.
   */
  Allocator_raw m_alloc_raw;

  /**
   * Pointer to currently allocated buffer of size #m_capacity; null if and only if `zero() == true`.
   * Buffer is auto-freed at destruction; or in make_zero(); but only if by that point any share()-generated
   * other `Basic_blob`s have done the same.  Otherwise the ref-count is merely decremented.
   * In the case of #S_SHARING being `false`, one can think of this ref-count as being always at most 1;
   * since share() is not compiled, and as a corollary a `unique_ptr` is used to avoid perf costs.
   * Thus make_zero() and dtor always dealloc in that case.
   *
   * For performance, we never initialize the values in the array to zeroes or otherwise.
   * This contrasts with `vector` and most other standard or Boost containers which use an `allocator` to
   * allocate any internal buffer, and most allocators default-construct (which means assign 0 in case of `uint8_t`)
   * any elements within allocated buffers, immediately upon the actual allocation on heap.  As noted in doc header,
   * this behavior is surprisingly difficult to avoid (involving custom allocators and such).
   */
  Buf_ptr m_buf_ptr;

  /// See capacity(); but #m_capacity is meaningless (and containing unknown value) if `!m_buf_ptr` (i.e., zero()).
  size_type m_capacity;

  /// See start(); but #m_start is meaningless (and containing unknown value) if `!m_buf_ptr` (i.e., zero()).
  size_type m_start;

  /// See size(); but #m_size is meaningless (and containing unknown value) if `!m_buf_ptr` (i.e., zero()).
  size_type m_size;
}; // class Basic_blob

// Free functions: in *_fwd.hpp.

// Template implementations.

// m_buf_ptr initialized to null pointer.  n_capacity and m_size remain uninit (meaningless until m_buf_ptr changes).
template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>::Basic_blob(const Allocator_raw& alloc_raw) :
  m_alloc_raw(alloc_raw) // Copy allocator; stateless allocator should have size 0 (no-op for the processor).
{
  // OK.
}

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>::Basic_blob
  (size_type size, log::Logger* logger_ptr, const Allocator_raw& alloc_raw) :

  Basic_blob(alloc_raw) // Delegate.
{
  resize(size, 0, logger_ptr);
}

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>::Basic_blob(const Basic_blob& src, log::Logger* logger_ptr) :
  // Follow rules established in m_alloc_raw doc header:
  m_alloc_raw(std::allocator_traits<Allocator_raw>::select_on_container_copy_construction(src.m_alloc_raw))
{
  /* What we want to do here, ignoring allocators, is (for concision): `assign(src, logger_ptr);`
   * However copy-assignment also must do something different w/r/t m_alloc_raw than what we had to do above
   * (again see m_alloc_raw doc header); so just copy/paste the rest of what operator=(copy) would do.
   * Skipping most comments therein, as they don't much apply in our case.  Code reuse level is all-right;
   * and we can skip the `if` from assign(). */
  assign_copy(src.const_buffer(), logger_ptr);
}

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>::Basic_blob(Basic_blob&& moved_src, log::Logger* logger_ptr) :
  // Follow rules established in m_alloc_raw doc header:
  m_alloc_raw(std::move(moved_src.m_alloc_raw))
{
  /* Similar to copy ctor above, do the equivalent of assign(move(moved_src), logger_ptr) minus the allocator work.
   * That reduces to simply: */
  swap_impl(moved_src, logger_ptr);
}

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>::~Basic_blob() = default;

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>&
  Basic_blob<Allocator, S_SHARING_ALLOWED>::assign(const Basic_blob& src, log::Logger* logger_ptr)
{
  if (this != &src)
  {
    // Take care of the "Sharing blobs" corner case from our doc header.  The rationale for this is pointed out there.
    if constexpr(S_SHARING)
    {
      assert(!blobs_sharing(*this, src));
    }

    // For m_alloc_raw: Follow rules established in m_alloc_raw doc header.
    if constexpr(std::allocator_traits<Allocator_raw>::propagate_on_container_copy_assignment::value)
    {
      m_alloc_raw = src.m_alloc_raw;
      /* Let's consider what just happened.  Allocator_raw's policy is to, yes, copy m_alloc_raw from
       * src to *this; so we did.  Now suppose !zero() and !src.zero(); and that old m_alloc_raw != src.m_alloc_raw.
       * (E.g., boost::interprocess::allocator<>s with same type but set to different SHM segments S1 and S2 would
       * compare unequal.)  What needs to happen is *m_buf_ptr buffer must be freed (more accurately, its
       * shared_ptr ref_count decremented and thus buffer possibly freed if not share()d); then allocated; then
       * contents linear-copied from *src.m_buf_ptr buffer to *m_buf_ptr buffer.  assign_copy() below naively
       * does all that; but will it work since we've thrown away the old m_alloc_raw?  Let's go through it:
       *   -# Basically m_buf_ptr.reset(<new buffer ptr>) is kinda like m_buf_ptr.reset() followed by
       *      m_buf_ptr.reset(<new buffer ptr>); the former part is the possible-dealloc.  So will it work?
       *      Yes: shared_ptr<> stores the buffer and aux data (ref-count, allocator, deleter) in one central
       *      place shared with other shared_ptr<>s in its group.  The .reset() dec-refs the ref-count and dissociates
       *      m_buf_ptr from the central place; if the ref-count is 0, then it also deallocs the buffer and the
       *      aux data and eliminates the central place... using the allocator/deleter cached in that central
       *      place itself.  Hence the old m_alloc_raw's copy will go in effect when the nullifying .reset() part
       *      happens.
       *   -# So then m_buf_ptr is .reset() to the newly allocated buffer which will be allocated by us explicitly
       *      using m_alloc_raw (which we've replaced just now).
       *   -# Then the linear copy in assign_copy() is uncontroversial; everything is allocated before this starts. */
    }
    /* else: Leave m_alloc_raw alone.  Everything should be fine once we assign_copy() below: existing m_buf_ptr
     * (if not null) will dealloc without any allocator-related disruption/change; then it'll be reset to a new buffer
     * with contents linear-copied over.  The unchanged m_alloc_raw will be used for the *next* allocating reserve()
     * if any. */

    // Now to the relatively uncontroversial stuff.  To copy the rest we'll just do:

    /* resize(N, 0); copy over N bytes.  Note that it disallows `N > capacity()` unless zero(), but they can explicitly
     * make_zero() before calling us, if they are explicitly OK with the performance cost of the reallocation that will
     * trigger.  This is all as advertised; and it satisfies the top priority listed just below. */
    assign_copy(src.const_buffer(), logger_ptr);

    /* ^-- Corner case: Suppose src.size() == 0.  The above then reduces to: if (!zero()) { m_size = m_start = 0; }
     * (Look inside its source code; you'll see.)
     *
     * We guarantee certain specific behavior in doc header, and below implements that.
     * We will indicate how it does so; but ALSO why those promises are made in the first place (rationale).
     *
     * In fact, we will proceed under the following priorities, highest to lowest:
     *   - User CAN switch order of our priorities sufficiently easily.
     *   - Be as fast as possible, excluding minimizing constant-time operations such as scalar assignments.
     *   - Use as little memory in *this as possible.
     *
     * We will NOT attempt to make *this have the same internal structure as src as its own independent virtue.
     * That doesn't seem useful and would make things more difficult obviously.  Now:
     *
     * Either src.zero(), or not; but regardless src.size() == 0.  Our options are essentially these:
     * make_zero(); or resize(0, 0).  (We could also perhaps copy src.m_buf_ptr[] and then adjust m_size = 0, but
     * this is clearly slower and only gains the thing we specifically pointed out is not a virtue above.)
     *
     * Let's break down those 2 courses of action, by situation, then:
     * - zero() && src.zero(): make_zero() and resize(0, 0) are equivalent; so nothing to decide.  Either would be fine.
     * - zero() && !src.zero(): Ditto.
     * - !zero() && !src.zero(): make_zero() is slower than resize(0, 0); and moreover the latter may mean faster
     *   operations subsequently, if they subsequently choose to reserve(N) (including resize(N), etc.) to
     *   N <= capacity().  So resize(0, 0) wins according to the priority order listed above.
     * - !zero() && src.zero(): Ditto.
     *
     * So then we decided: resize(0, 0).  And, indeed, resize(0, 0) is equivalent to the above snippet.
     * So, we're good. */
  } // if (this != &src)

  return *this;
} // Basic_blob::assign(copy)

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>& Basic_blob<Allocator, S_SHARING_ALLOWED>::operator=(const Basic_blob& src)
{
  return assign(src);
}

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>&
  Basic_blob<Allocator, S_SHARING_ALLOWED>::assign(Basic_blob&& moved_src, log::Logger* logger_ptr)
{
  if (this != &moved_src)
  {
    // For m_alloc_raw: Follow rules established in m_alloc_raw doc header.
    if constexpr(std::allocator_traits<Allocator_raw>::propagate_on_container_move_assignment::value)
    {
      m_alloc_raw = std::move(moved_src.m_alloc_raw);
      /* Let's consider what just happened.  Allocator_raw's policy is to, yes, move m_alloc_raw from
       * src to *this; so we did -- I guess src.m_alloc_raw is some null-ish empty-ish thing now.
       * Now suppose !zero() and !moved_src.zero(); and that old m_alloc_raw != new src.m_alloc_raw.
       * (That is fairly exotic; at least Allocator_raw is stateful to begin with.
       * E.g., boost::interprocess::allocator<>s with same type but set to different SHM pools S1 and S2 would
       * compare unequal.)  What needs to happen is m_buf_ptr buffer must be freed (more accurately, its
       * shared_ptr ref_count decremented and thus buffer possibly freed if not share()d) + ptr nullified); then ideally
       * simply swap m_buf_ptr (which will get moved_src.m_buf_ptr old value) and moved_src.m_buf_ptr (which will
       * become null).  That's what we do below.  So will it work?
       *   -# The m_buf_ptr.reset() below will work fine for the same reason the long comment in assign(copy)
       *      states that nullifying m_buf_ptr, even with m_alloc_raw already replaced, will still use old m_alloc_raw:
       *      for it is stored inside the central area linked-to in the m_buf_ptr being nullified.
       *   -# The swap is absolutely smooth and fine.  And indeed by making that swap we'll've ensured this->m_alloc_raw
       *      and the allocator stored inside m_buf_ptr are equal. */
    }
    /* else: Leave m_alloc_raw alone.  What does it mean though?  Let's consider it.  Suppose !zero() and
     * !moved_src.zero(), and the two `m_alloc_raw`s do not compare equal (e.g., boost::interprocess::allocator<>s
     * with mutually differing SHM-pools).  m_buf_ptr.reset() below will work fine: m_alloc_raw is unchanged so no
     * controversy.  However once m_buf_ptr is moved from moved_src.m_buf_ptr, it will (same reason as above --
     * it is cached) keep using old m_alloc_raw; meaning if/when it is .reset() or destroyed the old allocator
     * will deallocate.  That is in fact what we want.  It might seem odd that m_alloc_raw won't match what's
     * used for this->m_buf_ptr, but it is fine: m_alloc_raw affects the *next* allocating reserve().
     * (And, as usual, if Allocator_raw is stateless, then none of this matters.) */

    // Now to the relatively uncontroversial stuff.

    make_zero(logger_ptr); // Spoiler alert: it's: if (!zero()) { m_buf_ptr.reset(); }
    // So now m_buf_ptr is null; hence the other m_* (other than m_alloc_raw) are meaningless.

    swap_impl(moved_src, logger_ptr);
    // Now *this is equal to old moved_src; new moved_src is valid and zero(); and nothing was copied -- as advertised.
  } // if (this != &moved_src)

  return *this;
} // Basic_blob::assign(move)

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>& Basic_blob<Allocator, S_SHARING_ALLOWED>::operator=(Basic_blob&& moved_src)
{
  return assign(std::move(moved_src));
}

template<typename Allocator, bool S_SHARING_ALLOWED>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::swap_impl(Basic_blob& other, log::Logger* logger_ptr)
{
  using std::swap;

  if (this != &other)
  {
    if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, S_LOG_COMPONENT))
    {
      FLOW_LOG_SET_CONTEXT(logger_ptr, S_LOG_COMPONENT);
      FLOW_LOG_TRACE_WITHOUT_CHECKING("Blob [" << this << "] (internal buffer sized [" << capacity() << "]) "
                                      "swapping <=> Blob [" << &other << "] (internal buffer sized "
                                      "[" << other.capacity() << "]).");
    }

    swap(m_buf_ptr, other.m_buf_ptr);

    /* Some compilers in some build configs issue maybe-uninitialized warning here, when `other` is as-if
     * default-cted (hence the following three are intentionally uninitialized), particularly with heavy
     * auto-inlining by the optimizer.  False positive in our case, and in Blob-land we try not to give away perf
     * at all so: */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas" // For older versions, where the following does not exist/cannot be disabled.
#pragma GCC diagnostic ignored "-Wunknown-warning-option" // (Similarly for clang.)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

    swap(m_capacity, other.m_capacity); // Meaningless if zero() but harmless.
    swap(m_size, other.m_size); // Ditto.
    swap(m_start, other.m_start); // Ditto.

#pragma GCC diagnostic pop

    /* Skip m_alloc_raw: swap() has to do it by itself; we are called from it + move-assign/ctor which require
     * mutually different treatment for m_alloc_raw. */
  }
} // Basic_blob::swap_impl()

template<typename Allocator, bool S_SHARING_ALLOWED>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::swap(Basic_blob& other, log::Logger* logger_ptr)
{
  using std::swap;

  // For m_alloc_raw: Follow rules established in m_alloc_raw doc header.
  if constexpr(std::allocator_traits<Allocator_raw>::propagate_on_container_swap::value)
  {
    if (&m_alloc_raw != &other.m_alloc_raw) // @todo Is this redundant?  Or otherwise unnecessary?
    {
      swap(m_alloc_raw, other.m_alloc_raw);
    }
  }
  /* else: Leave both `m_alloc_raw`s alone.  What does it mean though?  Well, see either assign(); the same
   * theme applies here: Each m_buf_ptr's cached allocator/deleter will potentially not equal its respective
   * m_alloc_raw anymore; but the latter affects only the *next* allocating reserve(); so it is fine.
   * That said, to quote cppreference.com: "Note: swapping two containers with unequal allocators if
   * propagate_on_container_swap is false is undefined behavior."  So, while it will work for us, trying such
   * a swap() would be illegal user behavior in any case. */

  // Now to the relatively uncontroversial stuff.
  swap_impl(other, logger_ptr);
}

template<typename Allocator, bool S_SHARING_ALLOWED>
void swap(Basic_blob<Allocator, S_SHARING_ALLOWED>& blob1,
          Basic_blob<Allocator, S_SHARING_ALLOWED>& blob2, log::Logger* logger_ptr)
{
  return blob1.swap(blob2, logger_ptr);
}

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED> Basic_blob<Allocator, S_SHARING_ALLOWED>::share(log::Logger* logger_ptr) const
{
  static_assert(S_SHARING,
                "Do not invoke (and thus instantiate) share() or derived methods unless you set the S_SHARING_ALLOWED "
                  "template parameter to true.  Sharing will be enabled at a small perf cost; see class doc header.");
  // Note: The guys that call it will cause the same check to occur, since instantiating them will instantiate us.

  assert(!zero()); // As advertised.

  Basic_blob sharing_blob{m_alloc_raw, logger_ptr}; // Null Basic_blob (let that ctor log via same Logger if any).
  assert(!sharing_blob.m_buf_ptr);
  sharing_blob.m_buf_ptr = m_buf_ptr;
  // These are currently (as of this writing) uninitialized (possibly garbage).
  sharing_blob.m_capacity = m_capacity;
  sharing_blob.m_start = m_start;
  sharing_blob.m_size = m_size;

  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, S_LOG_COMPONENT))
  {
    FLOW_LOG_SET_CONTEXT(logger_ptr, S_LOG_COMPONENT);
    FLOW_LOG_TRACE_WITHOUT_CHECKING
      ("Blob [" << this << "] shared with new Blob [" << &sharing_blob << "]; ref-count incremented.");
  }

  return sharing_blob;
}

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>
  Basic_blob<Allocator, S_SHARING_ALLOWED>::share_after_split_left(size_type lt_size, log::Logger* logger_ptr)
{
  if (lt_size > size())
  {
    lt_size = size();
  }

  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, S_LOG_COMPONENT))
  {
    FLOW_LOG_SET_CONTEXT(logger_ptr, S_LOG_COMPONENT);
    FLOW_LOG_TRACE_WITHOUT_CHECKING("Blob [" << this << "] shall be shared with new Blob, splitting off the first "
                                    "[" << lt_size << "] values into that one and leaving the remaining "
                                    "[" << (size() - lt_size) << "] in this one.");
  }

  auto sharing_blob = share(logger_ptr); // sharing_blob sub-Basic_blob is equal to *this sub-Basic_blob.  Adjust:
  sharing_blob.resize(lt_size); // Note: sharing_blob.start() remains unchanged.
  start_past_prefix_inc(lt_size);

  return sharing_blob;
}

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>
  Basic_blob<Allocator, S_SHARING_ALLOWED>::share_after_split_right(size_type rt_size, log::Logger* logger_ptr)
{
  if (rt_size > size())
  {
    rt_size = size();
  }

  const auto lt_size = size() - rt_size;
  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, S_LOG_COMPONENT))
  {
    FLOW_LOG_SET_CONTEXT(logger_ptr, S_LOG_COMPONENT);
    FLOW_LOG_TRACE_WITHOUT_CHECKING("Blob [" << this << "] shall be shared with new Blob, splitting off "
                                    "the last [" << rt_size << "] values into that one and leaving the "
                                    "remaining [" << lt_size << "] in this one.");
  }

  auto sharing_blob = share(logger_ptr); // sharing_blob sub-Basic_blob is equal to *this sub-Basic_blob.  Adjust:
  resize(lt_size); // Note: start() remains unchanged.
  sharing_blob.start_past_prefix_inc(lt_size);

  return sharing_blob;
}

template<typename Allocator, bool S_SHARING_ALLOWED>
template<typename Emit_blob_func, typename Share_after_split_left_func>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::share_after_split_equally_impl
       (size_type size, bool headless_pool, Emit_blob_func&& emit_blob_func, log::Logger* logger_ptr,
        Share_after_split_left_func&& share_after_split_left_func)
{
  assert(size != 0);
  assert(!empty());

  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, S_LOG_COMPONENT))
  {
    FLOW_LOG_SET_CONTEXT(logger_ptr, S_LOG_COMPONENT);
    FLOW_LOG_TRACE_WITHOUT_CHECKING("Blob [" << this << "] of size [" << this->size() << "] shall be split into "
                                    "adjacent sharing sub-Blobs of size [" << size << "] each "
                                    "(last one possibly smaller).");
  }

  do
  {
    emit_blob_func(share_after_split_left_func(size, logger_ptr)); // share_after_split_left_func() logs plenty.
  }
  while (!empty());

  if (headless_pool)
  {
    make_zero(logger_ptr);
  }
} // Basic_blob::share_after_split_equally_impl()

template<typename Allocator, bool S_SHARING_ALLOWED>
template<typename Emit_blob_func>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::share_after_split_equally(size_type size, bool headless_pool,
                                                                         Emit_blob_func&& emit_blob_func,
                                                                         log::Logger* logger_ptr)
{
  share_after_split_equally_impl(size, headless_pool, std::move(emit_blob_func), logger_ptr,
                                 [this](size_type lt_size, log::Logger* logger_ptr) -> Basic_blob
  {
    return share_after_split_left(lt_size, logger_ptr);
  });
}

template<typename Allocator, bool S_SHARING_ALLOWED>
template<typename Blob_container>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::share_after_split_equally_emit_seq
       (size_type size, bool headless_pool, Blob_container* out_blobs_ptr, log::Logger* logger_ptr)
{
  // If changing this please see Blob_with_log_context::<same method>().

  assert(out_blobs_ptr);
  share_after_split_equally(size, headless_pool, [&](Basic_blob&& blob_moved)
  {
    out_blobs_ptr->push_back(std::move(blob_moved));
  }, logger_ptr);
}

template<typename Allocator, bool S_SHARING_ALLOWED>
template<typename Blob_ptr_container>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::share_after_split_equally_emit_ptr_seq(size_type size,
                                                                                      bool headless_pool,
                                                                                      Blob_ptr_container* out_blobs_ptr,
                                                                                      log::Logger* logger_ptr)
{
  // If changing this please see Blob_with_log_context::<same method>().

  // By documented requirements this should be, like, <...>_ptr<Basic_blob>.
  using Ptr = typename Blob_ptr_container::value_type;

  assert(out_blobs_ptr);

  share_after_split_equally(size, headless_pool, [&](Basic_blob&& blob_moved)
  {
    out_blobs_ptr->push_back(Ptr(new Basic_blob{std::move(blob_moved)}));
  }, logger_ptr);
}

template<typename Allocator, bool S_SHARING_ALLOWED>
bool blobs_sharing(const Basic_blob<Allocator, S_SHARING_ALLOWED>& blob1,
                   const Basic_blob<Allocator, S_SHARING_ALLOWED>& blob2)
{
  static_assert(S_SHARING_ALLOWED,
                "blobs_sharing() would only make sense on `Basic_blob`s with S_SHARING_ALLOWED=true.  "
                  "Even if we were to allow this to instantiate (compile) it would always return false.");

  return ((!blob1.zero()) && (!blob2.zero())) // Can't co-own a buffer if doesn't own a buffer.
         && ((&blob1 == &blob2) // Same object => same buffer.
             // Only share() (as of this writing) can lead to the underlying buffer's start ptr being identical.
             || ((blob1.begin() - blob1.start())
                 == (blob2.begin() - blob2.start())));
  // @todo Maybe throw in assert(blob1.capacity() == blob2.capacity()), if `true` is being returned.
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::size_type Basic_blob<Allocator, S_SHARING_ALLOWED>::size() const
{
  return zero() ? 0 : m_size; // Note that zero() may or may not be true if we return 0.
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::size_type Basic_blob<Allocator, S_SHARING_ALLOWED>::start() const
{
  return zero() ? 0 : m_start; // Note that zero() may or may not be true if we return 0.
}

template<typename Allocator, bool S_SHARING_ALLOWED>
bool Basic_blob<Allocator, S_SHARING_ALLOWED>::empty() const
{
  return size() == 0; // Note that zero() may or may not be true if we return true.
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::size_type Basic_blob<Allocator, S_SHARING_ALLOWED>::capacity() const
{
  return zero() ? 0 : m_capacity; // Note that zero() <=> we return non-zero.  (m_capacity >= 1 if !zero().)
}

template<typename Allocator, bool S_SHARING_ALLOWED>
bool Basic_blob<Allocator, S_SHARING_ALLOWED>::zero() const
{
  return !m_buf_ptr;
}

template<typename Allocator, bool S_SHARING_ALLOWED>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::reserve(size_type new_capacity, log::Logger* logger_ptr)
{
  using boost::make_shared_noinit;
  using boost::shared_ptr;
  using std::numeric_limits;

  /* As advertised do not allow enlarging existing buffer.  They can call make_zero() though (but must do so consciously
   * hence considering the performance impact). */
  assert(zero() || ((new_capacity <= m_capacity) && (m_capacity > 0)));

  /* OK, but what if new_capacity < m_capacity?  Then post-condition (see below) is already satisfied, and it's fastest
   * to do nothing.  If user believes lower memory use is higher-priority, they can explicitly call make_zero() first
   * but must make conscious decision to do so. */

  if (zero() && (new_capacity != 0))
  {
    if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, S_LOG_COMPONENT))
    {
      FLOW_LOG_SET_CONTEXT(logger_ptr, S_LOG_COMPONENT);
      FLOW_LOG_TRACE_WITHOUT_CHECKING("Blob [" << this << "] "
                                      "allocating internal buffer sized [" << new_capacity << "].");
    }

    if (new_capacity <= size_type(numeric_limits<std::ptrdiff_t>::max())) // (See explanation near bottom of method.)
    {
      /* Time to (1) allocate the buffer; (2) save the pointer; (3) ensure it is deallocated at the right time
       * and with the right steps.  Due to Allocator_raw support this is a bit more complex than usual.  Please
       * (1) see class doc header "Custom allocator" section; and (2) read Buf_ptr alias doc header for key background;
       * then come back here. */

      if constexpr(S_IS_VANILLA_ALLOC)
      {
        /* In this case they specified std::allocator, so we are to just allocate/deallocate in regular heap using
         * new[]/delete[].  Hence we don't need to even use actual std::allocator; we know by definition it would
         * use new[]/delete[].  So simply use typical ..._ptr initialization.  Caveats are unrelated to allocators:
         *   - For some extra TRACE-logging -- if enabled! -- use an otherwise-vanilla logging deleter.
         *     - Unnecessary in case of unique_ptr: dealloc always occurs in make_zero() or dtor and can be logged
         *       there.
         *   - If doing so (note it implies we've given up on performance) we cannot, and do not, use
         *     make_shared*(); the use of custom deleter requires the .reset() form of init. */

        /* If TRACE currently disabled, then skip the custom deleter that logs about dealloc.  (TRACE may be enabled
         * by that point; but, hey, that is life.)  This is for perf. */
        if constexpr(S_SHARING)
        {
          if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, S_LOG_COMPONENT))
          {
            /* This ensures delete[] call when m_buf_ptr ref-count reaches 0.
             * As advertised, for performance, the memory is NOT initialized. */
            m_buf_ptr.reset(new value_type[new_capacity],
                            // Careful!  *this might be gone if some other share()ing obj is the one that 0s ref-count.
                            [logger_ptr, original_blob = this, new_capacity]
                              (value_type* buf_ptr)
            {
              FLOW_LOG_SET_CONTEXT(logger_ptr, S_LOG_COMPONENT);
              FLOW_LOG_TRACE("Deallocating internal buffer sized [" << new_capacity << "] originally allocated by "
                             "Blob [" << original_blob << "]; note that Blob may now be gone and furthermore another "
                             "Blob might live at that address now.  A message immediately preceding this one should "
                             "indicate the last Blob to give up ownership of the internal buffer.");
              // Finally just do what the default one would've done, as we've done our custom thing (logging).
              delete [] buf_ptr;
            });
          }
          else // if (!should_log()): No logging deleter; just delete[] it.
          {
            /* This executes `new value_type[new_capacity]` and ensures delete[] when m_buf_ptr ref-count reaches 0.
             * As advertised, for performance, the memory is NOT initialized. */
            m_buf_ptr = make_shared_noinit<value_type[]>(new_capacity);
          }
        } // if constexpr(S_SHARING)
        else // if constexpr(!S_SHARING)
        {
          m_buf_ptr = boost::movelib::make_unique_definit<value_type[]>(new_capacity);
          // Again -- the logging in make_zero() (and Blob_with_log_context dtor) is sufficient.
        }
      } // if constexpr(S_IS_VANILLA_ALLOC)
      else // if constexpr(!S_IS_VANILLA_ALLOC)
      {
        /* Fancy (well, potentially) allocator time.  Again, if you've read the Buf_ptr and Deleter_raw doc headers,
         * you'll know what's going on. */

        if constexpr(S_SHARING)
        {
          m_buf_ptr.reset(m_alloc_raw.allocate(new_capacity), // Raw-allocate via Allocator_raw!  No value-init occurs.

                          /* Let them allocate aux data (ref count block) via Allocator_raw::allocate()
                           * (and dealloc it -- ref count block -- via Allocator_raw::deallocate())!
                           * Have them store internal ptr bits as `Allocator_raw::pointer`s, not
                           * necessarily raw `value_type*`s! */
                          m_alloc_raw,

                          /* When the time comes to dealloc, invoke this guy like: D(<the ptr>)!  It'll
                           * perform m_alloc_raw.deallocate(<what .allocate() returned>, n).
                           * Since only we happen to know the size of how much we actually allocated, we pass that info
                           * into the Deleter_raw as well, as it needs to know the `n` to pass to
                           * m_alloc_raw.deallocate(p, n). */
                          Deleter_raw{m_alloc_raw, new_capacity});
          /* Note: Unlike the S_IS_VANILLA_ALLOC=true case above, here we omit any attempt to log at the time
           * of dealloc, even if the verbosity is currently set high enough.  It is not practical to achieve:
           * Recall that the assumptions we take for granted when dealing with std::allocator/regular heap
           * may no longer apply when dealing with an arbitrary allocator/potentially SHM-heap.  To be able
           * to log at dealloc time, the Deleter_raw we create would need to store a Logger*.  Sure, we
           * could pass-in logger_ptr and Deleter_raw could store it; but now recall that we do not
           * store a Logger* in `*this` and why: because (see class doc header) doing so does not play well
           * in some custom-allocator situations, particularly when operating in SHM-heap.  That is why
           * we take an optional Logger* as an arg to every possibly-logging API (we can't guarantee, if
           * S_IS_VANILLA_ALLOC=false, that a Logger* can meaningfully be stored in likely-Allocator-stored *this).
           * For that same reason we cannot pass it to the Deleter_raw functor; m_buf_ptr (whose bits are in
           * *this) will save a copy of that Deleter_raw and hence *this will end up storing the Logger* which
           * (as noted) may be nonsensical.  (With S_IS_VANILLA_ALLOC=true, though, it's safe to store it; and
           * since deleter would only fire at dealloc time, it doesn't present a new perf problem -- since TRACE
           * log level alrady concedes bad perf -- which is the 2nd reason (see class doc header) for why
           * we don't generally record Logger* but rather take it as an arg to each logging API.)
           *
           * Anyway, long story short, we don't log on dealloc in this case, b/c we can't, and the worst that'll
           * happen as a result of that decision is: deallocs won't be trace-logged when a custom allocator
           * is enabled at compile-time.  That price is okay to pay. */
        } // if constexpr(S_SHARING)
        else // if constexpr(!S_SHARING)
        {
          /* Conceptually it's quite similar to the S_SHARING case where we do shared_ptr::reset() above.
           * However there is an API difference that is subtle yet real (albeit only for stateful Allocator_raw):
           * Current m_alloc_raw was used to allocate *m_buf_ptr, so it must be used also to dealloc it.
           * unique_ptr::reset() does *not* take a new Deleter_raw; hence if we used it (alone) here it would retain
           * the m_alloc from ction time -- and if that does not equal current m_alloc => trouble in make_zero()
           * or dtor.
           *
           * Anyway, to beat that, we can either manually overwrite get_deleter() (<-- non-const ref);
           * or we can assign via unique_ptr move-ct.  The latter is certainly pithier and prettier,
           * but the former might be a bit faster.  (Caution!  Recall m_buf_ptr is null currently.  If it were not
           * we would need to explicitly nullify it before the get_deleter() assignment.) */
          m_buf_ptr.get_deleter() = Deleter_raw{m_alloc_raw, new_capacity};
          m_buf_ptr.reset(m_alloc_raw.allocate(new_capacity));
        } // else if constexpr(!S_SHARING)
      } // else if constexpr(!S_IS_VANILLA_ALLOC)
    } // if (new_capacity <= numeric_limits<std::ptrdiff_t>::max()) // (See explanation just below.)
    else
    {
      assert(false && "Enormous or corrupt new_capacity?!");
    }
    /* ^-- Explanation of the strange if/else:
     * In some gcc versions in some build configs, particularly with aggressive auto-inlining optimization,
     * a warning like this can be triggered (observed, as of this writing, only in the movelib::make_unique_definit()
     * branch above, but to be safe we're covering all the branches with our if/else work-around):
     *   argument 1 value ‘18446744073709551608’ exceeds maximum object size
     *     9223372036854775807 [-Werror=alloc-size-larger-than=]
     * This occurs due to (among other things) inlining from above our frame down into the boost::movelib call
     * we make (and potentially the other allocating calls in the various branches above);
     * plus allegedly the C++ front-end supplying the huge value during the diagnostics pass.
     * No such huge value (which is 0xFFFFFFFFFFFFFFF8) is actually passed-in at run-time nor mentioned anywhere
     * in our code, here or in the unit-test(s) triggering the auto-inlining triggering the warning.  So:
     *
     * The warning is wholly inaccurate.  This situation is known in the gcc issue database; for example
     * see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=85783 and related (linked) tickets.
     * The question was how to work around it; I admit that the discussion in that ticket (and friends) at times
     * gets into topics so obscure and gcc-internal as to be indecipherable to me (ygoldfel).
     * Since I don't seem to be doing anything wrong above (though: @todo *Maybe* it has something to do with
     * lacking `nothrow`? Would need investigation, nothrow could be good anyway...), the top work-arounds would be
     * perhaps: 1, pragma-away the alloc-size-larger-than warning; 2, use a compiler-placating
     * explicit `if (new_capacity < ...limit...)` branch.  (2) was suggested in the above ticket by a
     * gcc person.  Not wanting to give even a tiny bit of perf I attempted the pragma way (1); but at least gcc-13
     * has some bug which makes the pragma get ignored.  So I reverted to (2) by default.
     * @todo Revisit this.  Should skip workaround unless gcc; + possibly solve it some more elegant way; look into the
     * nothrow thing the ticket discussion briefly mentions (but might be irrelevant). */

    m_capacity = new_capacity;
    m_size = 0; // Went from zero() to !zero(); so m_size went from meaningless to meaningful and must be set.
    m_start = 0; // Ditto for m_start.

    assert(!zero());
    // This is the only path (other than swap()) that assigns to m_capacity; note m_capacity >= 1.
  }
  /* else { !zero(): Since new_capacity <= m_capacity, m_capacity is already large enough; no change needed.
   *        zero() && (new_capacity == 0): Since 0-capacity wanted, we can continue being zero(), as that's enough. } */

  assert(capacity() >= new_capacity); // Promised post-condition.
} // Basic_blob::reserve()

template<typename Allocator, bool S_SHARING_ALLOWED>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::resize(size_type new_size, size_type new_start_or_unchanged,
                                                      log::Logger* logger_ptr)
{
  auto& new_start = new_start_or_unchanged;
  if (new_start == S_UNCHANGED)
  {
    new_start = start();
  }

  const size_type min_capacity = new_start + new_size;

  // Sanity checks/input checks (depending on how you look at it).
  assert(min_capacity >= new_size);
  assert(min_capacity >= new_start);

  /* Ensure there is enough space for new_size starting at new_start.  Note, in particular, this disallows
   * enlarging non-zero() buffer.
   * (If they want, they can explicitly call make_zero() first.  But they must do so consciously, so that they're
   * forced to consider the performance impact of such an action.)  Also note that zero() continues to be true
   * if was true. */
  reserve(min_capacity, logger_ptr);
  assert(capacity() >= min_capacity);

  if (!zero())
  {
    m_size = new_size;
    m_start = new_start;
  }
  // else { zero(): m_size is meaningless; size() == 0, as desired. }

  assert(size() == new_size);
  assert(start() == new_start);
} // Basic_blob::resize()

template<typename Allocator, bool S_SHARING_ALLOWED>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::start_past_prefix(size_type prefix_size)
{
  resize(((start() + size()) > prefix_size)
           ? (start() + size() - prefix_size)
           : 0,
         prefix_size); // It won't log, as it cannot allocate, so no need to pass-through a Logger*.
  // Sanity check: `prefix_size == 0` translates to: resize(start() + size(), 0), as advertised.
}

template<typename Allocator, bool S_SHARING_ALLOWED>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::start_past_prefix_inc(difference_type prefix_size_inc)
{
  assert((prefix_size_inc >= 0) || (start() >= size_type(-prefix_size_inc)));
  start_past_prefix(start() + prefix_size_inc);
}

template<typename Allocator, bool S_SHARING_ALLOWED>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::clear()
{
  // Note: start() remains unchanged (as advertised).  resize(0, 0) can be used if that is unacceptable.
  resize(0); // It won't log, as it cannot allocate, so no need to pass-through a Logger*.
  // Note corner case: zero() remains true if was true (and false if was false).
}

template<typename Allocator, bool S_SHARING_ALLOWED>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::make_zero(log::Logger* logger_ptr)
{
  /* Could also write more elegantly: `swap(Basic_blob());`, but following is a bit optimized (while equivalent);
   * logs better. */
  if (!zero())
  {
    if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, S_LOG_COMPONENT))
    {
      FLOW_LOG_SET_CONTEXT(logger_ptr, S_LOG_COMPONENT);
      if constexpr(S_SHARING_ALLOWED)
      {
        FLOW_LOG_TRACE_WITHOUT_CHECKING("Blob [" << this << "] giving up ownership of internal buffer sized "
                                        "[" << capacity() << "]; deallocation will immediately follow if no sharing "
                                        "`Blob`s remain; else ref-count merely decremented.");
      }
      else
      {
        FLOW_LOG_TRACE_WITHOUT_CHECKING("Blob [" << this << "] deallocating internal buffer sized "
                                        "[" << capacity() << "].");
      }
    }

    m_buf_ptr.reset();
  } // if (!zero())
} // Basic_blob::make_zero()

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::size_type
  Basic_blob<Allocator, S_SHARING_ALLOWED>::assign_copy(const boost::asio::const_buffer& src,
                                                        log::Logger* logger_ptr)
{
  const size_type n = src.size();

  /* Either just set m_start = 0 and decrease/keep-constant (m_start + m_size) = n; or allocate exactly n-sized buffer
   * and set m_start = 0, m_size = n.
   * As elsewhere, the latter case requires that zero() be true currently (but they can force that with make_zero()). */
  resize(n, 0); // It won't log, as it cannot allocate, so no need to pass-through a Logger*.

  // Performance: Basically equals: memcpy(m_buf_ptr, src.start, src.size).
  emplace_copy(const_begin(), src, logger_ptr);

  // Corner case: n == 0.  Above is equivalent to: if (!zero()) { m_size = m_start = 0; }.  That behavior is advertised.

  return n;
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Iterator
  Basic_blob<Allocator, S_SHARING_ALLOWED>::emplace_copy(Const_iterator dest, const boost::asio::const_buffer& src,
                                                         log::Logger* logger_ptr)
{
  using std::memcpy;

  // Performance: assert()s eliminated and values inlined, below boils down to: memcpy(dest, src.start, src.size);

  assert(valid_iterator(dest));

  const Iterator dest_it = iterator_sans_const(dest);
  const size_type n = src.size(); // Note the entire source buffer is copied over.

  if (n != 0)
  {
    const auto src_data = static_cast<Const_iterator>(src.data());

    if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, S_LOG_COMPONENT))
    {
      FLOW_LOG_SET_CONTEXT(logger_ptr, S_LOG_COMPONENT);
      FLOW_LOG_TRACE_WITHOUT_CHECKING("Blob [" << this << "] copying "
                                      "memory area [" << static_cast<const void*>(src_data) << "] sized "
                                      "[" << n << "] to internal buffer at offset [" << (dest - const_begin()) << "].");
    }

    assert(derefable_iterator(dest_it)); // Input check.
    assert(difference_type(n) <= (const_end() - dest)); // Input check.  (As advertised, we don't "correct" `n`.)

    // Ensure no overlap by user.
    assert(((dest_it + n) <= src_data) || ((src_data + n) <= dest_it));

    /* Some compilers in some build configs issue stringop-overflow warning here, when optimizer heavily auto-inlines:
     *   error: ‘memcpy’ specified bound between 9223372036854775808 and 18446744073709551615
     *     exceeds maximum object size 9223372036854775807 [-Werror=stringop-overflow=]
     * This occurs due to (among other things) inlining from above our frame down into the std::memcpy() call
     * we make; plus allegedly the C++ front-end supplying the huge values during the diagnostics pass.
     * No such huge values (which are 0x800000000000000F, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, respectively)
     * are actually passed-in at run-time nor mentioned anywhere
     * in our code, here or in the unit-test(s) triggering the auto-inlining triggering the warning.  So:
     *
     * The warning is wholly inaccurate in a way reminiscent of the situation in reserve() with a somewhat
     * similar comment.  In this case, however, a pragma does properly work, so we use that approach instead of
     * a run-time check/assert() which would give away a bit of perf. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas" // For older versions, where the following does not exist/cannot be disabled.
#pragma GCC diagnostic ignored "-Wunknown-warning-option" // (Similarly for clang.)
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wrestrict" // Another similar bogus one pops up after pragma-ing away preceding one.

    /* Likely linear-time in `n` but hopefully optimized.  Could use a C++ construct, but I've seen that be slower
     * than a direct memcpy() call in practice, at least in a Linux gcc.  Could use boost.asio buffer_copy(), which
     * as of this writing does do memcpy(), but the following is an absolute guarantee of best performance, so better
     * safe than sorry (hence this whole Basic_blob class's existence, at least in part). */
    memcpy(dest_it, src_data, n);

#pragma GCC diagnostic pop
  }

  return dest_it + n;
} // Basic_blob::emplace_copy()

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Const_iterator
  Basic_blob<Allocator, S_SHARING_ALLOWED>::sub_copy(Const_iterator src, const boost::asio::mutable_buffer& dest,
                                                     log::Logger* logger_ptr) const
{
  // Code similar to emplace_copy().  Therefore keeping comments light.

  using std::memcpy;

  assert(valid_iterator(src));

  const size_type n = dest.size(); // Note the entire destination buffer is filled.
  if (n != 0)
  {
    const auto dest_data = static_cast<Iterator>(dest.data());

    if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, S_LOG_COMPONENT))
    {
      FLOW_LOG_SET_CONTEXT(logger_ptr, S_LOG_COMPONENT);
      FLOW_LOG_TRACE_WITHOUT_CHECKING("Blob [" << this << "] copying to "
                                      "memory area [" << static_cast<const void*>(dest_data) << "] sized "
                                      "[" << n << "] from internal buffer offset [" << (src - const_begin()) << "].");
    }

    assert(derefable_iterator(src));
    assert(difference_type(n) <= (const_end() - src)); // Can't copy from beyond end of *this blob.

    assert(((src + n) <= dest_data) || ((dest_data + n) <= src));

    // See explanation for the pragma in emplace_copy().  While warning not yet observed here, preempting it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas" // For older versions, where the following does not exist/cannot be disabled.
#pragma GCC diagnostic ignored "-Wunknown-warning-option" // (Similarly for clang.)
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wrestrict"
    memcpy(dest_data, src, n);
#pragma GCC diagnostic pop
  }

  return src + n;
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Iterator
  Basic_blob<Allocator, S_SHARING_ALLOWED>::erase(Const_iterator first, Const_iterator past_last)
{
  using std::memmove;

  assert(derefable_iterator(first)); // Input check.
  assert(valid_iterator(past_last)); // Input check.

  const Iterator dest = iterator_sans_const(first);

  if (past_last > first) // (Note: `past_last < first` allowed, not illegal.)
  {
    const auto n_moved = size_type(const_end() - past_last);

    if (n_moved != 0)
    {
      // See explanation for the pragma in emplace_copy().  While warning not yet observed here, preempting it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas" // For older versions, where the following does not exist/cannot be disabled.
#pragma GCC diagnostic ignored "-Wunknown-warning-option" // (Similarly for clang.)
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wrestrict"
      memmove(dest, iterator_sans_const(past_last), n_moved); // Cannot use memcpy() due to possible overlap.
#pragma GCC diagnostic pop
    }
    // else { Everything past end() is to be erased: it's sufficient to just update m_size: }

    m_size -= (past_last - first);
    // m_capacity does not change, as we advertised minimal operations possible to achieve result.
  } // if (past_last > first)
  // else if (past_last <= first) { Nothing to do. }

  return dest;
} // Basic_blob::erase()

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::value_type const &
  Basic_blob<Allocator, S_SHARING_ALLOWED>::const_front() const
{
  assert(!empty());
  return *const_begin();
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::value_type
  const & Basic_blob<Allocator, S_SHARING_ALLOWED>::const_back() const
{
  assert(!empty());
  return const_end()[-1];
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::value_type&
  Basic_blob<Allocator, S_SHARING_ALLOWED>::front()
{
  assert(!empty());
  return *begin();
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::value_type&
  Basic_blob<Allocator, S_SHARING_ALLOWED>::back()
{
  assert(!empty());
  return end()[-1];
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::value_type const &
  Basic_blob<Allocator, S_SHARING_ALLOWED>::front() const
{
  return const_front();
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::value_type const &
  Basic_blob<Allocator, S_SHARING_ALLOWED>::back() const
{
  return const_back();
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Const_iterator
  Basic_blob<Allocator, S_SHARING_ALLOWED>::const_begin() const
{
  return const_cast<Basic_blob*>(this)->begin();
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Iterator
  Basic_blob<Allocator, S_SHARING_ALLOWED>::begin()
{
  if (zero())
  {
    return 0;
  }
  // else

  /* m_buf_ptr.get() is value_type* when Buf_ptr = regular shared_ptr; but possibly Some_fancy_ptr<value_type>
   * when Buf_ptr = boost::interprocess::shared_ptr<value_type, Allocator_raw>, namely when
   * Allocator_raw::pointer = Some_fancy_ptr<value_type> and not simply value_type* again.  We need value_type*.
   * Fancy-pointer is not really an officially-defined concept (offset_ptr<> is an example of one).
   * Anyway the following works for both cases, but there are a bunch of different things we could write.
   * Since it's just this one location where we need to do this, I do not care too much, and the following
   * cheesy thing -- &(*p) -- is OK.
   *
   * @todo In C++20 can replace this with std::to_address().  Or can implement our own (copy cppreference.com impl). */

  const auto raw_or_fancy_buf_ptr = m_buf_ptr.get();
  return &(*raw_or_fancy_buf_ptr) + m_start;
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Const_iterator
  Basic_blob<Allocator, S_SHARING_ALLOWED>::const_end() const
{
  return zero() ? const_begin() : (const_begin() + size());
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Iterator
  Basic_blob<Allocator, S_SHARING_ALLOWED>::end()
{
  return zero() ? begin() : (begin() + size());
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Const_iterator
  Basic_blob<Allocator, S_SHARING_ALLOWED>::begin() const
{
  return const_begin();
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Const_iterator
  Basic_blob<Allocator, S_SHARING_ALLOWED>::cbegin() const
{
  return const_begin();
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Const_iterator
  Basic_blob<Allocator, S_SHARING_ALLOWED>::end() const
{
  return const_end();
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Const_iterator
  Basic_blob<Allocator, S_SHARING_ALLOWED>::cend() const
{
  return const_end();
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::value_type
  const * Basic_blob<Allocator, S_SHARING_ALLOWED>::const_data() const
{
  return const_begin();
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::value_type*
  Basic_blob<Allocator, S_SHARING_ALLOWED>::data()
{
  return begin();
}

template<typename Allocator, bool S_SHARING_ALLOWED>
bool Basic_blob<Allocator, S_SHARING_ALLOWED>::valid_iterator(Const_iterator it) const
{
  return empty() ? (it == const_end())
                 : in_closed_range(const_begin(), it, const_end());
}

template<typename Allocator, bool S_SHARING_ALLOWED>
bool Basic_blob<Allocator, S_SHARING_ALLOWED>::derefable_iterator(Const_iterator it) const
{
  return empty() ? false
                 : in_closed_open_range(const_begin(), it, const_end());
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Iterator
  Basic_blob<Allocator, S_SHARING_ALLOWED>::iterator_sans_const(Const_iterator it)
{
  return const_cast<value_type*>(it); // Can be done without const_cast<> but might as well save some cycles.
}

template<typename Allocator, bool S_SHARING_ALLOWED>
boost::asio::const_buffer Basic_blob<Allocator, S_SHARING_ALLOWED>::const_buffer() const
{
  return boost::asio::const_buffer{const_data(), size()};
}

template<typename Allocator, bool S_SHARING_ALLOWED>
boost::asio::mutable_buffer Basic_blob<Allocator, S_SHARING_ALLOWED>::mutable_buffer()
{
  return boost::asio::mutable_buffer{data(), size()};
}

template<typename Allocator, bool S_SHARING_ALLOWED>
typename Basic_blob<Allocator, S_SHARING_ALLOWED>::Allocator_raw
  Basic_blob<Allocator, S_SHARING_ALLOWED>::get_allocator() const
{
  return m_alloc_raw;
}

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>::Deleter_raw::Deleter_raw(const Allocator_raw& alloc_raw, size_type buf_sz) :
  /* Copy allocator; a stateless allocator should have size 0 (no-op for the processor in that case... except
   * the optional<> registering it has-a-value). */
  m_alloc_raw(std::in_place, alloc_raw),
  m_buf_sz(buf_sz) // We store a T*, where T is a trivial-deleter PoD, but we delete an array of Ts: this many.
{
  // OK.
}

template<typename Allocator, bool S_SHARING_ALLOWED>
Basic_blob<Allocator, S_SHARING_ALLOWED>::Deleter_raw::Deleter_raw() :
  m_buf_sz(0)
{
  /* This ctor is never invoked (see this ctor's doc header).  It can be left `= default;`, but some gcc versions
   * then complain m_buf_sz may be used uninitialized (not true but such is life). */
}

template<typename Allocator, bool S_SHARING_ALLOWED>
void Basic_blob<Allocator, S_SHARING_ALLOWED>::Deleter_raw::operator()(Pointer_raw to_delete)
{
  // No need to invoke dtor: Allocator_raw::value_type is Basic_blob::value_type, a boring int type with no real dtor.

  // Free the raw buffer at location to_delete; which we know is m_buf_sz `value_type`s long.
  m_alloc_raw->deallocate(to_delete, m_buf_sz);
}

} // namespace flow::util
