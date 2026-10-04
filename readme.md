# RTOScope

> Real-Time Operating System Scheduler Visualizer for FreeRTOS on the **NXP FRDM-MCXN236**.

Captures microsecond hardware-precision context switches via Cortex-M33 DWT cycle counters and streams telemetry over UART to a zero-install browser dashboard rendering a live 60 FPS Gantt chart.

---

## 🛠️ Hardware Pinout

- **Board**: NXP FRDM-MCXN236 (Dual Arm Cortex-M33 @ 150 MHz)
- **UART**: MCU-Link Virtual COM (`COM8`, 115200 baud, 8N1)

| Peripheral | Pin | Role |
| :--- | :--- | :--- |
| **Blue LED** | `PIO4_17` | Toggled by `LED_Blink` |
| **Red LED** | `PIO4_18` | Lit during `Button_Press` preemption burst |
| **SW3 (User Button)** | `PIO0_6` | Primary preemption trigger (Active LOW) |
| **SW2 (ISP Button)** | `PIO0_20` | Secondary preemption trigger (Active LOW) |

---

## 📊 FreeRTOS Tasks

| Task | ID | Priority | Color | Rate / Trigger | Purpose |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **`IDLE`** | 0 | 0 | Gray | Always | Tracks CPU idle headroom |
| **`Sched_Monitor`** | 5 | 1 | Indigo | 15 ms | Flushes trace ring buffer over UART without starvation |
| **`LED_Blink`** | 1 | 1 | Cyan | 500 ms | Heartbeat toggle of Blue LED |
| **`UART_Print`** | 4 | 2 | Emerald| 1000 ms | Reports uptime & free heap telemetry |
| **`Sensor_Read`** | 3 | 3 | Amber | 250 ms | Periodic ADC sampling & digital filtering |
| **`Button_Press`** | 2 | 4 | Red | On press | Preempts lower tasks, lights Red LED for 25 ms |

---

## 📡 UART Protocol

- **`#T,<id>,<name>,<priority>`** : Task registration broadcast
- **`#E,<I|O>,<id>,<timestamp_us>`** : Context switch events (`I` = switch in, `O` = switch out)
- **`#M,<uptime_ms>,<free_heap>`** : Periodic system telemetry

---

## ⚡ Quick Start

### 1. Build & Flash
```bash
# In project root:
cmake --preset debug
cmake --build --preset debug
```
Flash `debug/RTOScope.elf` via VS Code (`F5` / LinkServer Debug) or GUI debugger.

### 2. Launch Dashboard
Open [`index.html`](index.html) in **Google Chrome** or **Microsoft Edge**:
1. Click **Connect FRDM-MCXN236**.
2. Select your MCU-Link COM port (e.g. `COM8`) at `115200` baud.
3. The live Gantt chart, task state indicators, and CPU utilization meter will start streaming immediately.

---

## 🧪 Testing Preemption

Press pushbutton **SW3** (or **SW2**) on the board:
- The **Red LED** illuminates.
- High-priority task **`Button_Press`** (Prio 4) preempts running tasks.
- A red execution block and `[PREEMPTION]` notification appear on the Gantt chart.