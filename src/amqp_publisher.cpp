#include <amqp_publisher.h>
#include <thread>
#include <iostream>
#include <boost/asio/spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include <amqp.h>
#include <amqp_tcp_socket.h>
#include "setting.h"



boost::asio::io_context context;
typedef boost::asio::experimental::concurrent_channel<void(boost::system::error_code, std::string)> Channel;
Channel ipc_channel(context, 100);

bool do_amqp_connection(boost::asio::yield_context& yield)
{
    const int channel_id = 1;

    const char* amqp_address = setting_get_value(SETTING_AMQP_ADDRESS);
    int amqp_port = setting_get_intvalue(SETTING_AMQP_PORT);
    const char* amqp_username = setting_get_value(SETTING_AMQP_USERNAME);
    const char* amqp_password = setting_get_value(SETTING_AMQP_PASSWORD);
    const char* amqp_exchange = setting_get_value(SETTING_AMQP_EXCHANGE);
    const char* amqp_routing_key = setting_get_value(SETTING_AMQP_ROUTING_KEY);

    if (!amqp_address || amqp_port == 0 || !amqp_username || !amqp_password || !amqp_exchange || !amqp_routing_key)
    {
        std::cout << "AMQP is not configured \n";
        return false;
    }

    amqp_connection_state_t amqp_connection = amqp_new_connection();
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

    amqp_rpc_reply_t login_ret = amqp_login(amqp_connection, "/", 0, 131072, 5/*hb*/, AMQP_SASL_METHOD_PLAIN, amqp_username, amqp_password);
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

    while (true)
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
 
            amqp_channel_close(amqp_connection, 1, AMQP_CHANNEL_ERROR);
            amqp_connection_close(amqp_connection, AMQP_CHANNEL_ERROR);
            amqp_destroy_connection(amqp_connection);
            return true;
        }
    }
}

void do_amqp(boost::asio::yield_context yield)
{
    while (true)
    {
        if (!do_amqp_connection(yield))
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void amqp_thread()
{

    //spawn a amqp publisher coroutine
    boost::asio::spawn(context, std::bind( &do_amqp, std::placeholders::_1));

    context.run();
}

void publish_to_amqp(const std::string& message)
{
    ipc_channel.try_send(boost::asio::error::eof, message);
}
