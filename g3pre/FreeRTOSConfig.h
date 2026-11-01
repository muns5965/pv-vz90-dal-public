/* pv-vz90-dal g3pre: FreeRTOS configuration for the PV test image. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H
#include <stdint.h>
#define configCPU_CLOCK_HZ				1000u	/* port.c divides this by the tick rate: PV timer counts at 1 kHz */
#define configTICK_RATE_HZ				100u	/* 10 ms tick */
#define configUSE_PREEMPTION			1
#define configUSE_TIME_SLICING			1
#define configUSE_IDLE_HOOK				0
#define configUSE_TICK_HOOK				0
#define configMAX_PRIORITIES			32	/* Palm priorities map onto 1..29; 30 = hardware events, 31 = KAL timers (g4m) */
#define configMINIMAL_STACK_SIZE		256
#define configTOTAL_HEAP_SIZE			(512 * 1024)	/* OS task stacks come from here (g4m) */
#define configAPPLICATION_ALLOCATED_HEAP	1	/* ucHeap defined in pv_bsp.c, so the DAL can tell its kernel heap range (g4i) */
#define configMAX_TASK_NAME_LEN			8
#define configUSE_16_BIT_TICKS			0
#define configIDLE_SHOULD_YIELD			1
#define configUSE_MUTEXES				1
#define INCLUDE_xTaskGetSchedulerState	1	/* RAL lock only once the scheduler runs (g4k) */
#define configUSE_RECURSIVE_MUTEXES		1	/* KAL mutexes are recursive (g4g) */
#define configUSE_COUNTING_SEMAPHORES	1
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS	1	/* debug line buffer slot per task (g4r) */
#define configTASK_NOTIFICATION_ARRAY_ENTRIES	3	/* 0 FreeRTOS/DAL use, 1 KAL object waits, 2 KALTaskWait counter (g4m) */
#define configQUEUE_REGISTRY_SIZE		0
#define configCHECK_FOR_STACK_OVERFLOW	0
#define configSUPPORT_DYNAMIC_ALLOCATION	1
#define configSUPPORT_STATIC_ALLOCATION		0
#define configUSE_TIMERS				1
#define configTIMER_TASK_PRIORITY		3
#define configTIMER_QUEUE_LENGTH		8
#define configTIMER_TASK_STACK_DEPTH	256
#define INCLUDE_vTaskDelay				1
#define INCLUDE_vTaskSuspend			1
#define INCLUDE_vTaskDelete			1	/* KALTaskDelete, task entry return (g4m) */
#define INCLUDE_xTaskGetCurrentTaskHandle	1
#define configUSE_TRACE_FACILITY		1	/* vTaskGetInfo(): stack base for KALTaskGetInfo (g4a) */
void g3_assert(const char *file, int line);
#define configASSERT(x)	do { if (!(x)) g3_assert(__FILE__, __LINE__); } while (0)
extern volatile uint32_t gTaskSwitches;
#define traceTASK_SWITCHED_IN()	(gTaskSwitches++)
#endif
