/*
 * RTOScope: Real-Time Operating System Scheduler Visualizer
 * Trace Driver and Telemetry Capture Engine for NXP FRDM-MCXN236
 */

#ifndef RTOSCOPE_TRACE_H
#define RTOSCOPE_TRACE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTOSCOPE_MAX_TASKS 8

/* Event type codes */
#define RTOSCOPE_EVT_SWITCH_IN   'I'  /* Context switch IN:  #E,I,<task_id>,<timestamp_us> */
#define RTOSCOPE_EVT_SWITCH_OUT  'O'  /* Context switch OUT: #E,O,<task_id>,<timestamp_us> */
#define RTOSCOPE_EVT_LED_BLUE    'B'  /* Blue LED state:     #E,B,<0|1>,<timestamp_us> */
#define RTOSCOPE_EVT_LED_RED     'R'  /* Red LED state:      #E,R,<0|1>,<timestamp_us> */
#define RTOSCOPE_EVT_BTN_IRQ     'P'  /* Button Press IRQ:   #E,P,<task_id>,<timestamp_us> */
#define RTOSCOPE_EVT_RFID        'K'  /* RFID Tag Read:      #E,K,<uid_hash_byte>,<timestamp_us> */
#define RTOSCOPE_EVT_STATE       'S'  /* Task State Change:  #E,S,<task_id>,<state_char>,<timestamp_us> */

/* State characters */
#define RTOSCOPE_STATE_RUNNING   'R'
#define RTOSCOPE_STATE_READY     'D'
#define RTOSCOPE_STATE_BLOCKED   'B'

/* Low-overhead Hook handlers invoked directly in FreeRTOS scheduler */
void RTOScope_RecordSwitchIn(void *pxTCB);
void RTOScope_RecordSwitchOut(void *pxTCB);
void RTOScope_RecordTaskState(void *pxTCB, uint8_t state);
void RTOScope_RecordCurrentTaskState(uint8_t state);

/* FreeRTOS trace hooks override */
#undef traceTASK_SWITCHED_IN
#define traceTASK_SWITCHED_IN()  do { \
    RTOScope_RecordSwitchIn((void *)pxCurrentTCB); \
    RTOScope_RecordTaskState((void *)pxCurrentTCB, RTOSCOPE_STATE_RUNNING); \
} while (0)

#undef traceTASK_SWITCHED_OUT
#define traceTASK_SWITCHED_OUT() RTOScope_RecordSwitchOut((void *)pxCurrentTCB)

#undef traceTASK_DELAY
#define traceTASK_DELAY()        RTOScope_RecordCurrentTaskState(RTOSCOPE_STATE_BLOCKED)

#undef traceTASK_DELAY_UNTIL
#define traceTASK_DELAY_UNTIL(xTimeToWake) RTOScope_RecordCurrentTaskState(RTOSCOPE_STATE_BLOCKED)

#undef traceBLOCKING_ON_QUEUE_RECEIVE
#define traceBLOCKING_ON_QUEUE_RECEIVE(pxQueue) RTOScope_RecordCurrentTaskState(RTOSCOPE_STATE_BLOCKED)

#undef traceTASK_NOTIFY_TAKE_BLOCK
#define traceTASK_NOTIFY_TAKE_BLOCK(xTicksToWait) RTOScope_RecordCurrentTaskState(RTOSCOPE_STATE_BLOCKED)

#undef traceTASK_NOTIFY_WAIT_BLOCK
#define traceTASK_NOTIFY_WAIT_BLOCK(xTicksToWait) RTOScope_RecordCurrentTaskState(RTOSCOPE_STATE_BLOCKED)

#undef traceMOVED_TASK_TO_READY_STATE
#define traceMOVED_TASK_TO_READY_STATE(pxTCB) RTOScope_RecordTaskState((void *)(pxTCB), RTOSCOPE_STATE_READY)

/* Lifecycle & Management Functions */
void RTOScope_Init(void);
uint32_t RTOScope_GetTimeUs(void);
void RTOScope_RecordEvent(uint8_t eventType, uint8_t idOrPayload, uint32_t timestampUs);
void RTOScope_RegisterTask(uint8_t id, const char *name, uint8_t priority);
void RTOScope_SendTaskRegistry(void);
void RTOScope_FlushTrace(void);
uint32_t RTOScope_GetDroppedEvents(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOSCOPE_TRACE_H */
