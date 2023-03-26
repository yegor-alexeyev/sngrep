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
            a_rtp_dest_ip = stream->media->address.ip;
            a_rtp_dest_port = std::to_string(stream->media->address.port);
            b_rtp_packet_count = std::to_string(stream->pktcnt);
            b_rtp_payload_bytes = std::to_string(stream->payload_bytes_count);
        }
        else
        {
            b_rtp_dest_ip = stream->media->address.ip;
            b_rtp_dest_port = std::to_string(stream->media->address.port);
            codec = media_get_format(stream->media, stream->media->fmtcode);
            a_rtp_packet_count = std::to_string(stream->pktcnt);
            a_rtp_payload_bytes = std::to_string(stream->payload_bytes_count);
        }
    }

    sip_msg_t *first = (sip_msg_t *)vector_first(call->msgs);
    if (first->reqresp != SIP_METHOD_INVITE) 
    {
        exit(99);
    }

    from = first->sip_from;
    ani = std::string(from.begin(), std::find(from.begin(), from.end(), '@'));

    to = first->sip_to;
    dnis = std::string(to.begin(), std::find(to.begin(), to.end(), '@'));


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

    source_ip = first->packet->src.ip;
    source_port = std::to_string(first->packet->src.port);

    destination_ip = first->packet->dst.ip;
    destination_port = std::to_string(first->packet->dst.port);

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


typedef boost::bimap< boost::bimaps::set_of<std::string>, boost::bimaps::multiset_of<time_t> > Backlog;

typedef std::map<std::string, SipCall> SipCalls;
typedef std::map<std::string, Class4Fields> Class4Info;

EgressIngressMap egress_ingress_map;

SipCalls sip_calls;
Class4Info class4_info;

Backlog unclassified_backlog;
Backlog classified_backlog;
/////////////////////////////////////////////////////////////////////////////////////////////////

bool update_backlog(Backlog& backlog, const std::string& value)
{
    time_t time;
    gmtime(&time);

    const auto [it, is_inserted] = backlog.left.insert({value, time});

    if (!is_inserted)
    {
        backlog.left.replace_data(it, time);
    }

    return is_inserted;
}

void cleanup_telnet_backlog(Backlog& backlog)
{
    time_t time;
    gmtime(&time);

    while (time - backlog.right.begin()->first > 60*10)
    {
        backlog.right.erase(backlog.right.begin());
    }
}

void cleanup_unclassified_backlog()
{
    time_t time;
    gmtime(&time);

    while (time - unclassified_backlog.right.begin()->first > 60*20)
    {
        const std::string callid = unclassified_backlog.right.begin()->second;

        auto maybe_ingress_leg = find_ingress_leg( callid);
        if (!maybe_ingress_leg)
        {
            sip_calls.erase(callid);
            class4_info.erase(callid);
        }
        /* if call is classified then it's subject to different cleanup procedure */

        unclassified_backlog.right.erase(unclassified_backlog.right.begin());
    }

}

void cleanup_single_call(const std::string& callid)
{
    sip_calls.erase(callid);
    class4_info.erase(callid);
    classified_backlog.left.erase(callid);
    unclassified_backlog.left.erase(callid);
}

void cleanup_classified_backlog()
{
    time_t time;
    gmtime(&time);

    while (time - classified_backlog.right.begin()->first > 60*30)
    {
        const std::string ingress_callid = classified_backlog.right.begin()->second;

        auto ingress_egress_subrange = egress_ingress_map.right.equal_range(ingress_callid);
        std::for_each(ingress_egress_subrange.first, ingress_egress_subrange.second, [](const auto& ingress_egress) {
            cleanup_single_call(ingress_egress.second);
        });
        cleanup_single_call(ingress_callid);
    }

}

bool try_insert_to_telnet_backlog(const std::string& value)
{
    static Backlog backlog;

    cleanup_telnet_backlog(backlog);
    return update_backlog(backlog, value);
}

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

bool is_field_filtered_out(std::map<std::string, std::string> data, const std::string& key, const std::string value)
{
    return data.count(key) && data.at(key) != value;
}

bool is_callid_filtered_out(const std::string& callId, std::map<std::string, std::string> filter)
{
    for (auto name_value: filter)
    {
        if (is_field_filtered_out(class4_info[callId], name_value.first, name_value.second))
        {
            return true;
        }
    }
    if (!sip_calls.count(callId))
    {
        //TODO, we should also check if there are sngrep-side fields to be filtered on
        return false;
    }

    SipCall& call = sip_calls.at(callId);

#define CHECK_CALL_IS_FILTERED_OUT(FILTERS, FIELD) \
if (is_field_filtered_out(FILTERS, #FIELD, call.FIELD)) { return true; }

    CHECK_CALL_IS_FILTERED_OUT(filter, ani);
    CHECK_CALL_IS_FILTERED_OUT(filter, dnis);
    CHECK_CALL_IS_FILTERED_OUT(filter, from);
    CHECK_CALL_IS_FILTERED_OUT(filter, source_ip);
    CHECK_CALL_IS_FILTERED_OUT(filter, source_port);
    CHECK_CALL_IS_FILTERED_OUT(filter, destination_ip);
    CHECK_CALL_IS_FILTERED_OUT(filter, destination_port);



    auto maybe_ingress_leg = find_ingress_leg( callId);
    if (!maybe_ingress_leg)
    {
        return true;
    }

    for (auto name_value: filter)
    {
        if (is_field_filtered_out(class4_info[*maybe_ingress_leg], name_value.first, name_value.second))
        {
            return true;
        }
    }

    return false;
}

void update_state_from_sngrep(SipCall& sngrep_call)
{
    sip_calls.insert_or_assign( sngrep_call.call_id, sngrep_call );

    

    update_backlog(unclassified_backlog, sngrep_call.call_id);

    auto maybeIngressLegId = find_ingress_leg( sngrep_call.call_id);
    if (maybeIngressLegId) {
        update_backlog(classified_backlog, *maybeIngressLegId);
    }

    cleanup_unclassified_backlog();
    cleanup_classified_backlog();
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

        result_object["from"] = sip_call.from;
        result_object["to"] = sip_call.to;
        result_object["ani"] = sip_call.ani;
        result_object["dnis"] = sip_call.dnis;


        result_object["source_ip"] = sip_call.source_ip;
        result_object["source_port"] = sip_call.source_port;

        result_object["destination_ip"] = sip_call.destination_ip;
        result_object["destination_port"] = sip_call.destination_port;

        MAYBE_SET_SIP_CALL_JSON_FIELD(result_object, a_rtp_dest_ip);
        MAYBE_SET_SIP_CALL_JSON_FIELD(result_object, a_rtp_dest_port);
        MAYBE_SET_SIP_CALL_JSON_FIELD(result_object, b_rtp_dest_ip);
        MAYBE_SET_SIP_CALL_JSON_FIELD(result_object, b_rtp_dest_port);
        MAYBE_SET_SIP_CALL_JSON_FIELD(result_object, codec);

        result_object["a_rtp_packet_count"] = sip_call.a_rtp_packet_count;
        result_object["b_rtp_packet_count"] = sip_call.b_rtp_packet_count;

        result_object["a_rtp_payload_bytes"] = sip_call.a_rtp_payload_bytes;
        result_object["b_rtp_payload_bytes"] = sip_call.b_rtp_payload_bytes;

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

    boost::json::object call_group_json = { 
            {"ingress", gather_leg_fields( ingress_leg_id) },
            {"egress", egress_legs_json }
    };

    boost::json::object state_message = { { "callgroup", call_group_json } };

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
    update_backlog(unclassified_backlog, egress_callid);

    class4_info.insert_or_assign( ingress_callid, ingress_fields );
    update_backlog(unclassified_backlog, ingress_callid);

/*         return fields.at("ingress_callid"); */

    egress_ingress_map.insert(EgressIngressMap::value_type(
         egress_callid ,  ingress_callid
    ));

    update_backlog(classified_backlog, ingress_callid);

    cleanup_unclassified_backlog();
    cleanup_classified_backlog();

    return ingress_callid;
}

std::vector<std::string> generate_update_message_list(const std::map<std::string, std::string>& filter)
{
    std::vector<std::string> result;
    std::set<std::string> uniques;
    /* for( auto it = egress_ingress_map.right.begin(); it != egress_ingress_map.right.end(); it = next_different_key(it, egress_ingress_map.right.end())) */
    for( auto& sip_call: sip_calls)
    {
        /* std::cout << "sent to websocket: " << update_message << "\n"; */
        if (!is_callid_filtered_out(sip_call.first, filter))
        {
            auto maybe_ingress_leg = find_ingress_leg( sip_call.first);
            if (maybe_ingress_leg && uniques.count(*maybe_ingress_leg) == 0)
            {
                const std::string update_message = prepare_sngrep_update(*maybe_ingress_leg);
                result.push_back(update_message);
                uniques.insert(*maybe_ingress_leg);
            }
        }
    }
    return result;
}

