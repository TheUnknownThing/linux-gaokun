# UCSI Investigation State

## Workspace

- Repository: `/home/wano/playground/linux-gaokun`
- Branch: `ucsi-dp-rework-plan`
- Baseline branch point: `8c15074d2d20`

## Why This Note Exists

This note records the important technical state from the current investigation so the next turn does not need the full prior chat context.

## Current Strategy

We intentionally reset to the `8c15074` baseline because it was the last known point where:

- the out-of-tree UCSI ABI matched the running kernel
- private EC sideband accesses were serialized with `ppm_lock`
- USB robustness was materially better than the later DP experiments

We are not carrying the later DP-routing patches forward directly. Instead, we are:

1. preserving the `8c15074` UCSI baseline
2. adding instrumentation first
3. using runtime traces to identify the first real failure point
4. only then planning the next minimal patch

## Phase 1 Instrumentation

Instrumentation has already been added on this branch to:

- [`gaokun-ec-dkms/ucsi_huawei_gaokun.c`](/home/wano/playground/linux-gaokun/gaokun-ec-dkms/ucsi_huawei_gaokun.c)
- [`gaokun-ec-dkms/huawei-gaokun-ec.c`](/home/wano/playground/linux-gaokun/gaokun-ec-dkms/huawei-gaokun-ec.c)

The traces cover:

- EC IRQ event IDs
- notifier entry/exit timing
- `EC_EVENT_UCSI` / `EC_EVENT_USB`
- `CCI` values
- per-port sideband snapshots
- USB follow-up timer arming and firing
- orientation results

One review issue was fixed locally:

- the per-port snapshot trace in `gaokun_ucsi_port_update()` was moved out of the spinlock by copying fields under lock and logging after unlock

Build verification passed after that fix with:

```sh
make -C /lib/modules/$(uname -r)/build M=$PWD modules
```

## Important Runtime Setup

The instrumented workspace modules were not initially active. The running kernel was still using the installed in-tree modules:

- `/lib/modules/6.19.6-gaokun3/kernel/drivers/usb/typec/ucsi/ucsi_huawei_gaokun.ko`
- `/lib/modules/6.19.6-gaokun3/kernel/drivers/platform/arm64/huawei-gaokun-ec.ko`

After that, the instrumented workspace modules were copied into `/lib/modules/...`, reloaded, and dynamic debug was enabled with:

```sh
sudo sh -c "printf '%s\n' 'module ucsi_huawei_gaokun +p' 'module huawei_gaokun_ec +p' > /sys/kernel/debug/dynamic_debug/control"
```

That means the later traces described below are from the instrumented build, not the earlier installed baseline.

## Confirmed Baseline Constraints

These constraints were already established before the trace capture and still stand:

### 1. Boot-time UCSI handling is display-sensitive

- Do not move registration earlier.
- Do not aggressively touch EC/UCSI state in the early boot window.
- Internal panel bring-up can regress if UCSI init ordering is disturbed.

### 2. The kernel UCSI core owns normal USB Type-C routing

- ordinary USB mode transitions are already handled by the core
- ordinary orientation updates are expected to come from the connector-status callback path

### 3. DP support still needs a board-specific mux path eventually

- the Qualcomm QMP combo PHY needs Type-C mux events for DP-only vs USB+DP mode
- but the current trace evidence shows the first failure happens before that DP-specific part becomes the main problem

## Main Trace Result

The most important finding from the instrumented runs is:

> The first failure is not that the USB-side follow-up event never arrives.
> The USB event *does* arrive, but the `EC_EVENT_USB` notifier path blocks long enough to collide with the UCSI core's own connector-change processing.

That means:

- the recurring `missing USB event` warning is often a downstream symptom
- the real first fault is the blocking private EC-sideband work done inline from the `EC_EVENT_USB` notifier path

## USB-Only Reproduction Result

User action:

- plug and unplug a USB flash drive repeatedly on one physical port, same orientation

Observed trace pattern:

1. `EC_EVENT_UCSI` arrives for connector 2.
2. The driver arms the USB follow-up timer for port 1.
3. A second `EC_EVENT_UCSI` for the same connector follows almost immediately.
4. `EC_EVENT_USB` then arrives.
5. The driver enters `gaokun_ucsi_altmode_notify_ind(..., true)`.
6. The notifier path effectively stalls for about 5 seconds.
7. During that stall, the delayed USB follow-up worker eventually fires and reports `missing USB event`.
8. Only after that delay does the UCSI core report:
   - `GET_CONNECTOR_STATUS failed (-110)`
9. Immediately after the timeout, the private sideband refresh finishes and the pending USB follow-up is completed.

Key timestamps from one captured sequence:

- `2430.031 ms`: `EC_EVENT_UCSI`, `cci=0x20000a04`, connector `2`
- `2435.098 ms`: `EC_EVENT_USB`, notifier enters the USB path
- `2437.120 ms`: delayed USB follow-up worker fires, still sees `done=0`
- `2440.160 ms`: `GET_CONNECTOR_STATUS failed (-110)`
- immediately afterward: private sideband refresh completes and `usb follow-up complete` is logged

Conclusion from USB-only trace:

- the USB event was not actually missing
- the USB notifier path was stuck long enough that the UCSI core timed out first

## Alternation Reproduction Result

User action:

1. plug USB flash drive
2. wait 3 seconds
3. unplug
4. wait 2 seconds
5. plug USB-C to DP cable
6. wait 5 seconds
7. unplug

Observed trace pattern:

The same primary failure happened again.

1. `EC_EVENT_UCSI` arrives for connector 2.
2. A second `EC_EVENT_UCSI` for connector 2 follows.
3. `EC_EVENT_USB` arrives.
4. The USB notifier path enters `gaokun_ucsi_altmode_notify_ind(..., true)`.
5. The notifier blocks for about 5 seconds.
6. The delayed USB follow-up worker fires while the notifier is still stuck and reports `missing USB event`.
7. The UCSI core then times out with:
   - `GET_CONNECTOR_STATUS failed (-110)`
8. Only after that timeout does the sideband refresh finish and complete the pending USB follow-up.

Key timestamps from one captured alternation sequence:

- `2574.303 ms`: `EC_EVENT_UCSI`, `cci=0x80000a04`, connector `2`
- `2574.320 ms`: `EC_EVENT_USB`, notifier enters the USB path
- `2576.320 ms`: delayed USB follow-up worker fires, still sees `done=0`
- `2579.424 ms`: `GET_CONNECTOR_STATUS failed (-110)`
- immediately afterward: sideband refresh completes and `usb follow-up complete` is logged

Conclusion from alternation trace:

- alternating USB and DP did **not** reveal a different first-order failure
- it reproduced the same USB-notifier stall and UCSI timeout pattern seen in USB-only testing

## What The Trace Proves

The trace now supports the following statements strongly:

### 1. The `EC_EVENT_USB` path is the first real failure point

The driver enters the USB notifier path on time, but it does not return quickly enough.

### 2. The private EC sideband work is colliding with normal UCSI command handling

The UCSI core is still trying to process connector change and obtain connector status while the driver's private USB-side notifier path is doing blocking EC-sideband work inline.

### 3. `missing USB event` is often misleading

In the captured failing sequences, the USB event *did* arrive. The timeout worker only fired because the pending completion was not marked done until much later.

### 4. This is already broken in plain USB-only handling

One of the timeout-side sideband snapshots during the USB-only reproduction showed a USB-like state:

- `raw_dcc=0x5`
- `raw_ddi=0x9`
- decoded as `ccx=1 mux=1 mode=1 svid=0xff00`

Later snapshots after timeout also showed a pure non-DP state:

- `raw_dcc=0x2`
- `raw_ddi=0x0`
- decoded as `ccx=2 mux=0 mode=0 svid=0x0`

So the first failure is not “DP mode was mishandled.”
The first failure already exists in USB-only event handling.

## Current Technical Hypothesis

The most likely current explanation is:

1. `EC_EVENT_UCSI` arms a per-port follow-up wait.
2. `EC_EVENT_USB` arrives on time.
3. The `EC_EVENT_USB` notifier path calls the private sideband handling path inline.
4. That private path performs blocking EC UCSI sideband register work while the UCSI core is still processing the connector change.
5. The UCSI core's `GET_CONNECTOR_STATUS` then times out with `-110`.
6. Only after the timeout does the private path complete and mark the pending USB follow-up as done.

In short:

> the driver is not primarily missing the USB event; it is processing the USB-side follow-up too synchronously and too slowly inside the notifier path

## What This Means For The Next Patch

The next patch should **not** jump back to DP mux or orientation work yet.

The next patch should target the `EC_EVENT_USB` path directly.

### Recommended direction

- keep the `8c15074` baseline
- keep the instrumentation for now
- make the `EC_EVENT_USB` notifier path smaller
- avoid blocking the notifier on the expensive sideband refresh in the same way
- preserve prompt EC ACK semantics carefully

### Important caution

We already tested a broad “defer the whole altmode path to a worker” idea earlier in the project history, and that was unsafe because delaying the wrong part of the EC-side acknowledgment path could destabilize the system.

So the next patch must be surgical:

- reduce the blocking portion of the USB notifier path
- but do **not** blindly defer every part of EC-side handling

## Things Not To Forget

- The user is on the instrumented branch `ucsi-dp-rework-plan`.
- The current branch intentionally starts from `8c15074`.
- The plan document already exists at:
  - [`docs/ucsi-dp-rework-plan.md`](/home/wano/playground/linux-gaokun/docs/ucsi-dp-rework-plan.md)
- This note is a supplement to that plan and records the actual runtime evidence.

## Suggested Immediate Next Step

Patch the `EC_EVENT_USB` handling path in [`gaokun-ec-dkms/ucsi_huawei_gaokun.c`](/home/wano/playground/linux-gaokun/gaokun-ec-dkms/ucsi_huawei_gaokun.c) so that:

- it no longer blocks the notifier for multiple seconds in the same way
- it still preserves the EC-side semantics needed to avoid event storms or stale EC state

After that:

1. rebuild the modules
2. reload them
3. re-enable dynamic debug
4. rerun:
   - USB-only repeated plug/unplug
   - USB/DP alternation

Success criterion for the next patch:

- `EC_EVENT_USB` should no longer remain stuck until after `GET_CONNECTOR_STATUS failed (-110)`
- the delayed USB follow-up worker should stop reporting “missing USB event” for cases where the USB event really arrived
