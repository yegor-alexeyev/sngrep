#define BOOST_STACKTRACE_USE_BACKTRACE
#include <boost/stacktrace.hpp>

#include "server.h"
#include "state.h"




/* #include <boost/range/combine.hpp> */

#include <boost/asio/experimental/concurrent_channel.hpp>


#include <boost/process/v2/process.hpp>
#include <boost/process/v2/popen.hpp>


#include  <functional>


#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/read_until.hpp>

#include <optional>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <fstream>

#include <signal.h>



namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

//------------------------------------------------------------------------------

typedef beast::websocket::stream<beast::tcp_stream> Websocket;
typedef std::shared_ptr<Websocket> WebsocketPtr;
typedef std::weak_ptr<Websocket> WebsocketWeakPtr;



// The io_context is required for all I/O
net::io_context context;

boost::process::v2::popen call_processor(context);






struct ClientMessage
{
    WebsocketWeakPtr client;
    std::string command;
};

typedef std::variant<SipCall, ClientMessage> Message;

typedef boost::asio::experimental::concurrent_channel<void(boost::system::error_code, Message)> MessageChannel;

MessageChannel sngrep_channel(context, 100);

/* typedef SipCall SipCallData; */

/* typedef std::shared_ptr<SipCallData> SipCallDataPtr; */

struct Session
{
    std::map<std::string, std::string> filter;
};

std::map<WebsocketPtr, Session> established_sessions;

//------------------------------------------------------------------------------

// Report a failure
void
fail(beast::error_code ec, char const* what)
{
    std::cout << what << ": " << ec.message() << "\n";
}


void process_messages(
    std::map<WebsocketPtr, Session>::iterator session,
    net::yield_context yield)
{
    boost::system::error_code ec;

    for(;;)
    {
        // This buffer will hold the incoming message
        beast::flat_buffer buffer;

        // Read a message
        session->first->async_read(buffer, yield[ec]);

        // This indicates that the session was closed
        if(ec == websocket::error::closed)
            break;

        if(ec)
            return fail(ec, "read");

        const std::string message = beast::buffers_to_string(buffer.data());


        std::map<std::string, std::string> members = collect_string_members(message);

        if (members.count("command") == 0)
        {
            return fail(ec, "invalid json: missing command attribute");
        }


        if (members["command"] == "subscribe")
        {
            session->second.filter = members;
            session->second.filter.erase("command");
        }
        else
        {
            sngrep_channel.try_send(boost::asio::error::eof, ClientMessage({session->first, members["command"] }));
        }


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


    auto session_it = established_sessions.try_emplace(ws, Session({ }) ).first;

    process_messages(session_it, yield);

    established_sessions.erase(session_it);

}

//------------------------------------------------------------------------------


void process_message(ClientMessage& message, net::yield_context& yield)
{
    WebsocketPtr client = message.client.lock();
    if (client)
    {
        if (message.command == "list")
        {
            auto list_of_messages = generate_update_message_list(established_sessions.at(client).filter);
            for ( const std::string& update_message: list_of_messages)
            {
                beast::error_code ec;
                client->async_write(boost::asio::buffer(update_message), yield[ec]);
                if (ec)
                {
                    std::cout << "client write failure " << ec << std::endl;
                }
                //todo close client
            }
        }
    }
}

void process_message(SipCall& what, net::yield_context& yield)
{
    update_state_from_sngrep(what);

    /* std::cout << "next from channel" << what.call_id << "\n"; */

    //std::cout << "callid from sngrep: " << what.callId() << " " << what.state << "\n";


    if (what.state != 0 && !has_class4_info(what.call_id))
    {
     //   std::cout << "notified processor: " << what.callId() << " " << what.state << " " << call_processor.id() <<"\n";
        boost::asio::write(call_processor, boost::asio::buffer("\n"));
    }

    auto maybeIngressLegId = find_ingress_leg( what.call_id);
    if (maybeIngressLegId) {

        const std::string update_message = prepare_sngrep_update(*maybeIngressLegId);
        for ( auto session: established_sessions)
        {
            beast::error_code ec;
            

            /* std::cout << "sent to websocket: " << update_message << "\n"; */
            if (!is_callid_filtered_out(*maybeIngressLegId, session.second.filter))
            {
                session.first->async_write(boost::asio::buffer(update_message), yield[ec]);
                if(ec)
                    return fail(ec, "write_sngrep_update");
            }
        }

    }
    else
    {
        //update for a leg which has not been classified yet. Do nothing
    }
}

void do_multiplex(net::yield_context yield)
{

    std::string out;

    while (true)
    {
        boost::system::error_code ec;
        Message what = sngrep_channel.async_receive( yield[ec]);

        std::visit([&yield](auto&& msg) {process_message(msg, yield);}, what);

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
    call_processor = boost::process::v2::popen(ioc, "get_active_call.expect", {});


    std::string buf;
    while (true)
    {
        std::string result = async_read_line(call_processor, buf, yield);
        if (boost::algorithm::starts_with(result, "get_active_call class4 ") ||
            boost::algorithm::starts_with(result, "display count: "))
        {
            continue;
        }

        if (result.size() <= 2)
        {
            exit(73);
        }

        result.pop_back(); //remove question mark from the end of line
        result.pop_back(); //remove question mark from the end of line

        if (!try_insert_to_telnet_backlog(result))
        {
            //filter out duplicate lines
            continue;
        }

        std::string ingress_callid = update_state_from_class4(result);
        std::string update_message = prepare_sngrep_update(ingress_callid);

        for ( auto session: established_sessions)
        {
            beast::error_code ec;
            /* std::cout << "sent to websocket: " << update_message << "\n"; */
            if (!is_callid_filtered_out(ingress_callid, session.second.filter))
                session.first->write(boost::asio::buffer(update_message), ec);
        }
    }
}

void terminate_handler() {
    std::cout << boost::stacktrace::stacktrace();

    exit(2);
}   

void exit_handler() {
    call_processor.interrupt();
}   

void server_thread()
{
    std::set_terminate( terminate_handler );
    std::atexit( exit_handler );

    auto const address = net::ip::make_address("0.0.0.0");
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

void on_new_sip_message(struct sip_msg * msg)
{
    if (!msg->call) {
        exit(81);
    }

    if (!call_is_invite(msg->call))
    {
        return;
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

    SipCall sip_call(msg->call);

    /* std::cout << "from sngrep: " << sip_call.call_id << " " << sip_call.state << "\n"; */
    /* std::cout << "call state" <<msg->call->state << std::endl; */

    /* std::string call_id( msg->call->callid ); */

    /* legs.try_emplace( call_id, msg->call); */

    bool send_result = sngrep_channel.try_send(boost::asio::error::eof, sip_call);
    if (!send_result)
    {
        exit(14);
    }
}

void on_new_active_call_info(const std::map<std::string, std::string> call_info)
{
}
