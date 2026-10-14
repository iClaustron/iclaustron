/* xxh3_bench: how long XXH3 takes in the xxHash the data nodes are
 * built with, over the same keys as ic_hashbench, printed the same way;
 * see tools/ic_hashbench/src/main.rs for how to build and run both.
 *
 * Copyright (c) 2026 Hopsworks and/or its affiliates.
 * Licensed under the MIT License. See LICENSE in the repository root. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The xxHash the data nodes call XXH3_64bits from, all of it inlined,
 * as their build compiles it. */
#define XXH_INLINE_ALL
#include "xxhash.h"

static const size_t sizes[] = { 4, 8, 16, 32, 64, 128, 240, 1024 };

static uint64_t iterations(size_t len)
{
  if (len <= 64)
    return 20000000;
  if (len <= 240)
    return 5000000;
  return 1000000;
}

static double now_nanos(void)
{
  struct timespec t;

  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

int main(void)
{
  static unsigned char key[1024];
  size_t s;

  printf("%6s %9s %18s\n", "bytes", "XXH3 ns", "XXH3 check");
  for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
    size_t len = sizes[s];
    size_t n_stamp = len < 4 ? len : 4;
    uint64_t n = iterations(len);
    uint64_t sink = 0;
    uint64_t i;
    double start;
    double nanos;

    /* Byte i is i times 7 plus 1, as in ic_hashbench. */
    for (i = 0; i < len; i++)
      key[i] = (unsigned char)(i * 7 + 1);
    start = now_nanos();
    for (i = 0; i < n; i++) {
      /* The first bytes are the iteration's number, little-endian. */
      uint32_t stamp = (uint32_t)i;
      unsigned char bytes[4];

      bytes[0] = (unsigned char)stamp;
      bytes[1] = (unsigned char)(stamp >> 8);
      bytes[2] = (unsigned char)(stamp >> 16);
      bytes[3] = (unsigned char)(stamp >> 24);
      memcpy(key, bytes, n_stamp);
      sink ^= XXH3_64bits(key, len);
    }
    nanos = now_nanos() - start;
    printf("%6zu %9.2f %18.16llx\n", len, nanos / (double)n,
           (unsigned long long)sink);
  }
  return 0;
}
