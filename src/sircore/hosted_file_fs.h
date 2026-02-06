#pragma once

#include <stdint.h>

#include "guest_mem.h"
#include "handles.h"

enum {
  ZI_FILE_O_READ = 1u << 0,
  ZI_FILE_O_WRITE = 1u << 1,
  ZI_FILE_O_CREATE = 1u << 2,
  ZI_FILE_O_TRUNC = 1u << 3,
  ZI_FILE_O_APPEND = 1u << 4,
};

typedef struct sir_hosted_file_fs_cfg {
  const char* fs_root;
} sir_hosted_file_fs_cfg_t;

typedef struct sir_hosted_file_fs {
  sir_hosted_file_fs_cfg_t cfg;
} sir_hosted_file_fs_t;

void sir_hosted_file_fs_init(sir_hosted_file_fs_t* fs, sir_hosted_file_fs_cfg_t cfg);

zi_handle_t sir_hosted_file_fs_open_from_params(sir_hosted_file_fs_t* fs, sem_handles_t* hs, sem_guest_mem_t* mem, zi_ptr_t params_ptr,
                                                zi_size32_t params_len);

