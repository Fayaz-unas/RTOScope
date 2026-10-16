/*
 * RTOScope - FreeRTOS Configuration Fragment
 * Injected automatically into FreeRTOSConfig.h via CONFIG_FREERTOS_USE_CUSTOM_CONFIG_FRAGMENT
 */

#ifndef FREERTOS_CONFIG_FRAG_H
#define FREERTOS_CONFIG_FRAG_H

#undef configMAX_PRIORITIES
#define configMAX_PRIORITIES 8

#include "rtoscope_trace.h"

#endif /* FREERTOS_CONFIG_FRAG_H */
