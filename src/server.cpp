#define BOOST_STACKTRACE_USE_BACKTRACE
#include <boost/stacktrace.hpp>

#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "sip.h"
#include "sip_msg.h"
#include "sip_call.h"

#ifdef __cplusplus
}
#endif


#include <set>

#include <boost/range/combine.hpp>

#include <boost/asio/experimental/concurrent_channel.hpp>

#include <boost/algorithm/string.hpp>

#include <boost/json/value_from.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/parse.hpp>

#include <boost/process/v2/process.hpp>
#include <boost/process/v2/popen.hpp>

#include <boost/stacktrace.hpp>

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



std::vector<std::string> read_file_as_lines(const std::string& filename)
{
    std::ifstream in(filename);
    std::string str;
    std::vector<std::string> result;

    while (std::getline(in, str))
    {
        if(str.size() > 0 && str.front() != '#')
        {
            result.push_back(str);
        }
    }

    return result;
}

std::vector<std::string> init_class4_fields_list(const std::string& filename)
{
    std::vector<std::string> result = read_file_as_lines(filename);
    result.insert(result.begin(), "UUID_special");
    return result;
}

std::set<std::string> vector_to_set(const std::vector<std::string> v)
{
    return std::set<std::string>(v.begin(), v.end());
}

std::vector<std::string> class4_fields = init_class4_fields_list("etc/class4_fields");
std::set<std::string> ingress_class4_fields = vector_to_set(read_file_as_lines("etc/ingress_class4_fields"));
std::set<std::string> egress_class4_fields = vector_to_set(read_file_as_lines("etc/egress_class4_fields"));

// The io_context is required for all I/O
net::io_context context;

boost::process::v2::popen call_processor(context);


struct RtpStream
{
    explicit RtpStream(rtp_stream_t *stream)
    {

        count = stream->pktcnt;
        type = stream->type;
        src_ip = std::string(stream->src.ip);
        src_port = stream->src.port;
        
        dest_ip = std::string(stream->dst.ip);
        dest_port = stream->dst.port;
    }

    int count;
    int type;
    std::string src_ip;
    int src_port;

    std::string dest_ip;
    int dest_port;
    
};

struct SipCall
{
    explicit SipCall()
    {
        /*
            Should not happen
        */
        exit(140);
    }

    explicit SipCall(struct sip_call * call)
    {
        call_id = std::string(call->callid);
        state = (call_state)call->state;

        rtp_stream_t *stream;
        vector_iter_t streams_it = vector_iterator(call->streams);

        while ( (stream = (rtp_stream_t*)vector_iterator_next(&streams_it))) {
            streams.emplace_back( stream );
        }

        sip_msg_t *first = (sip_msg_t *)vector_first(call->msgs);
        if (first->reqresp != SIP_METHOD_INVITE) 
        {
            exit(99);
        }

        from = first->sip_from;
        to = first->sip_to;


        init_time = msg_get_time(first);


        vector_iter_t msgs_it = vector_iterator(call->msgs);
        while (sip_msg_t* msg = (sip_msg_t*)vector_iterator_next(&msgs_it)) {
            if (!ring_time && (msg->reqresp == 180 || msg->reqresp == 183)) {
                ring_time = msg_get_time(msg);
            }
            if (!answer_time && (msg->reqresp == 200)) {
                answer_time = msg_get_time(msg);
            }
            if (!hangup_time && (msg->reqresp == SIP_METHOD_BYE || msg->reqresp == SIP_METHOD_CANCEL || (msg->reqresp >= 400 && msg->reqresp < 600))) {
                hangup_time = msg_get_time(msg);
            }

        }


        /* ring_time = msg_get_time(call->cstart_msg); */
        /* answer_time = msg_get_time(call->cstart_msg); */
        /* hangup_time = msg_get_time(call->cstart_msg); */

    }

    std::string call_id;
    call_state state;
    std::vector<RtpStream> streams;

    std::optional<std::string> from;
    std::optional<std::string> to;
    timeval init_time;
    std::optional<timeval> ring_time;
    std::optional<timeval> answer_time;
    std::optional<timeval> hangup_time;
};

/* struct Leg */
/* { */
/*     SipCall sip_call; */
/*     std::map<std::string, std::string> class4_fields; */
/* }; */

typedef std::map<std::string, std::string> Class4Fields;

/* struct IngressLeg */
/* { */
/*     std::string callId() const */
/*     { */
/*         return fields.at("ingress_callid"); */
/*     } */
/*     std::map<std::string, std::string> fields; */
/* }; */

/* struct EgressLeg */
/* { */
/*     std::string callId() const */
/*     { */
/*         return fields.at("egress_callid"); */
/*     } */

/*     std::map<std::string, std::string> fields; */
/* }; */


typedef boost::bimap<boost::bimaps::set_of<std::string>, boost::bimaps::multiset_of<std::string> > EgressIngressMap;



typedef std::map<std::string, SipCall> SipCalls;
typedef std::map<std::string, Class4Fields> Class4Info;

EgressIngressMap egress_ingress_map;

SipCalls sip_calls;
Class4Info class4_info;

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

        boost::json::value jv = boost::json::parse( message, ec );
        if( ec )
            return fail(ec, "parsing failed");

        if (!jv.is_object())
        {
            return fail(ec, "invalid json: root is not object");
        }
        boost::json::object jo = jv.as_object();
        if (!jo.contains("command"))
        {
            return fail(ec, "invalid json: missing command attribute");
        }
        boost::json::value command_val = jo["command"];
        if (!command_val.is_string())
        {
            return fail(ec, "invalid json: invalid command attribute type");
        }

        sngrep_channel.try_send(boost::asio::error::eof, ClientMessage({session->first, std::string(command_val.as_string())}));

        /* std::cout << "channel" << "canceled" << "\n"; */
        // Echo the message back
        /* session->first->text(session->first->got_text()); */
        /* session->first->async_write(boost::asio::buffer("Requests are not supported yet"), yield[ec]); */
        /* if(ec) */
        /*     return fail(ec, "write"); */
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
std::string call_state_to_string(call_state state)
{
#define CALL_STATE_CASE_STATEMENT(NAME) case SIP_CALLSTATE_ ## NAME : return #NAME

    switch (state)
    {
        CALL_STATE_CASE_STATEMENT(CALLSETUP);
        CALL_STATE_CASE_STATEMENT(INCALL);
        CALL_STATE_CASE_STATEMENT(CANCELLED);
        CALL_STATE_CASE_STATEMENT(REJECTED);
        CALL_STATE_CASE_STATEMENT(DIVERTED);
        CALL_STATE_CASE_STATEMENT(BUSY);
        CALL_STATE_CASE_STATEMENT(COMPLETED);
        default: exit(94);
    }
}

std::optional<std::string> find_ingress_leg(const std::string leg_id)
{
    if (egress_ingress_map.right.count(leg_id) > 0)
    {
        //leg is ingress by itself
        return leg_id;
    }

    if (egress_ingress_map.left.count(leg_id) > 0)
    {
        //update for egress leg, finding a corresponding ingress leg
        return egress_ingress_map.left.find(leg_id)->second;
    }

    return std::nullopt;
}

std::string format_timestamp(const timeval& timestamp)
{
    std::stringstream timestamp_stream;

    // timestamp_stream << std::put_time(std::gmtime(&sip_call.init_time.tv_sec), "%c") << "." << sip_call.init_time.tv_usec;

    timestamp_stream << std::put_time(std::gmtime(&timestamp.tv_sec), "%F %T.") << timestamp.tv_usec;
    return timestamp_stream.str();
}

boost::json::value gather_leg_fields(const std::string& leg_id)
{
    const auto sip_call_iterator = sip_calls.find(leg_id);

    boost::json::value result = boost::json::value_from(class4_info[leg_id]);
    boost::json::object& result_object = result.as_object();

    result_object["call_id"] = leg_id;
    if (sip_call_iterator != sip_calls.end())
    {
        auto sip_call = sip_call_iterator->second;
        result_object["status"] = call_state_to_string(sip_call.state);

        result_object["init_time"] = format_timestamp(sip_call.init_time);
        if (sip_call.ring_time)
        {
            result_object["ring_time"] = format_timestamp(*sip_call.ring_time);
        }
        if (sip_call.answer_time)
        {
            result_object["answer_time"] = format_timestamp(*sip_call.answer_time);
        }
        if (sip_call.hangup_time)
        {
            result_object["hangup_time"] = format_timestamp(*sip_call.hangup_time);
        }
        if (sip_call.from)
        {
            result_object["from"] = *sip_call.from;
        }
        if (sip_call.to)
        {
            result_object["to"] = *sip_call.to;
        }



        boost::json::array streams_json;
        for (const auto& stream: sip_call.streams)
        {
            std::map<std::string, std::string> fields;
            fields["count"] = stream.count;
            fields["type"] = std::to_string(stream.type);
            fields["src_ip"] = stream.src_ip;
            fields["src_port"] = std::to_string(stream.src_port);
            fields["dest_ip"] = stream.dest_ip;
            fields["dest_port"] = std::to_string(stream.dest_port);

            streams_json.push_back( boost::json::value_from( fields ) );
        }

        result_object["streams"] = streams_json;
    }
    return result;

}

std::string prepare_sngrep_update(const std::string ingress_leg_id)
{
    boost::json::array egress_legs_json;

    auto ingress_egress_subrange = egress_ingress_map.right.equal_range(ingress_leg_id);
    std::for_each(ingress_egress_subrange.first, ingress_egress_subrange.second, [&egress_legs_json](const auto& ingress_egress) {
        egress_legs_json.push_back( gather_leg_fields( ingress_egress.second ) );
    });

    boost::json::object state_message = {
        {"ingress", gather_leg_fields( ingress_leg_id) },
        {"egress", egress_legs_json }
        /* {"egress", boost::json::array( boost::json::value_from( class4_info[ingress_leg_id] ) ) }, */
        /* {"other", boost::json::value_from( egress_ingress_map.right.find(ingress_leg_id)->second ) } */
    };

    return boost::json::serialize(state_message);
}


template <typename Iterator>
Iterator next_different_key(const Iterator start, const Iterator end)
{
    Iterator result = start;
    if (result == end) {
        return end;
    }

    do  {
        result++;
    } while (result != end && result->first == start->first);

    return result;
}

void process_message(ClientMessage& message)
{
    WebsocketPtr client = message.client.lock();
    if (client)
    {
        if (message.command == "list")
        {
            for( auto it = egress_ingress_map.right.begin(); it != egress_ingress_map.right.end(); it = next_different_key(it, egress_ingress_map.right.end()))
            {
                const std::string update_message = prepare_sngrep_update(it->first);

                beast::error_code ec;
                /* std::cout << "sent to websocket: " << update_message << "\n"; */
                client->write(boost::asio::buffer(update_message), ec);
            }
        }
    }
}


void process_message(SipCall& what)
{
    sip_calls.insert_or_assign( what.call_id, what );

    /* std::cout << "next from channel" << what.call_id << "\n"; */

    //std::cout << "callid from sngrep: " << what.callId() << " " << what.state << "\n";


    if (what.state != 0 && class4_info.count(what.call_id) == 0)
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
            std::cout << "sent to websocket: " << update_message << "\n";
            session.first->write(boost::asio::buffer(update_message), ec);
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

        std::visit([](auto&& msg) {process_message(msg);}, what);

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

bool try_insert_to_backlog(const std::string& value)
{
    typedef boost::bimap< boost::bimaps::set_of<std::string>, boost::bimaps::multiset_of<time_t> > Backlog;
    static Backlog backlog;

    time_t time;
    gmtime(&time);

    const auto [it, is_inserted] = backlog.left.insert({value, time});

    if (!is_inserted)
    {
        backlog.left.replace_data(it, time);
    }
    
    if (backlog.size() > 3000)
    {
        backlog.right.erase(backlog.right.begin());
    }

    return is_inserted;
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

        if (!try_insert_to_backlog(result))
        {
            //filter out duplicate lines
            continue;
        }

        std::cout << "line: " << result << std::endl;

        result.pop_back(); //remove question mark from the end of line
        result.pop_back(); //remove question mark from the end of line


        std::vector<std::string> values;
        boost::split(values,result, boost::algorithm::is_any_of(";"));

        std::map<std::string, std::string> ingress_fields;
        std::map<std::string, std::string> egress_fields;


        if (values.size() != class4_fields.size())
        {
            exit(13);
        }

        std::string ingress_callid;
        std::string egress_callid;

        for (size_t i = 0; i < class4_fields.size(); i++)
        {
            if (ingress_class4_fields.count(class4_fields[i]) > 0)
            {
                ingress_fields[class4_fields[i]] = values[i];
            }
            if (egress_class4_fields.count(class4_fields[i]) > 0)
            {
                egress_fields[class4_fields[i]] = values[i];
            }
            if (class4_fields[i] == "egress_callid")
            {
                egress_callid = values[i];
            }
            if (class4_fields[i] == "ingress_callid")
            {
                ingress_callid = values[i];
            }
        }

        if (egress_ingress_map.right.count(ingress_callid) == 0) {
            std::cout << "new class4 ingress classified: " << ingress_callid << " " << egress_callid << "\n";
        }
        if (egress_ingress_map.left.count(egress_callid) == 0) {
            std::cout << "new class4 egress leg: " << ingress_callid << " " << egress_callid << "\n";
        }

        class4_info.insert_or_assign( egress_callid, egress_fields );
        class4_info.insert_or_assign( ingress_callid, ingress_fields );

/*         return fields.at("ingress_callid"); */

        egress_ingress_map.insert(EgressIngressMap::value_type(
             egress_callid ,  ingress_callid
        ));

        const std::string update_message = prepare_sngrep_update(ingress_callid);
        for ( auto session: established_sessions)
        {
            beast::error_code ec;
            std::cout << "sent to websocket: " << update_message << "\n";
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
