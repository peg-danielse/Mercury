/* Synthetic load for the policy integration tests.
 * spinner <threads> [duty_percent]   (default 100)
 * Each thread burns CPU for duty% of every 10 ms slice and sleeps the rest.
 * Started by Mercury with no arguments it reads SPIN_THREADS and SPIN_DUTY from the environment. */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int duty = 100;
static volatile uint64_t sink;

static int64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void* spin(void* arg) {
  (void)arg;
  uint64_t x = 0x9E3779B97F4A7C15ULL;
  const int64_t slice = 10000000LL;
  for (;;) {
    const int64_t start = now_ns();
    const int64_t burn = slice * duty / 100;
    while (now_ns() - start < burn) {
      x = x * 6364136223846793005ULL + 1;
    }
    sink = x;
    if (duty < 100) {
      struct timespec d = {0, (long)(slice - burn)};
      nanosleep(&d, NULL);
    }
  }
  return NULL;
}

int main(int argc, char** argv) {
  int threads = 1;
  const char* t = getenv("SPIN_THREADS");
  const char* d = getenv("SPIN_DUTY");
  if (argc > 1 && argv[1][0] != '-') threads = atoi(argv[1]);
  else if (t) threads = atoi(t);
  if (argc > 2) duty = atoi(argv[2]);
  else if (d) duty = atoi(d);
  if (threads < 1) threads = 1;
  if (duty < 1) duty = 1;
  if (duty > 100) duty = 100;
  printf("spinner threads=%d duty=%d pid=%d\n", threads, duty, (int)getpid());
  fflush(stdout);
  for (int i = 1; i < threads; ++i) {
    pthread_t id;
    pthread_create(&id, NULL, spin, NULL);
  }
  spin(NULL);
  return 0;
}
