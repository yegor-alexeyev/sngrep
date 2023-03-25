#include "state.h"

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
#include <iomanip>

RtpStream::RtpStream(rtp_stream_t *stream)
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
    m_format = media_get_format(stream->media, stream->media->fmtcode);

    if (!stream->media->msg)
    {
        exit(100);
    }
    m_reqresp = std::to_string(stream->media->msg->reqresp);
}

SipCall::SipCall(struct sip_call * call)
{
    call_id = std::string(call->callid);
    state = (call_state)call->state;

    rtp_stream_t *stream;
    vector_iter_t streams_it = vector_iterator(call->streams);

    while ( (stream = (rtp_stream_t*)vector_iterator_next(&streams_it))) {
        streams.emplace_back( stream );

        if (!stream->media)
        {
            exit(100);
        }
        if (!stream->media->msg)
        {
            exit(101);
        }

        if (std::string(stream->media->type) != std::string("audio") ||
            stream->type != PACKET_RTP
           )
        {
            continue;
        }
        if (stream->media->msg->reqresp == SIP_METHOD_INVITE)
        {
            a_rtp_ip = stream->media->address.ip;
            a_rtp_port = std::to_string(stream->media->address.port);
        }
        else
        {
            b_rtp_ip = stream->media->address.ip;
            b_rtp_port = std::to_string(stream->media->address.port);
            codec = media_get_format(stream->media, stream->media->fmtcode);
        }
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

typedef std::map<std::string, std::string> Class4Fields;

typedef boost::bimap<boost::bimaps::set_of<std::string>, boost::bimaps::multiset_of<std::string> > EgressIngressMap;



typedef std::map<std::string, SipCall> SipCalls;
typedef std::map<std::string, Class4Fields> Class4Info;

EgressIngressMap egress_ingress_map;

SipCalls sip_calls;
Class4Info class4_info;
/////////////////////////////////////////////////////////////////////////////////////////////////

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

bool check_filter_field(std::map<std::string, std::string> data, std::map<std::string, std::string> filter, const std::string&key)
{
    if (filter.count(key) == 0)
    {
        return true;
    }
    if (data.count(key) == 0)
    {
        return false;
    }
    return data.at(key) == filter.at(key);
}

bool check_filter(const std::string& ingressId, std::map<std::string, std::string> filter)
{
    auto ingress_fields = class4_info[ingressId];

    return check_filter_field( ingress_fields, filter, "ingress_carrier")
        && check_filter_field( ingress_fields, filter, "ingress_trunk");
}

void update_state_from_sngrep(SipCall& sngrep_call)
{
    sip_calls.insert_or_assign( sngrep_call.call_id, sngrep_call );
}

bool has_class4_info(const std::string& call_id)
{
    return class4_info.count(call_id) > 0;
}

std::map<std::string, std::string> collect_string_members(std::string message)
{
    boost::system::error_code ec;

    std::map<std::string, std::string> result;

    boost::json::value jv = boost::json::parse( message, ec );
    if( ec )
    {
        std::cout << "invalid json: parsing failed" << "\n";
        return result;
    }

    if (!jv.is_object())
    {
        std::cout << "invalid json: root is not object" << "\n";
        return result;
    }

    boost::json::object jo = jv.as_object();
    for (auto kvp: jo)
    {
        if (kvp.value().is_string())
        {
            result[kvp.key()] = kvp.value().as_string();
        }
        else
        {
            std::cout << "invalid object member " << kvp.key() << "\n";
        }
    }
    return result;
}

std::string format_timestamp(const timeval& timestamp)
{
    std::stringstream timestamp_stream;

    // timestamp_stream << std::put_time(std::gmtime(&sip_call.init_time.tv_sec), "%c") << "." << sip_call.init_time.tv_usec;

    timestamp_stream << std::put_time(std::gmtime(&timestamp.tv_sec), "%F %T.") << timestamp.tv_usec;
    return timestamp_stream.str();
}



template <typename T>
void optionally_set_json_field(boost::json::object& object, const std::string& field_name, const std::optional<T>& value)
{
    if (value)
    {
        object[field_name] = *value;
    }
}

void optionally_set_json_timeval_field(boost::json::object& object, const std::string& field_name, const std::optional<timeval>& value)
{
    if (value)
    {
        object[field_name] = format_timestamp(*value);
    }
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

#define MAYBE_SET_SIP_CALL_JSON_FIELD(JSON_OBJECT, FIELD_NAME) \
optionally_set_json_field(JSON_OBJECT, #FIELD_NAME, sip_call.FIELD_NAME)

        result_object["status"] = call_state_to_string(sip_call.state);

        result_object["init_time"] = format_timestamp(sip_call.init_time);

        optionally_set_json_timeval_field(result_object, "ring_time", sip_call.ring_time);
        optionally_set_json_timeval_field(result_object, "answer_time", sip_call.answer_time);
        optionally_set_json_timeval_field(result_object, "hangup_time", sip_call.hangup_time);

        MAYBE_SET_SIP_CALL_JSON_FIELD(result_object, from);
        MAYBE_SET_SIP_CALL_JSON_FIELD(result_object, to);


        result_object["source_ip"] = sip_call.src_ip;
        result_object["source_port"] = sip_call.src_port;

        result_object["destination_ip"] = sip_call.dest_ip;
        result_object["destination_port"] = sip_call.dest_port;

        MAYBE_SET_SIP_CALL_JSON_FIELD(result_object, a_rtp_ip);
        MAYBE_SET_SIP_CALL_JSON_FIELD(result_object, a_rtp_port);
        MAYBE_SET_SIP_CALL_JSON_FIELD(result_object, b_rtp_ip);
        MAYBE_SET_SIP_CALL_JSON_FIELD(result_object, b_rtp_port);
        MAYBE_SET_SIP_CALL_JSON_FIELD(result_object, codec);

        boost::json::array streams_json;
        for (const auto& stream: sip_call.streams)
        {
            std::map<std::string, std::string> fields;
            fields["count"] = std::to_string(stream.count);
            fields["type"] = std::to_string(stream.type);
            fields["src_ip"] = stream.src_ip;
            fields["src_port"] = std::to_string(stream.src_port);
            fields["dest_ip"] = stream.dest_ip;
            fields["dest_port"] = std::to_string(stream.dest_port);

            fields["m_ip"] = stream.m_ip;
            fields["m_port"] = stream.m_port;
            fields["m_type"] = stream.m_type;
            fields["m_fmtcode"] = stream.m_fmtcode;
            fields["m_format"] = stream.m_format;
            fields["m_reqresp"] = stream.m_reqresp;

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





std::string update_state_from_class4(const std::string& input_line)
{

    std::cout << "line: " << input_line << std::endl;



    std::vector<std::string> values;
    boost::split(values,input_line, boost::algorithm::is_any_of(";"));

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

    if (ingress_callid.empty())
    {
        exit(77);
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

    return ingress_callid;
}

std::vector<std::string> generate_update_message_list(const std::map<std::string, std::string>& filter)
{
    std::vector<std::string> result;
    for( auto it = egress_ingress_map.right.begin(); it != egress_ingress_map.right.end(); it = next_different_key(it, egress_ingress_map.right.end()))
    {
        /* std::cout << "sent to websocket: " << update_message << "\n"; */
        if (check_filter(it->first, filter))
        {
            const std::string update_message = prepare_sngrep_update(it->first);
            result.push_back(update_message);
        }
    }
    return result;
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
