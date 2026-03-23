# Writing a new driver {#writing-driver}

<!--
Copyright 2018-2021, Collabora, Ltd. and the Monado contributors
SPDX-License-Identifier: BSL-1.0
-->

## Map

The components you will be interacting with are the **prober** (`st_prober`) to
find hardware devices and set up a working system, along with the **auxiliary**
code (`aux`) that provides various helpers. You will actually be implementing the
`xrt_device` interface by writing a driver. It is convention for interfaces to
allow full, complete control of anything a device might want to modify/control,
and to provide helper functionality in `aux` to simplify implementation of the
most common cases.

## Getting started

The easiest way to begin writing a driver is to start from a working example.
The **sim_display** driver at `src/xrt/drivers/sim_display/` serves as a good
starting template: it creates an HMD device with a custom `xrt_auto_prober`
implementation for hardware discovery, display processor implementations for
all graphics APIs, and simple display parameters that should be easy to modify.

Copy that directory and rename the files in it, then adapt the code to your
device. You will want to go through each function, implement any missing
functionality, and adapt any existing functionality to match your device. Refer
to other drivers for additional guidance. Most drivers are fairly simple, as
large or complex functionality in drivers is often factored out into separate
auxiliary libraries.

## What to Implement

You will definitely make at least one implementation of `xrt_device`. If your
driver can talk to e.g. both a headset and corresponding controllers, you can
choose to expose all those through a single `xrt_device` implementation, or
through multiple implementations that may share some underlying component (by
convention called `..._system`). Both are valid choices, and the right one to
choose depends on which maps better to your underlying device or API you are
connecting to. It is more common to have one `xrt_device` per piece of hardware.

Depending on whether your device can be created from a detected USB HID device,
you will also need to implement either `xrt_auto_prober` or a function
matching `xrt_prober_found_func_t` which is the function pointer type of
`xrt_prober_entry::found`. See below for more details.

## Probing

When should I implement the `xrt_auto_prober` interface? The answer is not too
hard: you use the auto prober interface when the basic USB VID/PID-based
interface is not sufficient for you to detect presence/absence of your device,
or if you don't want to use the built-in HID support for some reason.

If you can detect based on VID/PID, you will instead implement a function
matching `xrt_prober_found_func_t` to perform detection of your device based
on the USB HID.

Either way, your device's detection details will need to be added to a list used
by the prober at `xrt_instance` startup time. The stock lists are in
`src/xrt/targets/common/target_lists.c`. These are shared by the various targets
(OpenXR runtime shared library, service executable, utility executables) also
found in `src/xrt/targets`.

## Display Processor Integration

If your driver provides a 3D display, you will also need to implement the
**display processor** vtable for each graphics API your display supports. See
[vendor-integration.md](vendor-integration.md) for the full display processor
contract, vtable design, and integration instructions.
