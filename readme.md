# RTOScope

> **Real-Time Operating System Scheduler Visualizer & Telemetry Analyzer**  
> Running on the **NXP FRDM-MCXN236** (Dual Arm Cortex-M33 @ 150 MHz, FreeRTOS tick 200 Hz).

RTOScope captures hardware-precision microsecond context switches and task state transitions directly inside FreeRTOS kernel hooks using the Cortex-M33 DWT cycle counter. Telemetry streams over a high-speed UART link (460800 baud) into a zero-install Web Serial dashboard rendering a 60 FPS live Gantt chart, hardware signal lanes, and real-time preemption metrics.

---

## 📊 Final FreeRTOS Task Set

All tasks are strictly registered and numbered using `vTaskSetTaskNumber()` / `uxTaskGetTaskNumber()`:

| Task Name | Task ID | Priority | Color | Timing / Trigger | Behavior & Role |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **`IDLE`** | 0 | 0 | Slate Gray | Continuous | Standard FreeRTOS idle task; used to compute active CPU utilization. |
| **`LED_Blink`** | 1 | 1 | Cyan | Every 500 ms | Periodic heartbeat using `vTaskDelayUntil()`; toggles onboard Blue LED (`PIO4_17`). |
| **`RFID_Read`** | 2 | 3 | Emerald | Every 200 ms | Polls RC522 RFID reader over LPSPI3 without busy-waiting. On valid tag: toggles Green LED (`PIO4_19`), emits `#E,K`, ignores same tag for 1 s. |
| **`Button_Press`** | 3 | 4 | Red | Falling edge IRQ | Woken by GPIO falling edge ISR on SW3 (`PIO0_6`) / SW2 (`PIO0_20`) via direct task notification. Illuminates Red LED (`PIO4_18`) for 200 ms. |
| **`Sched_Monitor`** | 4 | 5 | Purple | Every 15 ms | Highest-priority task; drains the 1024-entry trace buffer over UART in small batches (max 32 events) to prevent CPU hogging. Re-broadcasts registry every 2 s. |
| **`Background`** | 5 | 1 | Amber | Periodic burst | CPU-load demo task performing a checksum calculation burst followed by a 5 ms non-blocking delay. Provides workload for preemption without buffer flooding. |

---

## 🔌 Hardware Wiring Table (RC522 RFID Reader)

The RC522 RFID reader interfaces with the FRDM-MCXN236 via LP_FLEXCOMM3 (`LPSPI3`) exposed on the Arduino compatible headers:

> [!CAUTION]
> The MFRC522 module is **3.3 V only**. Connecting it to 5 V will permanently damage the chip!

| RC522 Pin | FRDM-MCXN236 Pin | Header & Pin | Function / Alternate Mode |
| :--- | :--- | :--- | :--- |
| **VCC (3.3V)** | `3V3` | **J3 pin 4** | 3.3 V Main Power Supply |
| **RST** | `PIO0_27` | **J2 pin 5** (Arduino D9) | Hardware Reset (GPIO output) |
| **GND** | `GND` | **J2 pin 14** or **J3 pin 6/7** | Common Ground |
| **IRQ** | *NC* | — | Not connected (polling mode) |
| **MISO** | `PIO1_2` | **J2 pin 10** (Arduino D12) | LPSPI3 SDI (FlexComm3 MuxAlt2) |
| **MOSI** | `PIO1_0` | **J2 pin 8** (Arduino D11) | LPSPI3 SDO (FlexComm3 MuxAlt2) |
| **SCK** | `PIO1_1` | **J2 pin 12** (Arduino D13) | LPSPI3 SCK (FlexComm3 MuxAlt2) |
| **SDA / SS** | `PIO1_3` | **J2 pin 6** (Arduino D10) | LPSPI3 PCS0 Chip Select (MuxAlt2) |

### Onboard Hardware Peripherals

- **Blue LED**: `PIO4_17` (Active LOW) — Driven by `LED_Blink`
- **Red LED**: `PIO4_18` (Active LOW) — Driven by `Button_Press`
- **Green LED**: `PIO4_19` (Active LOW) — Driven by `RFID_Read`
- **User Button (SW3)**: `PIO0_6` (Falling Edge Interrupt on `GPIO00_IRQn`)
- **ISP Button (SW2)**: `PIO0_20` (Falling Edge Interrupt on `GPIO00_IRQn`)
- **Debug / Trace UART**: MCU-Link Virtual COM (FlexComm4 / LPUART4) at **115200 baud** (8N1, default)

---

## 📡 UART Trace Protocol

All telemetry lines terminate with `\r\n`:

| Prefix | Format | Description |
| :--- | :--- | :--- |
| **`#T`** | `#T,<id>,<name>,<priority>` | Task registration metadata (broadcast at startup & every 2 s). |
| **`#E,I`** | `#E,I,<task_id>,<timestamp_us>` | Context switch IN (task starts executing). |
| **`#E,O`** | `#E,O,<task_id>,<timestamp_us>` | Context switch OUT (task relinquishes CPU). |
| **`#E,S`** | `#E,S,<task_id>,<state>,<timestamp_us>` | Task state transition: `'R'` (Running), `'D'` (Ready), `'B'` (Blocked). |
| **`#E,B`** | `#E,B,<0\|1>,<timestamp_us>` | Blue LED physical output state (0 = Off, 1 = On). |
| **`#E,R`** | `#E,R,<0\|1>,<timestamp_us>` | Red LED physical output state (0 = Off, 1 = On). |
| **`#E,P`** | `#E,P,<task_id>,<timestamp_us>` | Hardware button falling edge interrupt event. |
| **`#E,K`** | `#E,K,<uid_hash_hex>,<timestamp_us>` | RFID tag detection event with 8-bit XOR hash of tag UID. |
| **`#D`** | `#D,<dropped_events_count>` | Trace buffer overflow drop counter (0 in normal operation). |

---

## 🛠️ Build and Flash

### Prerequisites
- Arm GNU Toolchain 14.2 (`arm-none-eabi-gcc`)
- MCUXpresso SDK 26.09.00 for FRDM-MCXN236
- CMake 3.22+ and Ninja

### Build Firmware
```powershell
# Configure build with debug preset
cmake --preset debug

# Build ELF binary
cmake --build --preset debug

# (Optional) Build release preset
cmake --build --preset release
```

Flash the generated binary `debug/RTOScope.elf` using LinkServer, J-Link, or the MCUXpresso IDE debugger.

---

## 🚀 Live Dashboard Setup

1. Open [`index.html`](index.html) in **Google Chrome** or **Microsoft Edge**.
2. Ensure the baud rate dropdown is set to **`115200 Baud`**.
3. Click **Connect Board** and choose your MCU-Link Virtual COM Port.
4. The dashboard immediately renders:
   - Live Gantt chart with task execution blocks and LED hardware signal bars.
   - Live task state transitions (Running, Ready, Blocked).
   - Real-time CPU utilization meter.
   - RFID tag scanner card showing last scanned UID hash.
   - Button preemption latency measurement card.

---

## 🧪 4-Step Verification Procedure

Follow these steps to demonstrate and verify the system:

### 1. RFID Tag Detection Test
- Tap a 13.56 MHz RFID / NFC tag (Mifare Classic / Ultralight) onto the RC522 antenna.
- **Expected Result**: 
  - The onboard **Green LED** (`PIO4_19`) toggles instantly.
  - The dashboard side card updates to show the tag hash (e.g., `0xA3`) and scan counter.
  - A green diamond marker appears on the `RFID_Read` timeline lane.
  - Holding the tag on the reader does not flood the link (1-second duplicate filter).

### 2. Pushbutton Preemption Test
- Press pushbutton **SW3** (or **SW2**).
- **Expected Result**:
  - The onboard **Red LED** (`PIO4_18`) turns ON for 200 ms.
  - A dashed red `⚡ IRQ (SW3)` marker is drawn at the exact interrupt instant.
  - The high-priority **`Button_Press`** (Prio 4) task preempts the running task.
  - The **Button Latency** card displays the hardware latency from IRQ trigger to Red LED activation (typically < 100 µs).

### 3. Zero Dropped Events Over 5+ Minutes
- Run the board continuously for 5+ minutes while the background load task is executing and periodic LEDs are blinking.
- **Expected Result**:
  - The drop counter `#D` remains at `0`.
  - The top banner stays hidden (only triggers if drops > 0).
  - Blue blink period is measured consistently at `500.0 ms ± 0.5 ms`.

### 4. Task State Transition Verification
- Observe the **Registered Tasks** table in the sidebar.
- **Expected Result**:
  - Tasks dynamically cycle through `RUNNING` (green), `READY` (amber), and `BLOCKED` (slate) badges driven by `#E,S` hooks.
  - Sched_Monitor drains the ring buffer without starving lower tasks.