# Learning Log: ESP32 Trading Terminal

Concepts I worked through while building this project, written up after testing them on the board where possible. Entries marked **Plan** are design intent that is not built yet.

---

## 1. Pull-ups and active-low buttons

- A GPIO set as plain `INPUT` is high-impedance. With nothing driving it, it floats and reads random HIGH/LOW values from noise.
- A button wired between a GPIO and GND needs a defined idle level. `INPUT_PULLUP` enables a weak internal resistor to 3.3V, so the idle state is HIGH.
- Pressing the button connects the pin directly to GND, which overpowers the weak pull-up, so the pin reads LOW. The button is **active-low**.
- The HIGH-to-LOW transition is the falling edge. I choose to configure the interrupt on `FALLING` so it fires the moment a press happens.
- With a floating pin and an interrupt attached, the ISR could fire with nobody touching the button. On a trading device that would mean phantom orders.
- ESP32 note: GPIO 34-39 are input-only with no internal pull-ups, and GPIO 6-11 are wired to flash. Neither is used for buttons.

## 2. FreeRTOS tasks

- A task is a function that runs as if it owned the CPU, normally in an infinite loop. Arduino's `loop()` already runs inside a task.
- A task is Running, Ready, Blocked, or Suspended. A **Blocked** task (waiting on a delay or a queue) uses no CPU, so other tasks can run.
- A task that polls a flag in a loop keeps the CPU busy and starves lower-priority tasks.
- The higher-priority ready task always wins (preemption). A high-priority task that never blocks starves the idle task, and the task watchdog resets the chip.
- Each task has its own stack, sized in bytes on ESP32. `printf` and HTTPS need larger stacks, so sizes should be measured, not guessed.
- Why the HTTP POST must not run in `loop()`: it can take hundreds of milliseconds to seconds, which freezes the display and leaves button presses unserviced. A frozen screen invites a second press, which means a duplicate order.

## 3. Queues

- A queue is a thread-safe FIFO. It **copies items by value** into its own storage.
- `xQueueSend(q, &item, wait)` reads `sizeof(item)` bytes starting at the address I pass. The item size was fixed in `xQueueCreate(length, itemSize)`.
- `xQueueReceive(q, &dest, wait)` writes into my variable, so it needs the address too. With `portMAX_DELAY` the task blocks and uses no CPU until an item arrives.
- Because the bytes are copied at the moment of the send, a local variable can safely go out of scope afterward. If the item were a *pointer* to a local, the queue would copy the pointer and it could dangle, so queue small values like an enum.
- A queue holds several separate presses in order. A flag holds one bit, so two fast presses would collapse into one.

## 4. Pointers: `&` and `*`

- In an expression, `&x` is the address of `x` and `*p` is the value at the address `p` holds. They are inverses.
- With `int a = 3; int *p = &a; *p = 10;`, `a` becomes 10, because `p` holds the address of `a` and the write goes through it.
- In a declaration, `int *p` means p is a pointer type, and `int &r` means r is a C++ reference (an alias).

## 5. Why ISRs cannot block, and why `FromISR` exists

- An ISR is not a task. It has no task context to put to sleep, so it cannot block, wait on a mutex, call `malloc`, or use `Serial.print`.
- `xQueueSend` can block when the queue is full, so it is for tasks. `xQueueSendFromISR` never waits. It either succeeds or returns `errQUEUE_FULL`.
- If the send wakes a higher-priority task, `xQueueSendFromISR` sets a `higherPriorityTaskWoken` flag. It does not switch tasks itself. `portYIELD_FROM_ISR()` then requests the switch so the woken task runs as the ISR exits.
- ISRs should be marked `IRAM_ATTR` and kept to microseconds of work.

## 6. Experiment: producer faster than consumer (tested on the board)

Setup: queue length 5, producer sends every 100 ms, consumer takes one item then sleeps 1000 ms.

Observed:
- `Got 0` immediately, then a run of `Queue full, dropped item` lines.
- `Got 1` through `Got 5` followed, one per second: the old items from the first burst.
- Then `Got 11`, `Got 21`, `Got 31`, and so on. Each time the consumer freed a slot, the producer filled it on its next send, so about 1 item in 10 got in and 9 were dropped.

Takeaways:
- A full queue drops the **newest** item and keeps the **oldest**.
- A queue only hides a rate mismatch for a while. When the producer is permanently faster, data is lost or goes stale.
- On a trading device stale data is worse than lost data: a queued sell could execute seconds after it was pressed.

## 7. Debounce design (Plan)

- Mechanical contacts bounce for roughly 1-20 ms, so one press creates several falling edges and would queue several orders.
- Plan: each ISR compares the current time (`esp_timer_get_time()`) to that button's last **accepted** press and ignores edges inside a window of about 50 ms.
- The ISR cannot debounce with `delay()`: `delay()` relies on task blocking that an ISR does not have, and busy-waiting in an ISR would starve the rest of the system.
- Window tradeoff: about 5 ms lets bounce through as false presses, and about 200 ms rejects genuine quick presses. About 50 ms is the starting point, as a named constant to tune.
- Plan: one timestamp **per button**. A shared timestamp would cap the whole device at one accepted press per window, so a real SELL right after a BUY would be ignored. Bounce on the same button is filtered either way.

## 8. Queue-full policy (Plan)

- Decision: when the queue is full, **drop the new press** (this is what `xQueueSendFromISR` does by default).
- Keep the queue short (2-3 items) so an order cannot sit long before it is sent.
- A dropped press must be visible: error LED and "BUSY" on the OLED, so I press again on purpose instead of silently losing an order.
- Possible later addition: timestamp each order and have the network task discard anything older than a limit, such as 3 seconds.
- Open design question: the ISR cannot print, so it will report a drop through a shared `volatile` variable that a task reads and turns into LED and OLED updates.