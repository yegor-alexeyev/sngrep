#include <string>
#ifndef __SNGREP_AMQP_H
#define __SNGREP_AMQP_H

void stop_amqp();
void amqp_thread();
void publish_to_amqp(const std::string& message);

#endif /* __SNGREP_AMQP_H */
