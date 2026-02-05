// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdint.h>

extern int32_t sir_add2(int32_t a, int32_t b);

int main(void) {
  int32_t r = sir_add2(3, 4);
  return (r == 7) ? 0 : 1;
}

