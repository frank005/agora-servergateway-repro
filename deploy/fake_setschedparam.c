/*
 * Force repro of glibc tpp.c assertion: pretend pthread_setschedparam succeeded
 * without actually changing the thread. The thread stays at SCHED_OTHER/priority 0.
 * When it later locks a PTHREAD_PRIO_INHERIT mutex, glibc sees new_prio == 0 and aborts.
 *
 * Build: gcc -shared -fPIC -o fake_setschedparam.so fake_setschedparam.c -ldl
 * Run:   LD_PRELOAD=/path/to/fake_setschedparam.so ./repro_pthread_init
 *        or in Docker: -e LD_PRELOAD=/app/fake_setschedparam.so
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>

int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param) {
  (void)thread;
  (void)policy;
  (void)param;
  /* Do not call the real function; just report success. Thread is never changed
   * (stays SCHED_OTHER/0). SDK thinks it set RT priority; when this thread locks
   * a PRIO_INHERIT mutex, glibc sees priority 0 and triggers the assertion. */
  return 0;
}
