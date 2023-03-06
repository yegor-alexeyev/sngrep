//
// Copyright (c) 2016-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: WebSocket server, coroutine
//
//------------------------------------------------------------------------------
#include "server.h"
#include "sip_msg.h"
#include "sip_call.h"

#include <set>

#include <boost/asio/experimental/concurrent_channel.hpp>

#include <boost/algorithm/string.hpp>


#include <boost/process/v2/process.hpp>
#include <boost/process/v2/popen.hpp>

#include  <functional>

#include <boost/bimap.hpp>
#include <boost/bimap/multiset_of.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/read_until.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

//------------------------------------------------------------------------------

typedef beast::websocket::stream<beast::tcp_stream> Websocket;
typedef std::shared_ptr<Websocket> WebsocketPtr;

    

// The io_context is required for all I/O
net::io_context context;

struct Leg
{
    explicit Leg()
    {
        /*
            Should not happen
        */
        exit(140);
    }

    explicit Leg(struct sip_call * call)
    {
        call_id = std::string(call->callid);
        state = (call_state)call->state;
    }

    std::string call_id;
    call_state state;
};

typedef std::reference_wrapper<Leg> LegRef;



typedef boost::bimap<boost::bimaps::set_of<std::string>, boost::bimaps::multiset_of<std::string> > EgressIngressMap;

typedef std::map<std::string, Leg> Legs;

EgressIngressMap egress_ingress_map;
Legs legs;


typedef boost::asio::experimental::concurrent_channel<void(boost::system::error_code, Leg)> MessageChannel;

MessageChannel sngrep_channel(context, 0);

typedef Leg SipCallData;

typedef std::shared_ptr<SipCallData> SipCallDataPtr;

std::set<WebsocketPtr> established_sessions;

//------------------------------------------------------------------------------

// Report a failure
void
fail(beast::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

void process_messages(
    websocket::stream<beast::tcp_stream>& ws,
    net::yield_context yield)
{
    beast::error_code ec;

    for(;;)
    {
        // This buffer will hold the incoming message
        beast::flat_buffer buffer;

        // Read a message
        ws.async_read(buffer, yield[ec]);

        // This indicates that the session was closed
        if(ec == websocket::error::closed)
            break;

        if(ec)
            return fail(ec, "read");

        // Echo the message back
        ws.text(ws.got_text());
        ws.async_write(boost::asio::buffer("Requests are not supported yet"), yield[ec]);
        if(ec)
            return fail(ec, "write");
    }
}

// Echoes back all received WebSocket messages
void
do_session(
    tcp::socket& socket,
    net::yield_context yield)
{
    beast::error_code ec;

    WebsocketPtr ws = std::make_shared<Websocket>( std::move(socket));

    // Set suggested timeout settings for the websocket
    ws->set_option(
        websocket::stream_base::timeout::suggested(
            beast::role_type::server));

    // Set a decorator to change the Server of the handshake
    ws->set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res)
        {
            res.set(http::field::server,
                std::string(BOOST_BEAST_VERSION_STRING) +
                    " websocket-server-coro");
        }));

    // Accept the websocket handshake
    ws->async_accept(yield[ec]);
    if(ec)
        return fail(ec, "accept");


    established_sessions.insert(ws);

    process_messages(*ws, yield);

    established_sessions.erase(ws);

}

//------------------------------------------------------------------------------

void
do_multiplex(net::yield_context yield)
{

    std::string out;
    boost::system::error_code ec;

    while (true)
    {
        Leg what = sngrep_channel.async_receive( yield[ec]);
        for ( auto socket: established_sessions)
        {
            socket->write(boost::asio::buffer(what.call_id /*TODO*/), ec);
        }
    }
}

// Accepts incoming connections and launches the sessions
void
do_listen(
    net::io_context& ioc,
    tcp::endpoint endpoint,
    net::yield_context yield)
{
    beast::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(ioc);
    acceptor.open(endpoint.protocol(), ec);
    if(ec)
        return fail(ec, "open");

    // Allow address reuse
    acceptor.set_option(net::socket_base::reuse_address(true), ec);
    if(ec)
        return fail(ec, "set_option");

    // Bind to the server address
    acceptor.bind(endpoint, ec);
    if(ec)
        return fail(ec, "bind");

    // Start listening for connections
    acceptor.listen(net::socket_base::max_listen_connections, ec);
    if(ec)
        return fail(ec, "listen");

    for(;;)
    {
        tcp::socket socket(ioc);
        acceptor.async_accept(socket, yield[ec]);
        if(ec)
            fail(ec, "accept");
        else
        {
            socket.non_blocking(true);
            boost::asio::socket_base::send_buffer_size option(256000);
            socket.set_option(option);

            boost::asio::spawn(
                acceptor.get_executor(),
                std::bind(
                    &do_session, std::move(socket), std::placeholders::_1));
        }
    }
}

template <typename T>
std::string async_read_line(T& proc, std::string& buffer, net::yield_context yield)
{
    boost::system::error_code ec;
    const size_t n_read = boost::asio::async_read_until(proc, boost::asio::dynamic_buffer(buffer), '\n', yield[ec]);

    if (ec || n_read == 0)
    {
        exit(100);
    }

    const std::string line = buffer.substr(0, n_read - 1);
    buffer.erase(0, n_read);
    return line;
}

void
do_active_call_processor( net::io_context& ioc, net::yield_context yield)
{
    boost::system::error_code ec;
    boost::process::v2::popen proc(ioc, "get_active_call.expect", {});

    std::string buf;
    while (true)
    {
        const std::string result = async_read_line(proc, buf, yield);

        std::vector<std::string> fields;
        boost::split(fields,result, boost::algorithm::is_any_of(";"));

        printf("what %d\n", fields.size());
    }
}

void server_thread()
{
    auto const address = net::ip::make_address("127.0.0.1");
    auto const port = static_cast<unsigned short>(8080);

    // Spawns an active call script processor
    boost::asio::spawn(context,
        std::bind( &do_active_call_processor, std::ref(context), std::placeholders::_1)
    );

    // Spawn a listening port
    boost::asio::spawn(context,
        std::bind(
            &do_listen,
            std::ref(context),
            tcp::endpoint{address, port},
            std::placeholders::_1));

    //spawn a multiplexer
    boost::asio::spawn(context,
        std::bind(
            &do_multiplex, std::placeholders::_1));

    context.run();

    /* return EXIT_SUCCESS; */
}

/* void process_sip_data(const SipCallDataPtr& sip_call_data) */
/* { */
/*     for (auto& websocket_sip_data: established_sessions) */
/*     { */
/*         if (websocket_sip_data.second) */
/*         { */
/*             printf("dropping update\n"); */
/*         } */
/*         else */
/*         { */
/*             websocket_sip_data.second = sip_call_data; */
/*             websocket_sip_data.first->async_write(boost::asio::buffer(sip_call_data->call_id), */ 
/*                 [&data=websocket_sip_data.second] (beast::error_code const& ec, std::size_t bytes_transferred) */ 
/*                 { */
/*                     data.reset(); */
/*                 } */
/*             ); */
/*         } */
/*     } */
/* } */

void on_new_sip_message(struct sip_msg * msg)
{
    if (!msg->call) {
        exit(81);
    }
    if (!msg->call->callid) {
        exit(82);
    }

    /* boost::asio::post(context, */
    /*     std::bind( */
    /*         &process_sip_data, */
    /*         SipCallDataPtr(new SipCallData({std::string(msg->call->callid)})) */
    /*     ) */
    /* ); */

    Leg leg(msg->call);

    /* std::string call_id( msg->call->callid ); */

    /* legs.try_emplace( call_id, msg->call); */

    sngrep_channel.try_send(boost::asio::error::eof, leg);
}

void on_new_active_call_info(const std::map<std::string, std::string> call_info)
{
}
