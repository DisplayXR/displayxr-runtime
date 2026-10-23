# Writing a new driver {#writing-driver}

<!--
Copyright 2018-2021, Collabora, Ltd. and the Monado contributors
SPDX-License-Identifier: BSL-1.0
-->

## Map

DisplayXR does not probe for hardware. A display reaches the runtime as a
**display-processor plug-in** and input as an **input-provider plug-in**, both
discovered at `xrCreateInstance` and wired into the system by the
**sim_display builder** (`src/xrt/targets/common/target_builder_sim_display.c`),
the only builder in `target_lists.c`. The prober (`st_prober`) that remains is
just the builder selection plus the builders' lifetime; the Monado USB/HID
entry lists, auto-probers and udev enumeration were removed (#703).

You will actually be implementing the `xrt_device` interface. It is convention
for interfaces to allow full, complete control of anything a device might want
to modify/control, and to provide helper functionality in `aux` to simplify
implementation of the most common cases.

## Getting started

For a 3D display, do not write an in-tree driver: write a vendor plug-in. Start
from [vendor-plugin-onboarding.md](vendor-plugin-onboarding.md) and the
`sim_display` plug-in at `src/xrt/drivers/sim_display/`, which implements the
display processor for every graphics API with simple display parameters that
are easy to modify.

For an input device (controllers, hands, a gamepad), write an input-provider
plug-in — see `docs/specs/runtime/input-provider-discovery.md` and the in-tree
`sim_input/` reference.

## What to Implement

You will definitely make at least one implementation of `xrt_device`. If your
driver can talk to e.g. both a headset and corresponding controllers, you can
choose to expose all those through a single `xrt_device` implementation, or
through multiple implementations that may share some underlying component (by
convention called `..._system`). Both are valid choices, and the right one to
choose depends on which maps better to your underlying device or API you are
connecting to. It is more common to have one `xrt_device` per piece of hardware.

To see what the runtime built — the builder, the active display plug-in and
input provider, every device and the role assignments — run
`displayxr-cli devices`.

## Display Processor Integration

If your driver provides a 3D display, you will also need to implement the
**display processor** vtable for each graphics API your display supports. See
[vendor-plugin-onboarding.md](vendor-plugin-onboarding.md) for the full display processor
contract, vtable design, and integration instructions.
