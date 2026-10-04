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
#define RTOSCOPE_EVT_SWITCH_IN   'I'
#define RTOSCOPE_EVT_SWITCH_OUT  'O'

/* Low-overhead Hook handlers invoked directly in FreeRTOS scheduler */
void RTOScope_RecordSwitchIn(void *pxTCB);
void RTOScope_RecordSwitchOut(void *pxTCB);

/* FreeRTOS trace hooks override */
#undef traceTASK_SWITCHED_IN
#define traceTASK_SWITCHED_IN()  RTOScope_RecordSwitchIn((void *)pxCurrentTCB)

#undef traceTASK_SWITCHED_OUT
#define traceTASK_SWITCHED_OUT() RTOScope_RecordSwitchOut((void *)pxCurrentTCB)

/* Lifecycle & Management Functions */
void RTOScope_Init(void);
void RTOScope_SetUartMutex(void *mutex);
uint32_t RTOScope_GetTimeUs(void);
void RTOScope_RegisterTask(uint8_t id, const char *name, uint8_t priority);
void RTOScope_SendTaskRegistry(void);
void RTOScope_FlushTrace(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOSCOPE_TRACE_H */
