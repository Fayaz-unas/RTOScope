/*
 * RTOScope: Real-Time Operating System Scheduler Visualizer
 * NXP FRDM-MCXN236 FreeRTOS Firmware Implementation
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "semphr.h"

/* NXP MCUXpresso Drivers */
#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include "fsl_port.h"
#include "fsl_gpio.h"
#include "board.h"
#include "app.h"

/* RTOScope Telemetry & Trace Engine */
#include "rtoscope_trace.h"

/*******************************************************************************
 * Task ID & Priority Definitions
 ******************************************************************************/
#define TASK_ID_IDLE             0
#define TASK_ID_LED_BLINK        1
#define TASK_ID_BUTTON_PRESS     2
#define TASK_ID_SENSOR_READ      3
#define TASK_ID_UART_PRINT       4
#define TASK_ID_SCHED_MONITOR    5

/* Priorities: Monitor runs at Low Priority so it never starves user workloads */
#define PRIORITY_SCHED_MONITOR   (1U) /* Background Trace Streamer */
#define PRIORITY_LED_BLINK       (1U) /* Low Priority */
#define PRIORITY_UART_PRINT      (2U) /* Medium Priority */
#define PRIORITY_SENSOR_READ     (3U) /* Medium-High Priority */
#define PRIORITY_BUTTON_PRESS    (4U) /* Highest Priority (Preemptive) */

#define RTOSCOPE_STACK_SIZE      (configMINIMAL_STACK_SIZE + 128)

/*******************************************************************************
 * Task Handles & Synchronization
 ******************************************************************************/
static TaskHandle_t s_hLedTask       = NULL;
static TaskHandle_t s_hButtonTask    = NULL;
static TaskHandle_t s_hSensorTask    = NULL;
static TaskHandle_t s_hUartTask      = NULL;
static TaskHandle_t s_hMonitorTask   = NULL;

static SemaphoreHandle_t s_uartMutex = NULL;

/*******************************************************************************
 * Helper: Busy loop to simulate realistic workload
 ******************************************************************************/
static void simulate_workload_ms(uint32_t ms)
{
    /* SystemCoreClock is ~150MHz. 1ms is ~150,000 cycles (~37,500 iterations) */
    volatile uint32_t count = ms * (SystemCoreClock / 4000UL);
    while (count--)
    {
        __asm("NOP");
    }
}

/*******************************************************************************
 * Hardware Pin Initialization (LEDs & Buttons)
 ******************************************************************************/
static void RTOScope_InitHardwarePins(void)
{
    /* Enable PORT and GPIO peripheral clocks */
    CLOCK_EnableClock(kCLOCK_Port0);
    CLOCK_EnableClock(kCLOCK_Port4);
    CLOCK_EnableClock(kCLOCK_Gpio0);
    CLOCK_EnableClock(kCLOCK_Gpio4);

    /* Blue LED (P4_17) & Red LED (P4_18) Output Pins */
    const port_pin_config_t led_pin_cfg = {
        .pullSelect = kPORT_PullDisable,
        .pullValueSelect = kPORT_LowPullResistor,
        .slewRate = kPORT_FastSlewRate,
        .passiveFilterEnable = kPORT_PassiveFilterDisable,
        .openDrainEnable = kPORT_OpenDrainDisable,
        .driveStrength = kPORT_LowDriveStrength,
        .mux = kPORT_MuxAlt0,
        .inputBuffer = 1,
        .invertInput = 0,
        .lockRegister = 0
    };
    PORT_SetPinConfig(PORT4, BOARD_LED_BLUE_GPIO_PIN, &led_pin_cfg);
    PORT_SetPinConfig(PORT4, BOARD_LED_RED_GPIO_PIN, &led_pin_cfg);

    const gpio_pin_config_t led_out_cfg = {
        kGPIO_DigitalOutput,
        LOGIC_LED_OFF
    };
    GPIO_PinInit(BOARD_LED_BLUE_GPIO, BOARD_LED_BLUE_GPIO_PIN, &led_out_cfg);
    GPIO_PinInit(BOARD_LED_RED_GPIO, BOARD_LED_RED_GPIO_PIN, &led_out_cfg);

    /* User Button SW3 (P0_6) and SW2 (P0_20) with pull-up and input buffer */
    const port_pin_config_t btn_pin_cfg = {
        .pullSelect = kPORT_PullUp,
        .pullValueSelect = kPORT_LowPullResistor,
        .slewRate = kPORT_FastSlewRate,
        .passiveFilterEnable = kPORT_PassiveFilterDisable,
        .openDrainEnable = kPORT_OpenDrainDisable,
        .driveStrength = kPORT_LowDriveStrength,
        .mux = kPORT_MuxAlt0,
        .inputBuffer = 1,
        .invertInput = 0,
        .lockRegister = 0
    };
    PORT_SetPinConfig(PORT0, 6U, &btn_pin_cfg);
    PORT_SetPinConfig(PORT0, 20U, &btn_pin_cfg);

    const gpio_pin_config_t btn_in_cfg = {
        kGPIO_DigitalInput,
        0
    };
    GPIO_PinInit(GPIO0, 6U, &btn_in_cfg);
    GPIO_PinInit(GPIO0, 20U, &btn_in_cfg);
}

/*******************************************************************************
 * Task 1: LED Blink Task (Low Priority)
 * Toggles onboard Blue LED at 500ms periodic intervals.
 ******************************************************************************/
static void vLedBlinkTask(void *pvParameters)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(500);

    for (;;)
    {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        LED_BLUE_TOGGLE();

        /* Simulate lightweight computational workload (3ms) */
        simulate_workload_ms(3);
    }
}

/*******************************************************************************
 * Task 2: Button Press Task (High Priority)
 * Senses SW3 / SW2 button press; preempts lower priority tasks immediately.
 ******************************************************************************/
static void vButtonPressTask(void *pvParameters)
{
    for (;;)
    {
        /* Check SW3 (P0_6, main user button) and SW2 (P0_20) - active LOW when pressed */
        uint32_t sw3 = GPIO_PinRead(GPIO0, 6U);
        uint32_t sw2 = GPIO_PinRead(GPIO0, 20U);

        if (sw3 == 0 || sw2 == 0)
        {
            LED_RED_ON();

            if (s_uartMutex != NULL)
            {
                xSemaphoreTake(s_uartMutex, portMAX_DELAY);
            }
            PRINTF("#M,[PREEMPTION] Button %s pressed! High-priority task executed.\r\n", 
                   (sw3 == 0) ? "SW3" : "SW2");
            if (s_uartMutex != NULL)
            {
                xSemaphoreGive(s_uartMutex);
            }

            /* High priority burst execution (25ms) - clearly visible preemptive block */
            simulate_workload_ms(25);

            LED_RED_OFF();

            /* Debounce delay and wait until button release */
            while ((GPIO_PinRead(GPIO0, 6U) == 0) || (GPIO_PinRead(GPIO0, 20U) == 0))
            {
                vTaskDelay(pdMS_TO_TICKS(30));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/*******************************************************************************
 * Task 3: Sensor Read Task (Medium Priority)
 * Periodically simulates sensor acquisition and digital signal filtering.
 ******************************************************************************/
static void vSensorReadTask(void *pvParameters)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(200);

    for (;;)
    {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        /* Simulate sensor data acquisition & processing (6ms) */
        simulate_workload_ms(6);
    }
}

/*******************************************************************************
 * Task 4: UART Print Task (Medium Priority)
 * Sends periodic status and telemetry diagnostics every 1000ms.
 ******************************************************************************/
static void vUartPrintTask(void *pvParameters)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1000);

    for (;;)
    {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        uint32_t uptimeMs = (uint32_t)(xTaskGetTickCount() * 1000UL / configTICK_RATE_HZ);
        uint32_t freeHeap = (uint32_t)xPortGetFreeHeapSize();

        if (s_uartMutex != NULL)
        {
            xSemaphoreTake(s_uartMutex, portMAX_DELAY);
        }
        PRINTF("#M,[TELEMETRY] Uptime: %u ms | Free Heap: %u bytes\r\n", 
               (unsigned int)uptimeMs, (unsigned int)freeHeap);
        if (s_uartMutex != NULL)
        {
            xSemaphoreGive(s_uartMutex);
        }
    }
}

/*******************************************************************************
 * Task 5: Scheduler Monitor Task (Background Priority)
 * Captures and flushes scheduler trace events to UART for live visualization.
 ******************************************************************************/
static void vSchedulerMonitorTask(void *pvParameters)
{
    uint32_t registryCounter = 0;

    /* Initial registry broadcast */
    RTOScope_SendTaskRegistry();

    for (;;)
    {
        /* Flush buffered trace events every 35ms */
        vTaskDelay(pdMS_TO_TICKS(35));
        RTOScope_FlushTrace();

        /* Re-broadcast task registry every 2 seconds for newly connected clients */
        if (++registryCounter >= 60)
        {
            registryCounter = 0;
            RTOScope_SendTaskRegistry();
        }
    }
}

/*******************************************************************************
 * Application Main Entry Point
 ******************************************************************************/
int main(void)
{
    /* Board hardware, clock and debug console initialization */
    BOARD_InitHardware();
    RTOScope_InitHardwarePins();

    /* Create Mutex for thread-safe UART transmission */
    s_uartMutex = xSemaphoreCreateMutex();
    RTOScope_SetUartMutex(s_uartMutex);

    /* Initialize RTOScope DWT hardware timer and trace buffers */
    RTOScope_Init();

    PRINTF("\r\n========================================\r\n");
    PRINTF(" RTOScope: Scheduler Visualizer Starting\r\n");
    PRINTF(" Target: NXP FRDM-MCXN236 (Cortex-M33)\r\n");
    PRINTF(" SW3 (Pin 6) = %u | SW2 (Pin 20) = %u\r\n", 
           (unsigned int)GPIO_PinRead(GPIO0, 6U), (unsigned int)GPIO_PinRead(GPIO0, 20U));
    PRINTF("========================================\r\n");

    /* Register Tasks with RTOScope */
    RTOScope_RegisterTask(TASK_ID_IDLE,          "IDLE",          0);
    RTOScope_RegisterTask(TASK_ID_LED_BLINK,     "LED_Blink",     PRIORITY_LED_BLINK);
    RTOScope_RegisterTask(TASK_ID_BUTTON_PRESS,  "Button_Press",  PRIORITY_BUTTON_PRESS);
    RTOScope_RegisterTask(TASK_ID_SENSOR_READ,   "Sensor_Read",   PRIORITY_SENSOR_READ);
    RTOScope_RegisterTask(TASK_ID_UART_PRINT,    "UART_Print",    PRIORITY_UART_PRINT);
    RTOScope_RegisterTask(TASK_ID_SCHED_MONITOR, "Sched_Monitor", PRIORITY_SCHED_MONITOR);

    /* 1. LED Blink Task (Low Priority) */
    if (xTaskCreate(vLedBlinkTask, "LED_Blink", RTOSCOPE_STACK_SIZE, NULL, 
                    PRIORITY_LED_BLINK, &s_hLedTask) == pdPASS)
    {
        vTaskSetTaskNumber(s_hLedTask, TASK_ID_LED_BLINK);
    }

    /* 2. Button Press Task (High Priority) */
    if (xTaskCreate(vButtonPressTask, "Button_Press", RTOSCOPE_STACK_SIZE, NULL, 
                    PRIORITY_BUTTON_PRESS, &s_hButtonTask) == pdPASS)
    {
        vTaskSetTaskNumber(s_hButtonTask, TASK_ID_BUTTON_PRESS);
    }

    /* 3. Sensor Read Task (Medium-High Priority) */
    if (xTaskCreate(vSensorReadTask, "Sensor_Read", RTOSCOPE_STACK_SIZE, NULL, 
                    PRIORITY_SENSOR_READ, &s_hSensorTask) == pdPASS)
    {
        vTaskSetTaskNumber(s_hSensorTask, TASK_ID_SENSOR_READ);
    }

    /* 4. UART Print Task (Medium Priority) */
    if (xTaskCreate(vUartPrintTask, "UART_Print", RTOSCOPE_STACK_SIZE, NULL, 
                    PRIORITY_UART_PRINT, &s_hUartTask) == pdPASS)
    {
        vTaskSetTaskNumber(s_hUartTask, TASK_ID_UART_PRINT);
    }

    /* 5. Scheduler Monitor Task (Background Priority) */
    if (xTaskCreate(vSchedulerMonitorTask, "Sched_Monitor", RTOSCOPE_STACK_SIZE, NULL, 
                    PRIORITY_SCHED_MONITOR, &s_hMonitorTask) == pdPASS)
    {
        vTaskSetTaskNumber(s_hMonitorTask, TASK_ID_SCHED_MONITOR);
    }

    /* Start FreeRTOS Scheduler */
    vTaskStartScheduler();

    /* Should never reach here */
    for (;;)
    {
    }
}
