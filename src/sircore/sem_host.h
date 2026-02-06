#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
  SEM_ZI_OK = 0,
  SEM_ZI_E_INVALID = -1,
  SEM_ZI_E_BOUNDS = -2,
  SEM_ZI_E_NOSYS = -7,
  SEM_ZI_E_INTERNAL = -10,
};

enum {
  SEM_ZI_CTL_OP_CAPS_LIST = 1,
};

enum {
  SEM_ZI_CAP_CAN_OPEN = 1u << 0,
  SEM_ZI_CAP_PURE = 1u << 1,
  SEM_ZI_CAP_MAY_BLOCK = 1u << 2,
};

typedef struct sem_cap {
  const char* kind;
  const char* name;
  uint32_t flags;
  const uint8_t* meta;
  uint32_t meta_len;
} sem_cap_t;

typedef struct sem_host_cfg {
  const sem_cap_t* caps;
  uint32_t cap_count;
} sem_host_cfg_t;

typedef struct sem_host {
  sem_host_cfg_t cfg;
} sem_host_t;

void sem_host_init(sem_host_t* h, sem_host_cfg_t cfg);

int32_t sem_zi_ctl(void* user, const uint8_t* req, uint32_t req_len, uint8_t* resp, uint32_t resp_cap);

bool sem_build_caps_list_req(uint32_t rid, uint8_t* out, uint32_t cap, uint32_t* out_len);

