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
#include "fsl_reset.h"
#include "fsl_clock.h"
#include "fsl_lpspi.h"
#include "board.h"
#include "app.h"

/* RTOScope Telemetry & Trace Engine */
#include "rtoscope_trace.h"

/*******************************************************************************
 * Task ID & Priority Definitions (Exact Plan Specification)
 *   id 0  IDLE            prio 0  (FreeRTOS idle)
 *   id 1  LED_Blink       prio 1  every 500 ms, vTaskDelayUntil
 *   id 2  RFID_Read       prio 3  polls every 200 ms, blocks via vTaskDelayUntil
 *   id 3  Button_Press    prio 4  woken by GPIO ISR through task notification
 *   id 4  Sched_Monitor   prio 5  every 15 ms, flushes trace buffer
 *   id 5  Background      prio 1  CPU-load demo task
 ******************************************************************************/
#define TASK_ID_IDLE             0
#define TASK_ID_LED_BLINK        1
#define TASK_ID_RFID_READ        2
#define TASK_ID_BUTTON_PRESS     3
#define TASK_ID_SCHED_MONITOR    4
#define TASK_ID_BACKGROUND       5

#define PRIORITY_IDLE            (0U)
#define PRIORITY_LED_BLINK       (1U)
#define PRIORITY_BACKGROUND      (1U)
#define PRIORITY_RFID_READ       (3U)
#define PRIORITY_BUTTON_PRESS    (4U)
#define PRIORITY_SCHED_MONITOR   (5U)

#define RTOSCOPE_STACK_SIZE      (configMINIMAL_STACK_SIZE + 128)

/* Toggle background load demo task (set to 0 to disable) */
#define ENABLE_BACKGROUND_LOAD   1

/*******************************************************************************
 * Task Handles
 ******************************************************************************/
static TaskHandle_t s_hLedTask       = NULL;
static TaskHandle_t s_hRfidTask      = NULL;
static TaskHandle_t s_hButtonTask    = NULL;
static TaskHandle_t s_hMonitorTask   = NULL;
#if ENABLE_BACKGROUND_LOAD
static TaskHandle_t s_hBgTask        = NULL;
#endif

/*******************************************************************************
 * MFRC522 Minimal Driver Definitions & Helpers (SPI over LPSPI3)
 ******************************************************************************/
#define MFRC522_REG_COMMAND      0x01
#define MFRC522_REG_COMIEN       0x02
#define MFRC522_REG_COMIRQ       0x04
#define MFRC522_REG_ERROR        0x06
#define MFRC522_REG_FIFODATA     0x09
#define MFRC522_REG_FIFOLEVEL    0x0A
#define MFRC522_REG_CONTROL      0x0C
#define MFRC522_REG_BITFRAMING   0x0D
#define MFRC522_REG_MODE         0x11
#define MFRC522_REG_TXCONTROL    0x14
#define MFRC522_REG_TXASK        0x15
#define MFRC522_REG_TMODE        0x2A
#define MFRC522_REG_TPRESCALER   0x2B
#define MFRC522_REG_TRELOAD_H    0x2C
#define MFRC522_REG_TRELOAD_L    0x2D

#define PCD_IDLE                 0x00
#define PCD_TRANSCEIVE           0x0C
#define PCD_RESETPHASE           0x0F

#define PICC_REQIDL              0x26
#define PICC_ANTICOLL            0x93

#define MI_OK                    0
#define MI_NOTAGERR              1
#define MI_ERR                   2

static void MFRC522_WriteReg(uint8_t reg, uint8_t val)
{
    uint8_t tx[2];
    uint8_t rx[2];
    tx[0] = (uint8_t)((reg << 1) & 0x7EU);
    tx[1] = val;
    lpspi_transfer_t xfer = {
        .txData = tx,
        .rxData = rx,
        .dataSize = 2,
        .configFlags = (uint32_t)kLPSPI_MasterPcs0 | (uint32_t)kLPSPI_MasterPcsContinuous
    };
    (void)LPSPI_MasterTransferBlocking(LPSPI3, &xfer);
}

static uint8_t MFRC522_ReadReg(uint8_t reg)
{
    uint8_t tx[2];
    uint8_t rx[2] = {0, 0};
    tx[0] = (uint8_t)(((reg << 1) & 0x7EU) | 0x80U);
    tx[1] = 0x00;
    lpspi_transfer_t xfer = {
        .txData = tx,
        .rxData = rx,
        .dataSize = 2,
        .configFlags = (uint32_t)kLPSPI_MasterPcs0 | (uint32_t)kLPSPI_MasterPcsContinuous
    };
    if (LPSPI_MasterTransferBlocking(LPSPI3, &xfer) != kStatus_Success)
    {
        return 0xFF;
    }
    return rx[1];
}

static void MFRC522_SetBitMask(uint8_t reg, uint8_t mask)
{
    MFRC522_WriteReg(reg, MFRC522_ReadReg(reg) | mask);
}

static void MFRC522_ClearBitMask(uint8_t reg, uint8_t mask)
{
    MFRC522_WriteReg(reg, MFRC522_ReadReg(reg) & (~mask));
}

static void MFRC522_AntennaOn(void)
{
    uint8_t temp = MFRC522_ReadReg(MFRC522_REG_TXCONTROL);
    if (!(temp & 0x03))
    {
        MFRC522_SetBitMask(MFRC522_REG_TXCONTROL, 0x03);
    }
}

static void MFRC522_Reset(void)
{
    MFRC522_WriteReg(MFRC522_REG_COMMAND, PCD_RESETPHASE);
    for (volatile uint32_t i = 0; i < 20000; i++) { }
}

static void MFRC522_Init(void)
{
    /* Pull RST pin high */
    GPIO_PinWrite(GPIO0, 27U, 1);
    for (volatile uint32_t i = 0; i < 10000; i++) { }

    MFRC522_Reset();
    MFRC522_WriteReg(MFRC522_REG_TMODE, 0x8D);
    MFRC522_WriteReg(MFRC522_REG_TPRESCALER, 0x3E);
    MFRC522_WriteReg(MFRC522_REG_TRELOAD_L, 30);
    MFRC522_WriteReg(MFRC522_REG_TRELOAD_H, 0);
    MFRC522_WriteReg(MFRC522_REG_TXASK, 0x40);
    MFRC522_WriteReg(MFRC522_REG_MODE, 0x3D);
    MFRC522_AntennaOn();
}

static uint8_t MFRC522_ToCard(uint8_t command, const uint8_t *sendData, uint8_t sendLen, uint8_t *backData, uint32_t *backLen)
{
    uint8_t status = MI_ERR;
    uint8_t irqEn = 0x00;
    uint8_t waitIRq = 0x00;
    uint8_t lastBits;
    uint8_t n;
    uint32_t i;

    if (command == PCD_TRANSCEIVE)
    {
        irqEn = 0x77;
        waitIRq = 0x30;
    }

    MFRC522_WriteReg(MFRC522_REG_COMIEN, irqEn | 0x80);
    MFRC522_ClearBitMask(MFRC522_REG_COMIRQ, 0x80);
    MFRC522_SetBitMask(MFRC522_REG_FIFOLEVEL, 0x80);

    MFRC522_WriteReg(MFRC522_REG_COMMAND, PCD_IDLE);

    for (i = 0; i < sendLen; i++)
    {
        MFRC522_WriteReg(MFRC522_REG_FIFODATA, sendData[i]);
    }

    MFRC522_WriteReg(MFRC522_REG_COMMAND, command);
    if (command == PCD_TRANSCEIVE)
    {
        MFRC522_SetBitMask(MFRC522_REG_BITFRAMING, 0x80);
    }

    i = 200;
    do
    {
        n = MFRC522_ReadReg(MFRC522_REG_COMIRQ);
        i--;
    } while ((i != 0) && !(n & 0x01) && !(n & waitIRq));

    MFRC522_ClearBitMask(MFRC522_REG_BITFRAMING, 0x80);

    if (i != 0)
    {
        if (!(MFRC522_ReadReg(MFRC522_REG_ERROR) & 0x1B))
        {
            status = MI_OK;
            if (n & irqEn & 0x01)
            {
                status = MI_NOTAGERR;
            }

            if (command == PCD_TRANSCEIVE)
            {
                n = MFRC522_ReadReg(MFRC522_REG_FIFOLEVEL);
                lastBits = MFRC522_ReadReg(MFRC522_REG_CONTROL) & 0x07;
                if (lastBits)
                {
                    *backLen = (n - 1) * 8 + lastBits;
                }
                else
                {
                    *backLen = n * 8;
                }

                if (n == 0)
                {
                    n = 1;
                }
                if (n > 16)
                {
                    n = 16;
                }

                for (i = 0; i < n; i++)
                {
                    backData[i] = MFRC522_ReadReg(MFRC522_REG_FIFODATA);
                }
            }
        }
        else
        {
            status = MI_ERR;
        }
    }

    return status;
}

static uint8_t MFRC522_Request(uint8_t reqMode, uint8_t *tagType)
{
    uint8_t status;
    uint32_t backBits = 0;

    MFRC522_WriteReg(MFRC522_REG_BITFRAMING, 0x07);
    tagType[0] = reqMode;
    status = MFRC522_ToCard(PCD_TRANSCEIVE, tagType, 1, tagType, &backBits);

    if ((status != MI_OK) || (backBits != 0x10))
    {
        status = MI_ERR;
    }
    return status;
}

static uint8_t MFRC522_Anticoll(uint8_t *serNum)
{
    uint8_t status;
    uint8_t i;
    uint8_t serNumCheck = 0;
    uint32_t unLen = 0;

    MFRC522_WriteReg(MFRC522_REG_BITFRAMING, 0x00);
    serNum[0] = PICC_ANTICOLL;
    serNum[1] = 0x20;
    status = MFRC522_ToCard(PCD_TRANSCEIVE, serNum, 2, serNum, &unLen);

    if (status == MI_OK)
    {
        for (i = 0; i < 4; i++)
        {
            serNumCheck ^= serNum[i];
        }
        if (serNumCheck != serNum[i])
        {
            status = MI_ERR;
        }
    }
    return status;
}

/*******************************************************************************
 * GPIO00 Interrupt Handler (SW3: Pin 6, SW2: Pin 20)
 * Triggered on falling edge when button is pressed.
 ******************************************************************************/
void GPIO00_IRQHandler(void)
{
    uint32_t irqTimeUs = RTOScope_GetTimeUs();
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* Read and clear interrupt channel flags */
    uint32_t flags = GPIO_GpioGetInterruptChannelFlags(GPIO0, 0);
    GPIO_GpioClearInterruptChannelFlags(GPIO0, flags, 0);

    /* Software debounce (40 ms) */
    static uint32_t s_lastIrqTimeUs = 0;
    if ((irqTimeUs - s_lastIrqTimeUs) > 40000UL || s_lastIrqTimeUs == 0)
    {
        s_lastIrqTimeUs = irqTimeUs;

        /* Record button press hardware interrupt event immediately */
        RTOScope_RecordEvent(RTOSCOPE_EVT_BTN_IRQ, TASK_ID_BUTTON_PRESS, irqTimeUs);

        /* Signal high-priority Button_Press task to wake immediately */
        if (s_hButtonTask != NULL)
        {
            vTaskNotifyGiveFromISR(s_hButtonTask, &xHigherPriorityTaskWoken);
        }
    }

    SDK_ISR_EXIT_BARRIER;
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/*******************************************************************************
 * Hardware Pin Initialization (LEDs, Buttons & LPSPI3)
 ******************************************************************************/
static void RTOScope_InitHardwarePins(void)
{
    /* Enable PORT and GPIO peripheral clocks */
    CLOCK_EnableClock(kCLOCK_Port0);
    CLOCK_EnableClock(kCLOCK_Port1);
    CLOCK_EnableClock(kCLOCK_Port4);
    CLOCK_EnableClock(kCLOCK_Gpio0);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    CLOCK_EnableClock(kCLOCK_Gpio4);

    /* Release peripheral resets */
    RESET_ReleasePeripheralReset(kPORT0_RST_SHIFT_RSTn);
    RESET_ReleasePeripheralReset(kPORT1_RST_SHIFT_RSTn);
    RESET_ReleasePeripheralReset(kPORT4_RST_SHIFT_RSTn);
    RESET_ReleasePeripheralReset(kGPIO0_RST_SHIFT_RSTn);
    RESET_ReleasePeripheralReset(kGPIO1_RST_SHIFT_RSTn);
    RESET_ReleasePeripheralReset(kGPIO4_RST_SHIFT_RSTn);

    /* Attach FRO 12M clock to FlexComm3 for LPSPI3 */
    CLOCK_AttachClk(kFRO12M_to_FLEXCOMM3);
    CLOCK_SetClkDiv(kCLOCK_DivFlexcom3Clk, 1u);
    RESET_ReleasePeripheralReset(kFC3_RST_SHIFT_RSTn);

    /* Blue LED (P4_17), Red LED (P4_18), Green LED (P4_19) Output Pins */
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
    PORT_SetPinConfig(PORT4, BOARD_LED_GREEN_GPIO_PIN, &led_pin_cfg);

    const gpio_pin_config_t led_out_cfg = {
        kGPIO_DigitalOutput,
        LOGIC_LED_OFF
    };
    GPIO_PinInit(BOARD_LED_BLUE_GPIO, BOARD_LED_BLUE_GPIO_PIN, &led_out_cfg);
    GPIO_PinInit(BOARD_LED_RED_GPIO, BOARD_LED_RED_GPIO_PIN, &led_out_cfg);
    GPIO_PinInit(BOARD_LED_GREEN_GPIO, BOARD_LED_GREEN_GPIO_PIN, &led_out_cfg);

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

    /* Configure Falling-Edge Interrupt on Channel 0 for SW3 and SW2 */
    GPIO_SetPinInterruptChannel(GPIO0, 6U, kGPIO_InterruptOutput0);
    GPIO_SetPinInterruptConfig(GPIO0, 6U, kGPIO_InterruptFallingEdge);
    GPIO_PinClearInterruptFlag(GPIO0, 6U);

    GPIO_SetPinInterruptChannel(GPIO0, 20U, kGPIO_InterruptOutput0);
    GPIO_SetPinInterruptConfig(GPIO0, 20U, kGPIO_InterruptFallingEdge);
    GPIO_PinClearInterruptFlag(GPIO0, 20U);

    /* Set GPIO00 IRQ priority - safe for FreeRTOS ISR API */
    NVIC_SetPriority(GPIO00_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    EnableIRQ(GPIO00_IRQn);

    /* Arduino Header LPSPI3 Pin Configuration (FlexComm3) */
    /* PIO1_0 (MOSI / SDO), PIO1_1 (SCK), PIO1_2 (MISO / SDI), PIO1_3 (PCS0 / CS) */
    const port_pin_config_t spi_pin_cfg = {
        .pullSelect = kPORT_PullDisable,
        .pullValueSelect = kPORT_LowPullResistor,
        .slewRate = kPORT_FastSlewRate,
        .passiveFilterEnable = kPORT_PassiveFilterDisable,
        .openDrainEnable = kPORT_OpenDrainDisable,
        .driveStrength = kPORT_LowDriveStrength,
        .mux = kPORT_MuxAlt2,
        .inputBuffer = 1,
        .invertInput = 0,
        .lockRegister = 0
    };
    PORT_SetPinConfig(PORT1, 0U, &spi_pin_cfg); /* FC3_P0: SDO (MOSI) */
    PORT_SetPinConfig(PORT1, 1U, &spi_pin_cfg); /* FC3_P1: SCK */
    PORT_SetPinConfig(PORT1, 2U, &spi_pin_cfg); /* FC3_P2: SDI (MISO) */
    PORT_SetPinConfig(PORT1, 3U, &spi_pin_cfg); /* FC3_P3: PCS0 (CS) */

    /* RC522 RST Pin: PIO0_27 (Arduino D9) as GPIO Output */
    const port_pin_config_t rst_pin_cfg = {
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
    PORT_SetPinConfig(PORT0, 27U, &rst_pin_cfg);
    const gpio_pin_config_t rst_gpio_cfg = { kGPIO_DigitalOutput, 1 };
    GPIO_PinInit(GPIO0, 27U, &rst_gpio_cfg);
}

/*******************************************************************************
 * Task: Sched_Monitor (Priority 5 - Highest User Priority)
 * Drains the trace buffer every 15 ms with limited flush batches (max 32).
 * Periodically re-broadcasts the task registry for newly connected dashboards.
 ******************************************************************************/
static void vSchedMonitorTask(void *pvParameters)
{
    uint32_t registryCounter = 0;

    /* Initial registry broadcast */
    RTOScope_SendTaskRegistry();

    for (;;)
    {
        /* Periodically flush buffered trace events every 15 ms */
        vTaskDelay(pdMS_TO_TICKS(15));
        RTOScope_FlushTrace();

        /* Re-broadcast task registry every 2 seconds for late-connecting dashboards */
        if (++registryCounter >= 130)
        {
            registryCounter = 0;
            RTOScope_SendTaskRegistry();
        }
    }
}

/*******************************************************************************
 * Task: Button Press (Priority 4)
 * Blocks until signaled by GPIO falling edge interrupt via task notification.
 * Turns Red LED on, holds for 200 ms non-busy delay, waits for release, then off.
 ******************************************************************************/
static void vButtonPressTask(void *pvParameters)
{
    for (;;)
    {
        /* Block indefinitely until interrupt notification arrives */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Turn on Red LED and immediately record trace event */
        LED_RED_ON();
        RTOScope_RecordEvent(RTOSCOPE_EVT_LED_RED, 1, RTOScope_GetTimeUs());

        /* Real FreeRTOS delay for 200 ms (lower tasks run during this time) */
        vTaskDelay(pdMS_TO_TICKS(200));

        /* If button is still physically depressed, wait until release */
        while ((GPIO_PinRead(GPIO0, 6U) == 0) || (GPIO_PinRead(GPIO0, 20U) == 0))
        {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        /* Turn off Red LED and immediately record trace event */
        LED_RED_OFF();
        RTOScope_RecordEvent(RTOSCOPE_EVT_LED_RED, 0, RTOScope_GetTimeUs());
    }
}

/*******************************************************************************
 * Task: RFID Read (Priority 3)
 * Polls RFID sensor every 200 ms via vTaskDelayUntil (never busy waits).
 * On valid card read: toggles Green LED, reports #E,K,<uid_hash>,<time_us>.
 * Ignores duplicate card reads within 1 second.
 ******************************************************************************/
static void vRfidReadTask(void *pvParameters)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(200);
    uint8_t tagType[4];
    uint8_t uid[5];
    static uint8_t s_lastUid[4] = {0};
    static uint32_t s_lastUidTimeUs = 0;
    static bool s_greenLedActive = false;
    static bool s_rfidPresent = false;
    uint32_t probeCounter = 0;
    uint8_t ver = 0;

    /* Initialize LPSPI3 Master safely */
    uint32_t spiClk = CLOCK_GetLPFlexCommClkFreq(3u);
    if (spiClk > 0)
    {
        lpspi_master_config_t lpspiConfig;
        LPSPI_MasterGetDefaultConfig(&lpspiConfig);
        lpspiConfig.baudRate = 4000000U; /* 4 MHz */
        lpspiConfig.whichPcs = kLPSPI_Pcs0;
        lpspiConfig.pcsActiveHighOrLow = kLPSPI_PcsActiveLow;
        lpspiConfig.bitsPerFrame = 8;
        lpspiConfig.cpol = kLPSPI_ClockPolarityActiveHigh;
        lpspiConfig.cpha = kLPSPI_ClockPhaseFirstEdge;
        lpspiConfig.direction = kLPSPI_MsbFirst;
        LPSPI_MasterInit(LPSPI3, &lpspiConfig, spiClk);

        MFRC522_Init();
        ver = MFRC522_ReadReg(0x37);
        s_rfidPresent = (ver != 0x00 && ver != 0xFF);
    }

    for (;;)
    {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        uint32_t nowUs = RTOScope_GetTimeUs();

        /* Automatically turn OFF Green LED after 1 second pulse expires */
        if (s_greenLedActive && ((nowUs - s_lastUidTimeUs) >= 1000000UL))
        {
            LED_GREEN_OFF();
            s_greenLedActive = false;
        }

        /* If RC522 hardware is not yet attached, probe gently every 2 seconds */
        if (!s_rfidPresent)
        {
            if (++probeCounter >= 10)
            {
                probeCounter = 0;
                ver = MFRC522_ReadReg(0x37);
                if (ver != 0x00 && ver != 0xFF)
                {
                    s_rfidPresent = true;
                    MFRC522_Init();
                }
            }
            continue;
        }

        if (MFRC522_Request(PICC_REQIDL, tagType) == MI_OK)
        {
            if (MFRC522_Anticoll(uid) == MI_OK)
            {
                bool isSameTag = (memcmp(uid, s_lastUid, 4) == 0);

                if (!isSameTag || ((nowUs - s_lastUidTimeUs) > 1000000UL) || (s_lastUidTimeUs == 0))
                {
                    memcpy(s_lastUid, uid, 4);
                    s_lastUidTimeUs = nowUs;

                    /* Turn ON Green LED for 1 second on tag read (like button press) */
                    LED_GREEN_ON();
                    s_greenLedActive = true;

                    /* Calculate UID hash byte: XOR fold of 4-byte UID */
                    uint8_t uidHash = uid[0] ^ uid[1] ^ uid[2] ^ uid[3];
                    if (uidHash == 0)
                    {
                        uidHash = uid[0] ? uid[0] : 0xAA;
                    }

                    /* Record RFID trace event #E,K,<uid_hash>,<time_us> */
                    RTOScope_RecordEvent(RTOSCOPE_EVT_RFID, uidHash, nowUs);
                }
            }
        }
    }
}

/*******************************************************************************
 * Task: LED Blink (Priority 1)
 * Toggles onboard Blue LED at precise 500 ms intervals using vTaskDelayUntil.
 ******************************************************************************/
static void vLedBlinkTask(void *pvParameters)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(500);
    uint8_t ledState = 0;

    for (;;)
    {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        ledState = !ledState;
        if (ledState)
        {
            LED_BLUE_ON();
            RTOScope_RecordEvent(RTOSCOPE_EVT_LED_BLUE, 1, RTOScope_GetTimeUs());
        }
        else
        {
            LED_BLUE_OFF();
            RTOScope_RecordEvent(RTOSCOPE_EVT_LED_BLUE, 0, RTOScope_GetTimeUs());
        }
    }
}

/*******************************************************************************
 * Task: Background Load (Priority 1)
 * CPU-load demo calculation burst with 5 ms non-blocking delay between runs.
 * Gives higher-priority tasks something to preempt without flooding buffer.
 ******************************************************************************/
#if ENABLE_BACKGROUND_LOAD
static void vBackgroundLoadTask(void *pvParameters)
{
    volatile uint32_t checksum = 0x12345678;

    for (;;)
    {
        for (uint32_t i = 0; i < 50000; i++)
        {
            checksum = (checksum >> 1) ^ ((checksum & 1) ? 0xEDB88320UL : 0);
        }
        /* Non-blocking delay to throttle switch event generation and avoid buffer overflow */
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
#endif

/*******************************************************************************
 * Application Main Entry Point
 ******************************************************************************/
int main(void)
{
    /* Board hardware, clock and initial debug console */
    BOARD_InitHardware();
    RTOScope_InitHardwarePins();

    /* Debug console is initialized in BOARD_InitHardware() at BOARD_DEBUG_UART_BAUDRATE (115200) */

    /* Initialize RTOScope DWT cycle counter and trace ring buffer */
    RTOScope_Init();

    /* Register Tasks with RTOScope using exact IDs and priorities */
    RTOScope_RegisterTask(TASK_ID_IDLE,          "IDLE",          PRIORITY_IDLE);
    RTOScope_RegisterTask(TASK_ID_LED_BLINK,     "LED_Blink",     PRIORITY_LED_BLINK);
    RTOScope_RegisterTask(TASK_ID_RFID_READ,     "RFID_Read",     PRIORITY_RFID_READ);
    RTOScope_RegisterTask(TASK_ID_BUTTON_PRESS,  "Button_Press",  PRIORITY_BUTTON_PRESS);
    RTOScope_RegisterTask(TASK_ID_SCHED_MONITOR, "Sched_Monitor", PRIORITY_SCHED_MONITOR);
#if ENABLE_BACKGROUND_LOAD
    RTOScope_RegisterTask(TASK_ID_BACKGROUND,    "Background",    PRIORITY_BACKGROUND);
#endif

    /* 1. Sched_Monitor Task (Priority 5 - Highest User Priority) */
    if (xTaskCreate(vSchedMonitorTask, "Sched_Monitor", RTOSCOPE_STACK_SIZE, NULL, 
                    PRIORITY_SCHED_MONITOR, &s_hMonitorTask) == pdPASS)
    {
        vTaskSetTaskNumber(s_hMonitorTask, TASK_ID_SCHED_MONITOR);
    }

    /* 2. Button_Press Task (Priority 4) */
    if (xTaskCreate(vButtonPressTask, "Button_Press", RTOSCOPE_STACK_SIZE, NULL, 
                    PRIORITY_BUTTON_PRESS, &s_hButtonTask) == pdPASS)
    {
        vTaskSetTaskNumber(s_hButtonTask, TASK_ID_BUTTON_PRESS);
    }

    /* 3. RFID_Read Task (Priority 3) */
    if (xTaskCreate(vRfidReadTask, "RFID_Read", RTOSCOPE_STACK_SIZE + 128, NULL, 
                    PRIORITY_RFID_READ, &s_hRfidTask) == pdPASS)
    {
        vTaskSetTaskNumber(s_hRfidTask, TASK_ID_RFID_READ);
    }

    /* 4. LED_Blink Task (Priority 1) */
    if (xTaskCreate(vLedBlinkTask, "LED_Blink", RTOSCOPE_STACK_SIZE, NULL, 
                    PRIORITY_LED_BLINK, &s_hLedTask) == pdPASS)
    {
        vTaskSetTaskNumber(s_hLedTask, TASK_ID_LED_BLINK);
    }

#if ENABLE_BACKGROUND_LOAD
    /* 5. Background Load Task (Priority 1) */
    if (xTaskCreate(vBackgroundLoadTask, "Background", RTOSCOPE_STACK_SIZE, NULL, 
                    PRIORITY_BACKGROUND, &s_hBgTask) == pdPASS)
    {
        vTaskSetTaskNumber(s_hBgTask, TASK_ID_BACKGROUND);
    }
#endif

    /* Start FreeRTOS Scheduler */
    vTaskStartScheduler();

    /* Should never reach here */
    for (;;)
    {
    }
}
