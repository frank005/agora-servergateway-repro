#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <cmath>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <dlfcn.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "c/api2/agora_service.h"
#include "c/api2/agora_rtc_conn.h"
#include "c/api2/agora_local_user.h"

// Helper env readers (trim + bool/int parsing) can be shared with main repro if desired.

