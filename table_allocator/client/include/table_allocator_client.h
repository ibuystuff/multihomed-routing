#ifndef TABLE_ALLOCATOR_CLIENT_H
#define TABLE_ALLOCATOR_CLIENT_H

#include <uv.h>
#include <linux/if.h>
#include <netinet/in.h>
#include <libmnl/libmnl.h>

#include <table_allocator_shared_json.h>

#define REQUEST_RETRANSMISSION_MS   2000
#define NUM_FAILED_LIMIT            5

struct mnl_socket;

//keep all buffers, values, etc. related to an address here
struct tac_address {
    struct sockaddr_storage addr;
    uint32_t ifidx;

    //allocated variables
    uint32_t rt_table;
    uint32_t lease_expires;

    //will keep subnet or prefix len, depending on family
    uint8_t subnet_prefix_len;
    //read from command line
    uint8_t addr_family;
    uint8_t rules_added;
    char ifname[IFNAMSIZ];
    char address_str[INET6_ADDRSTRLEN];
    char tag[TA_SHARED_MAX_TAG_SIZE];
};

struct tac_ctx {
    uv_loop_t event_loop; 
    uv_udp_t unix_socket_handle;
    uv_udp_t netlink_handle;
	uv_timer_t unix_socket_timeout_handle;
	uv_timer_t request_timeout_handle;
	uv_timer_t netlink_timeout_handle;
    struct mnl_socket *rt_mnl_socket;
    FILE *logfile;
    struct tac_address *address;
    char destination[TA_SHARED_MAX_ADDR_SIZE];
    uint8_t rcv_buf[TA_SHARED_MAX_JSON_LEN];
    uint8_t *mnl_recv_buf;
    uint8_t use_syslog;
    //request or release
    uint8_t cmd;
    uint8_t daemonize;
    uint8_t daemonized;
    uint8_t closing;
    uint8_t num_failed;
};

void table_allocator_client_send_request(struct tac_ctx *ctx, uint8_t cmd);

#endif
