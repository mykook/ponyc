#ifdef __linux__
#define _GNU_SOURCE
#endif
#include <platform.h>

#if defined(PLATFORM_IS_LINUX) || defined(PLATFORM_IS_FREEBSD)
  #include <sched.h>
  #include <stdlib.h>
  #include <unistd.h>
  #include <stdio.h>
#elif defined(PLATFORM_IS_MACOSX)
  #include <unistd.h>
  #include <mach/mach.h>
  #include <mach/thread_policy.h>
#elif defined(PLATFORM_IS_WINDOWS)
  #include <processtopologyapi.h>
#endif

#include "cpu.h"
#include "../mem/pool.h"

#if defined(PLATFORM_IS_MACOSX) || defined(PLATFORM_IS_FREEBSD)

#include <sys/types.h>
#include <sys/sysctl.h>

static uint32_t property(const char* key)
{
  int value;
  size_t len = sizeof(int);
  sysctlbyname(key, &value, &len, NULL, 0);
  return value;
}
#endif

#if defined(PLATFORM_IS_LINUX)
static bool cpu_physical(uint32_t cpu)
{
  char file[FILENAME_MAX];
  snprintf(file, FILENAME_MAX,
    "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);

  FILE* fp = fopen(file, "r");

  if(fp != NULL)
  {
    char name[16];
    size_t len = fread(name, 1, 15, fp);
    name[len] = '\0';
    fclose(fp);

    if(cpu != (uint32_t)atoi(name))
      return false;
  }

  return true;
}
#endif

uint32_t cpu_count()
{
#if defined(PLATFORM_IS_LINUX)
  uint32_t count = pony_numa_cores();

  if(count > 0)
    return count;

  uint32_t max = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
  count = 0;

  for(uint32_t i = 0; i < max; i++)
  {
    if(cpu_physical(i))
      count++;
  }

  return count;
#elif defined(PLATFORM_IS_FREEBSD)
  return property("hw.ncpu");
#elif defined(PLATFORM_IS_MACOSX)
  return property("hw.physicalcpu");
#elif defined(PLATFORM_IS_WINDOWS)
  PSYSTEM_LOGICAL_PROCESSOR_INFORMATION info = NULL;
  PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = NULL;
  DWORD len = 0;
  DWORD offset = 0;
  uint32_t count = 0;

  while(true)
  {
    DWORD rc = GetLogicalProcessorInformation(info, &len);

    if(rc != FALSE)
      break;

    if(GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
      ptr = info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(len);
      continue;
    } else {
      return 0;
    }
  }

  while((offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION)) <= len)
  {
    switch(info->Relationship)
    {
      case RelationProcessorCore:
        count++;
        break;

      default: {}
    }

    offset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    info++;
  }

  if(ptr != NULL)
    free(ptr);

  return count;
#endif
}

void cpu_assign(uint32_t count, scheduler_t* scheduler)
{
#if defined(PLATFORM_IS_LINUX)
  uint32_t cpu_count = pony_numa_cores();

  if(cpu_count > 0)
  {
    uint32_t* list = pool_alloc_size(cpu_count * sizeof(uint32_t));
    pony_numa_core_list(list);

    for(uint32_t i = 0; i < count; i++)
    {
      uint32_t cpu = list[i % cpu_count];
      scheduler[i].cpu = cpu;
      scheduler[i].node = pony_numa_node_of_cpu(cpu);
    }

    pool_free_size(cpu_count * sizeof(uint32_t), list);
    return;
  }

  // Physical cores come first, so assign in sequence.
  cpu_count = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);

  for(uint32_t i = 0; i < count; i++)
  {
    scheduler[i].cpu = i % cpu_count;
    scheduler[i].node = 0;
  }
#elif defined(PLATFORM_IS_FREEBSD)
  // Spread across available cores.
  uint32_t cpu_count = property("hw.ncpu");

  for(uint32_t i = 0; i < count; i++)
  {
    scheduler[i].cpu = i % cpu_count;
    scheduler[i].node = 0;
  }
#else
  // Affinity groups rather than processor numbers.
  for(uint32_t i = 0; i < count; i++)
  {
    scheduler[i].cpu = i;
    scheduler[i].node = 0;
  }
#endif
}

void cpu_affinity(uint32_t cpu)
{
#if defined(PLATFORM_IS_LINUX) || defined(PLATFORM_IS_FREEBSD)
  // Affinity is handled when spawning the thread.
  (void)cpu;
#elif defined(PLATFORM_IS_MACOSX)
  thread_affinity_policy_data_t policy;
  policy.affinity_tag = cpu;

  thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY,
    (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT);
#elif defined(PLATFORM_IS_WINDOWS)
  GROUP_AFFINITY affinity;
  affinity.Mask = (uint64_t)1 << (cpu >> 6);
  affinity.Group = (cpu % 64);

  SetThreadGroupAffinity(pony_thread_self(), &affinity, NULL);
#endif
}

uint64_t cpu_rdtsc()
{
  return __pony_rdtsc();
}

/**
 * Only nanosleep if sufficient cycles have elapsed.
 */
void cpu_core_pause(uint64_t tsc, bool yield)
{
  uint64_t tsc2 = cpu_rdtsc();

  // 10m cycles is about 3ms
  if((tsc2 - tsc) < 10000000)
    return;

#ifndef PLATFORM_IS_WINDOWS
  struct timespec ts = {0, 0};

  if(yield)
  {
    // A billion cycles is roughly half a second, depending on clock speed.
    if((tsc2 - tsc) > 10000000000)
    {
      // If it has been 10 billion cycles, pause 30 ms.
      ts.tv_nsec = 30000000;
    } else if((tsc2 - tsc) > 3000000000) {
      // If it has been 3 billion cycles, pause 10 ms.
      ts.tv_nsec = 10000000;
    } else if((tsc2 - tsc) > 1000000000) {
      // If it has been 1 billion cycles, pause 1 ms.
      ts.tv_nsec = 1000000;
    }
  }

  nanosleep(&ts, NULL);
#else
  Sleep(0);
#endif
}
