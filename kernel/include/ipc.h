#pragma once
#include "kernel.h"
#include "abi.h"

#define IPC_NAME_NULL 0
#define IPC_NAME_DEAD 0xFFFFFFFFu

struct ipc_space;

struct ipc_space *ipc_space_create(void);
void              ipc_space_destroy(struct ipc_space *s);

int  ipc_port_alloc(struct ipc_space *s, uint32_t *name);
int  ipc_port_destroy(struct ipc_space *s, uint32_t name);
int  ipc_port_dealloc(struct ipc_space *s, uint32_t name);
int  ipc_port_insert(struct ipc_space *s, uint32_t name, uint32_t disp);
int  ipc_port_info(struct ipc_space *s, uint32_t name, struct port_info *out);
int  ipc_port_set_qlimit(struct ipc_space *s, uint32_t name, uint32_t limit);

int  ipc_grant(struct ipc_space *dst, struct ipc_space *src, uint32_t src_name,
               uint32_t disp, uint32_t *dst_name);

int  ipc_send(struct ipc_space *s, const void *msg, size_t size, uint64_t timeout_ms);
long ipc_recv(struct ipc_space *s, uint32_t name, void *buf, size_t cap, uint64_t timeout_ms);

unsigned ipc_live_ports(void);
unsigned ipc_live_msgs(void);
uint64_t ipc_msgs_sent(void);

unsigned ipctest_run(void);
