#pragma once
#include "kernel.h"
#include "obj.h"

struct task;

void sec_token_system(struct token *t);
void sec_token_user(struct token *t, uint32_t user, const uint32_t *groups, uint32_t ngroups,
                    uint32_t privs);

int  sec_check(const struct token *tok, struct object *o, uint32_t desired, uint32_t *granted);
uint32_t sec_map_generic(const struct obj_type *t, uint32_t mask);

int  sec_kset(struct object *o, uint32_t owner, const struct ace *aces, int n);
int  sec_obj_init(struct object *o, uint32_t owner);
int  sec_inherit(struct object *child, const struct object *parent, const struct token *tok);

long sys_sec_dispatch(struct task *t, uint64_t nr, uint64_t a, uint64_t b, uint64_t c);

unsigned sectest_run(void);
