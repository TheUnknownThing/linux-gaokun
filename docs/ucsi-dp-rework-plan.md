# UCSI / DP Rework Plan for Huawei MateBook E Go

## Branch

- Working branch: `ucsi-dp-rework-plan`
- Baseline commit: `8c15074d2d20`
- Baseline rationale:
  `8c15074` is the last point in the reviewed series where the UCSI transport was materially improved without introducing the later DP-routing regressions. It contains the required UCSI ABI sync for the current kernel and the `ppm_lock` serialization for private EC sideband accesses.

## Goal

Reintroduce robust USB-C DisplayPort support on top of the `8c15074` baseline without regressing:

- repeated USB drive plug/unplug stability
- alternating USB drive and USB-C-to-DP cable handling
- boot-time internal panel bring-up
- UCSI initialization reliability

## Current Understanding

### What `8c15074` fixed correctly

- Synced the out-of-tree UCSI ABI with the running kernel.
- Added the required `poll_cci` support.
- Serialized private EC UCSI sideband register access with the UCSI core `ppm_lock`.
- Kept the conservative delayed registration flow needed to avoid early-boot display breakage.

### What later commits added

- `c860a9cea264`
  Added bounded EC event draining and full UCSI object recreation on retry.
- `b57619764ae5`
  Added Type-C mux handling in the UCSI driver and `mode-switch;` for `usb_1_qmpphy`.
- `86677448309d`
  Added manual orientation switching from the EC altmode path.
- `cb7a710fd66a`
  Removed the manual orientation switching again and narrowed mux programming to DP-only.

### What later commits broke or exposed

- The DP work was attached to a fragile EC-sideband event path instead of a stable connector-state path.
- Some revisions duplicated kernel-owned Type-C responsibilities.
- The EC notifier path remained too expensive while the EC IRQ thread was draining queued events.
- Cross-port USB follow-up tracking became incorrect again in the later tree.

## Proven Constraints

These are the rules that the next implementation must respect.

### 1. Boot-time UCSI handling is display-sensitive

- Do not move back to early EC-driven registration.
- Do not register the EC notifier before the UCSI core is ready enough to tolerate incoming events.
- Do not aggressively touch EC/UCSI state during the boot window.

### 2. The kernel UCSI core already owns normal USB Type-C routing

- `typec_set_orientation()` is driven by the UCSI connector status callback path.
- `typec_set_mode(TYPEC_STATE_USB)` and `TYPEC_STATE_SAFE` are already handled by the UCSI core for ordinary USB and disconnect transitions.
- The driver must not take over routine USB-only mux or switch handling unless there is a proven hardware-specific gap.

### 3. DP routing still needs a board-specific addition

- The Qualcomm QMP combo PHY needs a Type-C mux event to select DP-only vs USB+DP mode.
- That is the real missing DP ingredient.
- `usb_1_qmpphy` also needs `mode-switch;` in the DTS if port 1 is expected to support DP.

### 4. The EC event path is the main architectural weakness

- The EC IRQ thread drains events synchronously.
- The notifier chain is called inline from the EC drain loop.
- Every extra EC transaction in that notifier path adds more serialized I2C work and delay.
- If that path becomes too heavy, the driver starts seeing:
  - `EC event queue did not drain after 16 queries`
  - `missing USB event for port X after UCSI event`
  - `GET_CONNECTOR_STATUS failed (-110)`

## Root Problems To Solve

### Problem A: DP support is currently hanging off the wrong event boundary

The current design relies on private EC sideband state sampled from `EC_EVENT_USB` or the delayed "missing USB event" fallback. That is not a robust foundation for DP routing, because if the EC follow-up event is delayed or the queue is congested, DP orientation and pin assignment become stale exactly when the DRM stack needs them.

### Problem B: EC notifier work is too heavy

The EC notifier path currently mixes:

- UCSI notifications
- private EC sideband refresh
- pin-assignment ACK writes
- DP mux programming
- HPD bridge notification

That is too much to do inline while the EC IRQ thread is still draining queued events.

### Problem C: USB follow-up tracking is not trustworthy in the later tree

The later DP branch regressed back toward global completion behavior, where one USB-side event can satisfy pending waits for the wrong connector. That is not acceptable on a two-port machine.

### Problem D: There are really two separate state machines

- The official UCSI core state machine
- The Huawei EC private sideband state machine for DP/pin-assignment details

The next design has to define a clean ownership boundary between them instead of letting both update the Type-C path opportunistically.

## Plan

## Phase 0: Reset To The Right Starting Point

- Keep the codebase on `8c15074d2d20` as the functional baseline.
- Bring over only the DTS `mode-switch;` addition for `usb_1_qmpphy` after it is reviewed in isolation.
- Do not carry over the later DP driver changes directly.

Deliverable:

- A clean branch where USB-only behavior matches the known-good baseline again.

## Phase 1: Add Minimal Instrumentation Before New Behavior

Add temporary debug logging, guarded and easy to remove, around:

- EC event IDs entering the notifier
- timestamped notifier entry/exit
- `EC_EVENT_UCSI` vs `EC_EVENT_USB`
- `CCI` values
- per-port sideband snapshot fields:
  - `ccx`
  - `mux`
  - `mode`
  - `hpd_state`
  - `hpd_irq`
- port update bitmask
- per-port USB follow-up wait/timeout transitions
- `typec_mux_set()` attempts and return codes
- `typec_set_orientation()` results

Purpose:

- Prove the event ordering during:
  - USB-only repeated plug/unplug
  - DP-only attach/detach
  - alternating USB drive and DP cable

Deliverable:

- One or two reproducible traces that show which transition fails first.

## Phase 2: Define The Ownership Boundary

The intended ownership should be:

- UCSI core:
  - connector lifetime
  - partner changes
  - ordinary USB mode
  - ordinary orientation updates from connector status
- Huawei EC private sideband path:
  - DP pin assignment details not surfaced cleanly through standard UCSI status
  - HPD mirror to the DRM AUX bridge
  - any required EC-specific pin-assignment ACK

Key design rule:

- The private EC path should not drive USB-only mode changes.
- The private EC path should only supply the missing DP-specific information.

Deliverable:

- A short design note in commit message or code comments stating exactly which layer owns:
  - orientation
  - mux mode
  - HPD
  - EC ACK

## Phase 3: Make The EC Fast Path Smaller

Rework the notifier handling so that the EC IRQ thread does the minimum necessary inline.

Target behavior:

- `EC_EVENT_UCSI`
  - read `CCI`
  - notify the UCSI core
  - start only the minimum per-port follow-up tracking needed
- `EC_EVENT_USB`
  - perform only the minimum sideband work needed to avoid leaving the EC latched
  - do not perform unrelated USB-wide routing changes

Important caution:

- The previous "defer everything to a worker" experiment was unsafe because it delayed the EC-side pin-assignment ACK too much.
- So the next version should not blindly move `pan_ack` out of the fast path.
- If we defer anything, it must be only the non-essential work, not the event-clear operation that the EC appears to require promptly.

Deliverable:

- A notifier path that preserves prompt EC ACK semantics but reduces unnecessary inline work.

## Phase 4: Rebuild Per-Port Follow-Up Tracking

Rework the USB follow-up logic so that it is genuinely per-port again.

Requirements:

- One connector's USB-side event must not satisfy another connector's wait.
- The fallback path must not trigger if the correct connector already completed.
- If the EC does not provide a stable per-port USB event bit, use a more defensible correlation strategy than "complete every port".

Possible approaches to test:

- correlate using the connector indicated by the immediately preceding `CCI`
- maintain a short-lived per-port pending state window
- only fall back for the connector that actually triggered the current UCSI connector change

Deliverable:

- No more cross-port suppression of the timeout path.

## Phase 5: Reintroduce DP Mux Programming Narrowly

After the event model is stable again, add back the DP-specific mux work only.

Rules:

- Acquire the Type-C mux from the connector fwnode.
- Only call `typec_mux_set()` for DisplayPort SVID cases.
- Do not use the private path to force USB-only mode.
- Preserve the existing DRM AUX HPD bridge signaling.
- Apply the `usb_1_qmpphy` `mode-switch;` DTS fix so both ports can participate.

Expected mapping:

- DP pin assignment C or E -> DP-only
- DP pin assignment D -> USB+DP

Deliverable:

- DP routing works on both ports without losing baseline USB robustness.

## Phase 6: Solve Orientation Only After The Above Is Stable

Orientation should be addressed last, because it depends on reliable event timing.

Preferred direction:

- First try to feed fresher orientation state into the normal UCSI connector-status path instead of manually driving the Type-C switch from the private EC altmode handler.
- Only if that proves insufficient should a board-specific manual orientation fix be considered.

Hard rule:

- Do not reintroduce broad manual `typec_switch_set()` calls from the sideband path until the event-ordering problem is understood and measured.

Deliverable:

- DP works in both plug orientations on both ports.

## Test Matrix

Every candidate patch set must be checked against the full matrix below.

### Boot / Init

- Cold boot with nothing attached
- Cold boot with USB drive attached
- Cold boot with DP monitor attached
- Confirm internal panel lights normally
- Confirm no repeating `ucsi connector is not initialized yet` spam after boot
- Confirm no `PPM init failed`

### USB-only

- Repeated USB drive plug/unplug on port 0, both orientations
- Repeated USB drive plug/unplug on port 1, both orientations
- Watch for:
  - `attempt power cycle`
  - `unable to enumerate USB device`
  - `GET_CONNECTOR_STATUS failed (-110)`

### DP-only

- DP attach/detach on port 0, both orientations
- DP attach/detach on port 1, both orientations
- Confirm:
  - monitor is detected
  - monitor lights
  - HPD/link training succeeds

### Alternation

- Alternate USB drive and DP cable on the same physical port
- Alternate USB drive and DP cable across both ports
- Repeat enough times to trigger any latent queueing issues
- Watch for:
  - `missing USB event`
  - `EC event queue did not drain after 16 queries`
  - `GET_CONNECTOR_STATUS failed (-110)`

### Power Management

- Suspend/resume with nothing attached
- Suspend/resume with USB attached
- Suspend/resume with DP attached

## Acceptance Criteria

The rework is acceptable only if all of the following are true:

- USB robustness is at least as good as `8c15074`.
- DP works on both supported ports and both plug orientations.
- Alternating USB and DP does not poison the port state.
- No repeated `GET_CONNECTOR_STATUS failed (-110)` under normal use.
- No recurring EC queue-drain warnings during ordinary attach/detach use.
- Boot-time internal display bring-up remains intact.

## Implementation Order

Recommended order of actual code changes:

1. Start from `8c15074`.
2. Bring in only the DTS `mode-switch;` fix for `usb_1_qmpphy`.
3. Add temporary instrumentation.
4. Fix EC notifier / follow-up tracking architecture.
5. Verify baseline USB robustness again.
6. Add DP-only mux programming.
7. Verify DP on both ports.
8. Address orientation only if still needed.
9. Remove temporary instrumentation before finalizing.

## Notes For Future Commits

- Keep patches small and single-purpose.
- Do not mix:
  - retry/lifecycle changes
  - EC event handling changes
  - DP mux changes
  - orientation changes
- After each step, re-test before stacking the next change.
- Preserve comments warning that early boot-time UCSI activity can indirectly affect internal display bring-up on this machine.
