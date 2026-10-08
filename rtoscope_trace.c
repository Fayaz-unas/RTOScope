/*
 * RTOScope: Real-Time Operating System Scheduler Visualizer
 * Trace Driver and Telemetry Capture Engine for NXP FRDM-MCXN236
 */

#include "rtoscope_trace.h"
#include "FreeRTOS.h"
#include "task.h"
#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include <string.h>

#define TRACE_QUEUE_SIZE 1024

typedef struct {
    uint32_t timestamp_us;
    uint8_t  task_id;
    uint8_t  event_type;
    uint8_t  payload;
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
static volatile uint32_t s_droppedEvents = 0;
static volatile bool s_tracingActive = false;

static TaskMeta_t s_taskRegistry[RTOSCOPE_MAX_TASKS];
static uint32_t s_cyclesPerUs = 150;
static volatile uint32_t s_lastCycCnt = 0;
static volatile uint64_t s_accumulatedUs = 0;

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

    s_lastCycCnt = DWT->CYCCNT;
    s_accumulatedUs = 0;
    s_queueHead = 0;
    s_queueTail = 0;
    s_droppedEvents = 0;
    memset(s_taskRegistry, 0, sizeof(s_taskRegistry));

    /* Pre-register Idle task */
    RTOScope_RegisterTask(0, "IDLE", 0);

    s_tracingActive = true;
}

uint32_t RTOScope_GetTimeUs(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint32_t currentCyc = DWT->CYCCNT;
    uint32_t elapsedCyc = currentCyc - s_lastCycCnt; /* Handles 32-bit unsigned wrap naturally */
    s_lastCycCnt = currentCyc;

    s_accumulatedUs += ((uint64_t)elapsedCyc / s_cyclesPerUs);
    uint32_t nowUs = (uint32_t)s_accumulatedUs;

    __set_PRIMASK(primask);
    return nowUs;
}

uint32_t RTOScope_GetDroppedEvents(void)
{
    return s_droppedEvents;
}

static inline void RTOScope_PushEvent(uint8_t type, uint8_t taskId, uint8_t payload, uint32_t timeUs)
{
    if (!s_tracingActive) {
        return;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint16_t nextHead = (s_queueHead + 1) % TRACE_QUEUE_SIZE;
    if (nextHead != s_queueTail) {
        s_eventQueue[s_queueHead].timestamp_us = timeUs;
        s_eventQueue[s_queueHead].task_id = taskId;
        s_eventQueue[s_queueHead].event_type = type;
        s_eventQueue[s_queueHead].payload = payload;
        s_queueHead = nextHead;
    } else {
        s_droppedEvents++;
    }

    __set_PRIMASK(primask);
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
    PRINTF("#START_REGISTRY\r\n");
    for (uint8_t i = 0; i < RTOSCOPE_MAX_TASKS; i++) {
        if (s_taskRegistry[i].active) {
            PRINTF("#T,%u,%s,%u\r\n", (unsigned int)s_taskRegistry[i].id, 
                   s_taskRegistry[i].name, (unsigned int)s_taskRegistry[i].priority);
        }
    }
    PRINTF("#END_REGISTRY\r\n");
}

static inline uint8_t RTOScope_ResolveTaskId(void *pxTCB)
{
    if (pxTCB == NULL) {
        return 0;
    }

    UBaseType_t taskNum = uxTaskGetTaskNumber((TaskHandle_t)pxTCB);
    if (taskNum < RTOSCOPE_MAX_TASKS) {
        return (uint8_t)taskNum;
    }

    return 0;
}

void RTOScope_RecordSwitchIn(void *pxTCB)
{
    uint8_t id = RTOScope_ResolveTaskId(pxTCB);
    RTOScope_PushEvent(RTOSCOPE_EVT_SWITCH_IN, id, 0, RTOScope_GetTimeUs());
}

void RTOScope_RecordSwitchOut(void *pxTCB)
{
    uint8_t id = RTOScope_ResolveTaskId(pxTCB);
    RTOScope_PushEvent(RTOSCOPE_EVT_SWITCH_OUT, id, 0, RTOScope_GetTimeUs());
}

void RTOScope_RecordTaskState(void *pxTCB, uint8_t state)
{
    if (pxTCB == NULL) {
        return;
    }
    uint8_t id = RTOScope_ResolveTaskId(pxTCB);
    RTOScope_PushEvent(RTOSCOPE_EVT_STATE, id, state, RTOScope_GetTimeUs());
}

void RTOScope_RecordCurrentTaskState(uint8_t state)
{
    TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
    if (currentTask != NULL) {
        RTOScope_RecordTaskState((void *)currentTask, state);
    }
}

void RTOScope_RecordEvent(uint8_t eventType, uint8_t idOrPayload, uint32_t timestampUs)
{
    if (eventType == RTOSCOPE_EVT_LED_BLUE || eventType == RTOSCOPE_EVT_LED_RED || eventType == RTOSCOPE_EVT_RFID) {
        RTOScope_PushEvent(eventType, 0, idOrPayload, timestampUs);
    } else {
        RTOScope_PushEvent(eventType, idOrPayload, 0, timestampUs);
    }
}

void RTOScope_FlushTrace(void)
{
    /* Drain up to 48 events per flush to smoothly empty bursts */
    for (uint32_t count = 0; count < 48; count++) {
        TraceEvent_t evt;
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        if (s_queueTail == s_queueHead) {
            __set_PRIMASK(primask);
            break;
        }

        evt = s_eventQueue[s_queueTail];
        s_queueTail = (s_queueTail + 1) % TRACE_QUEUE_SIZE;

        __set_PRIMASK(primask);

        if (evt.event_type == RTOSCOPE_EVT_SWITCH_IN || evt.event_type == RTOSCOPE_EVT_SWITCH_OUT) {
            PRINTF("#E,%c,%u,%u\r\n", evt.event_type, (unsigned int)evt.task_id, (unsigned int)evt.timestamp_us);
        } else if (evt.event_type == RTOSCOPE_EVT_LED_BLUE || evt.event_type == RTOSCOPE_EVT_LED_RED) {
            PRINTF("#E,%c,%u,%u\r\n", evt.event_type, (unsigned int)evt.payload, (unsigned int)evt.timestamp_us);
        } else if (evt.event_type == RTOSCOPE_EVT_BTN_IRQ) {
            PRINTF("#E,%c,%u,%u\r\n", evt.event_type, (unsigned int)evt.task_id, (unsigned int)evt.timestamp_us);
        } else if (evt.event_type == RTOSCOPE_EVT_RFID) {
            PRINTF("#E,K,%02X,%u\r\n", (unsigned int)evt.payload, (unsigned int)evt.timestamp_us);
        } else if (evt.event_type == RTOSCOPE_EVT_STATE) {
            PRINTF("#E,S,%u,%c,%u\r\n", (unsigned int)evt.task_id, (char)evt.payload, (unsigned int)evt.timestamp_us);
        }
    }

    static uint32_t s_lastReportedDrops = 0;
    if (s_droppedEvents != s_lastReportedDrops) {
        s_lastReportedDrops = s_droppedEvents;
        PRINTF("#D,%u\r\n", (unsigned int)s_droppedEvents);
    }
}
