#include "sir_jsonl.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit: %s\n", msg);
  return 1;
}

int main(void) {
  char path[] = "/tmp/sem_trace_smoke_XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) return fail("mkstemp failed");
  close(fd);

  const int rc = sem_run_sir_jsonl_trace_ex(SEM_SOURCE_DIR "/src/sircc/examples/cfg_if.sir.jsonl", NULL, 0, NULL, SEM_DIAG_TEXT, false, path);
  if (rc != 111) {
    fprintf(stderr, "sem_unit: expected rc=111 got rc=%d\n", rc);
    unlink(path);
    return fail("unexpected return code");
  }

  FILE* f = fopen(path, "rb");
  if (!f) {
    unlink(path);
    return fail("failed to open trace output");
  }
  char line[512];
  const bool ok = (fgets(line, sizeof(line), f) != NULL) && (strstr(line, "\"k\":\"trace_step\"") != NULL);
  fclose(f);
  unlink(path);
  if (!ok) return fail("trace output missing trace_step record");
  return 0;
}
