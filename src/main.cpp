/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dns_sd.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>

#include <minecraft.h>

#if defined(CONFIG_POSIX_API)
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/sys/socket.h>
#endif

LOG_MODULE_REGISTER(udp_sample, CONFIG_UDP_SAMPLE_LOG_LEVEL);

/* Macros used to subscribe to specific Zephyr NET management events. */
#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

#if defined(CONFIG_NET_HOSTNAME)
/* Register service */
DNS_SD_REGISTER_TCP_SERVICE(http_server_sd, CONFIG_NET_HOSTNAME, "_http", "local",
			    DNS_SD_EMPTY_TXT, 25565);
#endif /* CONFIG_NET_HOSTNAME */

/* Macro called upon a fatal error, reboots the device. */
#define FATAL_ERROR()					\
	LOG_ERR("Fatal error! Rebooting the device.");	\
	LOG_PANIC();					\
	IF_ENABLED(CONFIG_REBOOT, (sys_reboot(0)))

/* Zephyr NET management event callback structures. */
static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback conn_cb;

static K_SEM_DEFINE(network_connected_sem, 0, 1);

/* Variables used to perform socket operations. */
static int fd;
static struct sockaddr_storage host_addr;

typedef struct{
    int socket;
    uint8_t id;
} clients;

#define MAX_PLAYERS 5
#define STACK_SIZE 16384
clients serverClients[MAX_PLAYERS];

/* Processing threads for incoming connections */
K_THREAD_STACK_ARRAY_DEFINE(tcp4_handler_stack, MAX_PLAYERS, STACK_SIZE);
static struct k_thread tcp4_handler_thread[MAX_PLAYERS];
static k_tid_t tcp4_handler_tid[MAX_PLAYERS];

minecraft mc;

#define THREAD_PRIORITY			K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 1)

static int setup_server(int *sock, struct sockaddr *bind_addr, socklen_t bind_addrlen)
{
	int ret;
	int enable = 1;

	*sock = socket(bind_addr->sa_family, SOCK_STREAM, IPPROTO_TCP);

	if (*sock < 0) {
		LOG_ERR("Failed to create socket: %d", -errno);
		return -errno;
	}

	ret = setsockopt(*sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
	if (ret) {
		LOG_ERR("Failed to set SO_REUSEADDR %d", -errno);
		return -errno;
	}

	ret = bind(*sock, bind_addr, bind_addrlen);
	if (ret < 0) {
		LOG_ERR("Failed to bind socket %d", -errno);
		return -errno;
	}

	ret = listen(*sock, MAX_PLAYERS);
	if (ret < 0) {
		LOG_ERR("Failed to listen on socket %d", -errno);
		return -errno;
	}

	return ret;
}

static void client_conn_handler(void *ptr1, void *ptr2, void *ptr3)
{
	ARG_UNUSED(ptr1);
    clients client = *(clients*)ptr2;

	int ret;

    if(!mc.players[client.id].join()){  // try to join, end task if fail
        goto end;
    }

	while (true) {
        mc.players[client.id].handle();
		k_msleep(10);
	};

    mc.broadcastEntityDestroy(mc.players[client.id].id);
    mc.broadcastChatMessage(mc.players[client.id].username + " left the server", "Server");
    end:
	(void)close(client.socket);

	client.socket = -1;
}

static void process_tcp4(void)
{
	int ret;
	struct sockaddr_in addr4 = {
		.sin_family = AF_INET,
		.sin_port = htons(25565),
	};

	int i = 0;
	for(i = 0; i < MAX_PLAYERS; i++){
		if (serverClients[i].socket == 0) {
			break;
		}
	}
	int slot = i;

	int *tcp4_sock = &serverClients[i].socket;

	ret = setup_server(tcp4_sock, (struct sockaddr *)&addr4, sizeof(addr4));
	if (ret < 0) {
		LOG_ERR("Failed to create IPv4 socket %d", ret);
		return;
	}

	LOG_INF("Waiting for IPv4 HTTP connections on port %d, sock %d", 25565, serverClients[i].socket);

	int client;

	while (true) {
		struct sockaddr_in6 client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		char addr_str[INET6_ADDRSTRLEN];

		client = accept(*tcp4_sock, (struct sockaddr *)&client_addr,
				&client_addr_len);
		if (client < 0) {
			LOG_ERR("Error in accept %d, try again", -errno);
			return;
		}

		tcp4_handler_tid[slot] = k_thread_create(
			&tcp4_handler_thread[slot],
			tcp4_handler_stack[slot],
			K_THREAD_STACK_SIZEOF(tcp4_handler_stack[slot]),
			(k_thread_entry_t)client_conn_handler,
			INT_TO_POINTER(slot),
			&serverClients[i],
			&tcp4_handler_tid[slot],
			THREAD_PRIORITY,
			0, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(tcp4_thread_id, 16384,
		process_tcp4, NULL, NULL, NULL,
		THREAD_PRIORITY, 0, -1);

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint32_t event,
			     struct net_if *iface)
{
	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		LOG_INF("Network connectivity established");
		k_sem_give(&network_connected_sem);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		LOG_INF("Network connectivity lost");
		break;
	default:
		/* Don't care */
		return;
	}
}

static void connectivity_event_handler(struct net_mgmt_event_callback *cb,
				       uint32_t event,
				       struct net_if *iface)
{
	if (event == NET_EVENT_CONN_IF_FATAL_ERROR) {
		LOG_ERR("NET_EVENT_CONN_IF_FATAL_ERROR");
		FATAL_ERROR();
		return;
	}
}

int main(void)
{
	int err;

	LOG_INF("UDP sample has started");

	/* Setup handler for Zephyr NET Connection Manager events. */
	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);

	/* Setup handler for Zephyr NET Connection Manager Connectivity layer. */
	net_mgmt_init_event_callback(&conn_cb, connectivity_event_handler, CONN_LAYER_EVENT_MASK);
	net_mgmt_add_event_callback(&conn_cb);

	/* Connecting to the configured connectivity layer.
	 * Wi-Fi or LTE depending on the board that the sample was built for.
	 */
	LOG_INF("Bringing network interface up and connecting to the network");

	err = conn_mgr_all_if_up(true);
	if (err) {
		LOG_ERR("conn_mgr_all_if_up, error: %d", err);
		FATAL_ERROR();
		return err;
	}

	err = conn_mgr_all_if_connect(true);
	if (err) {
		LOG_ERR("conn_mgr_all_if_connect, error: %d", err);
		FATAL_ERROR();
		return err;
	}

    for (int i = 0; i < MAX_PLAYERS; i++) {
        mc.players[i].S = serverClients[i].socket;
        mc.players[i].id = i;
        serverClients[i].id = i;
        mc.players[i].mc = &mc;
    }

	k_sem_take(&network_connected_sem, K_FOREVER);

	k_msleep(3000);

	k_thread_start(tcp4_thread_id);

	while (true) {
        mc.handle();
        k_msleep(20000);
	}

	return 0;
}
