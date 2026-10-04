/*
 * RTOScope: Real-Time Operating System Scheduler Visualizer
 * Trace Driver and Telemetry Capture Engine for NXP FRDM-MCXN236
 */

#include "rtoscope_trace.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include <string.h>

#define TRACE_QUEUE_SIZE 256

typedef struct {
    uint32_t timestamp_us;
    uint8_t  task_id;
    uint8_t  event_type;
} TraceEvent_t;

typedef struct {
    uint8_t id;
    char name[16];
    uint8_t priority;
    bool active;
} TaskMeta_t;

static TraceEvent_t s_eventQueue[TRACE_QUEUE_SIZE];
static volatile uint16_t s_queueHead = 0;
static volatile uint16_t s_queueTail = 0;
static volatile bool s_tracingActive = false;

static TaskMeta_t s_taskRegistry[RTOSCOPE_MAX_TASKS];
static uint32_t s_cyclesPerUs = 150;
static SemaphoreHandle_t s_traceMutex = NULL;

void RTOScope_SetUartMutex(void *mutex)
{
    s_traceMutex = (SemaphoreHandle_t)mutex;
}

void RTOScope_Init(void)
{
    /* Enable DWT Cycle Counter on Arm Cortex-M33 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    s_cyclesPerUs = SystemCoreClock / 1000000UL;
    if (s_cyclesPerUs == 0) {
        s_cyclesPerUs = 150; /* Default fallback to 150 MHz */
    }

    s_queueHead = 0;
    s_queueTail = 0;
    memset(s_taskRegistry, 0, sizeof(s_taskRegistry));

    /* Pre-register Idle task */
    RTOScope_RegisterTask(0, "IDLE", 0);

    s_tracingActive = true;
}

uint32_t RTOScope_GetTimeUs(void)
{
    return (uint32_t)(DWT->CYCCNT / s_cyclesPerUs);
}

void RTOScope_RegisterTask(uint8_t id, const char *name, uint8_t priority)
{
    if (id < RTOSCOPE_MAX_TASKS) {
        s_taskRegistry[id].id = id;
        strncpy(s_taskRegistry[id].name, name, sizeof(s_taskRegistry[id].name) - 1);
        s_taskRegistry[id].name[sizeof(s_taskRegistry[id].name) - 1] = '\0';
        s_taskRegistry[id].priority = priority;
        s_taskRegistry[id].active = true;
    }
}

void RTOScope_SendTaskRegistry(void)
{
    if (s_traceMutex != NULL) {
        xSemaphoreTake(s_traceMutex, portMAX_DELAY);
    }

    PRINTF("#START_REGISTRY\r\n");
    for (uint8_t i = 0; i < RTOSCOPE_MAX_TASKS; i++) {
        if (s_taskRegistry[i].active) {
            PRINTF("#T,%u,%s,%u\r\n", (unsigned int)s_taskRegistry[i].id, 
                   s_taskRegistry[i].name, (unsigned int)s_taskRegistry[i].priority);
        }
    }
    PRINTF("#END_REGISTRY\r\n");

    if (s_traceMutex != NULL) {
        xSemaphoreGive(s_traceMutex);
    }
}

static inline uint8_t RTOScope_ResolveTaskId(void *pxTCB)
{
    if (pxTCB == NULL) {
        return 0;
    }

    UBaseType_t taskNum = uxTaskGetTaskNumber((TaskHandle_t)pxTCB);
    if (taskNum > 0 && taskNum < RTOSCOPE_MAX_TASKS) {
        return (uint8_t)taskNum;
    }

    /* Check if it is the Idle task */
    const char *name = pcTaskGetName((TaskHandle_t)pxTCB);
    if (name != NULL && name[0] == 'I' && name[1] == 'D') {
        return 0;
    }

    return (uint8_t)taskNum;
}

void RTOScope_RecordSwitchIn(void *pxTCB)
{
    if (!s_tracingActive) {
        return;
    }

    uint16_t nextHead = (s_queueHead + 1) % TRACE_QUEUE_SIZE;
    if (nextHead != s_queueTail) {
        uint8_t id = RTOScope_ResolveTaskId(pxTCB);
        s_eventQueue[s_queueHead].timestamp_us = RTOScope_GetTimeUs();
        s_eventQueue[s_queueHead].task_id = id;
        s_eventQueue[s_queueHead].event_type = RTOSCOPE_EVT_SWITCH_IN;
        s_queueHead = nextHead;
    }
}

void RTOScope_RecordSwitchOut(void *pxTCB)
{
    if (!s_tracingActive) {
        return;
    }

    uint16_t nextHead = (s_queueHead + 1) % TRACE_QUEUE_SIZE;
    if (nextHead != s_queueTail) {
        uint8_t id = RTOScope_ResolveTaskId(pxTCB);
        s_eventQueue[s_queueHead].timestamp_us = RTOScope_GetTimeUs();
        s_eventQueue[s_queueHead].task_id = id;
        s_eventQueue[s_queueHead].event_type = RTOSCOPE_EVT_SWITCH_OUT;
        s_queueHead = nextHead;
    }
}

void RTOScope_FlushTrace(void)
{
    if (s_traceMutex != NULL) {
        xSemaphoreTake(s_traceMutex, portMAX_DELAY);
    }

    /* Stream up to 10 buffered trace events per flush so monitor task never starves other tasks */
    for (uint32_t count = 0; (count < 10) && (s_queueTail != s_queueHead); count++) {
        TraceEvent_t evt = s_eventQueue[s_queueTail];
        s_queueTail = (s_queueTail + 1) % TRACE_QUEUE_SIZE;
        PRINTF("#E,%c,%u,%u\r\n", evt.event_type, (unsigned int)evt.task_id, (unsigned int)evt.timestamp_us);
    }

    if (s_traceMutex != NULL) {
        xSemaphoreGive(s_traceMutex);
    }
}
