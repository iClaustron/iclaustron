#ifdef COMM_H
#define COMM_H

#include <netinet/in.h>
#include <config.h>
#include <common.h>

int setup_client(in_addr_t server_ip, u_short server_port,
                 in_addr_t my_ip, u_short my_port);
int setup_server(in_addr_t server_ip, u_short server_port,
                 in_addr_t client_ip, u_short client_port,
                 int backlog);
#endif
