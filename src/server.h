#ifndef __SNGREP_SERVER_H
#define __SNGREP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

void server_thread();
void on_new_sip_message(struct sip_msg * msg);
void on_new_rtp_packet(struct rtp_stream * msg);

#ifdef __cplusplus
}
#endif

#endif /* __SNGREP_SERVER_H */
