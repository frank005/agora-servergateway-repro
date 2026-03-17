/* Wrapper that sets RLIMIT_RTPRIO=0 then execs the repro. Use as container entrypoint
   so Kubernetes (which has no ulimits.rtprio) also runs with RT priority limit 0.
   Set AGORA_REPRO_IMPL=v2 to run repro_v2_full (v2 C API + dlopen) instead of the
   default repro_pthread_init (C++ LL API). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/resource.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define REPRO_BIN_CPP "/app/repro_pthread_init"
#define REPRO_BIN_V2  "/app/repro_v2_full"

int main(int argc, char **argv) {
  struct rlimit r = { 0, 0 };
  (void)setrlimit(RLIMIT_RTPRIO, &r);

  const char* impl = getenv("AGORA_REPRO_IMPL");
  const char* bin  = (impl && strcmp(impl, "v2") == 0) ? REPRO_BIN_V2 : REPRO_BIN_CPP;

  fprintf(stderr, "[entrypoint] RLIMIT_RTPRIO=0 set, exec'ing %s.\n", bin);

  if (argc > 0 && argv) {
    argv[0] = (char *)bin;
    execv(bin, argv);
  } else {
    char *args[] = { (char *)bin, NULL };
    execv(bin, args);
  }
  return 127;
}
