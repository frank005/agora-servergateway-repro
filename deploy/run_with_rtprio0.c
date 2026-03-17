/* Wrapper that sets RLIMIT_RTPRIO=0 then execs the repro. Use as container entrypoint
   so Kubernetes (which has no ulimits.rtprio) also runs with RT priority limit 0. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/resource.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#define REPRO_BIN "/app/repro_pthread_init"

int main(int argc, char **argv) {
  struct rlimit r = { 0, 0 };
  (void)setrlimit(RLIMIT_RTPRIO, &r);
  fputs("[entrypoint] RLIMIT_RTPRIO=0 set, exec'ing repro.\n", stderr);

  if (argc > 0 && argv) {
    argv[0] = (char *)REPRO_BIN;
    execv(REPRO_BIN, argv);
  } else {
    char *args[] = { (char *)REPRO_BIN, NULL };
    execv(REPRO_BIN, args);
  }
  return 127;
}
