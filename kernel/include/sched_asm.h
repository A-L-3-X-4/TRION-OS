#pragma once
#include <stdint.h>

void ctx_switch(uint64_t *save_rsp, uint64_t new_rsp);
