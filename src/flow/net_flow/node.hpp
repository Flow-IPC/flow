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

#include "flow/net_flow/net_flow_fwd.hpp"
#include "flow/log/log.hpp"
#include "flow/net_flow/error/error.hpp"
#include "flow/net_flow/peer_socket.hpp"
#include "flow/net_flow/server_socket.hpp"
#include "flow/net_flow/event_set.hpp"
#include "flow/net_flow/detail/net_flow_fwd.hpp"
#include "flow/net_flow/detail/low_lvl_packet.hpp"
#include "flow/net_flow/detail/port_space.hpp"
#include "flow/net_flow/net_env_simulator.hpp"
#include "flow/util/util.hpp"
#include <boost/unordered_map.hpp>

/**
 * Flow module containing the API and implementation of the *Flow network protocol*, a TCP-inspired stream protocol
 * that uses UDP as underlying transport.  See the large doc header on class net_flow::Node for the "root" of all
 * documentation w/r/t `net_flow`, beyond the present brief sentences.
 *
 * ### Historical note ###
 * Historically, the Flow project only existed in the first place to deliver the functionality now in this
 * `namespace` flow::net_flow.  However, since then, `net_flow` has become merely one of several Flow modules, each
 * providing functionality independent of the others'.  In the past, all/most `net_flow{}`
 * contents resided directly in `namespace` ::flow, but now it has been segregated into its own namespace.
 *
 * `net_flow` may still be, by volume, the largest module (hence also perhaps the largest user of general-use modules
 * like flow::log and flow::util).  Nevertheless, it is no longer "special."
 *
 * @see Main class net_flow::Node.
 */
namespace flow::net_flow
{
// Types.

/**
 * An object of this class is a single Flow-protocol networking node, in the sense that: (1) it has a distinct IP
 * address and UDP port; and (2) it speaks the Flow protocol over a UDP transport layer.  Here we summarize class Node
 * and its entire containing Flow module flow::net_flow.
 *
 * See also flow::asio::Node, a subclass that allows for full use of our API (its superclass) and turns our sockets
 * into boost.asio I/O objects, able to participate with ease in all boost.asio event loops.  If you're already very
 * familiar with `boost::asio::ip::tcp`, you can skip to the asio::Node doc header.  If not, recommend becoming
 * comfortable with the asio-less API, then read the forementioned asio::Node doc header.
 *
 * The flow::asio::Node class doc header (as of this writing) includes a compact summary of all network operations
 * supported by the entire hierarchy and hence deserves a look for your convenience.
 *
 * Using flow::net_flow, starting with the present class Node
 * ----------------------------------------------------------
 *
 * Node is an important and central class of the `netflow` Flow module and thus deserves some semi-philosophical
 * discussion, namely what makes a Node a Node -- why the name?  Let's delve into the 2 aforementioned properties of a
 * Node.
 *
 * ### A Node has a distinct IP address and UDP port: util::Udp_endpoint ###
 * A Node binds to an IP address and UDP port, both of which are given (with the usual ephemeral port and
 * IP address<->interface(s) nomenclature) as an argument at Node::Node() construction and can never change over the
 * lifetime of the object.  The IP and port together are a util::Udp_endpoint, which is a `using`-alias of boost.asio's
 * `boost::asio::ip::udp::endpoint` .  In the same network (e.g., the Internet) no two Node
 * objects (even in separate processes; even on different machines) may be alive (as defined by
 * `Node::running() == true`) with constructor-provided util::Udp_endpoint objects `R1` and `R2` such that `R1 == R2`.
 * In particular, if `Node n1` exists, with `n1.running()` and `n1.local_low_lvl_endpoint() == R1`, and on the same
 * machine one attempts to construct `Node n2(R2)`, such that `R1 == R2` (their IPs and ports are equal), then `n2`
 * will fail to properly construct, hence `n2.running() == false` will be the case, probably due to port-already-bound
 * OS error.  (There are counter-examples with NAT'ed IP addresses and special values 0.0.0.0 and port 0, but please
 * just ignore those and other pedantic objections and take the spirit of what I am saying.  Ultimately, the point
 * is:
 *
 * <em>A successfully constructed (`running() == true`) Node occupies the same IP-and-UDP "real estate" as would a
 * a mere successfully bound UDP socket.</em>
 *
 * So all that was a long, overbearing way to emphasize that a Node binds to an IP address and UDP port, and a single
 * such combo may have at most one Node on it (unless it has `!running()`).
 * *That's why it is called a Node*: it's a node on the network, especially on Internet.
 *
 * ### A Node speaks the *Flow network protocol* to other, remote Nodes ###
 * If `Node n1` is successfully constructed, and `Node n2` is as well, the two can communicate via a new protocol
 * implemented by this Flow module.  This protocol is capable of working with stream (TCP-like) sockets
 * implemented on top of UDP in a manner analogous to how an OS's net-stack implements
 * TCP over IP.  So one could call this Flow/UDP.  One can talk Flow/UDP to another
 * Flow/UDP endpoint (a/k/a Node) only; no compatibility with any other protocol is supported.
 * (This isn't, for example, an improvement to one side of TCP that is still compatible with legacy TCPs on
 * the other end; though that is a fertile area for research in its own right.)  The socket can also operate in
 * unreliable, message boundary-preserving mode, controllable via a Flow-protocol-native socket option; in which case
 * reliability is the responsibility of the `net_flow` user.  By default, though, it's like TCP: message bounds are not
 * preserved; reliability is guaranteed inside the protocol.  `n1` and `n2` can be local in the same process, or local
 * in the same machine, or remote in the same overall network -- as long as one is routable to the other, they can talk.
 *
 * For practical purposes, it's important to have idea of a single running() Node's "weight."  Is it light-weight like
 * a UDP or TCP socket?  Is it heavy-weight like an Apache server instance?  The answer is that it's MUCH close to
 * the former: it is fairly light-weight.  As of this writing, internally, it stores a table of peer and server sockets
 * (of which there could be a handful or tons, depending on the user's own API calls prior); and uses at least one
 * dedicated worker thread (essentially not visible to the user but conceptually similar to a UDP or TCP stack user's
 * view of the kernel: it does stuff for one in the background -- for example it can wait for incoming connections,
 * if asked).  So, a Node is an intricate but fairly light-weight object that stores socket tables (proportional in
 * size to the sockets currently required by the Node's user) and roughly a single worker thread performing low-level
 * I/O and other minimally CPU-intensive tasks.  A Node can get busy if a high-bandwidth network is sending or
 * receiving intense traffic, as is the case for any TCP or UDP net-stack.  In fact, a Node can be seen as a little
 * Flow-protocol stack implemented on top of UDP transport.  (Historical note: `class Node` used to be `class Stack`,
 * but this implied a heavy weight and misleadingly discouraged multiple constructions in the same program; all that
 * ultimately caused the renaming to Node.)
 *
 * ### Essential properties of Flow network protocol (Flow ports, mux/demuxing) ###
 * A single Node supports 0 or more (an arbitrary # of) peer-to-peer connections to other `Node`s.
 * Moreover, given two `Node`s `n1` and `n2`, there can similarly be 0 or more peer-to-peer connections
 * running between the two.  In order to allow this, a port (and therefore multiplexing/demultiplexing) system is
 * a feature of Flow protocol.  (Whether this features is necessary or even desirable is slightly controversial and
 * not a settled matter -- a to-do on this topic can be found below.)
 *
 * More specifically, think of a *given* `Node n1` as analogous (in terms of is multiplexing capabilities) to
 * one TCP stack running on a one-interface machine.  To recap the TCP port-addressing scheme (assuming only 1
 * interface): The TCP stack has approximately 2^16 (~65k) ports available.  One may create and "bind" a server "socket"
 * to (more or less, for our discussion) any 1 of these ports.  Let's say a server socket is bound to port P1.
 * If a remote TCP stack successfully connects to such a server-bound port, this results in a passively-connected
 * client "socket," which -- also -- is bound to P1 (bear with me as to how this is possible).  Finally, the TCP
 * stack's user may bind an *actively* connecting client "socket" to another port P2 (P2 =/= P1; as P1 is reserved
 * to that server and passively connected clients from that server).  Recall that we're contriving a situation where
 * there is only one other remote stack, so suppose there is the remote, 1-interface TCP stack.
 * Now, let's say a packet arrives along an established connection from this stack.
 * How does our local TCP stack determine to which connection this belongs?  This is
 * "demultiplexing."  If the packet contains the info "destination port: P2," then that clearly belongs to the
 * actively-connected client we mentioned earlier... but what if it's "dest. port: P1"?  This could belong to any
 * number of connections originally passive-connected by incoming server connection requests to port P1.
 * Answer: the packet also contains a "source TCP port" field.  So the *connection ID*
 * (a/k/a *socket ID*) consists of BOTH pieces of data: (1) destination (local) port; (2) source (remote) port.
 * (Recall that, symmetrically, the remote TCP stack had to have a client bind to some port,
 * and that means no more stuff can bind to that port; so it is unique and can't clash with anything else -- inside that
 * remote stack.)  So this tuple uniquely identifies the connection in this scenario of a single-interface local TCP
 * that can have both active client sockets and passive-client-socket-spawning server sockets; and talk to other stacks
 * like it.  Of course, there can be more than one remote TCP stack.  So the 2-tuple (pair) becomes a 3-tuple (triplet)
 * in our slightly simplified version of reality: (1) destination (local) TCP port; (2) source (remote) IP address;
 * and (3) source (remote) TCP port.  (In reality, the local TCP stack can bind
 * to different interfaces, so it becomes a 4-tuple by adding in destination (local) IP address... but that's TCP and
 * is of no interest to our analogy to Flow protocol.)
 *
 * What about Flow protocol?  GIVEN `n1` and `n2`, it works just the same.  We have a special, TCP-like, Flow port space
 * WITHIN `n1` and similarly within `n2`.  So if only `n1` and `n2` are involved, an `n1` Server_socket (class) object
 * can listen() (<-- actual method) on a net_flow::flow_port_t (<-- alias to 2-byte unsigned as of this writing)
 * port P1; Server_socket::accept() (another method) incoming connections, each still bound to port P1; and `n1` can
 * also actively connect() (another method) to `n2` at some port over there.  Then an incoming UDP packet's
 * intended established connection is demuxed to by a 2-tuple: (1) destination (local) `flow_port_t`;
 * (2) source (remote) `flow_port_t`.
 *
 * In reality, other remote `Node`s can of course be involved: `n3`, `n4`, whatever.  As we've established, each Node
 * lives at a UDP endpoint: util::Udp_endpoint (again, IP address + UDP port).  Therefore, from the stand-point of
 * a given local `Node n1`, each established peer-to-peer connection is identified fully by the 5-tuple (marked here
 * with roman numerals):
 *   1. Local `flow_port_t` within `n1`'s port-space (not dissimilar to TCP's port space in size and behavior). (I)
 *   2. Remote endpoint identifying the remote Node: Remote_endpoint.
 *      1. util::Udp_endpoint.
 *         1. IP address. (II)
 *         2. UDP port. (III)
 *      3. Remote net_flow::flow_port_t. (IV)
 *
 * So, that is how it works.  Of course, if this complexity is not really necessary for some application, then
 * only really (II) and (III) are truly necessary.  (I) and (IV) can just be chosen to be some agreed-upon
 * constant port number.  Only one connection can ever exist in this situation, and one would need to create
 * more `Node`s one side or the other or both to achieve more connections between the same pair of IP addresses,
 * but that's totally reasonable: it's no different from simply binding to more UDP ports.  My point here is that
 * the Flow-protocol-invented construct of "Flow ports" (given as `flow_port_t` values) can be used to conserve UDP
 * ports; but they can also be NOT used, and one can just use more UDP ports, as a "regular" UDP-using pair of
 * applications would, if more than one flow of information is necessary between those two apps.  It is up to you.
 * (Again, some arguments can be made for getting rid of (I) and (IV), after all.  This possibility is discussed in
 * a below to-do.)
 *
 * (Do note that, while we've emulated TCP's port scheme, there is no equivalent of IP's "interfaces."  Each Node
 * just has a bunch of ports; there is no port table belonging to each of N interfaces or any such thing.)
 *
 * ### flow::net_flow API overview ###
 * This is a summary (and some of this is very briefly mentioned above); all the classes and APIs are much more
 * deeply documented in their own right.  Also, see above pointer to asio::Node whose doc header may be immediately
 * helpful to experienced users.  Meanwhile, to summarize:
 *
 * The Node hands out sockets as Peer_socket objects; it acts as a factory for them (directly) via its connect() and
 * (indirectly) Server_socket::accept() families of methods.  It is not possible to construct a Peer_socket
 * independently of a Node, due to tight coordination between the Node and each Peer_socket.  Moreover each Peer_socket
 * is handed out via `boost::shared_ptr` smart pointer.  While not strictly necessary, this is a situation where both
 * the user and a central registry (Node) can own the Peer_socket at a given time, which is an ideal application for
 * `shared_ptr<>` that can greatly simplify questions of object ownership and providing auto-`delete` to boot.
 *
 * Thus: `Node::listen(flow_port_t P)` yields a Server_socket::Ptr, which will listen for incoming connections on `P`.
 * Server_socket::accept() (and similar) yields a Peer_socket::Ptr, one side of a peer-to-peer connection.
 * On the other side, `Node::connect(Remote_endpoint R)` (where `R` contains `Udp_endpoint U`, where
 * value equal to `U` had been earlier passed to constructor of the `listen()`ing `Node`; and `R` also contains
 * `flow_port_t P`, passed to `Node::listen()`).  connect(), too, yields a Peer_socket::Ptr.  And thus, if all went
 * well, each side now has a Peer_socket::Ptr `S1` and `S2`, which -- while originating quite differently --
 * are now completely equal in capabilities: they are indeed *peer* sockets.  They have methods like Peer_socket::send()
 * and Peer_socket::receive().
 *
 * Further nuances can be explored in the various other doc headers, but I'll mention that both non-blocking behavior
 * (meaning the call always returns immediately, even if unable to immediately perform the desired task such as
 * accept a connection or receive 1 or more bytes) and blocking behavior as supported, as in (for example) a BSD
 * sockets API.  However, there is no "blocking" or "non-blocking" mode as in BSD or WinSock (personally I, Yuri, see it
 * as an annoying anachronism).  Instead you simply call a method named according to whether it will never block or
 * (possibly -- if appropriate) block.  The nomenclature convention is as follows: if the action is `X` (e.g.,
 * `X` is `receive` or `accept`), then `->X()` is the non-blocking version; and `->sync_X()` is the blocking one.
 * A non-blocking version always exists for any possible action; and a blocking version exists if it makes sense for it
 * to exist.  (Exception: Event_set::async_wait() explicitly includes `async_` prefix contrary to this convention.
 * Partially it's because just calling it `wait()` -- convention or not -- makes it sound like it's going to block,
 * whereas it emphatically will never do so.  ALSO it's because it's a "special" method with unique properties
 * including letting user execute their own code in a Node's internal worker thread.  So rules go out the window a
 * little bit for that method; hence the slight naming exception.)
 *
 * ### Nomenclature: "low-level" instead of "UDP" ###
 * Side note: You will sometimes see the phrase `low_lvl` in various identifiers among `net_flow` APIs.
 * `low_lvl` (low-level) really means "UDP" -- but theoretically some other packet-based transport could be used
 * instead in the future; or it could even be an option to chooose between possible underlying protocols.
 * For example, if `net_flow` moved to kernel-space, the transport could become IP, as it is for TCP.
 * So this nomenclature is a hedge; and also it argubly is nicer/more generic: the fact it's UDP is immaterial; that
 * it's the low-level (from our perspective) protocol is the salient fact.  However, util::Udp_endpoint is thus named
 * because it is very specifically a gosh-darned UDP port (plus IP address), so hiding from that by naming it
 * `Low_Lvl_endpoint` (or something) seemed absurd.
 *
 * ### Event, readability, writability, etc. ###
 * Any experienced use of BSD sockets, WinSock, or similar is probably wondering by now, "That sounds reasonable, but
 * how does the API allow me to wait until I can connect/accept/read/write, letting me do other stuff in the meantime?"
 * Again, one can use a blocking version of basically every operation; but then the wait for
 * readability/writability/etc. may block the thread.  One can work around this by creating multiple threads, but
 * multi-threaded coding introduced various difficulties.  So, the experienced socketeer will want to use non-blocking
 * operations + an event loop + something that allow one to wait of various states (again, readability, writability,
 * etc.) with various modes of operation (blocking, asynchronous, with or without a timeout, etc.).
 * The most advanced and best way to get these capabilities is to use boost.asio integration (see asio::Node).
 * As explained elsewhere (see Event_set doc header) this is sometimes not usable in practice.  In that case:
 * These capabilities are supplied in the class Event_set.  See that class's doc header for further information.
 * Event_set is the `select()` of this socket API.  However it is significantly more convenient AND indeed supports
 * a model that will allow one to use Flow-protocol sockets in a `select()`- or equivalent-based event loop, making
 * `net_flow` module usable in a true server, such as a web server.  That is, you don't just have to write a separate
 * Flow event loop operating independently of your other sockets, file handles, etc.  This is an important property in
 * practice.  (Again: Ideally you wouldn't need Event_set for this; asio::Node/etc. might be better to use.)
 *
 * ### Error reporting ###
 * Like all Flow modules, `net_flow` uses error reporting conventions/semantics introduced in `namespace` ::flow
 * doc header Error Reporting section.
 *
 * In particular, this module does add its own error code set.  See `namespace` net_flow::error doc header which
 * should point you to error::Code `enum`.  All error-emitting `net_flow` APIs emit `Error_code`s assigned from
 * error::Code `enum` values.
 *
 * ### Configurability, statistics, logging ###
 * Great care is taken to provide visibility into the "black box" that is Flow-protocol.  That is, while the API follows
 * good practices wherein implementation is shielded away from the user, at the same time the *human* user has powerful
 * tools to both examine the insides of the library/protocol's performance AND to tweak the parameters of its
 * behavior.  Picture a modern automobile: while you're driving at least, it's not going to let you look at or mess with
 * its engine or transmission -- nor do you need to understand how they work; BUT, the onboard monitor
 * will feature screens that tell you about its fuel economy performance, the engine's inner workings, and perhaps a
 * few knobs to control the transmission's performance (for example).  Same principles are followed here.
 *
 * More specifically:
 *   - *Configuration* Socket options are supported via Node_options and Peer_socket_options.  These control many
 *     aspects of the library's behavior, for example which congestion control algorithm to use.
 *     These options can be set programmatically, through a config file, or through command line options.
 *     Particular care was taken to make the latter two features seamlessly available by
 *     leveraging boost.program_options.
 *   - *Statistics* Very detailed stats are kept in Peer_socket_receive_stats and Peer_socket_send_stats, combined
 *     with more data in Peer_socket_info.  These can be accessed programmatically; their individual stats can also
 *     be accessed programmatically; or they can be logged to any `ostream`.  Plus, the logging system periodically logs
 *     them (assuming this logging level is enabled).
 *   - *Logging* Like all Flow modules, `net_flow` uses logging conventions/semantics introduced in `namespace` ::flow
 *     doc header Logging section.
 *
 * ### Multiple Node objects ###
 * As mentioned already many times, multiple Node objects can exist and function simultaneously (as long as they
 * are not bound to the same conceptual util::Udp_endpoint, or to the same UDP port of at least one IP interface).
 * However, it is worth emphasizing that -- practically speaking -- class Node is implemented in such a way as to make
 * a given Node 100% independent of any other Node in the same process.  They don't share working thread(s), data
 * (except `static` data, probably just constants), any namespaces, port spaces, address spaces, anything.  Each Node
 * is independent both API-wise and in terms of  internal implementation.
 *
 * ### Thread safety ###
 * All operations safe for simultaneous execution on 2+ separate Node objects *or on the same Node*,
 * or on any objects (e.g., Peer_socket) returned by Node.  (Please note the *emphasized* phrase.)
 * "Operations" are any Node or Node-returned-object method calls after construction and before destruction of the
 * Node.  (In particular, for example, one thread can listen() while another connect()s.)  The same guarantee may or
 * may not apply to other classes; see their documentation headers for thread safety information.
 *
 * ### Thread safety of destructor ###
 * Generally it is not safe to destruct a Node (i.e., let Node::~Node() get called) while a Node operation is in
 * progress on that Node (obviously, in another thread).  There is one exception to this: if a blocking operation
 * (any operation with name starting with `sync_`) has entered its blocking (sleep) phase, it is safe to delete the
 * underlying Node.  In practice this means simply that, while you need not lock a given Node with an external
 * mutex while calling its various methods from different threads (if you really must use multiple threads this way),
 * you should take care to probably join the various threads before letting a Node go away.
 *
 * Historical note re. FastTCP, Google BBR
 * ---------------------------------------
 *
 * ### Historical note re. FastTCP ###
 * One notable change in this `net_flow` vs. the original libgiga is this
 * one lacks the FastTCP congestion control strategy.  I omit the historical reasons for this for now
 * (see to-do regarding re-introducing licensing/history/location/author info, in common.hpp).
 *
 * Addendum to the topic of congestion control: I am not that interested in FastTCP, as I don't see it as cutting-edge
 * any longer.  I am interested in Google BBR.  It is a goal to implement Google BBR in `net_flow`, as that congestion
 * control algorithm is seen by many as simply the best one available; a bold conclusion given how much research
 * and given-and-take and pros-and-cons discussions have tramspired ever since the original Reno TCP became ubiquitous.
 * Google BBR is (modulo whatever proprietary improvements Google chooses to implement in their closed-source software)
 * publicly documented in research paper(s) and, I believe, available as Google open source.
 *
 * @todo flow::net_flow should use flow::cfg for its socket-options mechanism.  It is well suited for that purpose, and
 * it uses some ideas from those socket-options in the first place but is generic and much more advanced.  Currently
 * `net_flow` socket-options are custom-coded from long before flow::cfg existed.
 *
 * @todo `ostream` output operators for Node and asio::Node should exist.  Also scour through all types; possibly
 * some others could use the same.  (I have been pretty good at already implementing these as-needed for logging; but
 * I may have "missed a spot.")
 *
 * @todo Some of the `ostream<<` operators we have take `X*` instead of `const X&`; this should be changed to the latter
 * for various minor reasons and for consistency.
 *
 * @todo Actively support IPv6 and IPv4, particularly in dual-stack mode (wherein net_flow::Server_socket would bind to
 * an IPv6 endpoint but accept incoming V4 and V6 connections alike).  It already supports it nominally, in that one
 * can indeed listen on either type of address and connect to either as well, but how well it works is untested, and
 * from some outside experience it might involve some subtle provisions internally.
 *
 * @todo Based on some outside experience, there maybe be problems -- particularly considering the to-do regarding
 * dual-stack IPv6/v4 support -- in servers listening in multiple-IP situations; make sure we support these seamlessly.
 * For example consider a machine with a WAN IP address and a LAN (10.x.x.x) IP address (and possibly IPv6 versions
 * of each also) that (as is typical) binds on all of them at ANY:P (where P is some port; and ANY is the IPv6 version,
 * with dual-stack mode ensuring V4 datagrams are also received).  If a client connects to a LAN IP, while in our
 * return datagrams we set the source IP to the default, does it work?  Outside experience shows it might not,
 * depending, plus even if in our protocol it does, it might be defeated by some firewall... the point is it requires
 * investigation (e.g., mimic TCP itself; or look into what IETF or Google QUIC does; and so on).
 *
 * @internal
 *
 * Implementation notes
 * --------------------
 *
 * In this section, and within implementation, I may simply say "Flow" instead of "Flow [network] protocol," for
 * brevity.  This is not meant to represent all of the containing Flow project nor refer to any module other
 * that `net_flow`.
 *
 * ### Note on general design philosophy ###
 * The protocol is TCP-like.  However, it is not TCP.  Moreover, it is not
 * TCP even if you remove the fact that it sends UDP datagrams instead of IP packets.  It follows the basic
 * goals of TCP (stream of bytes, reliability, congestion control, etc.), and it borrows many of its internal
 * techniques (congestion window, receive window ACKs, drop timeout, etc.), but it specifically will not
 * inherit those limitations of TCP that are essentially historic and/or a result of having to be backwards-
 * compatible with the other side which may be behind.  For example, all ACKs in Flow are selective; whereas in
 * TCP ACKs are generally cumulative, while selective ACKs (SACKs) are an advanced option that the other side
 * may or may not support.  Making certain modern decisions in Flow that TCP implementations simply cannot
 * make means (1) a simpler protocol and implementation; and (2) potentially higher performance before we can
 * try any advanced stuff like new congestion control strategies or FEC (forward error correction).
 *
 * Moreover, I tried to take care in not copying various classic TCP RFCs mindlessly.  Instead, the idea was
 * to evaluate the spirit, or intent, of each technical detail of a given RFC -- and then translate it into
 * the (hopefully) more elegant and less historical baggage-encumbered world of Flow.  This is particularly
 * something I felt when translating the terse congestion control-related TCP RFCs into `net_flow` C++ code. (For
 * a more specific explanation of what I mean, check out the "general design note" in the class doc header of
 * the class Congestion_control_strategy.)  Hopefully this means terse, concern-conflating TCP RFCs (and at
 * times Linux kernel's somewhat less concern-conflating implementations of these RFCs) become clean, concern-
 * separated Flow code.  This means, also, a rather extremely high ratio of comments to code in many areas
 * of this project.  I wanted to clearly explain every non-trivial decision, even if it meant many long-winded
 * English explanations.
 *
 * ### Basic implementation ###
 * Constructor creates new thread, henceforth called thread W, which houses the Node's
 * main loop  and performs all blocking work (and generally most work) for the class; this exists until the
 * destructor executes.  In general, for simplicity (in a way) and consistency, even when a given call (e.g.,
 * non-blocking connect()) could perform some preliminary work (e.g., argument checking and ephemeral port
 * reservation) directly in the caller's thread and then place a callback (e.g., connect_worker()) on W for
 * the real work (e.g., filling out packet, sending SYN, etc.), we instead choose to put all the work (even
 * the aforementioned preliminary kind) onto W and (non-blockingly!) wait for that callback to finish, via a
 * boost.thread `future`.  While a bit slower, I find this simplifies the code by breaking it up less and
 * keeping it in one place in the code base for a given operation (in this example, connect()).  It can also
 * reduce mutex usage in our code.  Also it enables a slightly better user experience, as errors are reported
 * earlier in the user code and state changes are not asynchronous, when they don't need to be.
 *
 * The above can apply to such things as non-blocking connect(), listen().  It probably wouldn't apply
 * to Peer_socket::send(), Peer_socket::receive(), Server_socket::accept() for performance reasons.
 *
 * ### Note on threads W and U ###
 * In classic Berkeley sockets, one often thinks of "the kernel" performing certain
 * work in "the background," the results of which the user level code might access.  For example, the kernel
 * might receive IP packets on some port and deserialize them into a socket's receive buffer -- while the
 * user's program is busy doing something completely unrelated like showing graphics in a video game -- then
 * the `recv()` call would later transfer the deserialized stream into the user's own user-level buffer (e.g., a
 * `char[]` array).  In our terminology, "thread W" is the equivalent of "the kernel" doing stuff "in the
 * background"; while "thread U" refers to the user's own thread where they do other stuff and at times access
 * the results of the "kernel background" stuff.  However, Flow is a user-level tool, so thread W is not
 * actually running in the kernel... but conceptually it is like it.
 *
 * ### boost.asio ###
 * The implementation heavily uses the boost.asio library, as recommended by ::flow doc
 * header boost.asio section.  This implements the main loop's flow
 * (with a callback-based model), all UDP traffic, timers.
 * Why use boost.asio and not home-made networking and event loop code?  There are a few good reasons.  For
 * UDP networking, boost.asio provides a pain-free, portable, object-oriented wrapper around BSD
 * sockets/WinSock (that nevertheless exposes the native resources like sockets/FDs *if* needed).  For a high-
 * performance event loop, boost.asio gives a flexible *proactor pattern* implementation, supporting arbitrary
 * callbacks with lambdas or `bind()`, which is much easier and prettier than C-style function pointers with `void*`
 * arguments a-la old-school HTTP servers written in C.  For a complex event-driven loop like this, we'd have
 * to implement some callback system anyway, and it would likely be far worse than boost.asio's, which is
 * full-featured and proven through the years.
 *
 * To-dos and future features
 * --------------------------
 *
 * @todo Receive UDP datagrams as soon as possible (avoid internal buffer overflow):
 * The OS UDP net-stack buffers arriving datagrams until they're `recv()`d by the application
 * layer.  This buffer is limited; on my Linux test machine the default appears to buffer ~80 1k datagrams.
 * With a high sender CWND and high throughput (e.g., over loopback), thread W -- which both reads off UDP
 * datagrams and handles them, synchronously -- cannot always keep up, and the buffer fills up.  This
 * introduces Flow loss despite the fact that the datagram actually safely got to the destination; and this is
 * with just ONE sender; in a server situation there could be thousands.  In Linux I was able to raise, via
 * `setsockopt()`, the buffer size to somewhere between 1000 and 2000 1k datagrams.  This helps quite a bit.
 * However, we may still overflow the buffer in busy situations (I have seen it, still with only one
 * connection).  So, the to-do is to solve this problem.  See below to-dos for ideas.
 * WARNING: UDP buffer overflow can be hard to detect and may just look like loss; plus the user not
 * reading off the Receive buffer quickly enough will also incur similar-looking loss.  If there
 * were a way to detect the total # of bytes or datagrams pending on a socket, that would be cool,
 * but `sock.available()` (where `sock` is a UDP socket) just gives the size of the first queued datagram.
 * Congestion control (if effective) should prevent this problem, if it is a steady-state situation (i.e.,
 * loss or queueing delay resulting from not keeping up with incoming datagrams should decrease CWND).
 * However, if it happens in bursts due to high CPU use in the environment, then that may not help.
 * NOTE 1: In practice a Node with many connections is running on a server and thus doesn't
 * receive that much data but rather sends a lot.  This mitigates the UDP-receive-buffer-overflow
 * problem, as the Node receiving tons of data is more likely to be a client and thus have only one
 * or two connections.  Hence if we can handle a handful of really fast flows without such loss,
 * we're good.  (On other hand, ACKs are also traffic, and server may get a substantial amount of
 * them.  Much testing is needed.)  Packet pacing on the sender side may also avoid this loss
 * problem; on the other hand it may also decrease throughput.
 * NOTE 2: Queue delay-based congestion control algorithms, such as FastTCP and Vegas, are highly
 * sensitive to accurate RTT (round trip time) readings.  Heavy CPU load can delay the recording of the
 * "received" time stamp, because we call UDP `recv()` and handle the results all in one thread.  Any solution,
 * such as the dedicated thread proposed below, would _allow_ one to record the time stamp immediately upon receipt
 * of the packet by the dedicated thread; while W would independently handle everything else.  Is that a good
 * idea though?  Maybe not.  If the CPU load is such that ACK-receiver-side can't get to the time-stamp-saving,
 * RTT-measuring step without tricks like doing it immediately upon some low-level datagram receipt hook, then
 * that CPU-pegged jungle is, in a way, part of the network and should probably be fairly counted as part of
 * the RTT.  So perhaps we should continue to take the RTT time stamp while actually handling the individual
 * acknowledgments.  Instead we should strive to use multi-core resources efficiently, so that the gap between
 * receipt (on whatever thread) and acknowledgment processing (on whatever thread) is as small as possible.
 * Then RTT is RTT, but we make it smaller via improved performance.  Meanwhile, we hopefully also solve the
 * original problem (internal kernel buffer overflowing and dropping datagrams).
 *
 * @todo Receive UDP datagrams as soon as possible (avoid internal buffer overflow): APPROACH 1 (CO-WINNER!):
 * One approach is to note that, as of this writing, we call `m_low_lvl_sock.async_wait(wait_read)`;
 * our handler is called once there is at least 1 message TO read;
 * and then indeed our handler does read it (and any more messages that may also have arrived).
 * Well, if we use actual `async_receive()` and an actual buffer instead,
 * then boost.asio will read 1 (and no more, even if there are more)
 * message into that buffer and have it ready in the handler.  Assuming the mainstream case involves only 1
 * message being ready, and/or assuming that reading at least 1 message each time ASAP would help significantly,
 * this may be a good step toward relieving the problem, when it exists.  The code becomes a tiny bit less
 * elegant, but that's negligible.  This seems like a no-brainer that should be included in any solution, but it
 * by itself may not be sufficient, since more than 1 datagram may be waiting, and datagrams 2, 3, ... would
 * still have to be read off by our code in the handler.  So other approaches should still be considered.
 *
 * @todo Receive UDP datagrams as soon as possible (avoid internal buffer overflow): APPROACH 2:
 * To eliminate the problem to the maximum extent possible, we can dedicate its own thread --
 * call it W2 -- to reading #m_low_lvl_sock.  We could also write to #m_low_lvl_sock on W2 (which would also
 * allow us to use a different util::Task_engine from #m_task_engine to exclusively deal with W2 and
 * the UDP socket #m_low_lvl_sock -- simpler in some ways that the strand-based solution described below).
 * There are a couple of problems with this.  One, it may delay receive ops due to send ops, which compromises
 * the goals of this project in the first place.  Two, send pacing code is in thread W (and moving it to W2
 * would be complex and unhelpful); and once the decision to REALLY send a given datagram has been made,
 * this send should occur right then and there -- queuing that task on W2 may delay it, compromising the
 * quality of send pacing (whose entire nature is about being precise down to the microsecond or even better).
 * Therefore, we'd like to keep `m_low_lvl_sock.async_send()` in thread W along with all other work (which
 * allows vast majority of internal state to be accessed without locking, basically just the Send/Receive
 * buffers excluded); except the chain of [`m_low_lvl_sock.async_receive()` -> post handler of received datagram
 * onto thread W; and immediately `m_low_lvl_sock.async_receive()` again] would be on W2.
 * AS WRITTEN, this is actually hard or impossible to do with boost.asio because of its design: #m_low_lvl_sock
 * must belong to exactly one `Task_engine` (here, #m_task_engine), whereas to directly schedule a specific
 * task onto a specific thread (as above design requires) would require separate `Task_engine` objects (1 per
 * thread): boost.asio guarantees a task will run on *a* thread which is currently executing `run()` --
 * if 2 threads are executing `run()` on the same service, it is unknown which thread a given task will land
 * upon, which makes the above design (AS WRITTEN) impossible.  (Side note: I'm not sure it's possible in plain C
 * with BSD sockets either.  A naive design, at least, might have W `select()` on `m_low_lvl_sock.native()` for
 * writability as well other stuff like timers, while W2 `select()`s on same for readability; then the two
 * threads perform UDP `send()` and `recv()`, respectively, when so instructed by `select()`s.  Is it allowed
 * to use `select()` on the same socket concurrently like that?  StackOverflow.com answers are not clear cut, and
 * to me, at least, it seems somewhat dodgy.)  However, an equivalent design IS possible (and supported cleanly by
 * boost.asio):  In addition to the 2 threads, set up 2 strands, S and S2.  All work except the #m_low_lvl_sock
 * reads and posting of the handler onto S will be scheduled with a strand S.  All work regarding
 * #m_low_lvl_sock reads and posting of its handler onto S will be scheduled with a strand S2.
 * Recall that executing tasks in 1 strand, with 2+ threads executing `run()`, guarantees that no 2+ of
 * those tasks will execute simultaneously -- always in series.  This is actually -- in terms of efficiency
 * and thread safety -- equivalent to the above W/W2 design.  Since the S tasks will always execute serially,
 * no locking is necessary to prevent concurrent execution; thus what we know today as thread W tasks (which
 * need no locking against each other) will be equally thread safe; and same holds for the new S2 tasks
 * (which are considerably simpler and fewer in number).  So that's the thread safety aspect; but why is
 * efficiency guaranteed?  The answer becomes clear if one pictures the original thread W/W2 design;
 * basically little task blocks serially pepper thread W timeline; and same for W2.  By doing it with strands
 * S and S2 (running on top of threads W and W2), the only thing that changes is that the little blocks
 * might at random be swapped between threads.  So the series of tasks T1 -> T2 -> T3 -> T4 meant for
 * for S might actually jump between W and W2 randomly; but whenever thread W2 is chosen instead of
 * thread W, that leaves an empty "space" in thread W, which will be used by the S2 task queue if it
 * needs to do work at the same time.  So tasks may go on either thread, but the strands ensure
 * that both are used with maximum efficiency while following the expected concurrency constraints
 * (that each strand's tasks are to be executed in series).  Note, however, that #m_low_lvl_sock (the
 * socket object) is not itself safe for concurrent access, so we WILL need a lock to protect the tiny/short
 * calls `m_low_lvl_sock.async_receive()` and `m_low_lvl_sock.async_send()`: we specifically allow that
 * a read and write may be scheduled to happen simultaneously, since the two are independent of each
 * other and supposed to occur as soon as humanly possible (once the desire to perform either one
 * is expressed by the code -- either in the pacing module in strand S or the read handler in S2).
 * In terms of nomenclature, if we do this, it'd be more fair to call the threads W1 and W2 (as they
 * become completely equal in this design).  (In general, any further extensions of this nature (if
 * we want still more mostly-independent task queues to use available processor cores efficiently),
 * we would add 1 strand and 1 worker thread per each such new queue.  So basically there's a thread pool
 * of N threads for N mostly-independent queues, and N strands are used to use that pool efficiently
 * without needing to lock any data that are accessed exclusively by at most 1 queue's tasks only.
 * Resources accessed by 2 or more task queues concurrently would need explicit locking (e.g.,
 * #m_low_lvl_sock in this design).)  So then where we start thread W today, we'd start the thread
 * pool of 2 threads W1, W2, with each executing `m_task_engine.run()`.  Before the run()s execute,
 * the initial tasks (each wrapped in strand S or S2, as appropriate) need to be posted onto
 * #m_task_engine; this can even occur in the user thread U in the constructor, before W1 and W2
 * are created.  The destructor would `m_task_engine.stop()` (like today), ending each thread's
 * `run()` and trigger the imminent end of that thread; at which point destructor can `W1.join()` and `W2.join()`
 * (today it's just `W.join()`).
 *
 * @todo Receive UDP datagrams as soon as possible (avoid internal buffer overflow): APPROACH 3:
 * Suppose we take APPROACH 1 (no-brainer) plus APPROACH 2.  Certain decisions in the latter were made for
 * certain stated reasons, but let's examine those more closely.  First note that the APPROACH 1 part will
 * ensure that, given a burst of incoming datagrams, the first UDP `recv()` will occur somewhere inside boost.asio,
 * so that's definitely a success.  Furthermore, strand S will invoke `m_low_lvl_sock.async_send()` as soon as
 * the pacing module decides to do so; if I recall correctly, boost.asio will invoke the UDP `send()` right then
 * and there, synchronously (at least I wrote that unequivocally in a Node::async_low_lvl_packet_send_impl() comment).
 * Again, that's as good as we can possibly want.  Finally, for messages 2, 3, ... in that incoming datagram burst,
 * our handler will (indirectly but synchronously) perform the UDP `recv()`s in strand S2.  Here we're somewhat
 * at boost.asio's mercy, but assuming its strand task scheduling is as efficient as possible, it should occur
 * on the thread that's free, and either W1 or W2 should be free given the rest of the design.  Still, boost.asio
 * docs even say that different strands' tasks are NOT guaranteed to be invoked concurrently (though common
 * sense implies they will be when possible... but WHAT IF WE'RE WRONG!!!?).  Also, we don't know how much
 * computational overhead is involved in making strands work so nicely (again, hopefully it's well written...
 * but WHAT IF!!!?).  A negative is the small mutex section around the two #m_low_lvl_sock calls; not complex
 * and probably hardly a performance concern, but still, it's a small cost.  Finally, using strands -- while
 * not ugly -- does involve a bit more code, and one has to be careful not to forget to wrap each handler with
 * the appropriate strand (there is no compile- or runtime error if we forget!)  So what can we change about
 * APPROACH 2 to avoid those negatives?  As stated in that approach's description, we could have thread W
 * not deal with #m_low_lvl_sock at all; thread W2 would have a separate `Task_engine` handling only
 * #m_low_lvl_sock (so no mutex needed).  W2 would do both sends and receives on the socket; and absolutely
 * nothing else (to ensure it's as efficient as possible at getting datagrams off the kernel buffer, solving
 * the original problem).  Yes, the receiving would have to share time with the sends, but assuming nothing
 * else interferes, this feels like not much of a cost (at least compared with all the heavy lifting thread W
 * does today anyway).  Each receive would read off all available messages into raw buffers and pass those
 * (sans any copying) on to thread W via `post(W)`.  The other negative, also already mentioned, is that
 * once pacing module (in thread W) decides that a datagram should be sent, the `post(W2)` for the task that
 * would peform the send introduces a delay between the decision and the actual UDP `send()` done by boost.asio.
 * Thinking about it now, it is questionable to me how much of a cost that really is.  Without CPU contention,
 * we can measure it; I expect it to be quite cheap, but I coudl be wrong.  With CPU contention -- which would
 * have to come from many datagrams arriving at the same time -- I'm not sure.  It wouldn't be overly hard to
 * test; basically flood with UDP traffic over loopback and log the delay between W deciding to send datagram
 * and W2 calling `m_low_lvl_sock.async_send_to()` (obviously use #Fine_clock!).  All in all, if we name
 * the dedicated thread approach described here as APPROACH 3, then APPROACH 3 is appealingly simpler than
 * APPROACH 2; and in most ways appears like it'd be at least as efficient and good at solving the original
 * problem as APPROACH 2.  The only danger that worries me is this business with messing up pacing (and no,
 * moving pacing into W2 just endangers the receiving efficiency and introduces thread safety problems and
 * complexity) by having it compete with receiving during incoming-traffic-heavy times.  Ultimately, I'd
 * recommend timing this "danger zone" as described a bit earlier (also time delay introduced by `post(W2)`
 * without any traffic coming in); and if it looks good, do APPROACH 3.  Otherwise spring for APPROACH 2.
 *
 * @todo Receive UDP datagrams as soon as possible (avoid internal buffer overflow): APPROACH 4:
 * It now occurs to me how to solve the main questionable part about APPROACH 3.  If we fear that the reads and
 * writes in thread W2 may compete for CPU, especially the reads delaying timing-sensitive paced writes,
 * then we can eliminate the problem by taking W2's own util::Task_engine (which would be separate from
 * #m_task_engine) and have two equal threads W2' and W2'' start up and then each call `Task_engine::run()`.
 * Without having to use any strand, this will essentially (as documented in boost.asio's doc overview)
 * establish a thread pool of 2 threads and then perform the receive and send tasks at random on whichever
 * thread(s) is/are available at a particular time.  Since there's no strand(s) to worry about, the underlying
 * overhead in boost.asio is probably small, so there's nothing to fear about efficiency.  In fact, in this
 * case we've now created 3 separate threads W, W2', W2'', all doing things independently of each other, which
 * is an excellent feature in terms of using multiple cores.  Do note we will now need a mutex and very short
 * critical sections around the calls to `m_low_lvl_sock::async_receive()` and `m_low_lvl_sock::async_send()`,
 * but as noted before this seems extremely unlikely to have any real cost due to the shortess of critical
 * sections in both threads.  If this is APPROACH 4, then I'd say just time how much absolute delay is
 * introduced by a `post(W2')` or `post(W2'')` of the async send call compared to directly making such a
 * call on thread W, as is done today.  I suspect it's small, in which case the action is go for
 * APPROACH 4... finally.
 *
 * @todo Receive UDP datagrams as soon as possible (avoid internal buffer overflow): APPROACH 5 (CO-WINNER!):
 * Actually, the thing I've been dismissing in approaches 2-4, which was to combine the pacing logic with the
 * actual `m_low_lvl_sock.async_send()` (like today) but in their own dedicated thread, now seems like the best
 * way to solve the one questionable aspect of APPROACH 4.  So, APPROACH 4, but: Move the pacing stuff into
 * the task queue associated with threads W2' and W2''.  So then these 2 threads/cores will be available for
 * 2 task queues: one for pacing timers + datagram sending over #m_low_lvl_sock (with mutex); the other for
 * receiving over #m_low_lvl_sock (with same mutex).  Now, the only "delay" is moved to before pacing occurs:
 * whatever minimal time cost exists of adding a queue from thread W to thread W2' or W2'' occurs just before
 * the pacing logic, after which chances are the datagram will be placed on a pacing queue anyway and sent
 * off somewhat later; intuitively this is better than the delay occurring between pacing logic and the
 * actual UDP send.  Note, also, that the timing-sensitive pacing logic now gets its own thread/core and should
 * thus work better vs. today in situations when thread W is doing a lot of work.  This is even more logical
 * than APPROACH 4 in that sense; the pacing and sending are concern 1 and get their own thread (essentially;
 * really they get either W2' or W2'' for each given task); the receiving is concern 2 and gets its own thread
 * (same deal); and all the rest is concern 3 and remains in its own thread W (until we can think of ways to
 * split that into concerns; but that is another to-do).  Only one mutex with 2 very small critical sections,
 * as in APPROACH 4, is used.  The only subtlety regarding concurrent data access is in
 * Node::mark_data_packet_sent(), which is called just before `m_low_lvl_sock.async_send()`, and which
 * finalizes certain aspects of Peer_socket::Sent_packet::Sent_when; most notably
 * Peer_socket::Sent_packet::Sent_when::m_sent_time (used in RTT calculation upon ACK receipt later).
 * This is stored in Peer_socket::m_snd_flying_pkts_by_sent_when, which today is not protected by mutex due
 * to only being accessed from thread W; and which is extremely frequently accessed.  So either we protect
 * the latter with a mutex (out of the question: it is too frequently accessed and would quite possibly
 * reduce performance) or something else.  Currently I think Node::mark_data_packet_sent() should just
 * be placed onto #m_task_engine (thread W) via `post()` but perhaps take all or most of the items to
 * update Sent_when with as arguments, so that they (especially `Sent_when::m_sent_time`) could be determined
 * in thread W2' or W2'' but written thread-safely in W.  (There is no way some other thread W task would mess
 * with this area of Peer_socket::m_snd_flying_pkts_by_sent_when before the proposed mark_data_packet_sent()
 * was able to run; thread W had just decided to send that packet over wire in the first place; so there's no
 * reason to access it until ACK -- much later -- or some kind of socket-wide catastrophe.)  All that put
 * together I dub APPROACH 5.  Thus, APPROACH 1 + APPROACH 5 seems like the best idea of all, distilling all
 * the trade-offs into the the fastest yet close to simplest approach.
 *
 * @todo More uniform diagnostic logging: There is much diagnostic logging in the
 * implementation (FLOW_ERROR*(), etc.), but some of it lacks useful info like `sock` or `serv` (i.e., the
 * `ostream` representations of Peer_socket and Server_socket objects, which include the UDP/Flow endpoints
 * involved in each socket).  The messages that do include these have to do so explicitly.  Provide some
 * macros to automatically insert this info, then convert the code to use the macros in most places.  Note that
 * existing logging is rather exhaustive, so this is not the biggest of deals but would be nice for ease of coding
 * (and detailed logging).
 *
 * @todo It may be desirable to use not boost.asio's out-of-the-box UDP receive routines but rather extensions
 * capable of some advanced features, such as `recvmsg()` -- which can obtain kernel receipt time stamps and
 * destination IP address via the `cmsg` feature.  This would tie into various other to-dos listed around here.
 * There is, as of this writing, also a to-do in the top-level `flow` namespace doc header about bringing some code
 * into a new `io` namespace/Flow module; this includes the aforementioned `recvmsg()` wrapper.
 *
 * @todo It may be desirable to further use `recvmmsg()` for UDP input; this allows to read multiple UDP datagrams
 * with one call for performance.
 *
 * @todo By the same token, wrapping `sendmsg()` and `sendmmsg()` may allow for futher perf and feature
 * improvements -- in some ways potentially symmetrically to `recvmsg()` and `recvmmsg()` respectively.
 * However, as of this writing, I (ygoldfel) see this more of an opportunistic "look into it" thing and not something
 * of active value; whereas `recv[m]msg()` bring actual features we actively desire for practical reasons.
 *
 * @todo Send and Receive buffer max sizes:  These are set to some constants right now.  That's not
 * optimal.  There are two competing factors: throughput and RAM.  If buffer is too small, throughput can
 * suffer in practice, if the Receiver can't read the data fast enough (there are web pages that show this).
 * Possibly with today's CPUs it's no longer true, but I don't know.  If buffer is too large and with a lot of
 * users, a LOT of RAM can be eaten up (however note that a server will likely be mostly sending, not
 * receiving, therefore it may need smaller Receive buffers).  Therefore, as in Linux 2.6.17+, the buffer
 * sizes should be adaptively sized.  It may be non-trivial to come up with a good heuristic, but we may be
 * able to copy Linux.  The basic idea would probably be to use some total RAM budget and divide it up among
 * the # of sockets (itself a constant estimate, or adaptive based on the number of sockets at a given time?).
 * Also, buffer size should be determined on the Receive side; the Send side should make its buffer to be of
 * equal size. Until we implement some sensible buffer sizing, it might be a good idea (for demo purposes with
 * few users) to keep the buffers quite large.  However, flow control (receive window) is now implemented and
 * should cope well with momentary Receive buffer exhaustion.
 * Related facts found on the web: In Linux, since a long time ago, Send buffer size is determined by other
 * side's Receive buffer size (probably sent over in the SYN or SYN-ACK as the receive window).  Also, in
 * older Linuxes, Receive buffer defaults to 128k but can be manually set. Supposedly the default can lead to
 * low throughput in high-speed (gigabit+) situations.  Thus Linux 2.6.17+ apparently made the Receive buffer
 * size adaptive.
 *
 * @todo Drop Acknowledgments:  DCCP, a somewhat similar UDP-based protocol, uses the concept of
 * Data-Dropped acknowledgments.  If a packet gets to the receiver, but the receiver is forced to drop it (for
 * example, no Receive buffer space; not sure if there are other reasons in Flow protocol), then the sender will only
 * find out about this by inferring the drop via Drop Timeout or getting acknowledgments for later data.  That
 * may take a while, and since receiver-side drops can be quite large, it would be more constructive for the
 * receive to send an un-ACK of sorts: a Data-Dropped packet informing the sender that specific data were
 * dropped.  The sender can then update his congestion state (and retransmit if we enable that).  See RFC 4340
 * and 4341.
 *
 * @todo Add extra-thread-safe convention for setting options: We can provide a thread-safe (against other user
 * threads doing the same thing) macro to set a given option.  set_options() has a documented, albeit in
 * practice not usually truly problematic, thread safety flaw if one calls options(), modifies the result,
 * then calls set_options().  Since another thread may modify the Node's options between the two calls, the
 * latter call may unintentionally revert an option's value.  Macro would take an option "name" (identifier
 * for the Node_options member), a Node, and a target value for the option and would work together with a
 * helper method template to obtain the necessary lock, make the assignment to the internal option, and give
 * up the lock.  The implementation would probably require Node to expose its internal stored Node_options
 * for the benefit of this macro only.  Anyway, this feature is not super-important, as long as the user is
 * aware that modifying options from multiple threads simultaneously may result in incorrect settings being
 * applied.
 *
 * @todo The preceding to-do regarding Node_options applies to Peer_socket_options stored in Peer_socket in
 * in an analogous way.
 *
 * @todo Consider removing Flow ports and even Server_socket:
 * As explained above, we add the concept of a large set of available Flow ports within each
 * Node, and each Node itself has a UDP port all to itself.  So, for example, I could bind a Node to UDP
 * port 1010, and within that listen() on Flow ports 1010 (unrelated to UDP port 1010!) and 1011.  In
 * retrospect, though, is that complexity necessary?  We could save quite a few lines of code, particularly in
 * the implementation (class Port_space, for example) and the protocol (extra bytes for Flow source and target
 * ports, for example). (They're fun and pretty lines, but the absence of lines is arguably even prettier
 * albeit less fun.  On the other hand, bugs aren't fun, and more code implies a higher probability of bugs,
 * maintenance errors, etc.)  The interface would also be a bit simpler; and not just due to fewer items in
 * Remote_endpoint (which would in fact reduce to util::Udp_endpoint and cease to exist).  Consider Server_socket;
 * currently listen() takes a #flow_port_t argument and returns a Server_socket which is listening; calling
 * accept() (etc.) on the latter yields Peer_socket, as the other side connects.  Without Flow ports, there is
 * no argument to listen(); in fact, Server_socket itself is not strictly necessary and could be folded into
 * Node, with listen() becoming essentially something that turns on the "are we listening?" Boolean state,
 * while stop_listening() would turn it off (instead of something like `Server_socket::close()`).  (Important
 * note: That was not an endorsement of removing Server_socket.  Arguably it is still a nice abstraction.
 * Removing it would certainly remove some boiler-plate machinery to do with Server_socket's life cycle, on
 * the other hand.  Perhaps it's best to take a two-step appraoch; remove Flow ports first; then after a long
 * time, assuming it becomes clear that nothing like them is going to come back, remove Server_socket as
 * well.)  A key question is, of course what would we lose?  At first glance, Flow port allows multiple
 * connections on a single UDP-port-taking Flow server, including multiple connections from one client (e.g.,
 * with differing connection parameters such as reliability levels among the different connections, or
 * "channels")... but actually that'd still work without Flow ports, assuming the "one client's" multiple
 * connections can bind to different (presumably ephemeral) UDP ports; since the tuple (source host, source
 * UDP port) is still enough to distinguish from the 2+ "channels" of the same "client" connecting to the one
 * Flow Node (vs. today's tuple: source host, source UDP port, source Flow port, destination Flow port; see
 * `struct` Socket_id). However, without Flow ports, it is not possible for one Node to connect to another
 * Node twice, as each Node by definition is on one port.  Is this important?  Maybe, maybe not; for NAT
 * purposes it can be important to use only 1 port; but that typically applies only to the server, while the
 * client can send packets from anywhere.  However, gaming applications can be very demanding and for the most
 * restrictive NAT types might desire only a single port used on both sides.  So whether to remove Flow ports
 * is somewhat questionable, now that they exist; but arguably they didn't need to be added in the first
 * place, until they were truly needed.  I'd probably leave them alone, since they do exist.
 *
 * @todo Multi-core/multi-threading: The existing implementation already has a nice multi-threaded property,
 * namely that each Node (object that binds to a single UDP endpoint/port) is entirely independent of any other
 * such object -- they have entirely separate data, and each one does all its work on a separate thread.
 * So to make use of multiple cores/processors, one can set up multiple Node objects.  (Obviously this only makes
 * sense for apps where using multiple ports is acceptable or even desired.  E.g., a server could listen
 * on N different UDP ports, where N=# of cores.)  However, it would be nice for a single Node to be as
 * multi-core/processor-friendly as possible.  This is partially addressed by the "Dedicated thread to receive
 * UDP datagrams ASAP" to-do item elsewhere.  We could go further.   boost.asio lets one easily go from
 * 1 thread to multiple threads by simply starting more worker threads like W (W1, W2, ...) and executing
 * `m_task_engine::run()` in each one -- note that #m_task_engine is shared (sans lock).  Then subsequent
 * handlers (timer-fired handlers, ack-received handlers, data-received handlers, and many more) would be
 * assigned evenly to available threads currently executing run().  However, then all data these handlers
 * access would need to be protected by a mutex or mutexes, which would be a significant increase in
 * complexity and maintenance headaches compared to existing code, which features mutexes for data accessed
 * both by W and the user thread(s) U -- which excludes most Node, Server_socket, Peer_socket state --
 * essentially the user-visible "state" enums, and the Receive and Send buffers; but hugely complex things
 * like the scoreboards, etc. etc., needed no mutex protection, but with this change they would need it.
 * Actually, if the implementation essentially uses one mutex M, and every handler locks it for the entirety
 * of its execution, then one isn't really making use of multi-cores/etc. anyway.  One could make use of
 * boost.asio "strands" to avoid the need for the mutex -- just wrap every handler in a shared strand S,
 * and no locking is needed; boost.asio will never execute two handlers simultaneously in different threads.
 * While this would arguably make the code simpler, but in terms of performance it wouldn't make any
 * difference anyway, as it is functionally equivalent to the lock-M-around-every-operation solution (in
 * fact, internally, it might even amount to exactly that anyway).  So that's probably not worth it.
 * We need to have more mutexes or strands, based on some other criterion/criteria.  After a datagram is demuxed,
 * vast majority of work is done on a particular socket independently of all others.  Therefore we could
 * add a mutex (or use an equivalent) into the socket object and then lock on that mutex.  Multiple
 * threads could then concurrently handle multiple sockets.  However, for this to be used, one would have to
 * use a single Node (UDP endpoint) with multiple sockets at the same time.  Without any changes at all, one can
 * get the same concurrency by instead setting up multiple Node objects.  Other than a bit of lost syntactic sugar
 * (arguably) -- multiple Node objects needed, each one having to initialize with its own set of options,
 * for example -- this is particularly cost-free on the client side, as each Node can just use its own ephemeral
 * UDP port.  On the server side the network architecture has to allow for multiple non-ephemeral ports, and
 * the client must know to (perhaps randomly) connect to one of N UDP ports/endpoints on the server, which is
 * more restrictive than on the client.  So perhaps there are some reasons to add the per-socket concurrency -- but
 * I would not put a high priority on it.  IMPORTANT UPDATE: Long after the preceding text was written, flow::async
 * Flow module was created containing flow::async::Concurrent_task_loop interface.  That looks at the general
 * problem of multi-tasking thread pools and what's the user-friendliest-yet-most-powerful way of doing it.
 * While the preceding discussion in this to-do has been left unchanged, one must first familiarize self with
 * flow::async; and *then* read the above, both because some of those older ideas might need reevaluation; and because
 * some of those ideas may have been implemented by flow::async and are now available easily.
 *
 * @todo In Node::low_lvl_packet_sent(), the UDP `async_send()` handler, there is an inline to-do about specially
 * treating the corner case of the `would_block` and `try_again` boost.asio errors being reported (a/k/a POSIX
 * `EAGAIN`/`EWOULDBLOCK`).  Please see that inline comment for details.
 *
 * @todo Class Node `private` section is very large.  It's so large that the implementations of the methods therein
 * are split up among different files such as `flow_peer_socket.cpp`, `flow_event_set.cpp`, `flow_low_lvl_io.cpp`, etc.
 * Consider the following scheme to both better express this separation as well as enforce which of a given method
 * group's method(s) are meant to be called by outside code vs. being helpers thereof: Introduce `static`-method-only
 * inner classes (and, conceivably, even classes within those classes) to enforce this grouping (`public` methods
 * and `private` methods enforcing what is a "public" helper vs. a helper's helper).
 *
 * @todo Make use of flow::async::Concurrent_task_loop or flow::async::Single_thread_task_loop, instead of manually
 * setting up a thread and util::Task_engine, for #m_worker.  I, Yuri, wrote the constructor, worker_run(), destructor,
 * and related setup/teardown code as my very first boost.asio activity ever.  It's solid, but flow::async just makes
 * it easier and more elegant to read/maintain; plus this would increase Flow-wide consistency.  It would almost
 * certainly reduce the number of methods and, even nicely, state (such as #m_event_loop_ready).
 *
 * Misc. topic: Doxygen markup within a Doxygen command
 * ----------------------------------------------------
 * This section may seem random: Indeed, it is the meat of a similarly named ("Doxygen markup," etc.) subsection of
 * the doc header on the very-general namespace ::flow.  As noted in that subsection, this Node class is a more
 * convenient place to explain this, because it is a large class with many examples available.  Without further ado:
 *
 * ::flow doc header discusses which items should be accompanied by Doxygen comment snippets.  More specifically, each
 * item is accompanied by a Doxygen "command".  `"@param param_name Explain parameter here."`, for example, documents
 * the parameter `param_name` with the text following that.  (Some commands are implicit; namely without
 * an explicit `"@brief"`, the first sentence is the brief description of the class/function/whatever.)
 *
 * However, all that doesn't talk about formatting the *insides* of paragraphs in these commands.  Essentially
 * we are saying just use English.  However, Doxygen uses certain markup language conventions when interpreting
 * those paragraphs.  For example `backticks` will turn the thing inside the ticks into an inline code snippet,
 * in fixed-width font.  There are a few things to watch out for with this:
 *
 *   - Don't accidentally enable markup when you don't mean to.  E.g., an * asterisk as the first character
 *     in a paragraph will cause a bullet point to appear.  Also, sometimes you do want to
 *     use some character for semantical reason X which Doxygen shares with you, but automatic markup
 *     handling might make it come out a little wrong.  Just learn these through practice
 *     and check over the generated web page(s) before checking in the code.
 *   - DO use markup within reason for certain COMMON items.  Do not overdo it: mostly it should be things
 *     you're keen to do even if there were NO Doxygen or Javadoc involved.  Bullet point lists are an example.
 *     Basically:  If you were going to do something anyway, why not have it come out nicely in the doc page(s)?
 *   - Make use of auto-linking (a/k/a automatic liwnk generation) liberally.  This is when Doxygen sees a certain
 *     pattern within a Doxygen comment and understands it a reference to some other object, like a class or
 *     method; so in the output this will come out as a link to that thing.  The nice thing is that, usually,
 *     within raw code it looks fine/normal; AND the generated page has the convenient linking functionality.
 *     However, if enabling linking in a certain case is too tedious, feel free to not.
 *
 * That said, I will now list all of the pieces of markup that are allowed within comments inside Flow code.
 * Try not to add to this list without a very good reason.  Simplicity is a virtue.
 *
 *  - Bullet points: Just a dash after one indent level: `"  - Item text."`.  Nesting allowed.
 *    - Numbered points: just type out the numbers explicitly instead of auto-numbering: `" 2. Item text."`; not
 *      `"  -# Item text."`.  Yes, it leaves the numbering to you, but more importantly the raw comment remains
 *      readable, and you can refer to the numbers (e.g., "according to the condition in item 3" makes more sense
 *      when you can see a `3.` nearby).
 *  - Emphasis: Just one asterisk before and after the emphasized stuff: *word*, *multiple words*.  No "_underscores_"
 *    please.  In general try to avoid too much emphasis, as asterisks are common characters and can confuse
 *    Doxygen.  Plus, you shouldn't need to emphasize stuff THAT much.  Plus, feel free to use CAPITALS to emphasize
 *    instead.
 *  - Inline code snippets: Backticks.  `single_word`, `an_expression != other_expression * 2`.  Definitely use
 *    this liberally: it helps readability of BOTH raw code and looks delightful in the generated web page(s).
 *    However, note explanation below regarding how this relates to auto-linking.
 *    - Syntax-highlighted code spans: Use three tildes `"~~~"` to begin and end a code snippet.  This MUST be
 *      used for multi-line code snippets; and CAN be used instead of `backticks` for single-line code snippets.
 *      The output will be a separate paragraph, just like the raw code should be.  More precisely, the tildes
 *      and code should follow a single absolute indentation level:
 *
 *   ~~~
 *   if (some_condition) // Generated output will also be syntax-highlighted.
 *   {
 *     obj.call_it(arg1, "quote");
 *     return false;
 *   }
 *   ~~~
 *
 *  - Large heading in a long doc header: Use the format seen in this comment itself: Words, underlined by a
 *    row of dashes ("----", etc.) on the next line.  This results in a fairly large-fonted title.
 *  - Small heading is a long doc header: IF AND ONLY IF you need a sub-heading under a large heading
 *    (which would probably be in only quite long doc headers indeed), use the ### format.  Again use the
 *    format seen in this very doc header.  This results in a slightly large-fonted title (pretty close to
 *    normal).
 *    - Avoid any other levels of heading.  At that point things have become too complex.
 *  - Escape from Doxygen formatting: To ensure Doxygen interprets a bunch of characters literally, when you
 *    know there is danger of it applying unwanted formatting, surround it in quotes.  The quotes will
 *    be included in the output just like in the raw code; but anything inside quotes will be output verbatim
 *    even if full of Doxygen markup or commands.  For example, if I don't want a to-do item to begin in
 *    the middle of this paragraph, but I do want to refer to how a to-do is declared is Doxygen comments,
 *    I will surround it in quotes: To declare a to-do, use the `"@todo"` command.  Note that in that example
 *    I put `backticks` around the text to format the whole thing a certain way; any formatting already in
 *    effect will continue through the "quoted" text; but no formatting inside the "quotes" will go in effect.
 *    Plus, it looks appropriate in raw code.  Best of all worlds.
 *
 * The most tricky yet powerful technique to learn here is the interplay between auto-linking and `inline code
 * snippets`.  Before discussing that in some detail, note the auto-linking which is allowed in this source
 * code:
 *
 *   - Class/`struct`/union names are auto-linked: for example, just Peer_socket.  That's because every
 *     class/`struct`/union for us starts with a Capital letter.  Easy!
 *   - A method local to the class/`struct` being documented, like running() in this class Node which we are
 *     documenting right now, is auto-linked as written.
 *   - ANY member explicitly specified as belonging to a class/`struct`/union or namespace is
 *     auto-linked as written.  It can be a member function, variable, alias, nested class, or *anything* else.
 *     The presence of `"::"` will auto-link whatever it is.  Note this is a very powerful auto-linking technique;
 *     the vast majority of things are members of something, even if's merely a namespace, so if you absolutely must
 *     auto-link something, there is always at least one straightforward way: a *fully or partially qualified name*.
 *     It will be simple/readable as raw source and equally simple/readable AND linked in the doc output.  The only
 *     *possible* (not really probable, but it certainly happens) down-side is it can be too verbose.
 *     - Example: alias-type: Drop_timer::timer_wait_id_t; method: Port_space::return_port().
 *     - Macros are not members of anything and thus cannot be auto-linked by qualifying them.  However, read below
 *       on how to auto-link them too.
 *   - A free (non-member) function or functional macro will generally auto-link, even if it's in some other namespace.
 *     - This can result in name collisions, if some function `f()` is in two namespaces meaning two entirely
 *       different things.  And not just functions but anything else, like classes, can thus collide.
 *       That is, after all, why namespaces exist!  Just be careful and qualify things with namespace paths
 *       when needed (or even just for clarity).
 *   - For non-functions/methods: Things like variables/constants, type aliases, `enum` values will not auto-link
 *     if seen "naked."  For example S_PORT_ANY is, to Doxygen, just a word.  We use either `"#"` or `"::"` to force
 *     auto-linking.  Here is how to decide which one to use:
 *     - `"#id"`: To refer to a *member* of anything (compound type, namespace) member named `id`,
 *       such that the *currently documented item* is also a member of that [anything] -- either at the same depth
 *       (e.g., in the same class) or deeper (e.g., `id` is in namespace `flow`, while we are in namespace
 *       `flow::net_flow`, or in class `flow::Some_class`, or both -- `flow::net_flow::Some_class`).
 *       Example: #Udp_socket (member alias), #S_NUM_PORTS (member constant), #m_low_lvl_sock (member variable).
 *     - `"::id"`: To refer to an item in the global namespace.  Almost always, this will be an (outer) namespace.
 *       Global-namespace members that are not themselves namespaces are strongly discouraged elsewhere.
 *       Example: ::flow (namespace).
 *   - A functional macro is formatted the same as a free function in global namespace: e.g., FLOW_LOG_WARNING().
 *   - If a non-functional macro needs to be documented (VERY rare or non-existent given our coding style), use
 *     this special format: `"#MACRO_NAME"`.  `"::"` is inappropriate, since a macro does not belong to a namespace
 *     (global or otherwise), and that would look confusing in raw code.
 *
 * Now finally here are the guidelines about what to use: `backticks`, an auto-linked symbol, or both.
 * Suppose there is some comment *snippet* X that you are considering how to format.
 *
 *   - If X is just a piece of English language and not referring to or quoting code per se, then do not format
 *     it.  Type it verbatim: "The socket's state is ESTABLISHED here."  Even though ESTABLISHED may be thought
 *     of as code, here it's referring more to a concept (the state "ESTABLISHED") rather than code snippet.
 *     `S_ESTABLISHED` is a different story, on the other hand, and that one you must either backtick (as I just
 *     did there) or auto-link; read on for guidelines on that.
 *   - If the *entirety* of X is an identifier:
 *     - Auto-link it, WITHOUT backticks, if to auto-link it you would just write X verbatim anyway.
 *       For example, mentioning Peer_socket just like that will auto-link it.  So, that's great.  Do NOT
 *       add backticks, as that increases code verbosity and adds very little (making the auto-linked `Peer_socket`
 *       also use a fixed-width font; meh).
 *     - Auto-link it, WITHOUT backticks, if you would like the convenience of it being auto-linked in the output.
 *       - Do NOT auto-link it, but DO add `backticks`, if you do not need the convenience of the auto-linked output.
 *         Backticks are easy: auto-linking can be a little tricky/verbose.  So in that case just `backtick` it
 *         for readable raw source AND pretty output; without worrying about subtleties of proper auto-linking.
 *   - If X consists of some identifiers but also contains non-identifiers:
 *     - The non-code parts should be verbatim.
 *     - ALL code parts should be in `backticks`.
 *     - IF you want the convenience of some parts of the output being auto-linked, auto-link those parts.
 *       - IF you'd prefer shorter and clearer raw code, then don't auto-link where doing so would require extra
 *         raw code characters.
 *     - Example: Suppose X is: "The allowed range is [S_FIRST_SERVICE_PORT + 1, S_FIRST_EPHEMERAL_PORT + 2)."
 *       Variant 1 will auto-link but a bit longer and less readable as raw code.  Variant 2 will forego auto-linking
 *       but is short and readable as raw code.
 *       - *Variant 1*: The allowed range is [`#S_FIRST_SERVICE_PORT + 1`, `#S_FIRST_EPHEMERAL_PORT + 2`).
 *       - *Variant 2*: The allowed range is [`S_FIRST_SERVICE_PORT + 1`, `S_FIRST_EPHEMERAL_PORT + 2`).
 *     - Example: Suppose X is: "The condition holds if sock->m_local_port != 2223."  Variant 1 is brief and readable.
 *       Variant 2 is readable enough but much longer.  However, it will very conveniently auto-link to
 *       that obscure data member for the web page reader's pleasure, the convenience of which shouldn't be dismissed.
 *       - *Variant 1*: The condition holds if `sock->m_local_port != 2223`.
 *       - *Variant 2*: The condition holds if `Peer_socket::m_local_port != 2223` (for `sock`).
 */
class Node :
  public util::Null_interface,
  public log::Log_context,
  private boost::noncopyable
{
public:
  // Constants.

  /// Total number of Flow ports in the port space, including #S_PORT_ANY.
  static const size_t& S_NUM_PORTS;

  /// Total number of Flow "service" ports (ones that can be reserved by number with Node::listen()).
  static const size_t& S_NUM_SERVICE_PORTS;

  /**
   * Total number of Flow "ephemeral" ports (ones reserved locally at random with
   * `Node::listen(S_PORT_ANY)` or Node::connect()).
   */
  static const size_t& S_NUM_EPHEMERAL_PORTS;

  /**
   * The port number of the lowest service port, making the range of service ports
   * [#S_FIRST_SERVICE_PORT, #S_FIRST_SERVICE_PORT + #S_NUM_SERVICE_PORTS - 1].
   */
  static const flow_port_t& S_FIRST_SERVICE_PORT;

  /**
   * The port number of the lowest ephemeral Flow port, making the range of ephemeral ports
   * [#S_FIRST_EPHEMERAL_PORT, #S_FIRST_EPHEMERAL_PORT + #S_NUM_EPHEMERAL_PORTS - 1].
   */
  static const flow_port_t& S_FIRST_EPHEMERAL_PORT;

  // Constructors/destructor.

  /**
   * Constructs Node.
   * Post-condition: Node ready for arbitrary use.  (Internally this includes asynchronously
   * waiting for any incoming UDP packets on the given endpoint.)
   *
   * Does not block.  After exiting this constructor, running() can be used to determine whether
   * Node initialized or failed to do so; or one can get this from `*err_code`.
   *
   * ### Potential shared use of `Logger *logger` ###
   * All logging, both in this thread (from which the constructor executes) and any potential internally
   * spawned threads, by this Node and all objects created through it (directly
   * or otherwise) will be through this Logger.  `*logger` may have been used or not used
   * for any purpose whatsoever prior to this constructor call.  However, from now on,
   * Node will assume that `*logger` will be in exclusive use by this Node and no other code until
   * destruction.  It is strongly recommended that all code refrains from further use of
   * `*logger` until the destructor ~Node() exits.  Otherwise, quality of this Node's logging (until destruction)
   * may be lowered in undefined fashion except for the following formal guarantees: the output will not
   * be corrupted from unsafe concurrent logging; and the current thread's nickname (for logging purposes only) will
   * not be changed at any point.  Less formally, interleaved or concurrent use of the same Logger might
   * result in such things as formatters from Node log calls affecting output of your log calls or vice versa.
   * Just don't, and it'll look good.
   *
   * @param low_lvl_endpoint
   *        The UDP endpoint (IP address and UDP port) which will be used for receiving incoming and
   *        sending outgoing Flow traffic in this Node.
   *        E.g.: `Udp_endpoint(Ip_address_v4::any(), 1234)` // UDP port 1234 on all IPv4 interfaces.
   * @param logger
   *        The Logger implementation through which all logging from this Node will run.
   *        See notes on logger ownership above.
   * @param net_env_sim
   *        Network environment simulator to use to simulate (fake) external network conditions
   *        inside the code, e.g., for testing.  If 0, no such simulation will occur.  Otherwise the
   *        code will add conditions such as loss and latency (in addition to any present naturally)
   *        and will take ownership of the the passed in pointer (meaning, we will `delete` as we see fit;
   *        and you must never do so from now on).
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  error::Code generated:
   *        error::Code::S_NODE_NOT_RUNNING (Node failed to initialize),
   *        error::Code::S_OPTION_CHECK_FAILED.
   * @param opts
   *        The low-level per-Node options to use.  The default uses reasonable values that
   *        normally need not be changed.  No reference to opts is saved; it is only copied.
   *        See also Node::set_options(), Node::options(), Node::listen(), Node::connect(),
   *        Peer_socket::set_options(), Peer_socket::options().
   */
  explicit Node(log::Logger* logger, const util::Udp_endpoint& low_lvl_endpoint,
                Net_env_simulator* net_env_sim = 0, Error_code* err_code = 0,
                const Node_options& opts = Node_options());

  /**
   * Destroys Node.  Closes all Peer_socket objects as if by `sock->close_abruptly()`.  Then closes all
   * Server_socket objects  Then closes all Event_set objects as if by `event_set->close()`.
   * @todo Server_socket objects closed as if by what?
   *
   * Frees all resources except the objects still shared by `shared_ptr<>`s returned to the Node
   * user.  All `shared_ptr<>` instances inside Node sharing the latter objects are, however,
   * eliminated.  Therefore any such object will be deleted the moment the user also eliminates all
   * her `shared_ptr<>` instances sharing that same object; any object for which that is already the
   * case is deleted immediately.
   *
   * Does not block.
   *
   * Note: as a corollary of the fact this acts as if `{Peer|Server_}socket::close_abruptly()` and
   * Event_set::close(), in that order, were called, all event waits on the closed
   * sockets (`sync_send()`, `sync_receive()`, `sync_accept()`, Event_set::sync_wait(),
   * Event_set::async_wait()) will execute their on-event behavior (`sync_send()` return,
   * `sync_receive()` return, `sync_accept()` return, `sync_wait()` return and invoke handler, respectively).
   * Since Event_set::close() is executed soon after the sockets close, those Event_set objects are
   * cleared.  Therefore, the user on-event behavior handling may find that, despite a given
   * event firing, the containing Event_set is empty; or they may win the race and see an Event_set
   * with a bunch of `S_CLOSED` sockets.  Either way, no work is possible on these sockets.
   *
   * Rationale for previous paragraph: We want to wake up any threads or event loops waiting on
   * these sockets, so they don't sit around while the underlying Node is long since destroyed.  On
   * the other hand, we want to free any resources we own (including socket handles inside
   * Event_set).  This solution satisfies both desires.  It does add a bit of non-determinism
   * (easily handled by the user: any socket in the Event_set, even if user wins the race, will be
   * `S_CLOSED` anyway).  However it introduces no actual thread safety problems (corruption, etc.).
   *
   * @todo Provide another method to shut down everything gracefully?
   */
  ~Node() override;

  // Methods.

  /**
   * Returns `true` if and only if the Node is operating.  If not, all attempts to use this object or
   * any objects generated by this object (Peer_socket::Ptr, etc.) will result in error.
   * @return Ditto.
   */
  bool running() const;

  /**
   * Return the UDP endpoint (IP address and UDP port) which will be used for receiving incoming and
   * sending outgoing Flow traffic in this Node.  This is similar to to the value passed to the
   * Node constructor, except that it represents the actual bound address and port (e.g., if you
   * chose 0 as the port, the value returned here will contain the actual emphemeral port randomly chosen by
   * the OS).
   *
   * If `!running()`, this equals Udp_endpoint().  The logical value of the returned util::Udp_endpoint
   * never changes over the lifetime of the Node.
   *
   * @return See above.  Note that it is a reference.
   */
  const util::Udp_endpoint& local_low_lvl_endpoint() const;

  /**
   * Initiates an active connect to the specified remote Flow server.  Returns a safe pointer to a
   * new Peer_socket.  The socket's state will be some substate of `S_OPEN` at least initially.  The
   * connection operation, involving network traffic, will be performed asynchronously.
   *
   * One can treat the resulting socket as already connected; its Writable and Readable status can
   * be determined; once Readable or Writable one can receive or send, respectively.
   *
   * Port selection: An available local Flow port will be chosen and will be available for
   * information purposes via sock->local_port(), where `sock` is the returned socket.  The port will
   * be in the range [Node::S_FIRST_EPHEMERAL_PORT, Node::S_FIRST_EPHEMERAL_PORT +
   * Node::S_NUM_EPHEMERAL_PORTS - 1].  Note that there is no overlap between that range and the
   * range [Node::S_FIRST_SERVICE_PORT, Node::S_FIRST_SERVICE_PORT + Node::S_NUM_SERVICE_PORTS - 1].
   *
   * @param to
   *        The remote Flow port to which to connect.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  error::Code generated:
   *        error::Code::S_OUT_OF_PORTS, error::Code::S_INTERNAL_ERROR_PORT_COLLISION,
   *        error::Code::S_OPTION_CHECK_FAILED.
   * @param opts
   *        The low-level per-Peer_socket options to use in the new socket.
   *        If null (typical), the per-socket options template in Node::options() is used.
   *        If not null, the given per-socket options are first validated and, if valid, used.
   *        If invalid, it is an error.  See also Peer_socket::set_options(),
   *        Peer_socket::options().
   * @return Shared pointer to Peer_socket, which is in the `S_OPEN` main state; or null pointer,
   *         indicating an error.
   */
  Peer_socket::Ptr connect(const Remote_endpoint& to, Error_code* err_code = 0,
                           const Peer_socket_options* opts = 0);

  /**
   * Same as connect() but sends, as part of the connection handshake, the user-supplied metadata,
   * which the other side can access via Peer_socket::get_connect_metadata() after accepting the
   * connection.
   *
   * @note It is up to the user to serialize the metadata portably.  One recommended convention is to
   *       use `boost::endian::native_to_little()` (and similar) before connecting; and
   *       on the other side use the reverse (`boost::endian::little_to_native()`) before using the value.
   *       Packet dumps will show a flipped (little-endian) representation, while with most platforms the conversion
   *       will be a no-op at compile time.  Alternatively use `native_to_big()` and vice-versa.
   * @note Why provide this metadata facility?  After all, they could just send the data upon connection via
   *       send()/receive()/etc.  Answers: Firstly, this is guaranteed to be delivered (assuming successful
   *       connection), even if reliability (such as via retransmission) is disabled in socket options (opts
   *       argument).  For example, if a reliability mechanism (such as FEC) is built on top of the Flow layer,
   *       parameters having to do with configuring that reliability mechanism can be bootstrapped reliably
   *       using this mechanism.  Secondly, it can be quite convenient (albeit not irreplaceably so) for
   *       connection-authenticating techniques like security tokens known by both sides.
   * @param to
   *        See connect().
   * @param serialized_metadata
   *        Data copied and sent to the other side during the connection establishment.  The other side can get
   *        equal data using Peer_socket::get_connect_metadata().  The returned socket `sock` also stores it; it's
   *        similarly accessible via sock->get_connect_metadata() on this side.
   *        The metadata must fit into a single low-level packet; otherwise
   *        error::Code::S_CONN_METADATA_TOO_LARGE error is returned.
   * @param err_code
   *        See connect().  Added error: error::Code::S_CONN_METADATA_TOO_LARGE.
   * @param opts
   *        See connect().
   * @return See connect().
   */
  Peer_socket::Ptr connect_with_metadata(const Remote_endpoint& to,
                                         const boost::asio::const_buffer& serialized_metadata,
                                         Error_code* err_code = 0,
                                         const Peer_socket_options* opts = 0);

  /**
   * The blocking (synchronous) version of connect().  Acts just like connect() but instead of
   * returning a connecting socket immediately, waits until the initial handshake either succeeds or
   * fails, and then returns the socket or null, respectively.  Additionally, you can specify a
   * timeout; if the connection is not successful by this time, the connection attempt is aborted
   * and null is returned.
   *
   * Note that there is always a built-in Flow protocol connect timeout that is mandatory
   * and will report an error if it expires; but it may be too long for your purposes, so you can
   * specify your own that may expire before it.  The two timeouts should be thought of as fundamentally
   * independent (built-in one is in the lower level of Flow protocol; the one you provide is at the application
   * layer), so don't make assumptions about Flow's behavior and set a timeout if you know you need one -- even
   * if in practice it is longer than the Flow one (which as of this writing can be controlled via socket option).
   *
   * The following are the possible outcomes:
   *   1. Connection succeeds before the given timeout expires (or succeeds, if no timeout given).
   *      Socket is at least Writable at time of return.  The new socket is returned, no error is
   *      returned via `*err_code`.
   *   2. Connection fails before the given timeout expires (or fails, if no timeout given).  null
   *      is returned, `*err_code` is set to reason for connection failure.  (Note that a built-in
   *      handshake timeout -- NOT the given user timeout, if any -- falls under this category.)
   *      `*err_code == error::Code::S_WAIT_INTERRUPTED` means the wait was interrupted (similarly to POSIX's `EINTR`).
   *   3. A user timeout is given, and the connection does not succeed before it expires.  null is
   *      returned, and `*err_code` is set to error::Code::S_WAIT_USER_TIMEOUT.
   *      (Rationale: consistent with Server_socket::sync_accept(),
   *      Peer_socket::sync_receive(), Peer_socket::sync_send() behavior.)
   *
   * Tip: Typical types you might use for `max_wait`: `boost::chrono::milliseconds`,
   * `boost::chrono::seconds`, `boost::chrono::high_resolution_clock::duration`.
   *
   * @tparam Rep
   *         See `boost::chrono::duration` documentation (and see above tip).
   * @tparam Period
   *         See `boost::chrono::duration` documentation (and see above tip).
   * @param to
   *        See connect().
   * @param max_wait
   *        The maximum amount of time from now to wait before giving up on the wait and returning.
   *        `"duration<Rep, Period>::max()"` will eliminate the time limit and cause indefinite wait
   *        -- however, not really, as there is a built-in connection timeout that will expire.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  error::Code generated:
   *        error::Code::S_WAIT_INTERRUPTED, error::Code::S_WAIT_USER_TIMEOUT, error::Code::S_NODE_NOT_RUNNING,
   *        error::Code::S_CANNOT_CONNECT_TO_IP_ANY, error::Code::S_OUT_OF_PORTS,
   *        error::Code::S_INTERNAL_ERROR_PORT_COLLISION,
   *        error::Code::S_CONN_TIMEOUT, error::Code::S_CONN_REFUSED,
   *        error::Code::S_CONN_RESET_BY_OTHER_SIDE, error::Code::S_NODE_SHUTTING_DOWN,
   *        error::Code::S_OPTION_CHECK_FAILED.
   * @param opts
   *        See connect().
   * @return See connect().
   */
  template<typename Rep, typename Period>
  Peer_socket::Ptr sync_connect(const Remote_endpoint& to, const boost::chrono::duration<Rep, Period>& max_wait,
                                Error_code* err_code = 0,
                                const Peer_socket_options* opts = 0);

  /**
   * A combination of sync_connect() and connect_with_metadata() (blocking connect, with supplied
   * metadata).
   *
   * @param to
   *        See sync_connect().
   * @param max_wait
   *        See sync_connect().
   * @param serialized_metadata
   *        See connect_with_metadata().
   * @param err_code
   *        See sync_connect().  Added error: error::Code::S_CONN_METADATA_TOO_LARGE.
   * @param opts
   *        See sync_connect().
   * @return See sync_connect().
   */
  template<typename Rep, typename Period>
  Peer_socket::Ptr sync_connect_with_metadata(const Remote_endpoint& to,
                                              const boost::chrono::duration<Rep, Period>& max_wait,
                                              const boost::asio::const_buffer& serialized_metadata,
                                              Error_code* err_code = 0,
                                              const Peer_socket_options* opts = 0);

  /**
   * Equivalent to `sync_connect(to, duration::max(), err_code, opt)s`; i.e., sync_connect() with no user
   * timeout.
   *
   * @param to
   *        See other sync_connect().
   * @param err_code
   *        See other sync_connect().
   * @param opts
   *        See sync_connect().
   * @return See other sync_connect().
   */
  Peer_socket::Ptr sync_connect(const Remote_endpoint& to, Error_code* err_code = 0,
                                const Peer_socket_options* opts = 0);

  /**
   * Equivalent to `sync_connect_with_metadata(to, duration::max(), serialized_metadata, err_code, opts)`; i.e.,
   * sync_connect_with_metadata() with no user timeout.
   *
   * @param to
   *        See sync_connect().
   * @param serialized_metadata
   *        See connect_with_metadata().
   * @param err_code
   *        See sync_connect().  Added error: error::Code::S_CONN_METADATA_TOO_LARGE.
   * @param opts
   *        See sync_connect().
   * @return See sync_connect().
   */
  Peer_socket::Ptr sync_connect_with_metadata(const Remote_endpoint& to,
                                              const boost::asio::const_buffer& serialized_metadata,
                                              Error_code* err_code = 0,
                                              const Peer_socket_options* opts = 0);

  /**
   * Sets up a server on the given local Flow port and returns Server_socket which can be used to
   * accept subsequent incoming connections to this server.  Any subsequent incoming connections
   * will be established asynchronously and, once established, can be claimed (as Peer_socket
   * objects) via Server_server::accept() and friends.
   *
   * Port specification: You must select a port in the range [Node::S_FIRST_SERVICE_PORT,
   * Node::S_FIRST_SERVICE_PORT + Node::S_NUM_SERVICE_PORTS - 1] or the special value #S_PORT_ANY.
   * In the latter case an available port in the range [Node::S_FIRST_EPHEMERAL_PORT,
   * Node::S_FIRST_EPHEMERAL_PORT + Node::S_NUM_EPHEMERAL_PORTS - 1] will be chosen for you.
   * Otherwise we will use the port you explicitly specified.
   *
   * Note that using #S_PORT_ANY in this context typically makes sense only if you somehow
   * communicate `serv->local_port()` (where `serv` is the returned socket) to the other side through
   * some other means (for example if both client and server are running in the same program, you
   * could just pass it via variable or function call).  Note that there is no overlap between the
   * two aforementioned port ranges.
   *
   * @param local_port
   *        The local Flow port to which to bind.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  error::Code generated:
   *        error::Code::S_NODE_NOT_RUNNING, error::Code::S_PORT_TAKEN,
   *        error::Code::S_OUT_OF_PORTS, error::Code::S_INVALID_SERVICE_PORT_NUMBER,
   *        error::Code::S_INTERNAL_ERROR_PORT_COLLISION.
   * @param child_sock_opts
   *        If null, any Peer_sockets that `serv->accept()` may return (where `serv` is the returned
   *        Server_socket) will be initialized with the options set equal to
   *        `options().m_dyn_sock_opts`.  If not null, they will be initialized with a copy of
   *        `*child_sock_opts`.  No reference to `*child_sock_opts` is saved.
   * @return Shared pointer to Server_socket, which is in the Server_socket::State::S_LISTENING state at least
   *         initially; or null pointer, indicating an error.
   */
  Server_socket::Ptr listen(flow_port_t local_port, Error_code* err_code = 0,
                            const Peer_socket_options* child_sock_opts = 0);

  /**
   * Creates a new Event_set in Event_set::State::S_INACTIVE state with no sockets/events stored; returns this
   * Event_set.
   *
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  error::Code generated:
   *        error::Code::S_NODE_NOT_RUNNING.
   * @return Shared pointer to Event_set; or null pointer, indicating an error.
   */
  Event_set::Ptr event_set_create(Error_code* err_code = 0);

  /**
   * Interrupts any blocking operation, a/k/a wait, and informs the invoker of that operation that the
   * blocking operation's outcome was being interrupted.  Conceptually, this causes a similar fate as a POSIX
   * blocking function exiting with -1/`EINTR`, for all such functions currently executing.  This may be called
   * from any thread whatsoever and, particularly, from signal handlers as well.
   *
   * Before deciding to call this explicitly from signal handler(s), consider using the simpler
   * Node_options::m_st_capture_interrupt_signals_internally instead.
   *
   * The above is vague about how an interrupted "wait" exhibits itself.  More specifically, then:
   * Any operation with name `sync_...()` will return with an error, that error being
   * #Error_code error::Code::S_WAIT_INTERRUPTED.  Event_set::async_wait()-initiated wait will end, with the handler
   * function being called, passing the Boolean value `true` to that function.  `true` indicates the wait was
   * interrupted rather than successfully finishing with 1 or more active events (`false` would've indicated th
   * latter, more typical situation).
   *
   * Note that various calsses have `sync_...()` operations, including Node (Node::sync_connect()),
   * Server_socket (Server_socket::sync_accept()), and Peer_socket (Peer_socket::sync_receive()).
   *
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  error::Code generated:
   *        error::Code::S_NODE_NOT_RUNNING.
   */
  void interrupt_all_waits(Error_code* err_code = 0);

  /**
   * Dynamically replaces the current options set (options()) with the given options set.
   * Only those members of `opts` designated as dynamic (as opposed to static) may be different
   * between options() and `opts`.  If this is violated, it is an error, and no options are changed.
   *
   * Typically one would acquire a copy of the existing options set via options(), modify the
   * desired dynamic data members of that copy, and then apply that copy back by calling
   * set_options().  Warning: this technique is only safe if other (user) threads do not call
   * set_options() simultaneously.  There is a to-do to provide a thread-safe maneuver for when this is
   * a problem (see class Node doc header).
   *
   * @param opts
   *        The new options to apply to this socket.  It is copied; no reference is saved.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  error::Code generated:
   *        error::Code::S_OPTION_CHECK_FAILED, error::Code::S_NODE_NOT_RUNNING.
   * @return `true` on success, `false` on error.
   */
  bool set_options(const Node_options& opts, Error_code* err_code = 0);

  /**
   * Copies this Node's option set and returns that copy.  If you intend to use set_options() to
   * modify a Node's options, we recommend you make the modifications on the copy returned by
   * options().
   *
   * @return Ditto.
   */
  Node_options options() const;

  /**
   * The maximum number of bytes of user data per received or sent block on connections generated
   * from this Node, unless this value is overridden in the Peer_socket_options argument to
   * listen() or connect() (or friend).  See Peer_socket_options::m_st_max_block_size.
   *
   * @return Ditto.
   */
  size_t max_block_size() const;

protected:

  // Methods.

  // Basic setup/teardown/driver/general methods.

  /**
   * Returns a raw pointer to newly created Peer_socket or sub-instance like asio::Peer_socket, depending on
   * the template parameter.
   *
   * @tparam Peer_socket_impl_type
   *         Either net_flow::Peer_socket or net_flow::asio::Peer_socket, as of this writing.
   * @param opts
   *        See, for example, `Peer_socket::connect(..., const Peer_socket_options&)`.
   * @return Pointer to new object of type Peer_socket or of a subclass.
   */
  template<typename Peer_socket_impl_type>
  Peer_socket* sock_create_forward_plus_ctor_args(const Peer_socket_options& opts);

  /**
   * Like sock_create_forward_plus_ctor_args() but for Server_sockets.
   *
   * @tparam Server_socket_impl_type
   *         Either net_flow::Server_socket or net_flow::asio::Server_socket, as of this writing.
   * @param child_sock_opts
   *        See, for example, `Peer_socket::accept(..., const Peer_socket_options* child_sock_opts)`
   * @return Pointer to new object of type Server_socket or of a subclass.
   */
  template<typename Server_socket_impl_type>
  Server_socket* serv_create_forward_plus_ctor_args(const Peer_socket_options* child_sock_opts);

  // Constants.

  /**
   * Type and value to supply as user-supplied metadata in SYN, if user chooses to use
   * `[[a]sync_]connect()` instead of `[[a]sync_]connect_with_metadata()`.  If you change this value, please
   * update Peer_socket::get_connect_metadata() doc header.
   */
  static const uint8_t S_DEFAULT_CONN_METADATA;

private:
  // Friends.

  /**
   * Peer_socket must be able to forward `send()`, `receive()`, etc. to Node.
   * @see Peer_socket.
   */
  friend class Peer_socket;
  /**
   * Server_socket must be able to forward `accept()`, etc. to Node.
   * @see Server_socket.
   */
  friend class Server_socket;
  /**
   * Event_set must be able to forward `close()`, `event_set_async_wait()`, etc. to Node.
   * @see Event_set.
   */
  friend class Event_set;

  // Types.

  /// Short-hand for UDP socket.
  using Udp_socket = boost::asio::ip::udp::socket;

  /// boost.asio timer wrapped in a ref-counted pointer.
  using Timer_ptr = boost::shared_ptr<util::Timer>;

  /// Short-hand for a signal set.
  using Signal_set = boost::asio::signal_set;

  /// Short-hand for high-performance, non-reentrant, exclusive mutex used to lock #m_opts.
  using Options_mutex = Peer_socket::Options_mutex;

  /// Short-hand for lock that acquires exclusive access to an #Options_mutex.
  using Options_lock = Peer_socket::Options_lock;

  struct Socket_id;
  // Friend of Node: For ability to reference private `struct` Node::Socket_id.
  friend size_t hash_value(const Socket_id& socket_id);
  friend bool operator==(const Socket_id& lhs, const Socket_id& rhs);

  /**
   * A map from the connection ID (= remote-local socket pair) to the local Peer_socket that is
   * the local portion of the connection.  Applies to peer-to-peer (not server) sockets.
   */
  using Socket_id_to_socket_map = boost::unordered_map<Socket_id, Peer_socket::Ptr>;

  /// A map from the local Flow port to the local Server_socket listening on that port.
  using Port_to_server_map = boost::unordered_map<flow_port_t, Server_socket::Ptr>;

  /// A set of Event_set objects.
  using Event_sets = boost::unordered_set<Event_set::Ptr>;

  // Methods.

  // Basic setup/teardown/driver/general methods.

  /**
   * Worker thread W (main event loop) body.  Does not exit unless told to do so by Node's
   * destruction (presumably from a non-W thread, as W is not exposed to Node user).
   *
   * @param low_lvl_endpoint
   *        See that parameter on Node constructor.  Intentionally passed by value, to
   *        avoid race with user's Udp_endpoint object disappearing before worker_run() can
   *        use it.
   */
  void worker_run(const util::Udp_endpoint low_lvl_endpoint);

  /**
   * Helper to invoke for each thread in which this Node executes, whether or not it starts that thread,
   * that applies certain common settings to all subsequent logging from that thread.
   *
   * E.g., it might nickname the thread (w/r/t logging) and set a certain style of printing duration units (short
   * like "ms" or long like "milliseconds"): these probably won't change for the rest of the Node's logging.
   *
   * @param thread_type
   *        Roughly 3-letter character sequence identifying the thread's purpose, to be included in the thread's logged
   *        nickname in subsequent log message prefixes; or empty string to let the thread's nickname stay as-is.
   * @param logger
   *        The Logger whose logging to configure(); or null to assume `this->get_logger()` (which is typical but may
   *        not yet be available, say, during object construction).
   * @return Address of the Logger that was configured (either `logger` or `this->get_logger()`).
   */
  log::Logger* this_thread_init_logger_setup(const std::string& thread_type, log::Logger* logger = 0);

  /**
   * Given a new set of Node_options intended to replace (or initialize) a Node's #m_opts, ensures
   * that these new option values are legal.  In all cases, values are checked for individual and
   * mutual validity.  Additionally, unless init is true, which means we're being called from
   * constructor, ensures that no `static` data member is different between #m_opts and opts.  If any
   * validation fails, it is an error.
   *
   * Pre-condition: If `!init`, #m_opts_mutex is locked.
   *
   * @todo Is it necessary to return `opts` now that we've switched to C++11 or better?
   *
   * @param opts
   *        New option values to validate.
   * @param init
   *        True if called from constructor; false if called from set_options().
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  error::Code generated:
   *        error::Code::S_OPTION_CHECK_FAILED, error::Code::S_STATIC_OPTION_CHANGED.
   * @return `opts`.  The only reason we return this is so that it can be called during the
   *         construction's initializer section (go, C++03!).
   */
  const Node_options& validate_options(const Node_options& opts, bool init, Error_code* err_code) const;

  /**
   * Helper that compares `new_val` to `old_val` and, if they are not equal, logs and returns an error;
   * used to ensure static options are not changed.
   *
   * @tparam Opt_type
   *         Type of a Node_options, etc., data member.
   * @param new_val
   *        Proposed new value for the option.
   * @param old_val
   *        Current value of the option.
   * @param opt_id
   *        The name of the option, suitable for logging; this is presumably obtained using the
   *        macro `#` technique.
   * @param err_code
   *        See Peer_socket::set_options().
   * @return `true` on success, `false` on validation error.
   */
  template<typename Opt_type>
  bool validate_static_option(const Opt_type& new_val, const Opt_type& old_val, const std::string& opt_id,
                              Error_code* err_code) const;

  /**
   * Helper that, if the given condition is false, logs and returns an error; used to check for
   * option value validity when setting options.
   *
   * @param check
   *        `false` if and only if some validity check failed.
   * @param check_str
   *        String describing which condition was checked; this is presumably obtained using the
   *        macro # technique.
   * @param err_code
   *        See Peer_socket::set_options().
   * @return `true` on success, `false` on validation error.
   */
  bool validate_option_check(bool check, const std::string& check_str, Error_code* err_code) const;

  /**
   * Obtain a copy of the value of a given option in a thread-safe manner.  Because #m_opts may be
   * modified at any time -- even if the desired option is static and not being modified, this is
   * still unsafe -- #m_opts must be locked, the desired value must be copied, and #m_opts must be
   * unlocked.  This method does so.
   *
   * Do NOT read option values without opt().
   *
   * @tparam Opt_type
   *         The type of the option data member.
   * @param opt_val_ref
   *        A reference (important!) to the value you want; this may be either a data member of
   *        `this->m_opts` or the entire `this->m_opts` itself.
   * @return A copy of the value at the given reference.
   */
  template<typename Opt_type>
  Opt_type opt(const Opt_type& opt_val_ref) const;

  /**
   * Performs low-priority tasks that should be run on an infrequent, regular basis, such as stat
   * logging and schedules the next time this should happen.  This is the timer handler for that
   * timer.
   *
   * @param reschedule
   *        If `true`, after completing the tasks, the timer is scheduled to run again later;
   *        otherwise it is not.
   */
  void perform_regular_infrequent_tasks(bool reschedule);

  /* Methods dealing with low-level packet I/O.  Implementations are in low_lvl_io.cpp.  The
   * line between these methods and the ones further down (like handle_incoming())) is blurred, but basically the
   * latter methods deal with each incoming packet after it has been read off wire and gone through
   * the network simulator (if any is active).  By contrast the methods just below
   * (low_lvl_io.cpp) deal with receiving and sending low-level packets (including packet
   * pacing) and network condition simulation (if active) -- basically leaving the core protocol
   * logic to the aforementioned core logic methods. */

  // Input.

  /**
   * Registers so that during the current or next `m_task_engine.run()`, the latter will wait for a receivable UDP
   * packet and, when one is available, will call low_lvl_recv_and_handle().
   *
   * Pre-condition: we are in thread W.
   */
  void async_low_lvl_recv();

  /**
   * Handles the pre-condition that #m_low_lvl_sock has a UDP packet available for reading, or that there
   * was an error in waiting for this pre-condition.  If no error (`!sys_err_code`) then the packet is read
   * (thus erased) from the OS UDP net-stack's packet queue.  The packet is then properly handled (for
   * example it may result in more data decoded into an appropriate Peer_socket's stream buffer).
   *
   * @param sys_err_code
   *        Error code of the operation.
   */
  void low_lvl_recv_and_handle(Error_code sys_err_code);

  /**
   * Helper for low_lvl_recv_and_handle() that calls handle_incoming() on the not-yet-deserialized low-level
   * packet just read off the UDP socket, but first handles simulation of various network conditions
   * like latency, loss, and duplication.  Pre-condition is that a UDP receive just successfully
   * got the data, or that a simulation thereof occurred.
   *
   * @param packet_data
   *        See handle_incoming().  Note that, despite this method possibly acting asynchronously (e.g.,
   *        if simulating latency), `*packet_data` ownership is retained by the immediate caller.
   *        Caller must not assume anything about its contents upon return and is free to do anything else to it
   *        (e.g., read another datagram into it).
   * @param low_lvl_remote_endpoint
   *        See handle_incoming().
   * @param is_sim_duplicate_packet
   *        `false` if `packet_data` contains data actually just read from UDP socket.
   *        `true` if `packet_data` contains data placed there as a simulated duplicate packet.
   *        The latter is used to prevent that simulated duplicated packet from itself getting
   *        duplicated or dropped.
   * @return The number of times handle_incoming() was called *within* this call (before this call
   *         returned); i.e., the number of packets (e.g., packet and/or its duplicate) handled
   *         immediately as opposed to dropped or scheduled to be handled later.
   */
  unsigned int handle_incoming_with_simulation(util::Blob* packet_data,
                                               const util::Udp_endpoint& low_lvl_remote_endpoint,
                                               bool is_sim_duplicate_packet = false);

  /**
   * Sets up `handle_incoming(packet_data, low_lvl_remote_endpoint)` to be called asynchronously after a
   * specified period of time.  Used to simulate latency.
   *
   * @param latency
   *        After how long to call handle_incoming().
   * @param packet_data
   *        See handle_incoming_with_simulation().
   * @param low_lvl_remote_endpoint
   *        See handle_incoming_with_simulation().
   */
  void async_wait_latency_then_handle_incoming(const Fine_duration& latency,
                                               util::Blob* packet_data,
                                               const util::Udp_endpoint& low_lvl_remote_endpoint);

  // Output.

  /**
   * async_low_lvl_packet_send_impl() wrapper to call when `packet` is to be sent to the remote side of
   * the connection `sock`.  In particular, this records certain per-socket stats accordingly.
   *
   * @param sock
   *        Socket whose remote side to target when sending.
   * @param packet
   *        See async_low_lvl_packet_send_impl().
   * @param delayed_by_pacing
   *        See async_low_lvl_packet_send_impl().
   */
  void async_sock_low_lvl_packet_send(Peer_socket::Ptr sock, Low_lvl_packet::Const_ptr&& packet,
                                      bool delayed_by_pacing);

  /**
   * async_low_lvl_packet_send_impl() wrapper to call when `packet` is to be sent to the remote side of
   * the connection `sock`.  In particular, this records certain per-socket stats accordingly.
   *
   * @param low_lvl_remote_endpoint
   *        UDP endpoint for the Node to which to send the packet.
   * @param packet
   *        See async_low_lvl_packet_send_impl().
   */
  void async_no_sock_low_lvl_packet_send(const util::Udp_endpoint& low_lvl_remote_endpoint,
                                         Low_lvl_packet::Const_ptr packet);

  /**
   * Takes given low-level packet structure, serializes it, and initiates
   * asynchronous send of these data to the remote Node specified by the given UDP endpoint.
   * The local and target ports are assumed to be already filled out in `*packet`.
   * Once the send is possible (i.e., UDP net-stack is able to buffer it for sending; or there is an
   * error), low_lvl_packet_sent() is called (asynchronously).
   *
   * Takes ownership of `packet`; do not reference it in any way after this method returns.
   *
   * @note This method exiting in no way indicates the send succeeded (indeed,
   *       the send cannot possibly initiate until this method exits).
   *
   * @param low_lvl_remote_endpoint
   *        UDP endpoint for the Node to which to send the packet.
   * @param packet
   *        Pointer to packet structure with everything filled out as desired.
   * @param delayed_by_pacing
   *        `true` if there was a (pacing-related) delay between when higher-level code decided to send this packet
   *        and the execution of this method; `false` if there was not, meaning said higher-level code executed us
   *        immediately (synchronously), though not necessarily via a direct call (in fact that's unlikely; hence
   *        `_impl` in the name).
   * @param sock
   *        Peer_socket associated with this connection; null pointer if none is so associated.
   *        If not null, behavior undefined unless `low_lvl_remote_endpoint == sock->remote_endpoint().m_udp_endpoint`.
   */
  void async_low_lvl_packet_send_impl(const util::Udp_endpoint& low_lvl_remote_endpoint,
                                      Low_lvl_packet::Const_ptr packet, bool delayed_by_pacing, Peer_socket::Ptr sock);

  /**
   * Completion handler for async_low_lvl_packet_send_impl(); called when the packet is either
   * successfully fed to the UDP net-stack for sending, or when there is an error in doing so.
   *
   * @warning It is important to pass `packet` to this, because the serialization operation produces
   *          a bunch of pointers into `*packet`; if one does not pass it here through the
   *          boost.asio send call, `*packet` might get deleted, and then send op will try to access
   *          pointer(s) to invalid memory.
   * @param packet
   *        Ref-counted pointer to the packet that was hopefully sent.
   *        Will be destroyed at the end of low_lvl_packet_sent() unless a copy of this pointer is
   *        saved elsewhere before that point.  (Usually you should indeed let it be destroyed.)
   * @param sock
   *        See async_low_lvl_packet_send_impl().  Note the null pointer is allowed.
   * @param bytes_expected_transferred
   *        Size of the serialization of `*packet`, that being the total # of bytes we want sent
   *        over UDP.
   * @param sys_err_code
   *        Result of UDP send operation.
   * @param bytes_transferred
   *        Number of bytes transferred assuming `!err_code`.
   *        Presumably that would equal `bytes_expected_transferred`, but we will see.
   */
  void low_lvl_packet_sent(Peer_socket::Ptr sock, Low_lvl_packet::Const_ptr packet, size_t bytes_expected_transferred,
                           const Error_code& sys_err_code, size_t bytes_transferred);

  /**
   * Performs important book-keeping based on the event "DATA packet was sent to destination."
   * The affected data structures are: Sent_packet::m_sent_when (for the Sent_packet in question),
   * Peer_socket::m_snd_last_data_sent_when, Drop_timer Peer_socket::m_snd_drop_timer (in `*sock`).
   * sock->m_snd_drop_timer.  More information is in the doc headers for
   * those data members.
   *
   * @param sock
   *        Socket for which the given DATA packet is sent.
   * @param seq_num
   *        The first sequence number for the sent DATA packet.
   *        Sent_packet::m_sent_when for its Sent_packet should contain the time at which send_worker() removed
   *        the data from Send buffer and packetized it; it's used to log the difference between
   *        that time and now.
   */
  void mark_data_packet_sent(Peer_socket::Ptr sock, const Sequence_number& seq_num);

  /**
   * Sends an RST to the given UDP endpoint in response to the given incoming low-level packet that
   * came from that endpoint, when there is no associated Peer_socket for that remote endpoint/local port combo.
   * An error is unlikely, but if it happens there is no reporting other than logging.
   *
   * You should use this to reply with an RST in situations where no Peer_socket is applicable; for
   * example if anything but a SYN or RST is sent to a server port.  In situations where a
   * Peer_socket is applicable (which is most of the time an RST is needed), use
   * async_sock_low_lvl_rst_send().
   *
   * @param causing_packet
   *        Packet we're responding to (used at least to set the source and destination Flow ports
   *        of the sent packet).
   * @param low_lvl_remote_endpoint
   *        Where `causing_packet` came from (the Node low-level endpoint).
   */
  void async_no_sock_low_lvl_rst_send(Low_lvl_packet::Const_ptr causing_packet,
                                      const util::Udp_endpoint& low_lvl_remote_endpoint);

  /**
   * Begins the process of asynchronously sending the given low-level packet to the remote Node
   * specified by the given Peer_socket.  The method, if this feature is applicable and enabled,
   * applies packet pacing (which attempts to avoid burstiness by spreading out packets without
   * changing overall sending rate).  Therefore the given packet may be sent as soon as a UDP send
   * is possible according to OS (which is typically immediate), or later, if pacing delays it.  Once it is
   * time to send it, async_sock_low_lvl_packet_send() is used.
   *
   * Takes ownership of packet; do not reference it in any way after this method returns.
   *
   * Note that an error may occur in asynchronous operations triggered by this method; if this
   * happens the socket will be closed via close_connection_immediately().
   *
   * @param sock
   *        Socket whose `remote_endpoint()` specifies to what Node and what Flow port within that
   *        Node this socket will go.
   * @param packet
   *        Pointer to packet structure with everything except the source, destination, and
   *        retransmission mode fields (essentially, the public members of Low_lvl_packet proper but
   *        not its derived types) filled out as desired.
   */
  void async_sock_low_lvl_packet_send_paced(const Peer_socket::Ptr& sock,
                                            Low_lvl_packet::Ptr&& packet);

  /**
   * async_sock_low_lvl_packet_send_paced() pacing helper: Handles a DATA or ACK packet that was just
   * passed into async_sock_low_lvl_packet_send_paced(), i.e., is available for sending.  That is, either
   * sends the packet via async_sock_low_lvl_packet_send() immediately or queues it for sending later.
   *
   * Pre-conditions: pacing is enabled for the socket in options; an SRTT value has been computed
   * (is not undefined); packet is DATA or ACK; packet is fully filled out; `sock` is in OPEN state;
   * invariants described for `struct` Send_pacing_data hold.
   *
   * Note that an error may occur in asynchronous operations triggered by this method; if this
   * happens the socket will be closed via close_connection_immediately().
   *
   * Takes ownership of packet; do not reference it in any way after this method returns.
   *
   * @param sock
   *        Socket under consideration.
   * @param packet
   *        Packet to send.
   */
  void sock_pacing_new_packet_ready(Peer_socket::Ptr sock, Low_lvl_packet::Ptr packet);

  /**
   * async_sock_low_lvl_packet_send_paced() pacing helper: Resets the socket's Send_pacing_data structure
   * to reflect that a new pacing time slice should begin right now.  The slice start is set to now,
   * its period is set based on the current SRTT and congestion window (so that packets are evenly
   * spread out over the next SRTT); and the number of full packets allowed over this time slice are
   * computed.
   *
   * Pre-conditions: pacing is enabled for the socket in options; an SRTT value has been computed
   * (is not undefined); `sock` is in OPEN state; invariants described for `struct` Send_pacing_data
   * hold.
   *
   * @see `struct` Send_pacing_data doc header.
   * @param sock
   *        Socket under consideration.  Should be in OPEN state.
   * @param now
   *        For performance (so that we don't need to acquire the current time again), this is the
   *        very recent time point at which it was determined it is time for a new pacing time slice.
   */
  void sock_pacing_new_time_slice(Peer_socket::Ptr sock, const Fine_time_pt& now);

  /**
   * async_sock_low_lvl_packet_send_paced() pacing helper: Given that we are currently in the pacing time
   * slice in `sock->m_snd_pacing_data`, sends as many queued packets as possible given the time
   * slice's budget, and if any remain queued after this, schedules for them to be sent in the next
   * time slice.
   *
   * Pre-conditions: pacing is enabled for the socket in options; an SRTT value has been computed
   * (is not undefined); `sock` is in OPEN state; invariants described for `struct` Send_pacing_data
   * hold; the current time is roughly within the current pacing time slice.
   *
   * Note that an error may occur in asynchronous operations triggered by this method; if this
   * happens to socket will be closed via close_connection_immediately().  However if the error
   * happens IN this method (`false` is returned), it is up to the caller to handle the error as
   * desired.
   *
   * @param sock
   *        Socket under consideration.
   * @param executing_after_delay
   *        `true` if executing from a pacing-related timer handler; `false` otherwise (i.e.,
   *        if sock_pacing_new_packet_ready() is in the call stack).
   */
  void sock_pacing_process_q(Peer_socket::Ptr sock, bool executing_after_delay);

  /**
   * async_sock_low_lvl_packet_send_paced() pacing helper: If sock_pacing_process_q() ran out of the last
   * time slice's budget and still had packets to send, this is the handler that triggers when the
   * out-of-budget time slice ends.  Sets up a new time slice starting now and tries to send as many
   * queud packets as possible with the new budget; if still more packets remain after this,
   * schedules yet another timer.
   *
   * This may also be called via `cancel()` of the timer.  In this case, the pre-condition is that
   * `sock->state() == Peer_socket::State::S_CLOSED`; the method will do nothing.
   *
   * Otherwise, pre-conditions: Send_pacing_data::m_packet_q for `sock` is NOT empty; the byte budget for
   * the current time slice is less than the packet at the head `m_packet_q`; `sock` is in OPEN state;
   * invariants described for `struct` Send_pacing_data hold; the current time is roughly just past
   * the current pacing time slice.
   *
   * Note that an error may occur in asynchronous operations triggered by this method; if this
   * happens to socket will be closed via close_connection_immediately().  However if the error
   * happens IN this method (`false` is returned), it is up to the caller to handle the error as
   * desired.
   *
   * @param sock
   *        Socket under consideration.
   * @param sys_err_code
   *        boost.asio error code.
   */
  void sock_pacing_time_slice_end(Peer_socket::Ptr sock, const Error_code& sys_err_code);

  /**
   * Sends an RST to the other side of the given socket asynchronously when possible.  An error is
   * unlikely, but if it happens there is no reporting other than logging.
   *
   * @param sock
   *        Socket the remote side of which will get the RST.
   */
  void async_sock_low_lvl_rst_send(Peer_socket::Ptr sock);

  /**
   * Sends an RST to the other side of the given socket, synchronously.  An error is
   * unlikely, but if it happens there is no reporting other than logging.  Will block (though
   * probably not for long, this being UDP) if #m_low_lvl_sock is in blocking mode.
   *
   * @param sock
   *        Socket the remote side of which will get the RST.
   */
  void sync_sock_low_lvl_rst_send(Peer_socket::Ptr sock);

  // Methods for core protocol logic dealing with deserialized packets before demuxing to Peer_socket or Server_socket.

  /**
   * Handles a just-received, not-yet-deserialized low-level packet.  A rather important method....
   *
   * @param packet_data
   *        Packet to deserialize and handle.  Upon return, the state of `*packet_data` is not known; and caller retains
   *        ownership of it (e.g., can read another datagram into it if desired).
   * @param low_lvl_remote_endpoint
   *        From where the packet came.
   */
  void handle_incoming(util::Blob* packet_data,
                       const util::Udp_endpoint& low_lvl_remote_endpoint);

  /**
   * Performs all tasks to be performed at the end of low_lvl_recv_and_handle() or
   * async part of async_wait_latency_then_handle_incoming(), as determined over the course of the execution
   * of either of those methods.  This includes at least performing event_set_all_check_delta() for
   * anything in #m_sock_events, etc., and any accumulated ACK-related tasks stored in the Peer_sockets
   * in #m_socks_with_accumulated_pending_acks and similar.  This is done for efficiency and to
   * reduce network overhead (for example, to combine several individual acknowledgments into one
   * ACK packet).
   */
  void perform_accumulated_on_recv_tasks();

  // Methods dealing with individual Peer_sockets.  Implementations are in peer_socket.cpp.

  /**
   * Handles a just-deserialized, just-demultiplexed low-level SYN_ACK packet delivered to the given
   * peer socket in `S_SYN_SENT` state.  So it will hopefully send back a SYN_ACK_ACK, etc.
   *
   * @param socket_id
   *        Connection ID (socket pair) identifying the socket in #m_socks.
   * @param sock
   *        Peer socket in Peer_socket::Int_state::S_SYN_SENT internal state.
   * @param syn_ack
   *        Deserialized immutable SYN_ACK.
   */
  void handle_syn_ack_to_syn_sent(const Socket_id& socket_id,
                                  Peer_socket::Ptr sock,
                                  boost::shared_ptr<const Syn_ack_packet> syn_ack);

  /**
   * Handles a just-deserialized, just-demultiplexed, duplicate (equal to already-received SYN_ACK)
   * low-level SYN_ACK packet delivered to the given peer socket in `S_ESTABLISHED` state.  This will
   * hopefully reply with SYN_ACK_ACK again.  Reasoning for this behavior is given in
   * handle_incoming() at the call to this method.
   *
   * @param sock
   *        Peer socket in Peer_socket::Int_state::S_ESTABLISHED internal state with sock->m_active_connect.
   * @param syn_ack
   *        Deserialized immutable SYN_ACK.
   */
  void handle_syn_ack_to_established(Peer_socket::Ptr sock,
                                     boost::shared_ptr<const Syn_ack_packet> syn_ack);

  /**
   * Handles a just-deserialized, just-demultiplexed, low-level DATA packet delivered to the given
   * peer socket in `S_ESTABLISHED` state.  This will hopefully reply with ACK and deliver the data to
   * the Receive buffer, where the user can receive() them.
   *
   * Also similarly handles packets received and queued earlier while in `S_SYN_RCVD` state.
   *
   * @param socket_id
   *        Connection ID (socket pair) identifying the socket in #m_socks.
   * @param sock
   *        Peer socket in Peer_socket::Int_state::S_ESTABLISHED internal state.
   * @param packet
   *        Deserialized DATA packet.  (For performance when moving data to Receive
   *        buffer, this is modifiable.)
   * @param syn_rcvd_qd_packet
   *        If `true`, this packet was saved during Peer_socket::Int_state::S_SYN_RCVD by handle_data_to_syn_rcvd() and
   *        is being handled now that socket is Peer_socket::Int_state::S_ESTABLISHED.  If `false`, this packet was
   *        received normally during `S_ESTABLISHED` state.
   */
  void handle_data_to_established(const Socket_id& socket_id,
                                  Peer_socket::Ptr sock,
                                  boost::shared_ptr<Data_packet> packet,
                                  bool syn_rcvd_qd_packet);

  /**
   * Helper for handle_data_to_established() that categorizes the DATA packet received as either
   * illegal; legal but duplicate of a previously received DATA packet;
   * legal but out-of-order; and finally legal and in-order. Illegal means sender can never validly send
   * such sequence numbers in a DATA packet. Legal means it can, although network problems may still lead to
   * the received DATA being not-useful in some way. Out-of-order means that `packet` occupies seq. numbers
   * past the start of the first unreceived data, or "first gap," which starts at Peer_socket::m_rcv_next_seq_num.
   * In-order, therefore, means `packet` indeed begins exactly at Peer_socket::m_rcv_next_seq_num (which means typically
   * one should increment the latter by `packet->m_data.size()`).
   *
   * No statistics are marked down on `sock`; the caller should proceed depending on the output as described
   * just below.
   *
   * If a truthy value is returned, packet is illegal; other outputs are meaningless. Otherwise, falsy is returned;
   * and: If `*dupe`, then packet is a legal dupe; and other outputs are meaningless. Otherwise, `!*dupe`. and:
   * `*slide` if and only if the packet is in-order (hence receive window left edge should "slide" right).
   * `*slide_size` is the number of bytes by which Peer_socket::m_rcv_next_seq_num should increment ("slide");
   * it is meaningful if and only if `*slide`.
   *
   * (Aside: Every attempt to detect illegality is made, within reason, but NOT every illegal behavior can be detected
   * as such; but defensive coding strives that a failure to detect such leads to nothing worse than meaningless data
   * received by user.)
   *
   * @param sock
   *        See handle_data_to_established().
   * @param packet
   *        See handle_data_to_established(). Note it is read-only, however.
   *.@param dupe
   *        Output for whether the packet is a dupe (true if so). Meaningless if truthy is returned.
   * @param slide
   *        Output for whether the packet consists of the next data to be passed to Receive buffer.
   *        Meaningless if truthy is returned, or else if `*dupe` is set to `true`.
   * @param slide_size
   *        By how much to increment Peer_socket::m_rcv_next_seq_num due to this in-order packet.
   *        Meaningless unless `*slide` is set to `true`.
   * @return Success if `packet` is legal; the recommended error to accompany the connection-breaking RST due
   *        to the illegal `packet`, otherwise.
   */
  Error_code sock_categorize_data_to_established(Peer_socket::Ptr sock,
                                                 boost::shared_ptr<const Data_packet> packet,
	                                               bool* dupe, bool* slide, size_t* slide_size);

  /**
   * Helper for handle_data_to_established() that aims to pass the payload of the given DATA packet to
   * the given socket's Receive buffer for user consumption; but detects and reports overflow if appropriate,
   * instead. Certain relevant stats are logged in all cases. `packet.m_data` is emptied due to moving it
   * elsewhere -- for performance (recommend saving its `.size()` before-hand, if needed for later) --
   * and the implications on rcv_wnd recovery (if any) are handled. `true` is returned assuming no overflow.
   *
   * If overflow detected, only statistical observations and logs are made, and `false` is returned.
   *
   * @param sock
   *        See handle_data_to_established().
   * @param packet
   *        See handle_data_to_established().
   * @return `false` on overflow; `true` on success.
   */
  bool sock_data_to_rcv_buf_unless_overflow(Peer_socket::Ptr sock,
                                            boost::shared_ptr<Data_packet> packet);

  /**
   * Helper for handle_data_to_established() that assumes the given's socket Receive buffer is currently
   * readable and handles implications on the Event_set subsystem.
   *
   * @param sock
   *        See handle_data_to_established().
   * @param syn_rcvd_qd_packet
   *        See handle_data_to_established().
   */
  void sock_rcv_buf_now_readable(Peer_socket::Ptr sock, bool syn_rcvd_qd_packet);

  /**
   * Helper for handle_data_to_established() that aims to register the given DATA packet as an out-of-order
   * packet in `sock->m_rcv_packets_with_gaps` -- in retransmission-off mode. The retransmission-on counterpart
   * is, roughly speaking, sock_data_to_reassembly_q_unless_overflow().
   *
   * This assumes that sock_categorize_data_to_established() returned
   * `*slide == false`. However, due to overflow considerations
   * this helper itself set its own `*slide` (and `*slide_size`) value. The `*slide` argument should be
   * interpereted the same way as from sock_categorize_data_to_established(); `*slide_size` (meaningful if
   * and only if `*slide = true` is set) specifies by how much Peer_socket::m_rcv_next_seq_num must now increment.
   * (Note, then, that in the caller this can only set `*slide` from `false` to `true`; or not touch it.)
   *
   * @param sock
   *        See handle_data_to_established().
   * @param packet
   *        See handle_data_to_established(). Note it is read-only, however.
   * @param data_size
   *        Original `packet->m_data.size()` value; by now presumbly that value is 0, but we want the original.
   * @param slide
   *        Same semantics as in sock_categorize_data_to_established() (except it is always set; no "illegal" case).
   * @param slide_size
   *        By how much to increment Peer_socket::m_rcv_next_seq_num due certain overflow considerations.
   */
  void sock_track_new_data_after_gap_rexmit_off(Peer_socket::Ptr sock,
                                                boost::shared_ptr<const Data_packet> packet,
                                                size_t data_size,
                                                bool* slide, size_t* slide_size);

  /**
   * Helper for handle_data_to_established() that aims to register the given DATA packet as an out-of-order
   * packet in the reassembly queue `sock->m_rcv_packets_with_gaps` -- in retransmission-on mode; but detects
   * and reports overflow if appropriate, instead.  Certain relevant stats are logged in all cases.
   * `packet.m_data` is emptied due to moving it elsewhere -- for performance (recommend saving its `.size()`
   * before-hand, if needed for later) -- and the implications on rcv_wnd recovery (if any) are handled.
   * `true` is returned assuming no overflow. The retransmission-off counterpart
   * is, roughly speaking, sock_track_new_data_after_gap_rexmit_off().
   *
   * If overflow detected, only statistical observations and logs are made, and `false` is returned.
   *
   * This assumes that sock_categorize_data_to_established() returned `*slide == false`.
   *
   * @param sock
   *        See handle_data_to_established().
   * @param packet
   *        See handle_data_to_established().
   * @return `false` on overflow; `true` on success.
   */
  bool sock_data_to_reassembly_q_unless_overflow(Peer_socket::Ptr sock,
                                                 boost::shared_ptr<Data_packet> packet);

  /**
   * Helper for handle_data_to_established() that aims to register a set of received DATA packet data as in-order
   * payload in the structures Peer_socket::m_rcv_packets_with_gaps and Peer_socket::m_rcv_next_seq_num
   * in `sock`.  Both structures are updated given the precondition that a set of data had arrived with data
   * starting at `sock->m_rcv_next_seq_num`.  If `reassembly_in_progress` (which should be `true` if and only
   * if retransmission is on), then the reassembly queue is popped into `sock->m_rcv_buf` to the appropriate
   * extent (as the just-arrived packet may have bridged the entire gap to the first packet in that queue).
   *
   * Certain relevant stats are logged in all cases.  Note that it's possible to simulate DATA packets' receipt
   * without actually having received such a packet.  This method will slide the window as directed regardless.
   *
   * @param sock
   *        See handle_data_to_established().
   * @param slide_size
   *        By how much to increment (slide right) Peer_socket::m_rcv_packets_with_gaps.
   *        See handle_data_to_established().
   * @param reassembly_in_progress
   *        Basically, `sock->rexmit_on()`.
   */
  void sock_slide_rcv_next_seq_num(Peer_socket::Ptr sock, size_t slide_size, bool reassembly_in_progress);

  /**
   * Computes and returns the max size for Peer_socket::m_rcv_packets_with_gaps for `sock`.
   *
   * @param sock
   *        An open socket.
   * @return See above.
   */
  size_t sock_max_packets_after_unrecvd_packet(Peer_socket::Const_ptr sock) const;

  /**
   * Causes an acknowledgment of the given received packet to be included in a future Ack_packet
   * sent to the other side.  That ACK low-level UDP packet is not sent in this handler, even if
   * the low-level UDP socket is currently writable.  The sending of this packet is performed
   * asynchronously in the manner of `boost::asio::post(io_context&)`.
   *
   * Note that the Ack_packet may include other packets being acknowledged; and that ACK may be
   * artificially delayed for reasons like the desire to accumulate more acknowledgments before
   * sending ACK (to cut down on overhead).
   *
   * @param sock
   *        Peer socket in Peer_socket::Int_state::S_ESTABLISHED.
   * @param seq_num
   *        Sequence number of first datum in the packet to be acknowledged.
   * @param rexmit_id
   *        Which attempt are we acknowledging (0 = initial send, 1 = first retransmission, 2 =
   *        second retransmission, ...).  Always 0 if retransmission is off.
   * @param data_size
   *        Number of bytes in the user data in the packet to be acknowledged.
   */
  void async_acknowledge_packet(Peer_socket::Ptr sock, const Sequence_number& seq_num, unsigned int rexmit_id,
                                size_t data_size);

  /**
   * Helper of perform_accumulated_on_recv_tasks() that handles any additional individual outgoing
   * acknowledgments accumulated during the currently running receive handler.  Pre-conditions:
   * executed from perform_accumulated_on_recv_tasks(); `!(Peer_socket::m_rcv_pending_acks).empty()`
   * for `sock`; Peer_socket::m_rcv_pending_acks_size_at_recv_handler_start (for `sock`) has been set;
   * `sock` is in #m_socks_with_accumulated_pending_acks.
   *
   * If state is not Peer_socket::Int_state::S_ESTABLISHED, method does nothing except possibly log.
   *
   * @param socket_id
   *        Connection ID (socket pair) identifying the socket in #m_socks.
   * @param sock
   *        Peer socket.
   */
  void handle_accumulated_pending_acks(const Socket_id& socket_id, Peer_socket::Ptr sock);

  /**
   * Helper for handle_data_to_established() that gets simple info about
   * Peer_socket::m_rcv_packets_with_gaps in `sock`.
   *
   * @param sock
   *        Socket to examine.
   * @param first_gap_exists
   *        Pointer to value to set to true if and only if !(Peer_socket::m_rcv_packets_with_gaps).empty()
   *        in `sock`.  If the Peer_socket::m_rcv_packets_with_gaps invariant fully holds, this means that
   *        there is at least one gap of unreceived packets between some received packets and other received packets,
   *        by sequence number order.
   * @param seq_num_after_first_gap
   *        Pointer to value that will be set to the first sequence number of the first element of
   *        `sock->m_rcv_packets_with_gaps`; untouched if `!*first_gap_exists` at return.
   */
  void rcv_get_first_gap_info(Peer_socket::Const_ptr sock,
                              bool* first_gap_exists, Sequence_number* seq_num_after_first_gap);

  /**
   * Logs TRACE or DATA messages that show the detailed state of the receiving sequence number
   * space.  Quite slow if DATA log level is enabled or `force_verbose_info_logging` is `true`.
   *
   * @param sock
   *        Socket whose data to log.
   * @param force_verbose_info_logging
   *        If `true`, then the method acts as if DATA logging is enabled, i.e., the maximum amount of
   *        information is logged (but with INFO verbosity).  You should only do this if you know
   *        for a fact that this is being called infrequently (such as from
   *        perform_regular_infrequent_tasks()).
   */
  void log_rcv_window(Peer_socket::Const_ptr sock, bool force_verbose_info_logging = false) const;

  /**
   * Handles a just-deserialized, just-demultiplexed, low-level ACK packet delivered to the given
   * peer socket in Peer_socket::Int_state::S_ESTABLISHED state.  This will hopefully
   * update internal data structures and inform congestion control (or queue that to be done by the end of the
   * current receive handler, low_lvl_recv_and_handle() or async part of async_wait_latency_then_handle_incoming().
   *
   * @param sock
   *        Peer socket in Peer_socket::Int_state::S_ESTABLISHED.
   * @param ack
   *        Deserialized immutable ACK.
   */
  void handle_ack_to_established(Peer_socket::Ptr sock,
                                 boost::shared_ptr<const Ack_packet> ack);

  /**
   * Helper of perform_accumulated_on_recv_tasks() that handles any incoming acknowledgments and
   * rcv_wnd updates accumulated during the currently running receive handler.  Pre-conditions:
   * executed from perform_accumulated_on_recv_tasks(); Peer_socket::m_rcv_acked_packets and
   * Peer_socket::m_snd_pending_rcv_wnd (in `sock`) have been set; `sock` is in
   * #m_socks_with_accumulated_acks.
   *
   * If `sock` is not in Peer_socket::Int_state::S_ESTABLISHED, method does nothing except possibly log.
   *
   * @param socket_id
   *        Connection ID (socket pair) identifying the socket in #m_socks.
   * @param sock
   *        Peer socket.
   */
  void handle_accumulated_acks(const Socket_id& socket_id, Peer_socket::Ptr sock);

  /**
   * Helper of perform_accumulated_on_recv_tasks() that categorizes the given accumulated individual acknowledgment
   * w/r/t legality and validity; determines the DATA packet being acked if possible; logs and record stats accordingly;
   * and closes underlying socket if ack is illegal.
   *
   * In all cases, all relevant (to the categorization of the given ack) information is logged and stats are recorded.
   *
   * Furthermore, if the ack is illegal, the socket is closed (while `false` is returned).  Otherwise, `true` is
   * returned, and `*dupe_or_late` is set to indicate whether the ack is valid or not.  If valid,
   * `*acked_pkt_it` is definitely set to indicate which DATA packet is being acked.  If invalid, `*acked_pkt_it`
   * may or may not be set, as that information may or may not be available any longer (example of it being available:
   * the ack is for an earlier transmission attempt of packet P, but packet P is currently In-flight due to a
   * subsequent retransmission attempt).
   *
   * @param socket_id
   *        Connection ID (socket pair) identifying the socket in #m_socks.
   * @param sock
   *        Peer socket.
   * @param ack
   *        Individual acknowledgment being categorized.
   * @param dupe_or_late
   *        Set to false if ack refers to currently In-flight instance of a packet; true if no longer In-flight
   *        (late = considered Dropped laready; duplicate = was acked before); untouched if `false` returned.
   * @param acked_pkt_it
   *        Set to point into Peer_socket::m_snd_flying_pkts_by_sent_when that is being acked if `!*dupe_or_late`,
   *        or if `*dupe_or_late` but the acked packet is still known; set to `end()` a/k/a `past_oldest()`
   *        otherwise; untouched if `false`
   *        returned.
   * @return `false` if and only if the ack is sufficiently invalid to have made this method close the socket.
   */
  bool categorize_individual_ack(const Socket_id& socket_id, Peer_socket::Ptr sock,
                                 Ack_packet::Individual_ack::Const_ptr ack,
                                 bool* dupe_or_late, Peer_socket::Sent_pkt_ordered_by_when_iter* acked_pkt_it);

  /**
   * Helper of perform_accumulated_on_recv_tasks() that computes the RTT implied by a given individual acknowledgment.
   * In addition to returning the RTT, note the convenience out-param.
   *
   * @param flying_pkt
   *        The In-flight DATA packet to which the ack pertains.
   * @param time_now
   *        The current time to use for the RTT computation (not using value within to allow for caller to simulate
   *        simultaneity between nearby RTT computations).
   * @param ack
   *        Individual acknowledgment being categorized.
   * @param sent_when
   *        This out-param is set to point within Peer_socket::m_snd_flying_pkts_by_sent_when's `Sent_when`
   *        structure pertaining to the DATA packet send attempt to which `ack` refers.
   * @return The RTT.  May be zero.
   */
  Fine_duration compute_rtt_on_ack(Peer_socket::Sent_packet::Const_ptr flying_pkt,
                                   const Fine_time_pt& time_now,
                                   Ack_packet::Individual_ack::Const_ptr ack,
                                   const Peer_socket::Sent_packet::Sent_when** sent_when) const;

  /**
   * Handles a just-computed new RTT (round trip time) measurement for an individual packet earlier
   * sent: updates smoothed RTT, DTO, and anything else relevant.
   *
   * @param sock
   *        Peer socket in Peer_socket::Int_state::S_ESTABLISHED.
   * @param round_trip_time
   *        The RTT just computed, with as much resolution as is available.
   */
  void new_round_trip_time_sample(Peer_socket::Ptr sock, Fine_duration round_trip_time);

  /**
   * Helper of perform_accumulated_on_recv_tasks() that determines the range of In-flight packets that should be
   * Dropped due to given individual acks that have just been processed; and updates the relevant `m_acks_after_me`
   * members in the socket.
   *
   * Logging is minimal, and no stats are recorded.  However, see associated drop_pkts_on_acks() method.
   *
   * Peer_socket::Sent_packet::m_acks_after_me data members, as documented, are incremented where relevant based
   * on the just-processed acks in `flying_now_acked_pkts`.
   *
   * Finally, the following In-flight packets must be considered Dropped due to acks:
   *   - The packet referred to by the returned iterator into Peer_socket::m_snd_flying_pkts_by_sent_when.
   *   - All packets contained in the same structure appearing later in it (i.e., sent out earlier), up to
   *     `past_oldest()` (a/k/a `end()`).
   *
   * Note that this method does not actually perform the various tasks: it only updates `m_acks_after_me` and
   * computes/returns the start of the to-be-Dropped range.  See drop_pkts_on_acks() for the actual dropping.
   *
   * @param sock
   *        Peer socket.
   * @param flying_now_acked_pkts
   *        The individual DATA packet send attempts acks of which have just been processed.
   *        The Peer_socket::Sent_packet (and within it, the Peer_socket::Sent_packet::Sent_when) with the order ID
   *        P, where P is in `flying_now_acked_pkts`, must be in Peer_socket::m_snd_flying_pkts_by_sent_when.
   * @return Iterator into `sock->m_snd_flying_pkts_by_sent_when` indicating the latest-sent packet that should
   *         be Dropped due to acks; `past_oldest()` a/k/a `end()` if none should be so Dropped.
   */
  Peer_socket::Sent_pkt_ordered_by_when_iter
    categorize_pkts_as_dropped_on_acks(Peer_socket::Ptr sock,
                                       const boost::unordered_set<Peer_socket::order_num_t>& flying_now_acked_pkts);

  /**
   * Helper of perform_accumulated_on_recv_tasks() that acts on the determination made by
   * categorize_pkts_as_dropped_on_acks().
   *
   * In all cases, all relevant (to the categorization of the In-flight packets as Dropped) information is logged
   * and stats are recorded.
   *
   * This acts, or gathers information necessary to act, on the determination by categorize_pkts_as_dropped_on_acks()
   * that a certain range of In-flight packets should be Dropped due to excess acks of packets sent before them.
   * Namely:
   *   - `*cong_ctl_dropped_...` are set to the values to report congestion control as part of a new loss event.
   *   - `*dropped_...` are set to values that indicate totals w/r/t the packets Dropped (regardless of whether it's
   *     a new or existing loss event).
   *   - `*pkts_marked_to_drop` are loaded with the Peer_socket::Sent_packet::Sent_when::m_order_num order IDs
   *     specifying the Dropped packets.
   *   - `sock` members `m_snd_flying_pkts*` and related are updated, meaning the newly Dropped packets are removed.
   *   - On the other hand, if retransmission is on, Peer_socket::m_snd_rexmit_q is pushed onto, gaining the
   *     just-Dropped packets to retransmit.
   *   - `true` is returned.
   *
   * However, if it is determined that a retransmission placed onto `sock->m_snd_rexmit_q` would indicate one
   * retransmission too many, the socket is closed, and `false` is returned.
   *
   * @param sock
   *        Peer socket.
   * @param last_dropped_pkt_it
   *        Return value of of categorize_pkts_as_dropped_on_acks().
   * @param cong_ctl_dropped_pkts
   *        Will be set to total # of packets marked as Dropped to report to congestion control as part of
   *        a loss event (`<= *dropped_pkts`).
   * @param cong_ctl_dropped_bytes
   *        Total data size corresponding to `cong_ctl_dropped_pkts` (`<= *dropped_bytes)`).
   * @param dropped_pkts
   *        Will be set to total # of packets marked as Dropped by this method.
   * @param dropped_bytes
   *        Total data size corresponding to `dropped_pkts`.
   * @param pkts_marked_to_drop
   *        Will be filled with packet IDs (`sock->m_snd_flying_pkts_by_sent_when[...]->m_sent_when->m_order_num`)
   *        of the packets marked dropped by this method.  Results undefined unless empty at method start.
   * @return `true` normally; `false` if too many retransmissions detected, and thus `sock` was closed.
   */
  bool drop_pkts_on_acks(Peer_socket::Ptr sock,
                         const Peer_socket::Sent_pkt_ordered_by_when_iter& last_dropped_pkt_it,
                         size_t* cong_ctl_dropped_pkts, size_t* cong_ctl_dropped_bytes,
                         size_t* dropped_pkts, size_t* dropped_bytes,
                         std::vector<Peer_socket::order_num_t>* pkts_marked_to_drop);

  /**
   * Helper of handle_accumulated_acks() that logs the about-to-be-handled accumulated individual acknowledgments.
   *
   * @param sock
   *        Peer socket with 0 or more accumulated acks recorded.
   */
  void log_accumulated_acks(Peer_socket::Const_ptr sock) const;

  /**
   * Handles a Drop_timer (Peer_socket::m_snd_drop_timer) event in ESTABLISHED state by dropping the specified
   * packets.  To be executed as a Drop_timer callback.
   *
   * @param sock
   *        Peer socket is Peer_socket::Int_state::S_ESTABLISHED with at least one In-flight sent packet.
   * @param drop_all_packets
   *        If `true`, will consider all packets Dropped.  If `false`, will consider only the earliest
   *        In-flight packet dropped.
   */
  void drop_timer_action(Peer_socket::Ptr sock, bool drop_all_packets);

  /**
   * Logs TRACE or DATA messages thats show the detailed state of the sending sequence number space.
   * Quite slow if DATA log level is enabled or `force_verbose_info_logging` is `true`.
   *
   * @param sock
   *        Socket whose data to log.
   * @param force_verbose_info_logging
   *        Similar to same argument in log_rcv_window().
   */
  void log_snd_window(Peer_socket::Const_ptr sock, bool force_verbose_info_logging = false) const;

  /**
   * Thread W implementation of connect().  Performs all the needed work up to waiting for network
   * traffic, gives the resulting Peer_socket to the user thread, and signals that user thread.
   *
   * Pre-condition: We're in thread W; thread U != W is waiting for us to return having set `*sock`.  Post-condition:
   * `*sock` contains a Peer_socket::Ptr in an OPEN+CONNECTING state if `!(Peer_socket::m_disconnect_cause)`
   * for `*sock`; otherwise an error occurred, and that error is Peer_socket::m_disconnect_cause (in `*sock`).
   *
   * @param to
   *        See connect().
   * @param serialized_metadata
   *        Serialized metadata to provide to the peer when the connection is being established.
   * @param opts
   *        See connect().
   * @param sock
   *        `*sock` shall be set to the resulting new Peer_socket.  Check `(*sock)->m_disconnect_cause`.
   */
  void connect_worker(const Remote_endpoint& to,
                      const boost::asio::const_buffer& serialized_metadata,
                      const Peer_socket_options* opts,
                      Peer_socket::Ptr* sock);

  /**
   * Implementation core of `sync_connect*()` that gets rid of templated or missing arguments thereof.
   *
   * E.g., the API would wrap this and supply a Fine_duration instead of generic `duration`; and supply
   * `Fine_duration::max()` if user omitted the timeout argument.  Code bloat and possible circular definition issues
   * are among the reasons for this "de-templating" pattern.
   *
   * @param to
   *        See connect().
   * @param max_wait
   *        See the public `sync_connect(timeout)`.  `"duration<Rep, Period>::max()"` maps to the value
   *        `Fine_duration::max()` for this argument.
   * @param serialized_metadata
   *        See connect_with_metadata().
   * @param err_code
   *        See sync_connect().
   * @param opts
   *        See connect().
   * @return See sync_connect().
   */
  Peer_socket::Ptr sync_connect_impl(const Remote_endpoint& to, const Fine_duration& max_wait,
                                     const boost::asio::const_buffer& serialized_metadata,
                                     Error_code* err_code,
                                     const Peer_socket_options* opts);

  /**
   * Assuming we've just sent SYN or SYN_ACK, sets up an asynchronous scheduled task to fire within some
   * amount of time, so that we may try the SYN[_ACK] again if we don't get the acknowledgement by
   * then (or we may close socket after too many such retries).  If `initial` is `true`, an overall
   * connection timeout scheduled task is also set up, to trigger the aforementioned close on timeout.
   *
   * @param socket_id
   *        Connection ID (socket pair) identifying the socket in #m_socks.
   * @param sock
   *        Peer socket in SYN_SENT or SYN_RCVD internal state.
   * @param initial
   *        `true` if and only if the first SYN or SYN_ACK; otherwise it is a retry.
   */
  void setup_connection_timers(const Socket_id& socket_id, Peer_socket::Ptr sock, bool initial);

  /**
   * Handles the triggering of the retransmit timer wait set up by
   * setup_connection_timers(); it will re-send the SYN or SYN_ACK.
   *
   * @param socket_id
   *        Connection ID (socket pair) identifying the socket in #m_socks.
   * @param sock
   *        Peer socket.
   */
  void handle_connection_rexmit_timer_event(const Socket_id& socket_id, Peer_socket::Ptr sock);

  /**
   * Cancel any timers and scheduled tasks active in the given socket.  More precisely, causes for each handler
   * scheduled to happen in the future to be called as soon as possible with error code
   * `operation_aborted`.  If, by the time the current handler has begun, the handler was about to be
   * called due the timer triggering, this method will not be able to induce `operation_aborted`.
   * Therefore the handler should be careful to check state and not rely on `operation_aborted`,
   * despite this method.
   *
   * Update: The caveats in previous paragraph do not apply to scheduled tasks (`util::schedule_task_*()`).
   * Canceling such tasks (which this method also does) prevents their handlers from running.
   *
   * @param sock
   *        Socket whose timers/scheduled tasks to abort.
   */
  void cancel_timers(Peer_socket::Ptr sock);

  /**
   * Creates a new Drop Timer and saves it to `sock->m_snd_drop_timer`.  Pre-condition: `m_int_state ==
   * S_ESTABLISHED`, and `sock->m_snd_drop_timer` is null.
   *
   * @param socket_id
   *        Connection ID (socket pair) identifying the socket in #m_socks.
   * @param sock
   *        Socket that just entered ESTABLISHED state.
   */
  void setup_drop_timer(const Socket_id& socket_id, Peer_socket::Ptr sock);

  /**
   * Implementation of non-blocking `sock->close_abruptly()` for socket `sock` in all cases except when
   * `sock->state() == State::S_CLOSED`.  See Peer_socket::close_abruptly() doc
   * header; this method is the entirety of that method's implementation after CLOSED is
   * eliminated as a possibility.
   *
   * Pre-conditions:
   *   - current thread is not W;
   *   - `sock->m_mutex` is locked and just after entering `sock->close_abruptly()`;
   *   - no changes to `*sock` have been made since `m_mutex` was locked;
   *   - `sock->state() == Stated::S_OPEN` (so `sock` is in #m_socks);
   *   - `sock` has been given to user via accept() or connect() or friends.
   *
   * Post-condition (not exhaustive): `sock->m_mutex` is unlocked.
   *
   * @param sock
   *        Socket in OPEN state.
   * @param err_code
   *        See Peer_socket::close_abruptly().
   */
  void close_abruptly(Peer_socket::Ptr sock, Error_code* err_code);

  /**
   * A thread W method that handles the transition of the given socket from OPEN (any sub-state)
   * to CLOSED (including eliminating the given Peer_socket from our data structures).  For
   * example, if an invalid packet comes in on the socket, and we send back an RST, then we're free
   * to then close our side immediately, as no further communication (with the other side or the
   * local user) is needed.  As another example, if we there is a graceful close while Receive buffer
   * has data, user must Receive all of it, and the final handshake must finish, and then this is called.
   *
   * @todo Graceful close not yet implemented w/r/t close_connection_immediately().
   *
   * Pre-condition: if `err_code` is failure: `sock` is in #m_socks; `sock->state() == S_OPEN` (and any
   * `sock->m_int_state` that corresponds to it); `err_code` contains the reason for the close.
   *
   * Pre-condition: if `err_code` is success: `sock` is in #m_socks; `sock` state is
   * OPEN+DISCONNECTING; `m_int_state` is CLOSED; Send and Receive buffers are empty;
   * `m_disconnect_cause` is not success.
   *
   * Post-condition: `sock` Receive and Send buffers are empty; `sock->state() == S_CLOSED` (and `sock`
   * is no longer in #m_socks or any other Node structures, directly or indirectly) with
   * `sock->m_disconnect_cause` set to reason for closing.  Other decently memory-consuming structures
   * are also cleared to conserve memory.
   *
   * Any socket that is in #m_socks MUST be eventually closed using this method.  No
   * socket that is not in #m_socks must be passed to this method.  In particular, do not call this
   * method during connect() or handle_syn_to_listening_server().
   *
   * @param socket_id
   *        Connection ID (socket pair) identifying the socket in #m_socks.
   * @param sock
   *        Socket to close.
   * @param err_code
   *        If this is not success, then it is an abrupt close, and this is why `sock` is being
   *        abruptly closed.  `m_disconnect_cause` is set accordingly and logged.
   *        If `err_code` is failure, then: `sock` is OPEN+DISCONNECTING (graceful close), and all
   *        criteria required for it to move so CLOSED are satisfied: internal state is CLOSED
   *        (goodbye handshake finished), and Receive and Send buffers are empty; `m_disconnect_cause`
   *        is already set.
   * @param defer_delta_check
   *        Same meaning as in event_set_all_check_delta().
   */
  void close_connection_immediately(const Socket_id& socket_id, Peer_socket::Ptr sock,
                                    const Error_code& err_code, bool defer_delta_check);

  /**
   * Helper that creates a new SYN packet object to the extent that is suitable for immediately passing to
   * async_sock_low_lvl_packet_send_paced().  `sock` members that reflect any data in Syn_packet must already be
   * saved and are not used as the source for such data.
   *
   * @param sock
   *        See async_sock_low_lvl_packet_send().
   * @return Pointer to new packet object suitable for async_sock_low_lvl_packet_send_paced() without having to fill
   *         any further data members in the object.
   */
  Syn_packet::Ptr create_syn(Peer_socket::Const_ptr sock);

  /**
   * Like create_syn() but for SYN_ACK.
   *
   * @param sock
   *        See create_syn().
   * @return See create_syn().
   */
  Syn_ack_packet::Ptr create_syn_ack(Peer_socket::Const_ptr sock);

  /**
   * Helper to create, fully fill out, and asynchronously send via async_sock_low_lvl_packet_send_paced()
   * a SYN_ACK_ACK packet.  Since rcv_wnd is advertised, Peer_socket::m_rcv_last_sent_rcv_wnd is updated for `sock`.
   *
   * @param sock
   *        See async_sock_low_lvl_packet_send().
   * @param syn_ack
   *        SYN_ACK to which the resulting SYN_ACK_ACK is the reply.
   */
  void async_low_lvl_syn_ack_ack_send(const Peer_socket::Ptr& sock,
                                      boost::shared_ptr<const Syn_ack_packet>& syn_ack);

  /**
   * Asynchronously send RST to the other side of the given socket and
   * close_connection_immediately().
   *
   * @param socket_id
   *        See close_connection_immediately().
   * @param sock
   *        See close_connection_immediately().
   * @param err_code
   *        See close_connection_immediately().
   * @param defer_delta_check
   *        Same meaning as in event_set_all_check_delta().
   */
  void rst_and_close_connection_immediately(const Socket_id& socket_id, Peer_socket::Ptr sock,
                                            const Error_code& err_code, bool defer_delta_check);

  /**
   * Implementation of non-blocking `sock->send()` for socket `sock` in all cases except when
   * `sock->state() == State::S_CLOSED`.
   *
   * Pre-conditions:
   *   - current thread is not W;
   *   - `sock->m_mutex` is locked and after entering `sock->[sync_]send()`;
   *   - no changes to `*sock` have been made since `m_mutex` was locked;
   *   - `sock->state() == State::S_OPEN` (so `sock` is in #m_socks);
   *   - `snd_buf_feed_func is as described below.
   *
   * This method completes the functionality of `sock->send()`.
   *
   * @see Important: see giant comment inside Node::send() for overall design and how send_worker()
   *      fits into it.
   * @param sock
   *        Socket, which must be in #m_socks, on which `[sync_]send()` was called.
   * @param snd_buf_feed_func
   *        Pointer to function with signature `size_t fn(size_t x)` that will perform
   *        `sock->m_snd_buf.feed_bufs_copy(...)` call with `max_data_size == X`, which will feed the
   *        data the user wants to `sock->send()` into `sock->m_snd_buf`, and return the return value
   *        of that call (which indicates how many bytes the call was able to fit into `m_snd_buf`).
   *        Doing it this way prevents this Node::send() from being a template, which prevents
   *        circular dependency unpleasantness.  See Peer_socket::send() for details.
   * @param err_code
   *        See Peer_socket::send().
   * @return See Peer_socket::send().
   */
  size_t send(Peer_socket::Ptr sock,
              const Function<size_t (size_t max_data_size)>& snd_buf_feed_func,
              Error_code* err_code);

  /**
   * Returns `true` if and only if calling `sock->send()` with at least some arguments would return
   * either non-zero (i.e., successfully enqueued data to send) or zero and an error (but not
   * zero and NO error).  `sock` will be locked and unlocked; safe to call from any thread.
   *
   * @param sock_as_any
   *        Socket to examine, as an `any` wrapping a Peer_socket::Ptr.
   * @return See above.
   */
  bool sock_is_writable(const boost::any& sock_as_any) const;

  /**
   * Helper placed by send() onto W to invoke send_worker() but ensures that the socket has not
   * entered some state such that sending data is not possible and no longer going to be possible.
   *
   * Example: `send(sock)` runs while `sock` is in ESTABLISHED state; queues up
   * send_worker_check_state() on thread W; thread W detects a connection reset and moves `sock` to
   * CLOSED; send_worker_check_state() gets its turn on thread W; detects state is now CLOSED and
   * returns without doing anything.
   *
   * @see Important: see giant comment inside Node::send() for overall design and how send_worker()
   *      fits into it.
   * @param sock
   *        Socket on which to possibly send low-level packets.
   */
  void send_worker_check_state(Peer_socket::Ptr sock);

  /**
   * Thread W implemention of send(): synchronously or asynchronously send the contents of
   * `sock->m_snd_buf` to the other side.  This locks the socket and examines `m_snd_buf`.  If a low-level
   * UDP packet cannot be produced from the front of `m_snd_buf` (i.e., not enough data in `m_snd_buf`),
   * then there is nothing to do.  Otherwise, determines whether network conditions (e.g.,
   * congestion control) allow for 1 or more such packets to be sent.  If not, then there is nothing
   * to do.  Otherwise (if 1 or more packets can be sent), 1 or more packets are sent and removed
   * from `sock->m_snd_buf`.  Finally, `m_snd_buf` is unlocked.
   *
   * Pre-condition: `sock->m_int_state == S_ESTABLISHED`.  @todo Are there other states where sending
   * DATA packets is OK?  If so it would be during graceful termination, if we implement it.  See
   * send_worker() for contedt for this to-do.
   *
   * @see Important: see giant comment inside Node::send() for overall design and how send_worker()
   *      fits into it.
   * @param sock
   *        Socket on which to possibly send low-level packets.
   * @param defer_delta_check
   *        Same meaning as in event_set_all_check_delta().
   */
  void send_worker(Peer_socket::Ptr sock, bool defer_delta_check);

  /**
   * Answers the perennial question of congestion and flow control: assuming there is a DATA packet
   * to send to the other side on the given socket, should we do so at this moment?  Over a perfect
   * link and with a perfect receiver, this would always return true, and we would always send every
   * packet as soon as we could make it.  As it is, some congestion control algorithm is used here
   * to determine if the link should be able to handle a packet, and rcv_wnd is used to determine if
   * the receive would be able to buffer a packet if it did arrive.
   *
   * @param sock
   *        Socket for which we answer the question.
   * @return `true` if should send; `false` if should wait until it becomes `true` and THEN send.
   */
  bool can_send(Peer_socket::Const_ptr sock) const;

  /**
   * Implementation of non-blocking sock->receive() for socket `sock` in all cases except when
   * `sock->state() == State::S_CLOSED`.
   *
   * Pre-conditions:
   *   - current thread is not W;
   *   - `sock->m_mutex` is locked and just after entering `sock->receive()`;
   *   - no changes to `*sock` have been made since `m_mutex` was locked;
   *   - `sock->state() == Stated::S_OPEN` (so `sock` is in #m_socks);
   *   - `rcv_buf_feed_func` is as described below.
   *
   * This method completes the functionality of `sock->receive()`.
   *
   * @param sock
   *        Socket, which must be in #m_socks, on which `receive()` was called.
   * @param rcv_buf_consume_func
   *        Pointer to function with signature `size_t fn()` that will perform
   *        `sock->m_rcv_buf.consume_bufs_copy(...)` call, which will consume data from `m_rcv_buf`,
   *        and return the return value of that call (which indicates how many bytes
   *        Socket_buffer::consume_bufs_copy() was able to fit into the user's data structure).  Doing it this way
   *        prevents this Node::receive() from being a template, which prevents circular dependency
   *        unpleasantness.  See Peer_socket::receive() for details.
   * @param err_code
   *        See Peer_socket::receive().
   * @return See Peer_socket::receive().
   */
  size_t receive(Peer_socket::Ptr sock,
                 const Function<size_t ()>& rcv_buf_consume_func,
                 Error_code* err_code);

  /**
   * Returns `true` if and only if calling sock->receive() with at least some arguments would return
   * either non-zero (i.e., successfully dequeued received data) or zero and an error (but not
   * zero and NO error).  `sock` will be locked and unlocked; safe to call from any thread.
   *
   * @param sock_as_any
   *        Socket to examine, as an `any` wrapping a Peer_socket::Ptr.
   * @return See above.
   */
  bool sock_is_readable(const boost::any& sock_as_any) const;

  /**
   * Placed by receive() onto W if it has dequeued data from Receive buffer and given it to the
   * user, which would free up space in the Receive buffer, which *possibly* should result in a
   * window update sent to the server, so that it knows it can now send more data.
   *
   * @see Node::receive().
   * @param sock
   *        Socket (whose state is ESTABLISHED or later).
   */
  void receive_wnd_updated(Peer_socket::Ptr sock);

  /**
   * receive_wnd_updated() helper that continues rcv_wnd recovery: that is, sends unsolicited ACK
   * with a rcv_wnd advertisement only and schedules the next iteration of a timer to have this
   * occur again, unless that timer is canceled due to too long a recovery phase or DATA packets
   * arriving from the other side.
   *
   * @param sock
   *        See receive_wnd_updated().
   * @param rcv_wnd
   *        The rcv_wnd (free Receive buffer space) to advertise to the other side.
   */
  void async_rcv_wnd_recovery(Peer_socket::Ptr sock, size_t rcv_wnd);

  /**
   * Pertaining to the async_rcv_wnd_recovery() mechanism, this handles the event that we have
   * received an acceptable (either into Receive buffer or reassembly queue) DATA packet from the
   * other side.  If we are currently in rcv_wnd recovery, this signifies the recovery "worked" --
   * the sender is sending data again -- so we can now end this phase.
   *
   * @param sock
   *        See receive_wnd_updated().
   */
  void receive_wnd_recovery_data_received(Peer_socket::Ptr sock);

  /**
   * Computes and returns the currently correct rcv_wnd value; that is the amount of space free in
   * Receive buffer for the given socket.  This may only be called from thread W.
   *
   * @param sock
   *        A socket.
   * @return See above.
   */
  size_t sock_rcv_wnd(Peer_socket::Const_ptr sock) const;

  /**
   * Placed by receive() onto W during a graceful close, after the Receive buffer had been emptied
   * by the user; determines whether the socket can now proceed to
   * `Peer_socket::m_state == Peer_socket::State::S_CLOSED`
   * and be removed from the Node.
   *
   * @see Node::receive().
   * @param sock
   *        Socket which may possibly now move to `m_state == S_CLOSED`.
   */
  void receive_emptied_rcv_buf_while_disconnecting(Peer_socket::Ptr sock);

  /**
   * Sends a low-level ACK packet, with all accumulated in Peer_socket::m_rcv_pending_acks of `sock` individual packet
   * acknowledgments, to the other side's UDP endpoint.  If the pending acknowledgments don't fit
   * into one ACK, more ACKs are generated and sent as necessary.  If there is an error sending or
   * preparing to send, `sock` is closed abruptly (close_connection_immediately()).
   *
   * This may be called either directly or by boost.asio due to delayed ACK timer being triggered.
   * If `sock` is not in Peer_socket::Int_state::S_ESTABLISHED, this does nothing except possibly logging.
   *
   * @param sock
   *        Socket the remote side of which will get the RST.  Method is basically a NOOP unless
   *        state is Peer_socket::Int_state::S_ESTABLISHED.
   * @param sys_err_code
   *        If invoked via timer trigger, this is boost.asio's error code.  If invoked directly,
   *        this should be set to the default (success).  Value is handled as follows: assuming
   *        ESTABLISHED state: `operation_aborted` => NOOP; success or any other error => attempt to
   *        send ACK(s).
   */
  void async_low_lvl_ack_send(Peer_socket::Ptr sock, const Error_code& sys_err_code = Error_code());

  /**
   * Return `true` if and only if there are enough data either in Peer_socket::m_snd_rexmit_q of `sock` (if
   * retransmission is on) or in Peer_socket::m_snd_buf of `sock` to send a DATA packet to the other
   * side.
   *
   * Pre-condition: `sock->m_mutex` is locked.
   *
   * @param sock
   *        Socket whose retransmission queue and Send buffer to examine.
   * @return See above.
   */
  bool snd_deqable(Peer_socket::Const_ptr sock) const;

  /**
   * Return `true` if and only if there is enough free space in Peer_socket::m_snd_buf of `sock` to enqueue any given
   * atomic piece of user data.
   *
   * Pre-condition: `sock->m_mutex` is locked.
   *
   * Currently this simply means that there is space for at least max-block-size bytes (i.e., one
   * maximally large block) in `sock->m_snd_buf`.
   *
   * Design rationale for the latter: See code.
   *
   * @param sock
   *        Socket whose Send buffer to examine.
   * @return See above.
   */
  bool snd_buf_enqable(Peer_socket::Const_ptr sock) const;

  /**
   * Return true if and only if there are enough data in Peer_socket::m_rcv_buf of `sock` to give the user some
   * data in a Peer_socket::receive() call.
   *
   * Pre-condition: `sock->m_mutex` is locked.
   *
   * Currently this simply means that there is at least 1 block of data in `m_rcv_buf`.
   *
   * Design rationale: see snd_buf_deqable().
   *
   * @param sock
   *        Socket whose Receive buffer to examine.
   * @return See above.
   */
  bool rcv_buf_deqable(Peer_socket::Const_ptr sock) const;

  /**
   * Sets internal state of given socket to the given state and logs a TRACE message about it.
   * Should only be run from thread W; performs no locking.
   *
   * @param sock
   *        Socket under consideration.
   * @param new_state
   *        New state.
   */
  void sock_set_int_state(Peer_socket::Ptr sock, Peer_socket::Int_state new_state);

  /**
   * Sets Peer_socket::m_state and Peer_socket::m_open_sub_state.  If moving to Peer_socket::State::S_CLOSED, resets
   * the required data to their "undefined" values (e.g., Peer_socket::m_local_port = #S_PORT_ANY).  Thread-safe.
   *
   * @warning Only set `state` = `S_CLOSED` if no more data are in Receive buffer, so that the
   * user can get those data before `S_CLOSED` state.  See Peer_socket::State::S_DISCONNECTING.

   * @param sock
   *        Socket under consideration.
   * @param state
   *        New Peer_socket::m_state.
   * @param open_sub_state
   *        Ignored if `state != S_OPEN`; otherwise the new value for Peer_socket::m_open_sub_state.
   */
  void sock_set_state(Peer_socket::Ptr sock,
                      Peer_socket::State state,
                      Peer_socket::Open_sub_state open_sub_state = Peer_socket::Open_sub_state::S_CONNECTED);

  /**
   * Records that thread W shows underlying connection is broken (graceful termination, or error)
   * and sets Peer_socket::m_disconnect_cause and Peer_socket::m_state, Peer_socket::m_open_sub_state accordingly.
   * Optionally also empties the Send and Receive buffers and any other decently memory-consuming structures.
   * Thread-safe.
   *
   * So the mutually exclusive closure scenarios are:
   *   - `sock_disconnect_detected(sock, err_code, false); ...; sock_disconnect_completed(sock);`
   *     Graceful close initiated; ...buffers emptied...; graceful close completed.
   *   - `sock_disconnect_detected(sock, err_code, true);`
   *     Abrupt close, or graceful close when the buffers already happen to be empty.
   *
   * @param sock
   *        Socket under consideration.
   * @param disconnect_cause
   *        The cause of the disconnect.
   * @param close
   *        If `true`, the target public state should be the super-final `S_CLOSED`, and the Send and
   *        Receive buffers are cleared; if `false`, the target public state should be the ominous
   *        `S_OPEN`+`S_DISCONNECTING`, and the buffers are left alone.  The caller's responsibility is
   *        to decide which one it is, but `true` is typically either for an abrupt close (e.g.,
   *        RST) or for a graceful close when buffers are empty; while `false` is typically for a
   *        graceful close before buffers are empty, so that the user can get Receive buffer, and
   *        the Node can send out Send buffer.
   */
  void sock_disconnect_detected(Peer_socket::Ptr sock,
                                const Error_code& disconnect_cause, bool close);

  /**
   * While in `S_OPEN`+`S_DISCONNECTING` state (i.e., after beginning a graceful close with
   * `sock_disconnect_detected(..., false)`, moves the socket to `S_CLOSED` state and clears Receive/Send
   * buffers and any other decently memory-consuming structures.
   *
   * Pre-conditions: state is `S_OPEN`+`S_DISCONNECTING`; Peer_socket::m_disconnect_cause is set to non-success
   * value.
   *
   * @param sock
   *        Socket under consideration.
   */
  void sock_disconnect_completed(Peer_socket::Ptr sock);

  /**
   * Helper that clears all non-O(1)-space data structures stored inside `sock`.  Intended to be
   * called from `sock_disconnect_*()`, not anywhere else.  Pre-condition: `sock->m_mutex` is
   * locked.
   *
   * @param sock
   *        Socket under consideration.
   */
  void sock_free_memory(Peer_socket::Ptr sock);

  /**
   * Analogous to validate_options() but checks per-socket options instead of per-Node
   * options.
   *
   * `*prev_opts` is replaced with `opts`.  Leave `prev_opts` as null unless an
   * existing Peer_socket's options are being changed via Peer_socket::set_options().  Otherwise a
   * Node_options::m_dyn_sock_opts Peer_socket_options is being changed, and that is
   * always allowed (since if a per-socket option were not dynamic in that way, it would simply be a
   * per-Node option instead).
   *
   * @param opts
   *        New option values to validate.
   * @param prev_opts
   *        null if called from constructor; `&sock->m_opts` if called from sock->set_options().
   *        Used to ensure no static per-socket option is being changed.
   * @param err_code
   *        After return, `*err_code` is success or: error::Code::S_OPTION_CHECK_FAILED,
   *        error::Code::S_STATIC_OPTION_CHANGED.
   *        If `!err_code`, error::Runtime_error() with that #Error_code is thrown instead.
   * @return `true` on success, `false` on validation error.
   */
  bool sock_validate_options(const Peer_socket_options& opts, const Peer_socket_options* prev_opts,
                             Error_code* err_code) const;

  /**
   * Thread W implementation of sock->set_options().  Performs all the needed work to complete
   * `sock->set_options()` call.
   *
   * Pre-condition: `sock->state()` is not Peer_socket::State::S_CLOSED.
   *
   * @param sock
   *        See Peer_socket::set_options().
   * @param opts
   *        See Peer_socket::set_options().
   * @param err_code
   *        See Peer_socket::set_options().
   * @return See Peer_socket::set_options().
   */
  bool sock_set_options(Peer_socket::Ptr sock, const Peer_socket_options& opts, Error_code* err_code);

  /**
   * Implementation of `sock->info()` for socket `sock` in all cases except when
   * `sock->state() == Peer_socket::State::S_CLOSED`.  See Peer_socket::info() doc header; this method is the entirety
   * of that method's implementation after `S_CLOSED` is eliminated as a possibility.
   *
   * Pre-conditions:
   *   - current thread is not W;
   *   - `sock->m_mutex` is locked and just after entering `sock->info()`;
   *   - no changes to *sock have been made since `m_mutex` was locked;
   *   - `sock->state() == Peer_socket::State::S_OPEN`.
   *
   * Post-condition (not exhaustive): `sock->m_mutex` is unlocked.
   *
   * @param sock
   *        Socket in consideration.
   * @return See Peer_socket::info().
   */
  Peer_socket_info sock_info(Peer_socket::Const_ptr sock);

  /**
   * Given a Peer_socket, copies all stats info (as available via Peer_socket::info()) from various
   * structures into the given stats `struct`.  This can then be logged, given to the user, etc.
   *
   * This should be run from thread W only.
   *
   * @param sock
   *        Socket in consideration.  It can be in any state, but see above.
   * @param stats
   *        All members (direct or indirect) of this `struct` will be filled.
   */
  void sock_load_info_struct(Peer_socket::Const_ptr sock, Peer_socket_info* stats) const;

  /**
   * Constructs the socket pair (connection ID) for the given socket.  For performance, try not to
   * use this, as this is usually already available in most points in Node code and can be passed
   * around to places where it's not.  However there are situations when one must reconstruct it
   * from a Peer_socket::Ptr alone.
   *
   * Call from thread W only.
   *
   * @todo Could make it a Socket_id constructor instead.
   * @param sock
   *        Source socket.
   * @return Ditto.
   */
  static Socket_id socket_id(Peer_socket::Const_ptr sock);

  /**
   * Obtain the sequence number for the datum just past the last (latest) In-flight (i.e., sent but
   * neither Acknowledged nor Dropped) packet, for the given socket.  If there are no In-flight
   * packets, returns the default Sequence_number -- which is < all other Sequence_numbers.
   *
   * Note that "last" in this case refers to position in the sequence number space, not time at which packets
   * are sent.  (A packet with a given Sequence_number may be sent several times due to retransmission.)
   *
   * @param sock
   *        Socket whose In-flight packets to examine.
   * @return See above.
   */
  static Sequence_number snd_past_last_flying_datum_seq_num(Peer_socket::Const_ptr sock);

  /**
   * Erases (for example if considered Acknowledged or Dropped) a packet `struct` from the
   * "scoreboard" (Peer_socket::m_snd_flying_pkts_by_sent_when) and adjusts all related structures.
   *
   * Note: It does NOT inform `sock->m_snd_drop_timer` (namely calling Drop_timer::on_packet_no_longer_in_flight()).
   * This is left to the caller; in particular because the timing may not be appropriate for what such a
   * call might trigger (e.g., on-Drop-Timeout actions such as massive retransmission).
   *
   * @param sock
   *        Socket to modify.
   * @param pkt_it
   *        Iterator into `m_snd_flying_pkts_by_sent_when` which will be deleted.
   */
  void snd_flying_pkts_erase_one(Peer_socket::Ptr sock, Peer_socket::Sent_pkt_ordered_by_when_iter pkt_it);

  /**
   * Adds a new packet `struct` (presumably representing packet to be sent shortly) to the
   * "scoreboard" (Peer_socket::m_snd_flying_pkts_by_sent_when) and adjusts all related structures as applicable.  Note,
   * however, that mark_data_packet_sent() is NOT called, because we should do that when the DATA
   * packet is actually sent (after pacing, if any).
   *
   * @param sock
   *        Socket to modify.
   * @param seq_num
   *        The first sequence number of the DATA packet.
   * @param sent_pkt
   *        Ref-counted pointer to new packet `struct`.
   */
  void snd_flying_pkts_push_one(Peer_socket::Ptr sock,
                                const Sequence_number& seq_num,
                                Peer_socket::Sent_packet::Ptr sent_pkt);

  /**
   * Updates Peer_socket::m_snd_flying_bytes according to an operation (add packets, remove packets)
   * caller is about to undertake or has just undertaken on Peer_socket::m_snd_flying_pkts_by_sent_when (= the
   * scoreboard).  Call this WHENEVER `m_snd_flying_pkts_by_sent_when` is about to be modified (if erasing) or
   * has just been modified (if adding) to ensure `m_snd_flying_bytes` is updated accordingly.
   *
   * @warning This has strong implications for congestion control!  Do not forget.
   * @param sock
   *        Socket to modify.
   * @param pkt_begin
   *        Iterator to first packet that was added or will be removed.
   * @param pkt_end
   *        Iterator one past the last packet that was added or will be removed.
   * @param added
   *        If `true`, the given range of packets was just added (e.g., Sent); if `false`, the given
   *        range of packets is about to be removed (e.g., Dropped or Acknowledged).
   */
  void snd_flying_pkts_updated(Peer_socket::Ptr sock,
                               Peer_socket::Sent_pkt_ordered_by_when_const_iter pkt_begin,
                               const Peer_socket::Sent_pkt_ordered_by_when_const_iter& pkt_end,
                               bool added);

  /**
   * Checks whether the given sent packet has been retransmitted the maximum number of allowed
   * times; if so then performs rst_and_close_connection_immediately() and returns `false`; otherwise
   * returns `true`.
   *
   * @param sock
   *        Socket to check and possibly close.
   * @param pkt_it
   *        Iterator info Peer_socket::m_snd_flying_pkts_by_sent_when of `sock` for packet in question.  Its
   *        `m_rexmit_id` should not yet be incremented for the potential new retransmission.
   * @param defer_delta_check
   *        Same meaning as in event_set_all_check_delta().
   * @return See above.
   */
  bool ok_to_rexmit_or_close(Peer_socket::Ptr sock,
                             const Peer_socket::Sent_pkt_ordered_by_when_iter& pkt_it,
                             bool defer_delta_check);

  /**
   * Logs a verbose state report for the given socket.  This is suitable for calling from
   * perform_regular_infrequent_tasks() and other infrequently executed spots.
   *
   * @param sock
   *        Socket whose state to log.
   */
  void sock_log_detail(Peer_socket::Const_ptr sock) const;

  /**
   * Assuming `*seq_num` points to the start of data.m_data, increments `*seq_num` to point
   * to the datum just past `data->m_data`.
   *
   * @param seq_num
   *        Pointer to sequence number to increment.
   * @param data
   *        DATA packet whose `m_data` to examine.
   */
  static void advance_seq_num(Sequence_number* seq_num,
                              boost::shared_ptr<const Data_packet> data);

  /**
   * Assuming `*seq_num` points to the start of some data of the given size, increments
   * `*seq_num` to point to the datum just past that amount of data.
   *
   * @param seq_num
   *        Pointer to sequence number to increment.
   * @param data_size
   *        Data size.
   */
  static void advance_seq_num(Sequence_number* seq_num, size_t data_size);

  /**
   * Given an iterator into a Peer_socket::Sent_pkt_by_sent_when_map or Peer_socket::Recv_pkt_map, gets the range of
   * sequence numbers in the packet represented thereby.
   *
   * @tparam Packet_map_iter
   *         Iterator type (`const` or otherwise) into one of the above-mentioned maps.
   * @param packet_it
   *        A valid, non-`end()` iterator into such a map.
   * @param seq_num_start
   *        If 0, ignored; otherwise the sequence number of the first datum in that packet is placed
   *        there.
   * @param seq_num_end
   *        If 0, ignored; otherwise the sequence number just past the last datum in that packet is
   *        placed there.
   */
  template<typename Packet_map_iter>
  static void get_seq_num_range(const Packet_map_iter& packet_it,
                                Sequence_number* seq_num_start, Sequence_number* seq_num_end);

  /**
   * Returns the "order number" to use for Peer_socket::Sent_packet::Sent_when structure corresponding to the next
   * packet to be sent.  This will be higher than the last sent packet's number.  Make sure you send packets
   * in exactly increasing numeric order of this order number.
   *
   * 0 is reserved and never returned by this.
   *
   * @param sock
   *        Socket to consider.
   * @return See above.
   */
  static Peer_socket::order_num_t sock_get_new_snd_order_num(Peer_socket::Ptr sock);

  /**
   * Internal factory used for ALL Peer_socket objects created by this Node (including subclasses).
   *
   * @param opts
   *        See Peer_socket::Peer_socket().
   * @return Pointer to newly constructed socket.
   */
  virtual Peer_socket* sock_create(const Peer_socket_options& opts);

  // Methods dealing with individual Server_sockets.  Implementations are in server_socket.cpp.

  /**
   * Implementation of non-blocking `serv->accept()` for server socket `serv` in all cases except when
   * `serv->state() == Server_socket::State::S_CLOSED`.
   *
   * Pre-conditions:
   *   - current thread is not W;
   *   - `serv->m_mutex` is locked and just after entering `serv->accept()`;
   *   - no changes to `*serv` have been made since `m_mutex` was locked;
   *   - `serv->state() != Server_socket::State::S_CLOSED` (so `serv` is in `m_servs`).
   *
   * This method completes the functionality of `serv->accept()`.
   *
   * @param serv
   *        Server socket, which must be in #m_servs, on which Server_socket::accept() was called.
   * @param err_code
   *        See Server_socket::accept().
   * @return See Server_socket::accept().
   */
  Peer_socket::Ptr accept(Server_socket::Ptr serv, Error_code* err_code);

  /**
   * Returns `true` if and only if calling `serv->accept()` with at least some arguments would return
   * either non-null (i.e., successfully dequeued a connected socket) or null and an error (but not
   * null and NO error).  `serv` will be locked and unlocked; safe to call from any thread.
   *
   * @param serv_as_any
   *        Socket to examine, as an `any` wrapping a Server_socket::Ptr.
   * @return See above.
   */
  bool serv_is_acceptable(const boost::any& serv_as_any) const;

  /**
   * Thread W implementation of listen().  Performs all the needed work, gives the resulting
   * Server_socket to the user thread, and signals that user thread.
   *
   * Pre-condition: We're in thread W; thread U != W is waiting for us to return having set `*serv`.  Post-condition:
   * `*serv` contains a `Server_socket::Ptr` in a Server_socket::State::S_LISTENING state if
   * `!(*serv)->m_disconnect_cause`; otherwise an error occurred, and that error is `(*serv)->m_disconnect_cause`.
   *
   * @param local_port
   *        See listen().
   * @param child_sock_opts
   *        See listen().
   * @param serv
   *        `*serv` shall be set to the resulting Server_socket.  Check `(*serv)->m_disconnect_cause`.
   */
  void listen_worker(flow_port_t local_port, const Peer_socket_options* child_sock_opts,
                     Server_socket::Ptr* serv);

  /**
   * Handles a just-deserialized, just-demultiplexed low-level SYN packet delivered to the given
   * server socket.  So it will hopefully create a #m_socks entry, send back a SYN_ACK, etc.
   *
   * @param serv
   *        Server socket in LISTENING state to which this SYN was demuxed.
   * @param syn
   *        Deserialized immutable SYN.
   * @param low_lvl_remote_endpoint
   *        The remote Node address.
   * @return New socket placed into Node socket table; or `Ptr()` on error, wherein no socket was saved.
   */
  Peer_socket::Ptr handle_syn_to_listening_server(Server_socket::Ptr serv,
                                                  boost::shared_ptr<const Syn_packet> syn,
                                                  const util::Udp_endpoint& low_lvl_remote_endpoint);

  /**
   * Handles a just-deserialized, just-demultiplexed low-level SYN_ACK_ACK packet delivered to the
   * given peer socket in Peer_socket::Int_state::S_SYN_RCVD state.  So it will hopefully finish up establishing
   * connection on our side.
   *
   * @param socket_id
   *        Connection ID (socket pair) identifying the socket in #m_socks.
   * @param sock
   *        Peer socket in Peer_socket::Int_state::S_SYN_RCVD.
   * @param syn_ack_ack
   *        Deserialized immutable SYN_ACK_ACK.
   */
  void handle_syn_ack_ack_to_syn_rcvd(const Socket_id& socket_id,
                                      Peer_socket::Ptr sock,
                                      boost::shared_ptr<const Syn_ack_ack_packet> syn_ack_ack);

  /**
   * Handles a just-deserialized, just-demultiplexed, low-level DATA packet delivered to the given
   * peer socket in SYN_RCVD state.  This is legitimate under loss and re-ordering conditions.
   * This will hopefully save the packet for later handling once we have entered ESTABLISHED state.
   *
   * @param sock
   *        Peer socket in Peer_socket::Int_state::S_SYN_RCVD.
   * @param packet
   *        Deserialized packet of type DATA.
   *        (For performance when moving data to Receive buffer, this is modifiable.)
   */
  void handle_data_to_syn_rcvd(Peer_socket::Ptr sock,
                               boost::shared_ptr<Data_packet> packet);

  /**
   * Handles the transition of the given server socket from `S_LISTENING`/`S_CLOSING` to `S_CLOSED`
   * (including eliminating the given Peer_socket from our data structures).
   *
   * Pre-condition: there is no socket `sock` such that `sock->m_originating_serv == serv`; i.e., there
   * are no sockets having to do with this server that have not yet been `accept()`ed.
   *
   * Pre-condition: `serv` is in `m_servs`; `serv->state() != S_OPEN`.
   *
   * Post-condition: `serv->state() == Server_socket::State::S_CLOSED` (and `serv` is no longer in `m_servs` or any
   * other Node structures, directly or indirectly) with `serv->m_disconnect_cause` set to `err_code` (or see
   * below).
   *
   * Any server socket that is in #m_servs MUST be eventually closed using this method.  No
   * socket that is not in #m_servs must be passed to this method.  In particular, do not call this
   * method during listen().
   *
   * @param local_port
   *        Flow port of the server to delete.
   * @param serv
   *        Socket to close.
   * @param err_code
   *        Why is it being closed?  Server_socket::m_disconnect_cause is set accordingly and logged.
   * @param defer_delta_check
   *        Same meaning as in event_set_all_check_delta().
   */
  void close_empty_server_immediately(const flow_port_t local_port, Server_socket::Ptr serv,
                                      const Error_code& err_code, bool defer_delta_check);

  /**
   * Sets Server_socket::m_state.  If moving to `S_CLOSED`, resets the required data to their "undefined" values
   * (e.g., `Server_socket::m_local_port = #S_PORT_ANY`).  Thread-safe.
   *
   * @param serv
   *        Server socket under consideration.
   * @param state
   *        New `m_state`.
   */
  void serv_set_state(Server_socket::Ptr serv, Server_socket::State state);

  /**
   * Records that thread W shows this socket is not to listen to incoming connections and is to
   * abort any not-yet-established (i.e., not yet queued) and established-but-unclaimed (i.e.,
   * queued) connections; and sets Server_socket::m_disconnect_cause and Server_socket::m_state in `serv` accordingly.
   * Thread-safe.
   *
   * @param serv
   *        Server socket under consideration.
   * @param disconnect_cause
   *        The cause of the disconnect.
   * @param close
   *        If `true`, the target public state should be the super-final `S_CLOSED`; if false, the target public state
   *        should be the ominous `S_CLOSING`. The caller's responsibility is to decide which one it
   *        is.
   */
  void serv_close_detected(Server_socket::Ptr serv, const Error_code& disconnect_cause, bool close);

  /**
   * Records that an unestablished socket `sock` (Peer_socket::Int_state::S_SYN_RCVD) has just become established
   * and can be `accept()`ed (Peer_socket::Int_state::S_ESTABLISHED).  Moves `sock` from
   * Server_socket::m_connecting_socks to Server_socket::m_unaccepted_socks (in `serv`).
   * To be called from thread W only.  Thread-safe.
   *
   * @param serv
   *        Server socket under consideration.
   * @param sock
   *        Socket that was just moved to Peer_socket::Int_state::S_ESTABLISHED.
   */
  void serv_peer_socket_acceptable(Server_socket::Ptr serv, Peer_socket::Ptr sock);

  /**
   * Records a new (just received SYN) peer socket from the given server socket.  Adds `sock` to
   * Server_socket::m_connecting_socks (in `serv`) and maintains the Peer_socket::m_originating_serv (in `sock`)
   * invariant.  To be called from thread W only.  Thread-safe.
   *
   * @param serv
   *        Server that originated `sock`.
   * @param sock
   *        Socket that was just moved to Peer_socket::Int_state::S_SYN_RCVD.
   */
  void serv_peer_socket_init(Server_socket::Ptr serv, Peer_socket::Ptr sock);

  /**
   * Records that a `Server_socket`-contained (i.e., currently un-established, or established but not yet accepted
   * by user) Peer_socket is being closed and should be removed from the given Server_socket.  To be
   * called from thread W only.  Thread-safe.
   *
   * If `sock` is not contained in `*serv`, method does nothing.
   *
   * @param serv
   *        Server socket under consideration.
   * @param sock
   *        Socket to remove (moving from `S_SYN_RCVD` or `S_ESTABLISHED` to `S_CLOSED`).
   */
  void serv_peer_socket_closed(Server_socket::Ptr serv, Peer_socket::Ptr sock);

  /**
   * Internal factory used for ALL Server_socket objects created by this Node (including subclasses).
   *
   * @param child_sock_opts
   *        See Server_socket::Server_socket().
   * @return Pointer to newly constructed socket.
   */
  virtual Server_socket* serv_create(const Peer_socket_options* child_sock_opts);

  // Methods dealing with individual Peer_sockets OR Server_sockets (determined via template at compile time).

  /**
   * Implementation of core *blocking* transfer methods, namely Peer_socket::sync_send(), Peer_socket::sync_receive(),
   * and Server_socket::sync_accept() for all cases except when `sock->state() == Peer_socket::State::S_CLOSED`.
   * It is heavily templated and shared among those three implementations to avoid massive
   * copy/pasting, since the basic pattern of the blocking wrapper around Event_set::sync_wait() and
   * a non-blocking operation (Peer_socket::receive(), Peer_socket::send(), Server_socket::accept(), respectively)
   * is the same in all cases.
   *
   * Pre-conditions:
   *   - current thread is not W;
   *   - `sock->m_mutex` is locked;
   *   - no changes to `*sock` have been made since `sock->m_mutex` was locked;
   *   - `sock->state()` is OPEN (so `sock` is in #m_socks or #m_servs, depending on socket type at compile time);
   *   - other arguments are as described below.
   *
   * This method completes the functionality of `sock->sync_send()`, `sock->sync_receive()`, and
   * `sock->sync_accept()`.
   *
   * @tparam Socket
   *         Underlying object of the transfer operation (Peer_socket or Server_socket).
   * @tparam Non_blocking_func_ret_type
   *         The return type of the calling transfer operation (`size_t` or Peer_socket::Ptr).
   * @param sock
   *        Socket on which user called `sync_*()`.
   * @param non_blocking_func
   *        When this method believes it should attempt a non-blocking transfer op, it will execute
   *        `non_blocking_func()`.
   *        If `non_blocking_func.empty()`, do not call `non_blocking_func()` --
   *        return indicating no error so far, and let them do actual operation, if they want; we just tell them it
   *        should be ready for them.  This is known
   *        as reactor pattern mode.  Otherwise, do the successful operation and then
   *        return.  This is arguably more typical.
   * @param would_block_ret_val
   *        The value that `non_blocking_func()` returns to indicate it was unable to perform the
   *        non-blocking operation (i.e., no data/sockets available).
   * @param ev_type
   *        Event type applicable to the type of operation this is.  See Event_set::Event_type doc header.
   * @param wait_until
   *        See `max_wait` argument on the originating `sync_*()` method.  This is absolute timeout time point
   *        derived from it; zero-valued if no timeout.
   * @param err_code
   *        See this argument on the originating `sync_*()` method.
   *        However, unlike that calling method's user-facing API, the present sync_op() method
   *        does NOT allow null `err_code` (behavior undefined if `err_code` is null).
   *        Corollary: we will NOT throw Runtime_error().
   * @return The value that the calling `sync_*()` method should return to its caller.
   *         Corner/special case: If `non_blocking_func.empty()` (a/k/a "reactor pattern" mode), then
   *         this will always return `would_block_ret_val`; the caller shall interpret
   *         `bool(*err_code) == false` as meaning the socket has reached the desired state in time and without
   *         error.  In that special case, as of this writing, you can't just return this return value, since it's
   *         always a zero/null/whatever.
   */
  template<typename Socket, typename Non_blocking_func_ret_type>
  Non_blocking_func_ret_type sync_op(typename Socket::Ptr sock,
                                     const Function<Non_blocking_func_ret_type ()>& non_blocking_func,
                                     Non_blocking_func_ret_type would_block_ret_val,
                                     Event_set::Event_type ev_type,
                                     const Fine_time_pt& wait_until,
                                     Error_code* err_code);

  /**
   * Helper method that checks whether the given Peer_socket or Server_socket is CLOSED; if so, it
   * sets `*err_code` to the reason it was closed (which is in `sock->m_disconnect`) and returns `false`;
   * otherwise it returns `true` and leaves `*err_code` untouched.  This exists to improve code reuse, as
   * this is a frequent operation for both socket types.
   *
   * Pre- and post-conditions: `sock->m_mutex` is locked.
   *
   * @tparam Socket_ptr
   *         Peer_socket::Ptr or Server_socket::Ptr.
   * @param sock
   *        The socket in question.
   * @param err_code
   *        `*err_code` is set to `sock->m_disconnect_cause` if socket is closed.
   * @return `true` if state is not CLOSED; otherwise `false`.
   */
  template<typename Socket_ptr>
  static bool ensure_sock_open(Socket_ptr sock, Error_code* err_code);

  // Methods dealing with individual Event_sets.  Implementations are in event_set.cpp.

  /**
   * Implementation of Event_set::async_wait() when `Event_set::state() == Event_set::State::S_INACTIVE`.
   *
   * Pre-conditions:
   *   - current thread is not W;
   *   - `event_set->m_mutex` is locked and just after entering async_wait();
   *   - no changes to `*event_set` have been made since `m_mutex` was locked;
   *   - `event_set->state() == Event_set::State::S_INACTIVE` (so `event_set` is in #m_event_sets);
   *   - on_event is as originally passed into async_wait().
   *
   * This method completes the functionality of `event_set->async_wait()`.
   *
   * @param event_set
   *        Event_set in question.
   * @param on_event
   *        See Event_set::async_wait().
   * @param err_code
   *        See Event_set::async_wait().
   * @return See Event_set::async_wait().
   */
  bool event_set_async_wait(Event_set::Ptr event_set, const Event_set::Event_handler& on_event,
                            Error_code* err_code);

  /**
   * Helper placed by event_set_async_wait() onto thread W to invoke event_set_check_baseline() but first ensure
   * that the `Event_set event_set` has not exited Event_set::State::S_WAITING (which would make any checking for
   * active events nonsense).  If it has exited that state, does nothing.  (That situation is possible due to
   * concurrently deleting the overarching Node (IIRC) and maybe other similar races.)
   *
   * @param event_set
   *        Event_set in question.
   */
  void event_set_check_baseline_assuming_state(Event_set::Ptr event_set);
  /**
   * Checks each desired (Event_set::m_want) event in `event_set`; any that holds true is saved into `event_set`
   * (Event_set::m_can).  This is the exhaustive, or "baseline," check.  This should only be performed when
   * necessary, as it is typically slower than checking individual active sockets against the
   * Event_set ("delta" check).
   *
   * This check is skipped if `Event_set::m_baseline_check_pending == false` (for `event_set`).
   *
   * See Event_set::async_wait() giant internal comment for context on all of the above.
   *
   * Pre-conditions: `event_set` state is Event_set::State::S_WAITING; `event_set->m_mutex` is locked.
   *
   * This method, unlike most, is intended to be called from either W or U != W.  All actions it
   * takes are on non-W-exclusive data (namely, actions on: `event_set`; and non-W-exclusive data in
   * Peer_socket and Server_socket, namely their state() and Receive/Send/Accept structures).
   *
   * @param event_set
   *        Event_set in question.
   * @return `true` if and only if the check was performed; `false` returned if
   *         `!event_set->m_baseline_check_pending`.
   */
  bool event_set_check_baseline(Event_set::Ptr event_set);

  /**
   * Check whether given Event_set contains any active sockets (Event_set::m_can); if so, signals the user (who
   * previously called `async_wait()` to set all this in motion): set state back to Event_set::State::S_INACTIVE from
   * Event_set::State::S_WAITING; calls the handler passed to `async_wait()`; forgets handler.  If no active sockets,
   * does nothing.
   *
   * Pre-conditions: same as event_set_check_baseline().
   *
   * @param event_set
   *        Event_set in question.
   */
  void event_set_fire_if_got_events(Event_set::Ptr event_set);

  /**
   * For each WAITING Event_set within the Node: checks for any events that hold, and if any do
   * hold, signals the user (calls handler, goes to INACTIVE, etc.).  The logic for how it does so
   * is complex.  For background, please see Event_set::async_wait() giant internal comment first.
   * Then read on here.
   *
   * For each WAITING Event_set: If baseline check (event_set_check_baseline()) is still required
   * and hasn't been performed, perform it.  Otherwise, for efficiency perform a "delta" check,
   * wherein EVERY active (for all definitions of active: Readable, Writable, Acceptable) socket
   * detected since the last baseline check is checked against the desired event/socket pairs in the
   * Event_set.  Any socket in both sets (active + desired) is saved in `event_set->m_can`.  If
   * either the baseline or delta check yields at least one active event, signal user (call handler,
   * go INACTIVE, etc.).
   *
   * For the delta check just described, how does it know which sockets have been active since the
   * last check?  Answer: `Node::m_sock_events` members (NOTE: not the same as `Event_set::m_can`, though
   * they are related).  See #m_sock_events doc header for details.
   *
   * @param defer_delta_check
   *        Set to `true` if and only if you know, for a FACT, that within a non-blocking amount of
   *        time `event_set_all_check_delta(false)` will be called.  For example, you may know
   *        `event_set_all_check_delta(false)` will be called within the present boost.asio handler.
   *        Then this method will only log and not perform the actual check, deferring to the
   *        promised `event_set_all_check_delta(false)` call, by which point more events may have been
   *        detected in #m_sock_events.
   */
  void event_set_all_check_delta(bool defer_delta_check);

  /**
   * Implementation of Event_set::close() when `Event_set::state() != Event_set::State::S_CLOSED` for `event_set`.
   *
   * Pre-conditions:
   *   - current thread is not W;
   *   - `event_set->m_mutex` is locked and just after entering async_wait();
   *   - no changes to `*event_set` have been made since `m_mutex` was locked;
   *   - `event_set->state() != Event_set::State::S_CLOSED` (so `event_set` is in #m_event_sets).
   *
   * This method completes the functionality of `event_set->close()`.
   *
   * @param event_set
   *        Event_set in question.
   * @param err_code
   *        See Event_set::close().
   */
  void event_set_close(Event_set::Ptr event_set, Error_code* err_code);

  /**
   * The guts of event_set_close_worker_check_state(): same thing, but assumes
   * `Event_set::state() == Event_set::State::S_CLOSED`, and Event_set::m_mutex is locked (for `event_set`).
   * May be called directly from thread W assuming those pre-conditions holds.
   *
   * @param event_set
   *        Event_set in question.
   */
  void event_set_close_worker(Event_set::Ptr event_set);

  /**
   * Thread W implementation of interrupt_all_waits().  Performs all the needed work, which is to
   * trigger any WAITING Event_set objects to fire their on-event callbacks, with the Boolean argument set
   * to `true`, indicating interrupted wait.
   *
   * Pre-condition: We're in thread W.
   */
  void interrupt_all_waits_worker();

  /**
   * `signal_set` handler, executed on SIGINT and SIGTERM, if user has enabled this feature:
   * causes interrupt_all_waits_worker() to occur on thread W.
   *
   * Pre-condition: We're in thread W [sic].
   *
   * @param sys_err_code
   *        boost.asio error code indicating the circumstances of the callback executing.
   *        It is unusual for this to be truthy.
   * @param sig_number
   *        Signal number of the signal that was detected.
   */
  void interrupt_all_waits_internal_sig_handler(const Error_code& sys_err_code, int sig_number);

  // Constants.

  /**
   * For a given unacknowledged sent packet P, the maximum number of times any individual packet
   * with higher sequence numbers than P may be acknowledged before P is considered Dropped (i.e.,
   * we give up on it).  If we enable retransmission, that would trigger Fast Retransmit, using TCP's
   * terminology.
   */
  static const Peer_socket::Sent_packet::ack_count_t S_MAX_LATER_ACKS_BEFORE_CONSIDERING_DROPPED;

  /**
   * Time interval between performing "infrequent periodic tasks," such as stat logging.  This
   * should be large enough to ensure that the tasks being performed incur no significant processor
   * use.
   */
  static const Fine_duration S_REGULAR_INFREQUENT_TASKS_PERIOD;

  // Data.

  /**
   * This Node's global set of options.  Initialized at construction; can be subsequently
   * modified by set_options(), although only the dynamic members of this may be modified.
   *
   * Accessed from thread W and user thread U != W.  Protected by #m_opts_mutex.  When reading, do
   * NOT access without locking (which is encapsulated in opt()).
   */
  Node_options m_opts;

  /// The mutex protecting #m_opts.
  mutable Options_mutex m_opts_mutex;

  /**
   * The object used to simulate stuff like packet loss and latency via local means directly in the
   * code.  If 0, no such simulation is performed.  `shared_ptr<>` used for basic auto-`delete` convenience.
   */
  boost::shared_ptr<Net_env_simulator> m_net_env_sim;

  /**
   * The main loop engine, functioning in the single-threaded-but-asynchronous callback-based
   * "reactor" style (or is it "proactor"?).  The Node constructor creates a single new thread W, which then places
   * some callbacks onto this guy and invoke `m_task_engine.run()`, at which point the main loop
   * begins in thread W.
   *
   * Thus, per boost.asio's model, any work items (functions) placed
   * onto #m_task_engine (e.g.: `post(m_task_engine, do_something_fn);`) will execute in thread W,
   * as it's the one invoking `run()` at the time -- even if the placing itself is done on some
   * other thread, such as a user thread U.  An example of the latter is a Peer_socket::send() implementation
   * might write to the socket's internal Send buffer in thread U, check whether it's currently possible
   * to send over the wire, and if and only if the answer is yes, `post(m_task_engine, S)`, where S
   * is a function/functor (created via lambdas usually) that will perform the hairy needed Node/socket
   * work on thread W.
   *
   * All threads may access this (no mutex required, as explicitly announced in boost.asio docs).
   *
   * Adding more threads that would call `m_task_engine.run()` would create a thread pool.  With "strands" one
   * can avoid concurrency in this situation.  An intelligent combination of those two concepts can lead to efficient
   * multi-core use without complex and/or inefficient locking.  This is non-trivial.
   *
   * @see Class Node doc header for to-do items regarding efficient multi-core use and how that relates to
   * using an #m_task_engine thread pool and/or strands.
   */
  util::Task_engine m_task_engine;

  /**
   * The UDP socket used to receive low-level packets (to assemble into application layer data) and send them
   * (vice versa).
   *
   * Only thread W can access this.
   *
   * Access to this may be highly contentious in high-traffic situations.  Since only thread W accesses this, and that
   * thread does the vast bulk of the work of the entire Node, at least one known problem is that the internal OS
   * UDP receive buffer may be exceeded, as we may not read datagrams off this socket quickly enough.
   *
   * @see Class Node doc header for to-do items regarding the aforementioned UDP receive buffer overflow problem.
   */
  Udp_socket m_low_lvl_sock;

  /**
   * After we bind #m_low_lvl_sock to a UDP endpoint, this is a copy of that endpoint.  Thus it
   * should contain the actual local address and port (even if user specified 0 for the latter,
   * say).
   *
   * This is equal to `Udp_endpoint()` until the constructor exits.  After the constructor exits, its
   * value never changes, therefore all threads can access it without mutex.  If the constructor
   * fails to bind, this remains equal to `Udp_endpoint()` forever.
   */
  util::Udp_endpoint m_low_lvl_endpoint;

  /**
   * OS-reported #m_low_lvl_sock UDP receive buffer maximum size, obtained right after we
   * OS-set that setting and never changed subsequently.  Note the OS may not respect whatever value we
   * passed into the OS socket option setting call, or it may respect it but only approximately.
   */
  size_t m_low_lvl_max_buf_size;

  /// Stores incoming raw packet data; re-used repeatedly for possible performance gains.
  util::Blob m_packet_data;

  /// Flow port space for both client and server sockets.  All threads may access this.
  Port_space m_ports;

  /// Sequence number generator (at least to generate ISNs).  Only thread W can access this.
  Sequence_number::Generator m_seq_num_generator;

  /**
   * Random number generator for picking security tokens; seeded on time at Node construction and generates
   * integers from the entire range.  (Not thread-safe.  Use only in thread W.)
   */
  util::Rnd_gen_uniform_range<Peer_socket::security_token_t> m_rnd_security_tokens;

  /**
   * The peer-to-peer connections this Node is currently tracking.  Their states are not Peer_socket::State::S_CLOSED.
   * Only thread W can access this.
   */
  Socket_id_to_socket_map m_socks;

  /**
   * The server sockets this Node is currently tracking.  Their states are not Server_socket::State::S_CLOSED.
   * Only thread W can access this.
   */
  Port_to_server_map m_servs;

  /**
   * Every Event_set to have been returned by event_set_create() and not subsequently reached
   * Event_set::State::S_CLOSED.  Only thread W can access this.
   */
  Event_sets m_event_sets;

  /**
   * All sockets that have been detected to be "ready" (by the Event_set doc header definition) at
   * any point since the last time #m_sock_events's contained sets were cleared (which happens initially and after each
   * event_set_all_check_delta() call).  EVERY piece of code in thread W to potentially set a
   * socket's status to "ready" (e.g.: DATA received, error detected) MUST add that socket's handle
   * to this data structure.  This enables the Event_set machinery to efficiently but thoroughly
   * detect every event in which the Event_set user is interested.  The theory behind this is
   * described in the giant comment inside Event_set::async_wait().
   *
   * This maps Event_set::Event_type `enum` members to Event_set::Sockets socket sets, exactly the same way
   * Event_set::m_can and Event_set::m_want are set up.
   *
   * A question arises: why use this set to store such active sockets?  Why not just call
   * event_set_all_check_delta() EVERY time we see a socket is now Readable, etc., thus handling it right
   * away and not needing to store it?  Answer: we could.  However, we want to collect as many
   * possibly active events as possible, without blocking, before performing the check.  That way
   * the user is informed of as many events as possible, instead of the very first one (when there
   * could be hundreds more; for example if hundreds of DATA packets have arrived simultaneously).
   * The theory behind this is also discussed in Event_set::async_wait() giant comment.  So we
   * insert into #m_sock_events and defer `event_set_all_check_delta(false)` to the end of the current
   * boost.asio handler, since we know we won't block (sleep) until the handler exits.
   *
   * Only thread W can access this.
   */
  Event_set::Ev_type_to_socks_map m_sock_events;

  /**
   * Within a given low_lvl_recv_and_handle() or async part of async_wait_latency_then_handle_incoming() (async part)
   * call, by the time perform_accumulated_on_recv_tasks() is called, this stores exactly those sockets for which
   * possible ACK sending tasks have been accumulated during the low_lvl_recv_and_handle()/etc.
   * call.  The idea is that, for efficiency and reduced overhead,
   * all simultaneously available incoming data are examined first, and some tasks are accumulated
   * to perform at the end.  For example, all DATA packets to be acknowledged at the same time are
   * collected and then sent in as few ACKs as possible.
   *
   * Details on the acks to potentially send are stored within that Peer_socket itself (e.g.,
   * Peer_socket::m_rcv_pending_acks).
   *
   * This should be added to throughout the method, used in perform_accumulated_on_recv_tasks(), and
   * then cleared for the next run.
   *
   * Only thread W can access this.
   */
  boost::unordered_set<Peer_socket::Ptr> m_socks_with_accumulated_pending_acks;

  /**
   * Within a given low_lvl_recv_and_handle() or async part of async_wait_latency_then_handle_incoming() call,
   * by the time perform_accumulated_on_recv_tasks() is called, this stores exactly those sockets for which
   * possible incoming-ACK handling tasks have been accumulated during the low_lvl_recv_and_handle()/etc.
   * call.  The idea is that, for congestion control robustness,
   * all simultaneously available acknowledgments and rcv_wnd updates are collected first, and then
   * they're all handled together at the end.
   *
   * Details on the acks to potentially send are stored within that Peer_socket
   * itself (Peer_socket::m_rcv_acked_packets scan all).
   *
   * This should be added to throughout the method, used in perform_accumulated_on_recv_tasks(), and
   * then cleared for the next run.
   *
   * Only thread W can access this.
   */
  boost::unordered_set<Peer_socket::Ptr> m_socks_with_accumulated_acks;

  /**
   * For debugging, when we detect loss of data we'd sent, we log the corresponding socket's state;
   * this is the last time this was done for any socket (or epoch if never).  It's used to
   * throttle such messages, since they are CPU-intensive and disk-intensive (when logging to disk).
   */
  Fine_time_pt m_last_loss_sock_log_when;

  /**
   * Promise that thread W sets to truthy `Error_code` if it fails to initialize or falsy once event loop is running.
   * The truthy payload can be returned or thrown inside an error::Runtime_exception if desired.
   */
  boost::promise<Error_code> m_event_loop_ready;

  /// The future object through which the non-W thread waits for #m_event_loop_ready to be set to success/failure.
  boost::unique_future<Error_code> m_event_loop_ready_result;

  /**
   * Signal set which we may or may not be using to trap SIGINT and SIGTERM in order to auto-fire
   * interrupt_all_waits().  `add()` is called on it at initialization if and only if that feature is enabled
   * by the user via `Node_options`.  Otherwise this object just does nothing for the Node's lifetime.
   */
  Signal_set m_signal_set;

  /// Worker thread (= thread W).  Other members should be initialized before this to avoid race condition.
  util::Thread m_worker;
}; // class Node

/**
 * @private
 *
 * The data nugget uniquely identifying a peer-to-peer connection from a remote endpoint to
 * a port in this Node.  Its (unmodifiable after initialization) fields are to be constructed via direct
 * initialization (assuming the defaults are unacceptable).
 */
struct Node::Socket_id
{
  // Data.

  /// The other side of the connection.
  const Remote_endpoint m_remote_endpoint = Remote_endpoint();
  /// This side of the connection (within this Node).
  const flow_port_t m_local_port = S_PORT_ANY;

  // Methods.

  /**
   * Hash value of this Socket_id for `unordered<>`.
   * @return Ditto.
   */
  size_t hash() const;
};

// Free functions: in *_fwd.hpp.

// However the following refer to inner type(s) and hence must be declared here and not _fwd.hpp.

/**
 * @internal
 *
 * Free function that returns socket_id.hash(); has to be a free function named `hash_value()` for
 * boost.hash to pick it up.
 *
 * @relatesalso Node::Socket_id
 *
 * @param socket_id
 *        Socket ID to hash.
 * @return socket_id.hash().
 */
size_t hash_value(const Node::Socket_id& socket_id);

/**
 * @internal
 *
 * Whether `lhs` is equal to `rhs`.
 *
 * @relatesalso Node::Socket_id
 * @param lhs
 *        Object to compare.
 * @param rhs
 *        Object to compare.
 * @return See above.
 */
bool operator==(const Node::Socket_id& lhs, const Node::Socket_id& rhs);

// Template implementations.

template<typename Rep, typename Period>
Peer_socket::Ptr Node::sync_connect_with_metadata(const Remote_endpoint& to,
                                                  const boost::chrono::duration<Rep, Period>& max_wait,
                                                  const boost::asio::const_buffer& serialized_metadata,
                                                  Error_code* err_code,
                                                  const Peer_socket_options* opts)
{
  assert(max_wait.count() > 0);
  return sync_connect_impl(to, util::chrono_duration_to_fine_duration(max_wait), serialized_metadata, err_code, opts);
}

template<typename Rep, typename Period>
Peer_socket::Ptr Node::sync_connect(const Remote_endpoint& to,
                                    const boost::chrono::duration<Rep, Period>& max_wait,
                                    Error_code* err_code, const Peer_socket_options* opts)
{
  return sync_connect_with_metadata(to, max_wait,
                                    boost::asio::buffer(&S_DEFAULT_CONN_METADATA, sizeof(S_DEFAULT_CONN_METADATA)),
                                    err_code, opts);
}

template<typename Socket, typename Non_blocking_func_ret_type>
Non_blocking_func_ret_type Node::sync_op(typename Socket::Ptr sock,
                                         const Function<Non_blocking_func_ret_type ()>& non_blocking_func,
                                         Non_blocking_func_ret_type would_block_ret_val,
                                         Event_set::Event_type ev_type,
                                         const Fine_time_pt& wait_until,
                                         Error_code* err_code)
{
  using boost::adopt_lock;
  using boost::chrono::milliseconds;
  using boost::chrono::round;

  // We are in user thread U != W.

  {
    /* WARNING!!!  sock->m_mutex is locked, but WE must unlock it before returning!  Can't leave that
     * to the caller, because we must unlock at a specific point below, right before sync_wait()ing.
     * Use a Lock_guard that adopts an already-locked mutex. */
    typename Socket::Lock_guard lock(sock->m_mutex, adopt_lock);

    if (!running())
    {
      FLOW_ERROR_EMIT_ERROR(error::Code::S_NODE_NOT_RUNNING);
      return would_block_ret_val;
    }
    // else

    /* Unlock.  Why?  Simply because we can't forbid other threads from accessing sock while we
     * shamelessly block (in sync_wait()).  */
  } // Lock.

  /* This is actually pretty simple.  We create an Event_set with just the one event we care
   * about (e.g., sock is Writable) and sync_wait() for it.  Then we invoke non_blocking_func()
   * (e.g., Node::send()) once that event holds. So, create Event_set. */

  /* Note that we assume "this" remains valid throughout this method until we start sleeping
   * (sync_wait()).  This is explicitly guaranteed by the "Thread safety" note in class Node doc
   * header (which says that Node deletion is allowed only once a blocking operation's sleep has
   * been entered). */

  const Event_set::Ptr event_set = event_set_create(err_code);
  if (!event_set)
  {
    return would_block_ret_val; // *err_code is set.  This is pretty weird but nothing we can do.
  }
  // else event_set ready.

  // We must clean up event_set at any return point below.
  util::Auto_cleanup cleanup = util::setup_auto_cleanup([&]()
  {
    // Eat any error when closing Event_set, as it's unlikely and not interesting to user.
  	Error_code dummy_prevents_throw;
  	event_set->close(&dummy_prevents_throw);
  });

  // We care about just this event, ev_type.
  if (!(event_set->add_wanted_socket<Socket>(sock, ev_type, err_code)))
  {
    return would_block_ret_val; // *err_code is set.  Node must have shut down or something.
  }
  // else go ahead and wait.

  Non_blocking_func_ret_type op_result;
  const bool timeout_given = wait_until != Fine_time_pt();
  do
  {
    // We may have to call sync_wait() repeatedly; if timeout is given we must give less and less time each time.
    bool wait_result;
    if (timeout_given)
    {
      // Negative is OK but cleaner to clamp it to 0.
      const Fine_duration time_remaining = std::max(Fine_duration::zero(), wait_until - Fine_clock::now());

      /* Do NOT log.  We have waited already, so `this` Node may have been deleted, so get_logger() may be undefined!
       * @todo I don't like this.  Want to log it.  Maybe get rid of the allowing for `this` deletion during wait.
       * We don't allow it in async_*() case for instance. */
      /* FLOW_LOG_TRACE("Waiting again; timeout reduced "
       *                "to [" << round<milliseconds>(time_remaining) << "] = [" << time_remaining << "]."); */

      // Perform the wait until event detected, time_remaining expires, or wait is interrupted (a-la EINTR).
      wait_result = event_set->sync_wait(time_remaining, err_code);
    }
    else
    {
      // No timeout given.  Perform the wait until event detected, or wait is interrupted (a-la EINTR).
      wait_result = event_set->sync_wait(err_code);
    }

    if (!wait_result)
    {
      /* *err_code is set.  Node must have shut down or something; or, maybe more likely, S_WAIT_INTERRUPTED
       * or S_WAIT_USER_TIMEOUT occurred.  In all cases, it's correct to pass it on to our caller. */
      return would_block_ret_val;
    }
    // else sync_wait() has returned success.

    // Warning: "this" may be have been deleted by any point below this line, unless specifically guaranteed.

#ifndef NDEBUG
    const bool active = event_set->events_detected(err_code);
#endif
    assert(active); // Inactive but no error, so it must have been a timeout -- but that should've been error.

    /* OK.  sync_wait() reports event is ready (sock is active, e.g., Writable).  Try to perform
     * non-blocking operation (e.g., Node::send()).  We must lock again (to access m_node again,
     * plus it's a pre-condition of the non-blocking operation (e.g., Node::send()).  In the
     * meantime sock may have gotten closed. Ensure that's not so (another pre-condition).
     *
     * Alternatively, in reactor-pattern mode, they want us to basically do a glorified sync_wait() for
     * one of the 3 events, depending on ev_type, and just return without performing any non_blocking_func();
     * in fact this mode is indicated by non_blocking_func.empty(). */

    {
      typename Socket::Lock_guard lock(sock->m_mutex);
      if (sock->m_state == Socket::State::S_CLOSED) // As in the invoker of this method....
      {
        assert(sock->m_disconnect_cause);
        *err_code = sock->m_disconnect_cause;
        // Do NOT log.  "this" Node may have been deleted, so get_logger() may be undefined!

        return would_block_ret_val;
      }
      // else do it.  Note that `this` is guaranteed to still exist if sock->m_state is not CLOSED.

      if (non_blocking_func.empty())
      {
      	FLOW_LOG_TRACE("Sync op of type [" << ev_type << "] with Event_set [" << event_set << "] in reactor pattern "
      		             "mode on object [" << sock << "] successful; returning without non-blocking op.");
        assert(!*err_code); // In reactor pattern mode: No error <=> yes, socket is in desired state now.
      	return would_block_ret_val;
      }

      op_result = non_blocking_func(); // send(), receive(), accept(), etc.
    } // Lock.
    // Cannot log below this line for aforementioned reasons.  Also cannot log in subsequent iterations!

    if (*err_code)
    {
      // *err_code is set.  Any number of errors possible here; error on socket => socket is active.
      return would_block_ret_val;
    }
    // else no error.

    /* If op_result > 0, then data was transferred (enqueued to Send buffer, dequeued from Receive
     * buffer or Accept queue, etc.); cool.  If op_result == 0, sock is still not active.  How
     * is that possible if sync_wait() returned non-zero events?  Because some jerk in another
     * thread may also be non_blocking_func()ing at the same time.  In that case we must try again
     * (must not return would_block_ret_val and no error).  (And give less time, if timeout was
     * provided.) */

    // Do NOT log as explained.  @todo I don't like this.  See similar @todo above.
    /* if (op_result == would_block_ret_val)
     * {
     *   // Rare/interesting enough for INFO.
     *   FLOW_LOG_INFO('[' << sock << "] got Active status "
     *                "but the non-blocking operation still returned would-block.  Another thread is interfering?");
     * } */
  }
  while (op_result == would_block_ret_val);

  // Excellent.  At least some data was transferred (e.g., enqueued on Send buffer).  *err_code is success.
  return op_result;
} // Node::sync_op()

template<typename Socket_ptr>
bool Node::ensure_sock_open(Socket_ptr sock, Error_code* err_code) // Static.
{
  // Pre-condition: sock is suitably locked.  We are in thread U != W or W.

  if (sock->m_state == Socket_ptr::element_type::State::S_CLOSED)
  {
    // CLOSED socket -- Node has disowned us.
    assert(!sock->m_node);
    assert(sock->m_disconnect_cause);

    // Mark (already-determined) error in *err_code and log.
    FLOW_LOG_SET_CONTEXT(sock->get_logger(), sock->get_log_component()); // Static, so must do <--that to log this--v.
    FLOW_ERROR_EMIT_ERROR_LOG_INFO(sock->m_disconnect_cause);
    return false;
  }
  // else
  return true;
} // Node::ensure_sock_open()

template<typename Opt_type>
bool Node::validate_static_option(const Opt_type& new_val, const Opt_type& old_val, const std::string& opt_id,
                                  Error_code* err_code) const
{
  using std::string;

  if (new_val != old_val)
  {
    const string& opt_id_nice = Node_options::opt_id_to_str(opt_id);
    FLOW_LOG_WARNING("Option [" << opt_id_nice << "] is static, but attempted to change "
                     "from [" << old_val << "] to [" << new_val << "].  Ignoring entire option set.");
    FLOW_ERROR_EMIT_ERROR(error::Code::S_STATIC_OPTION_CHANGED);
    return false;
  }
  // else

  return true;
}

template<typename Opt_type>
Opt_type Node::opt(const Opt_type& opt_val_ref) const
{
  /* They've given the REFERENCE to the value they want to read.  Another thread may write to that
   * value concurrently.  Therefore, acquire ownership of the enclosing m_opts.  Copy the value.  Unlock.
   * Then return the copy.  Most options are small (typically primitive types, typically integers;
   * or boost.chrono time values which are internally also usually just integers), so the copy
   * should not be a big deal. */

  Options_lock lock(m_opts_mutex);
  return opt_val_ref;
}

template<typename Peer_socket_impl_type>
Peer_socket* Node::sock_create_forward_plus_ctor_args(const Peer_socket_options& opts)
{
  return new Peer_socket_impl_type(get_logger(), &m_task_engine, opts);
}

template<typename Server_socket_impl_type>
Server_socket* Node::serv_create_forward_plus_ctor_args(const Peer_socket_options* child_sock_opts)
{
  return new Server_socket_impl_type(get_logger(), child_sock_opts);
}

} // namespace flow::net_flow
