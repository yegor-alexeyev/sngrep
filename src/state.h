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
#include "setting.h"

#ifdef __cplusplus
}
#endif

inline void log_and_exit(int code)
{
    std::cout << "program failed with code " << code << std::endl;
    exit(code);
}

struct Filter
{
    std::map<std::string, std::string> ingress;
    std::map<std::string, std::string> egress;
};

#define COMPARE(field) if (!(field == rh.field)) { return false; }


inline bool operator ==(const timeval& a, const timeval& b)
{
    return a.tv_sec == b.tv_sec && a.tv_usec == b.tv_usec;
}

struct RtpStream
{
    explicit RtpStream(rtp_stream_t *stream);

    bool operator==(const RtpStream& rh) const {

        COMPARE(count);
        COMPARE(type);
        COMPARE(src_ip);
        COMPARE(src_port);

        COMPARE(dest_ip);
        COMPARE(dest_port);
        
        COMPARE(m_ip);
        COMPARE(m_port);
        COMPARE(m_type);
        COMPARE(m_fmtcode);
        COMPARE(m_format);
        COMPARE(m_reqresp);

        return true;

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
    std::string m_format;
    std::string m_reqresp;
};

struct SipCall
{
    explicit SipCall();

    explicit SipCall(struct sip_call * call);


    bool operator==(const SipCall& rh) const
    {
        COMPARE(call_id);
        COMPARE(state);
        /* COMPARE(streams); */

        COMPARE(from);
        COMPARE(to);
        COMPARE(ani);
        COMPARE(dnis);
        COMPARE(init_time);
        COMPARE(ring_time);
        COMPARE(answer_time);
        COMPARE(hangup_time);

        COMPARE(source_ip);
        COMPARE(source_port);

        COMPARE(destination_ip);
        COMPARE(destination_port);

        COMPARE(a_rtp_dest_ip);
        COMPARE(a_rtp_dest_port);

        COMPARE(b_rtp_dest_ip);
        COMPARE(b_rtp_dest_port);

        /* COMPARE(a_rtp_packet_count); */
        /* COMPARE(b_rtp_packet_count); */

        COMPARE(codec);

        COMPARE(reason_header);
        COMPARE(identity_header);

        /* COMPARE(a_rtp_payload_bytes); */
        /* COMPARE(b_rtp_payload_bytes); */

        return true;
    }


    void updateRtpData(struct sip_call * call);

    std::string call_id;
    call_state state;
    /* std::vector<RtpStream> streams; */

    std::string from;
    std::string to;
    std::string ani;
    std::string dnis;
    timeval init_time;
    std::optional<timeval> ring_time;
    std::optional<timeval> answer_time;
    std::optional<timeval> hangup_time;


    std::string source_ip;
    std::string source_port;

    std::string destination_ip;
    std::string destination_port;

    std::optional<std::string> a_rtp_dest_ip;
    std::optional<std::string> a_rtp_dest_port;

    std::optional<std::string> b_rtp_dest_ip;
    std::optional<std::string> b_rtp_dest_port;

    std::string a_rtp_packet_count;
    std::string b_rtp_packet_count;

    std::optional<std::string> codec;

    std::optional<std::string> reason_header;
    std::optional<std::string> identity_header;

    std::string a_rtp_payload_bytes;
    std::string b_rtp_payload_bytes;
};

std::vector<std::string> read_file_as_lines(const std::string& filename);
std::vector<std::string> init_class4_fields_list(const std::string& filename);
std::set<std::string> vector_to_set(const std::vector<std::string> v);
std::optional<std::string> find_ingress_leg(const std::string leg_id);
std::string get_ingress_leg(const std::string& egress_leg_id);

bool is_call_filtered_out(const std::string& ingressId, const Filter& filter);
bool update_state_from_sngrep(SipCall& sngrep_call);
bool has_class4_info(const std::string& callid);
std::map<std::string, std::string> collect_string_members(const boost::json::object& jo);
boost::json::object gather_leg_fields(const std::string& leg_id);
std::string prepare_sngrep_update(const std::string ingress_leg_id);
std::string update_state_from_class4(const std::string& input_line);

void send_call_to_amqp(const std::string call_id);

std::vector<std::string> generate_update_message_list(const Filter& filter, bool only_active);
std::string generate_stats(const Filter& filter);
bool try_insert_to_telnet_backlog(const std::string& value);
void init_state();
void state_on_new_rtp_packet(struct rtp_stream * stream);

#endif /* __SNGREP_STATE_H */
