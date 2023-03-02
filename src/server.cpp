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

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/spawn.hpp>
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

typedef boost::asio::experimental::concurrent_channel<void(boost::system::error_code, std::string)> MessageChannel;

MessageChannel sngrep_channel(context, 0);

struct SipCallData
{
    std::string call_id;

};
typedef std::shared_ptr<SipCallData> SipCallDataPtr;

std::map<WebsocketPtr, SipCallDataPtr> established_sessions;

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


    established_sessions[ws] = SipCallDataPtr();

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
        std::string what = sngrep_channel.async_receive( yield[ec]);
        for ( auto socket_buffer: established_sessions)
        {
            if (!socket_buffer.second) {
                socket_buffer.second = SipCallDataPtr( new SipCallData({what}));
                socket_buffer.first->async_write(boost::asio::buffer(socket_buffer.second->call_id),
                    [&data=socket_buffer.second] (beast::error_code const& ec, std::size_t bytes_transferred) 
                    {
                        data.reset();
                    }
                );
            }
            else
            {
                printf("dropping distribution\n");
            }
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
            boost::asio::spawn(
                acceptor.get_executor(),
                std::bind(
                    &do_session, std::move(socket), std::placeholders::_1));
    }
}


void server_thread()
{
    auto const address = net::ip::make_address("127.0.0.1");
    auto const port = static_cast<unsigned short>(8080);


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

void process_sip_data(const SipCallDataPtr& sip_call_data)
{
    for (auto& websocket_sip_data: established_sessions)
    {
        if (websocket_sip_data.second)
        {
            printf("dropping update\n");
        }
        else
        {
            websocket_sip_data.second = sip_call_data;
            websocket_sip_data.first->async_write(boost::asio::buffer(sip_call_data->call_id), 
                [&data=websocket_sip_data.second] (beast::error_code const& ec, std::size_t bytes_transferred) 
                {
                    data.reset();
                }
            );
        }
    }
}

void on_new_sip_message(struct sip_msg * msg)
{
    if (!msg->call) {
        exit(81);
    }

    /* boost::asio::post(context, */
    /*     std::bind( */
    /*         &process_sip_data, */
    /*         SipCallDataPtr(new SipCallData({std::string(msg->call->callid)})) */
    /*     ) */
    /* ); */
    sngrep_channel.try_send(boost::asio::error::eof, std::string(msg->call->callid));

}

