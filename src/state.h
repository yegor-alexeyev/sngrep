#ifndef __SNGREP_STATE_H
#define __SNGREP_STATE_H

#include <boost/json/value_from.hpp>
#include <boost/json/value_to.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/parse.hpp>

#include <boost/bimap.hpp>
#include <boost/bimap/multiset_of.hpp>

#include <boost/algorithm/string.hpp>

#include <set>
#include <vector>
#include <string>
#include <map>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <functional>
#include <optional>

#ifdef __cplusplus
extern "C" {
#endif

#include "sip.h"
#include "sip_msg.h"
#include "sip_call.h"

#ifdef __cplusplus
}
#endif

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
        if (!stream->media)
        {
            exit(100);
        }

        m_ip = std::string(stream->media->address.ip);
        m_port = std::to_string(stream->media->address.port);
        m_type = std::string(stream->media->type);
        m_fmtcode = std::to_string(stream->media->fmtcode);
    }

    int count;
    int type;
    std::string src_ip;
    int src_port;

    std::string dest_ip;
    int dest_port;
    
    std::string m_ip;
    std::string m_port;
    std::string m_type;
    std::string m_fmtcode;
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

        src_ip = first->packet->src.ip;
        src_port = std::to_string(first->packet->src.port);

        dest_ip = first->packet->dst.ip;
        dest_port = std::to_string(first->packet->dst.port);

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


    std::string src_ip;
    std::string src_port;

    std::string dest_ip;
    std::string dest_port;
};

std::vector<std::string> read_file_as_lines(const std::string& filename);
std::vector<std::string> init_class4_fields_list(const std::string& filename);
std::set<std::string> vector_to_set(const std::vector<std::string> v);
std::optional<std::string> find_ingress_leg(const std::string leg_id);

bool check_filter(const std::string& ingressId, std::map<std::string, std::string> filter);
void update_state_from_sngrep(SipCall& sngrep_call);
bool has_class4_info(const std::string& callid);
std::map<std::string, std::string> collect_string_members(std::string message);
boost::json::value gather_leg_fields(const std::string& leg_id);
std::string prepare_sngrep_update(const std::string ingress_leg_id);
std::optional<std::string> find_ingress_leg(const std::string leg_id);
std::string update_state_from_class4(const std::string& input_line);
std::vector<std::string> generate_update_message_list(const std::map<std::string, std::string>& filter);
bool try_insert_to_backlog(const std::string& value);

#endif /* __SNGREP_STATE_H */
