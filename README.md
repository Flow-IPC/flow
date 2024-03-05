# Flow -- Modern C++ toolkit for async loops, logs, config, benchmarking, and more

C++ power users, these days, are likely to use the standard library (a/k/a STL), Boost, and/or any number of
third-party libraries.  Nevertheless every large project or organization tends to need more reusable goodies,
whether to add to STL/Boost/etc. or in some cases do something better, perhaps in a specialized way.

Flow is such a library (provided as both headers and an actual library).  It's written in modern C++ (C++ 17
as of this writing) and is meant to be generally usable as opposed to particularly specialized.
(One exception to this is the included, but wholly optional, NetFlow protocol, contained in `flow::net_flow`
namespace.  While still reusable in a general way, interest in this functionality is likely niche.)

We refrain from delving into any particulars as to what's in Flow, aside from the following brief list of
its top-level modules.  The documentation (see Documentation below) covers all of its contents in great detail.
So, that said, Flow includes (alphabetically ordered):
  - `flow::async`: Single-threaded and multi-threaded event loops, augmenting boost.asio so as to actually create
    boost.asio-powered threads and thread pools, schedule timers more easily, and other niceties.
  - `flow::cfg`: Key-value configuration file parsing, augmenting a boost.program_options core with a large number of
    quality-of life additions including support for high-speed dynamically updated configuration `struct`s.
  - `flow::error`: A few niceties around the ubiquitous boost.system (now adopted by STL also) error-reporting
    system.  It also adds a simple convention for reporting errors, used across Flow, wherein each error-reporting
    API handles reporting results via error code return or exception (whichever the caller prefers at the call-site).
  - `flow::log`: A powerful and performant logging system.  While it can actually output to console and files,
    if so configured, its loggers will also easily integrate with your own log system of choice.
  - `flow::perf`: Benchmarking with multiple clock types and checkpoint-based accounting (if desired).
  - `flow::util`: General goodies.  Highlights: `Blob` (efficient and controlled `vector<uint8_t>` replacement with
    optional sharing and mem-pools), `Linked_hash_{map|set}` (hashed-lookup containers that maintain MRU-LRU iterator
    ordering as opposed to being unordered as in `unordered_{map|set}`), `String_ostream` (a sped-up `ostringstream`
    replacement leveraging boost.iostreams internally) plus `ostream_op_string()` function.

## Documentation

A comprehensive [Reference]([documentation](https://flow-ipc.github.io/doc/flow/versions/main/generated/html_public/namespaceflow.html))
is available.

The [project web site](https://flow-ipc.github.io) contains links to documentation for each individual release as well.

## Obtaining the source code

- As a tarball/zip: The [project web site](https://flow-ipc.github.io) links to individual releases with notes, docs,
  download links.
- For Git access: `git clone git@github.com:Flow-IPC/flow.git`

## Installation

See [INSTALL](./INSTALL.md) guide.

## Contributing

See [CONTRIBUTING](./CONTRIBUTING.md) guide.
