#include "sir_jsonl.h"

#include <stdio.h>

static int fail(const char* msg) {
  fprintf(stderr, "sem_unit: %s\n", msg);
  return 1;
}

int main(void) {
  // 74565 (0x12345) stored into i16 truncates to 0x2345 == 9029.
  const int rc = sem_run_sir_jsonl(SEM_SOURCE_DIR "/src/sem/tests/fixtures/i16_store_load_zext.sir.jsonl", NULL, 0, NULL);
  if (rc != 9029) {
    fprintf(stderr, "sem_unit: expected rc=9029 got rc=%d\n", rc);
    return fail("unexpected return code");
  }
  return 0;
}

