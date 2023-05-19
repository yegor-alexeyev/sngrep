#include <amqp_publisher.h>
#include <thread>
#include <iostream>
#include <boost/asio/spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/steady_timer.hpp>
#include <atomic>

#include <amqp.h>
#include <amqp_tcp_socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "setting.h"

#ifdef __cplusplus
}
#endif


static std::atomic<bool> running = true;

static boost::asio::io_context context;
static boost::asio::steady_timer hb_timer(context);
typedef boost::asio::experimental::concurrent_channel<void(boost::system::error_code, std::string)> Channel;
Channel ipc_channel(context, 100);

static const int channel_id = 1;

static const char* amqp_address;
static int amqp_port;
static const char* amqp_username;
static const char* amqp_password;
static const char* amqp_exchange;
static const char* amqp_routing_key;

static amqp_connection_state_t amqp_connection;

void do_amqp_heartbeat(std::atomic<bool>& running_flag, boost::asio::yield_context yield)
{
    boost::system::error_code ec;
    while (running_flag)
    {
        amqp_frame_t ignored;
        struct timeval t{0, 0};
        int ret = amqp_simple_wait_frame_noblock(amqp_connection, &ignored, &t);
        if ( ret)
        {
            //abandon current messagee
        }
        /* std::this_thread::sleep_for(std::chrono::milliseconds(1000)); */

        hb_timer.expires_after(std::chrono::milliseconds(1000));
        hb_timer.async_wait(yield[ec]);
    }
}

void do_amqp(std::atomic<bool>& running_flag, boost::asio::yield_context yield)
{
    while (running)
    {
        boost::system::error_code ec;
        std::string message = ipc_channel.async_receive( yield[ec]);

        amqp_basic_properties_t props;
        props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
        props.content_type = amqp_cstring_bytes("application/json");
        props.delivery_mode = 2; /* persistent delivery mode */

        int publish_ret = amqp_basic_publish(amqp_connection, channel_id, amqp_cstring_bytes(amqp_exchange),
                                    amqp_cstring_bytes(amqp_routing_key), 0, 0,
                                    &props, amqp_cstring_bytes(message.c_str()));

        if ( publish_ret)
        {
            //abandon current messagee
            std::cout << "AMQP error, will reconnect \n";

            running_flag = false;
            hb_timer.cancel();
            return;
        }
        else 
        {
            /* std::cout << "AMQP published \n"; */
        }
    }
}

bool do_amqp_connection()
{


    if (!amqp_address || amqp_port == 0 || !amqp_username || !amqp_password || !amqp_exchange || !amqp_routing_key)
    {
        std::cout << "AMQP is not configured \n";
        return false;
    }

    amqp_connection = amqp_new_connection();
    amqp_socket_t *amqp_socket = amqp_tcp_socket_new(amqp_connection);
    if (!amqp_socket) {
        std::cout << "AMQP, error creating TCP socket \n";
        return false;
    }

    int status = amqp_socket_open(amqp_socket, amqp_address, amqp_port);
    if (status) {
        std::cout << "AMQP, error connecting to  \n" << amqp_address << ":" << amqp_port;
        amqp_destroy_connection(amqp_connection);
        return true;
    }

    amqp_rpc_reply_t login_ret = amqp_login(amqp_connection, "/", 0, 131072, 10/*hb*/, AMQP_SASL_METHOD_PLAIN, amqp_username, amqp_password);
    if (login_ret.reply_type != AMQP_RESPONSE_NORMAL) {
        std::cout << "AMQP, error login \n";
        amqp_destroy_connection(amqp_connection);
        return true;
    }

    amqp_channel_open(amqp_connection, channel_id);
    amqp_rpc_reply_t reply_ret = amqp_get_rpc_reply(amqp_connection);
    if (reply_ret.reply_type != AMQP_RESPONSE_NORMAL) {
        std::cout << "AMQP, error rpc reply \n";

        amqp_channel_close(amqp_connection, channel_id, AMQP_CHANNEL_ERROR);
        amqp_connection_close(amqp_connection, AMQP_CHANNEL_ERROR);
        amqp_destroy_connection(amqp_connection);

        return true;
    }

    std::cout << "AMQP connected \n";

    std::atomic<bool> running_flag = true;

    //spawn a amqp publisher coroutine
    boost::asio::spawn(context, std::bind( &do_amqp, std::ref(running_flag), std::placeholders::_1));

    boost::asio::spawn(context, std::bind( &do_amqp_heartbeat, std::ref(running_flag), std::placeholders::_1));


    try {
        context.run();
    } 
    catch (std::exception& ex) 
    {
        std::cout << "server " << ex.what() << std::endl;
    }
    catch (...) 
    {
        std::cout << "amqp unknown exception" << std::endl;
    }

    amqp_channel_close(amqp_connection, 1, AMQP_CHANNEL_ERROR);
    amqp_connection_close(amqp_connection, AMQP_CHANNEL_ERROR);
    amqp_destroy_connection(amqp_connection);

    std::cout << "AMQP disconnected \n";
    context.restart();

    return true;
}


void amqp_thread()
{

    amqp_address = setting_get_value(SETTING_AMQP_ADDRESS);
    amqp_port = setting_get_intvalue(SETTING_AMQP_PORT);
    amqp_username = setting_get_value(SETTING_AMQP_USERNAME);
    amqp_password = setting_get_value(SETTING_AMQP_PASSWORD);
    amqp_exchange = setting_get_value(SETTING_AMQP_EXCHANGE);
    amqp_routing_key = setting_get_value(SETTING_AMQP_ROUTING_KEY);

    while (running) {
        if (!do_amqp_connection())
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

}

void publish_to_amqp(const std::string& message)
{
    

    ipc_channel.try_send(boost::asio::error::eof, message);
}

void stop_amqp()
{
    running = false;
    ipc_channel.cancel();
}
