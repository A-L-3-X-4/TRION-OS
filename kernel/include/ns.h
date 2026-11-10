#pragma once
#include "kernel.h"
#include "obj.h"

#define NS_PATH_MAX 128
#define NS_NAME_MAX 31
#define NS_DIR_MAX  128

int ns_init(void);
int ns_mkdir(const char *path);
int ns_insert(const char *path, struct object *o);
int ns_lookup(const char *path, struct object **out);
int ns_unlink(const char *path);
int ns_list(const char *path, unsigned idx, char *name, uint32_t *type);

struct token;
int ns_mkdir_as(const struct token *tok, const char *path);
int ns_insert_as(const struct token *tok, const char *path, struct object *o);
int ns_lookup_as(const struct token *tok, const char *path, struct object **out);
int ns_unlink_as(const struct token *tok, const char *path);

struct ipc_space;
int ns_publish(const char *path, struct ipc_space *s, uint32_t name);
int ns_lookup_port(const char *path, struct ipc_space *s, uint32_t *name);

int ns_publish_as(const struct token *tok, const char *path, struct ipc_space *s, uint32_t name);
int ns_lookup_port_as(const struct token *tok, const char *path, struct ipc_space *s, uint32_t *name);

extern const struct obj_type directory_type, portref_type;
struct ipc_space *ns_kernel_space(void);
struct task;
long sys_ns_dispatch(struct task *t, uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d);

unsigned nstest_run(void);
