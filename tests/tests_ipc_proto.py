#!/usr/bin/env python3
# Copyright 2026, DisplayXR contributors.
# SPDX-License-Identifier: BSL-1.0
"""Unit tests for the IPC protocol generator (src/xrt/ipc/shared/proto.py).

#1699: weave_submit_dmabuf is the first call in proto.json that carries BOTH
in_handles (the input / overlay dma-bufs + the acquire sync_file) and
out_handles (the per-frame release sync_file). The generator's two handle
paths were written independently and had never met; with the default argument
stem ("handles") on both sides the client proxy would declare two parameters
of the same name. These tests pin:

  - the optional per-handle "name" key, and that it is validated;
  - that a call with both handle kinds and colliding names is rejected;
  - the shape of the code generated for the real weave_submit_dmabuf.

Run: tests_ipc_proto.py <path to src/xrt/ipc/shared>
"""

import os
import sys
import tempfile
import unittest

SHARED_DIR = None


def _load():
    sys.dont_write_bytecode = True  # never litter the source tree from ctest
    sys.path.insert(0, SHARED_DIR)
    import proto  # noqa: E402  (path set above)
    from ipcproto.common import Proto  # noqa: E402
    return proto, Proto



#: XR_DXR_lift spec 4 (#2054): the conv_* family, appended in this order.
SPEC4_CONV_CALLS = ['conv_get_generation', 'conv_enum_task', 'conv_request_warm', 'conv_stream_create', 'conv_stream_reset',
                    'conv_acquire_result', 'conv_poll_event', 'conv_submit_frame', 'conv_get_cpu_output']

class GeneratorMixedHandlesTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.proto, cls.Proto = _load()
        cls.p = cls.Proto.load_and_parse(os.path.join(SHARED_DIR, "proto.json"))

    def _call(self, name):
        for call in self.p.calls:
            if call.name == name:
                return call
        self.fail("call %s not in proto.json" % name)

    def _generate(self, fn_name, suffix):
        fd, path = tempfile.mkstemp(suffix=suffix)
        os.close(fd)
        try:
            getattr(self.proto, fn_name)(path, self.p)
            with open(path) as f:
                return f.read()
        finally:
            os.remove(path)

    @staticmethod
    def _function_body(text, start_marker):
        i = text.index(start_marker)
        j = text.find("\n}\n", i)
        return text[i:j]

    def test_every_call_parses_and_names_are_unique(self):
        for call in self.p.calls:
            if call.in_handles and call.out_handles:
                self.assertFalse(
                    set(call.in_handles.arg_names) & set(call.out_handles.arg_names),
                    call.name)

    def test_weave_submit_dmabuf_has_both(self):
        call = self._call("weave_submit_dmabuf")
        self.assertEqual(call.in_handles.typename, "xrt_graphics_buffer_handle_t")
        self.assertEqual(call.in_handles.arg_name, "handles")
        self.assertEqual(call.out_handles.typename, "xrt_graphics_sync_handle_t")
        self.assertEqual(call.out_handles.arg_name, "release_fences")
        self.assertEqual(call.out_handles.count_arg_name, "release_fence_count")

    def test_weave_v12_fields_are_trailing(self):
        # XR_DXR_weave v12 (browser-pvt#180) grew two messages. Both additions
        # are APPENDED, so every pre-v12 field keeps its offset — the wire
        # carries no version negotiation (the u_git_tag gate refuses a skewed
        # pair at connect), and this keeps any future reader of an older layout
        # from being silently misaligned.
        sub = [a.name for a in self._call("weave_submit_dmabuf").out_args]
        self.assertEqual(sub, ["have_output", "width", "height", "fence_value", "eyes", "origin"])
        geo = [a.name for a in self._call("weave_set_window_geometry").in_args]
        self.assertEqual(geo[:5], ["origin_x", "origin_y", "client_w", "client_h", "display_id"])
        self.assertEqual(geo[5:], ["logical_valid", "logical_x", "logical_y", "logical_scale"])
        # The once-per-allocation export is untouched: the origin is per frame.
        out = [a.name for a in self._call("weave_get_output_dmabuf").out_args]
        self.assertEqual(out, ["have_output", "desc"])

    def test_client_proxy(self):
        text = self._generate("generate_client_c", "ipc_client_generated.c")
        body = self._function_body(text, "ipc_call_weave_submit_dmabuf(")
        self.assertIn("const xrt_graphics_buffer_handle_t *handles", body)
        self.assertIn("const uint32_t handle_count", body)
        self.assertIn("xrt_graphics_sync_handle_t *release_fences", body)
        self.assertIn("uint32_t release_fence_count", body)
        # In handles go out first (after the server's sync), out handles come
        # back with the reply.
        send = body.index("ipc_send_handles_graphics_buffer(")
        recv = body.index("ipc_receive_handles_graphics_sync(")
        self.assertLess(send, recv)
        self.assertIn("release_fences,", body[recv:])
        self.assertIn(".handle_count = handle_count", body)

    def test_server_dispatch(self):
        text = self._generate("generate_server_c", "ipc_server_generated.c")
        body = self._function_body(text, "case IPC_WEAVE_SUBMIT_DMABUF: {")
        self.assertIn("xrt_graphics_buffer_handle_t in_handles[XRT_MAX_IPC_HANDLES]", body)
        self.assertIn("xrt_graphics_sync_handle_t release_fences[XRT_MAX_IPC_HANDLES]", body)
        self.assertIn("ipc_receive_handles_graphics_buffer(", body)
        handler = body.index("ipc_handle_weave_submit_dmabuf(")
        self.assertIn("&release_fence_count", body[handler:])
        self.assertIn("&in_handles[0]", body[handler:])
        send = body.index("ipc_send_handles_graphics_sync(")
        self.assertLess(handler, send)

    def test_server_handler_decl(self):
        text = self._generate("generate_server_header", "ipc_server_generated.h")
        i = text.index("ipc_handle_weave_submit_dmabuf(")
        decl = text[i:text.index(";", i)]
        self.assertIn("uint32_t max_release_fence_count", decl)
        self.assertIn("xrt_graphics_sync_handle_t *out_release_fences", decl)
        self.assertIn("uint32_t *out_release_fence_count", decl)
        self.assertIn("const xrt_graphics_buffer_handle_t *handles", decl)

    def test_colliding_names_rejected(self):
        with self.assertRaises(RuntimeError):
            self.Proto.parse({
                "bad": {
                    "in_handles": {"type": "xrt_graphics_buffer_handle_t"},
                    "out_handles": {"type": "xrt_graphics_sync_handle_t"},
                }
            })

    def test_bad_name_rejected(self):
        for bad in ("fence", "Fences", "1fences", "fen-ces"):
            with self.assertRaises(RuntimeError, msg=bad):
                self.Proto.parse({
                    "bad": {"out_handles": {"type": "xrt_graphics_sync_handle_t", "name": bad}}
                })

    def test_default_name_unchanged(self):
        call = self._call("weave_get_fence")
        self.assertEqual(call.out_handles.arg_names, ("handles", "handle_count"))

    def test_segment_calls_are_appended(self):
        # Multi-screen on the service path (ADR-047 Amendment 3): the three
        # calls ride at the END of proto.json, so every earlier command keeps
        # its enum value (append-only; the u_git_tag gate refuses a skewed
        # client/service pair at connect anyway).
        names = [c.name for c in self.p.calls]
        tail = ["compositor_segments_enable", "compositor_get_segment_metrics", "compositor_set_view_routing"]
        i = names.index(tail[0])
        self.assertEqual(names[i:i + 3], tail)
        # Only later appends may follow them (ADR-051's status calls, display
        # dashboard phase 7's re-probe request, phase 8's launch settings,
        # ADR-046 Phase 4's runtime-drawn cursor over IPC).
        later = ("system_request_display_reprobe", "system_reload_service_config", "system_workspace_launch",
                 "system_workspace_hotkey_suspend", "weave_get_segments", "workspace_enumerate_screens",
                 "workspace_set_window_placement", "workspace_get_window_placement",
                 "workspace_set_overlay_on_screen", "conv_hint_interest", "compositor_set_cursor_draw_request",
                 "compositor_set_cursor_measure_request", "system_request_component_reselect",
                 "conv_warm") + tuple(SPEC4_CONV_CALLS)
        self.assertTrue(all(n.startswith("system_get_") or n in later
                            for n in names[i + 3:]), names[i + 3:])
        enable = self._call("compositor_segments_enable")
        self.assertEqual([(a.name, a.typename) for a in enable.in_args], [("pinned_display_id", "uint64_t")])
        get = self._call("compositor_get_segment_metrics")
        self.assertEqual([(a.name, a.typename) for a in get.out_args],
                         [("metrics", "struct xrt_segment_metrics")])
        route = self._call("compositor_set_view_routing")
        self.assertEqual([(a.name, a.typename) for a in route.in_args],
                         [("routing", "struct xrt_segment_view_routing")])

    def test_status_calls_are_appended(self):
        # ADR-051 D3 (display dashboard phase 2): the three session-free DIAG
        # status calls ride at the END of proto.json (append-only), and the
        # snapshot crosses in fixed-size pieces — the head + one screen row per
        # call, one client row per call — each carrying its generation.
        names = [c.name for c in self.p.calls]
        tail = ["system_get_status_generation", "system_get_status_snapshot", "system_get_client_segments"]
        i = names.index(tail[0])
        self.assertEqual(names[i:i + 3], tail)
        self.assertEqual(names[i + 3], "system_request_display_reprobe")
        gen = self._call("system_get_status_generation")
        self.assertEqual(gen.in_args, [])
        self.assertEqual([(a.name, a.typename) for a in gen.out_args],
                         [("generation", "struct xrt_status_generation")])
        snap = self._call("system_get_status_snapshot")
        self.assertEqual([(a.name, a.typename) for a in snap.in_args], [("screen_index", "uint32_t")])
        self.assertEqual([(a.name, a.typename) for a in snap.out_args],
                         [("head", "struct xrt_status_head"), ("screen", "struct xrt_status_screen")])
        seg = self._call("system_get_client_segments")
        self.assertEqual([(a.name, a.typename) for a in seg.in_args], [("client_id", "uint32_t")])
        self.assertEqual([(a.name, a.typename) for a in seg.out_args],
                         [("client", "struct xrt_status_client"), ("metrics", "struct xrt_segment_metrics"),
                          ("generation", "struct xrt_status_generation")])
        for call in (gen, snap, seg):
            self.assertFalse(call.in_handles, call.name)
            self.assertFalse(call.out_handles, call.name)
            self.assertFalse(call.varlen, call.name)

    def test_reprobe_request_is_appended(self):
        # Display dashboard phase 7: `dp use|reset --screen` asks the running
        # service to re-probe now. Session-free, DIAG only, no payload —
        # appended, so every earlier command keeps its enum value (only phase
        # 8's three calls, #1884's weave_get_segments and ADR-052's three
        # workspace-screen calls follow it).
        names = self._names_before_spec4()
        self.assertEqual(names[-16], "system_request_display_reprobe")
        call = self._call("system_request_display_reprobe")
        self.assertEqual(call.in_args, [])
        self.assertEqual(call.out_args, [])
        self.assertFalse(call.in_handles)
        self.assertFalse(call.out_handles)
        self.assertFalse(call.varlen)

    def test_workspace_launch_calls_are_appended(self):
        # Display dashboard phase 8: `workspace set` asks the running service
        # to re-apply service.json (no payload), `workspace launch <id>` to
        # spawn a controller through the hotkey's own path (the id in, a launch
        # status out), and a hotkey-capture box takes the launch hook out /
        # puts it back (one bool in). Session-free, DIAG only; only #1884's
        # weave_get_segments and ADR-052's workspace-screen calls follow them.
        names = self._names_before_spec4()
        self.assertEqual(names[-15:-12], ["system_reload_service_config", "system_workspace_launch",
                                        "system_workspace_hotkey_suspend"])
        suspend = self._call("system_workspace_hotkey_suspend")
        self.assertEqual([(a.name, a.typename) for a in suspend.in_args], [("suspend", "bool")])
        self.assertEqual(suspend.out_args, [])
        self.assertFalse(suspend.in_handles or suspend.out_handles or suspend.varlen)
        reload_call = self._call("system_reload_service_config")
        self.assertEqual(reload_call.in_args, [])
        self.assertEqual(reload_call.out_args, [])
        launch = self._call("system_workspace_launch")
        self.assertEqual([(a.name, a.typename) for a in launch.in_args],
                         [("controller", "struct ipc_workspace_controller_id")])
        self.assertEqual([(a.name, a.typename) for a in launch.out_args], [("status", "uint32_t")])
        for call in (reload_call, launch):
            self.assertFalse(call.in_handles, call.name)
            self.assertFalse(call.out_handles, call.name)
            self.assertFalse(call.varlen, call.name)
        # The generated server dispatch reaches both handlers.
        text = self._generate("generate_server_c", "ipc_server_generated.c")
        self.assertIn("ipc_handle_system_reload_service_config(", text)
        self.assertIn("ipc_handle_system_workspace_launch(", text)
        self.assertIn("ipc_handle_system_workspace_hotkey_suspend(", text)

    def test_weave_segments_call_is_appended(self):
        # XR_DXR_weave v19 (#1884, ADR-047 Amendment 4): a present-owner's
        # per-screen segment table (with every screen's eyes) crosses as its
        # own call, APPENDED at the end, so a caller that does not chain
        # XrWeaveOutputRectPartsDXR never sends it (weave_submit's reply only
        # gained a trailing count, below). The table is the per-segment views'
        # struct.
        names = self._names_before_spec4()
        self.assertEqual(names[-12], "weave_get_segments")  # ADR-052's four calls, ADR-053's three, then ADR-046 Phase 4's follow it
        call = self._call("weave_get_segments")
        self.assertEqual(call.in_args, [])
        self.assertEqual([(a.name, a.typename) for a in call.out_args],
                         [("metrics", "struct xrt_segment_metrics")])
        self.assertFalse(call.in_handles or call.out_handles or call.varlen)
        text = self._generate("generate_server_c", "ipc_server_generated.c")
        self.assertIn("ipc_handle_weave_get_segments(", text)

    def test_workspace_screen_calls_are_appended(self):
        # XR_DXR_spatial_workspace v25 (ADR-052, the workspace on N screens):
        # the screen enumeration and the per-screen placement pair ride at the
        # END of proto.json, so every earlier command keeps its enum value. A
        # pre-v25 controller sends none of them; set_window_pose is untouched.
        names = self._names_before_spec4()
        self.assertEqual(names[-11:-8], ["workspace_enumerate_screens", "workspace_set_window_placement",
                                        "workspace_get_window_placement"])
        enum = self._call("workspace_enumerate_screens")
        self.assertEqual(enum.in_args, [])
        self.assertEqual([(a.name, a.typename) for a in enum.out_args],
                         [("list", "struct ipc_workspace_screen_list")])
        put = self._call("workspace_set_window_placement")
        self.assertEqual([(a.name, a.typename) for a in put.in_args],
                         [("client_id", "uint32_t"), ("placement", "struct ipc_workspace_window_placement")])
        self.assertEqual(put.out_args, [])
        get = self._call("workspace_get_window_placement")
        self.assertEqual([(a.name, a.typename) for a in get.in_args], [("client_id", "uint32_t")])
        self.assertEqual([(a.name, a.typename) for a in get.out_args],
                         [("placement", "struct ipc_workspace_window_placement")])
        for call in (enum, put, get):
            self.assertFalse(call.in_handles or call.out_handles or call.varlen, call.name)
        pose = self._call("workspace_set_window_pose")
        self.assertEqual([a.name for a in pose.in_args], ["client_id", "pose", "width_m", "height_m"])
        text = self._generate("generate_server_c", "ipc_server_generated.c")
        for n in ("workspace_enumerate_screens", "workspace_set_window_placement", "workspace_get_window_placement"):
            self.assertIn("ipc_handle_%s(" % n, text)

    def test_workspace_overlay_on_screen_is_appended(self):
        # XR_DXR_spatial_workspace v26 (ADR-052 Amendment 2, overlays per
        # screen): a screen-tagged overlay crosses as its own call APPENDED at
        # the end -- the untagged workspace_set_overlay keeps its enum value and
        # its wire form, so a pre-v26 controller sends exactly what it always did.
        names = self._names_before_spec4()
        self.assertEqual(names[-8], "workspace_set_overlay_on_screen")  # ADR-053's three calls, then ADR-046 Phase 4's, follow it
        call = self._call("workspace_set_overlay_on_screen")
        self.assertEqual([(a.name, a.typename) for a in call.in_args],
                         [("info", "struct ipc_workspace_overlay_info"), ("screen_id", "uint64_t")])
        self.assertEqual(call.out_args, [])
        self.assertFalse(call.in_handles or call.out_handles or call.varlen)
        old = self._call("workspace_set_overlay")
        self.assertEqual([(a.name, a.typename) for a in old.in_args],
                         [("info", "struct ipc_workspace_overlay_info")])
        text = self._generate("generate_server_c", "ipc_server_generated.c")
        self.assertIn("ipc_handle_workspace_set_overlay_on_screen(", text)

    def test_conv_status_row_calls_are_appended(self):
        # ADR-053 (#2006): the status snapshot's conversion rows cross one per
        # reply, by index (each under the 1 KB DIAG limit), APPENDED at the end
        # so every earlier command keeps its enum value. Session-free, DIAG only;
        # the head carries only the two row counts. #2005's conv_hint_interest and ADR-046 Phase 4's call follow.
        names = self._names_before_spec4()
        self.assertEqual(names[-7:-5], ["system_get_conv_provider_row", "system_get_conv_task_row"])
        prov = self._call("system_get_conv_provider_row")
        task = self._call("system_get_conv_task_row")
        self.assertEqual([(a.name, a.typename) for a in prov.in_args], [("index", "uint32_t")])
        self.assertEqual([(a.name, a.typename) for a in prov.out_args],
                         [("row", "struct xrt_status_conv_provider"), ("generation", "struct xrt_status_generation")])
        self.assertEqual([(a.name, a.typename) for a in task.in_args], [("index", "uint32_t")])
        self.assertEqual([(a.name, a.typename) for a in task.out_args],
                         [("row", "struct xrt_status_conv_task"), ("generation", "struct xrt_status_generation")])
        for call in (prov, task):
            self.assertFalse(call.in_handles or call.out_handles or call.varlen, call.name)
        text = self._generate("generate_server_c", "ipc_server_generated.c")
        self.assertIn("ipc_handle_system_get_conv_provider_row(", text)
        self.assertIn("ipc_handle_system_get_conv_task_row(", text)

    def _names_before_spec4(self):
        """Command names up to the spec-4 conv_* family (#2054), which is
        appended LAST (after conv_hint_interest, the two cursor calls, #2051's reselect
        and #2029's DIAG conv_warm); the older end-relative pins read
        against this prefix."""
        names = [c.name for c in self.p.calls]
        self.assertEqual(names[-len(SPEC4_CONV_CALLS):], SPEC4_CONV_CALLS)
        return names[:-len(SPEC4_CONV_CALLS)]

    def test_spec4_conv_family_is_appended(self):
        # XR_DXR_lift spec 4 (#2054): the conv_* family rides at the END,
        # after conv_hint_interest, so every v3 lift_* message keeps its enum
        # value and wire form.
        names = [c.name for c in self.p.calls]
        self.assertEqual(names[-len(SPEC4_CONV_CALLS):], SPEC4_CONV_CALLS)
        self.assertLess(names.index("conv_hint_interest"), names.index(SPEC4_CONV_CALLS[0]))
        for name in SPEC4_CONV_CALLS:
            call = self._call(name)
            self.assertFalse(call.varlen, name)
            # Only the transport calls carry handles: the submit's image (+ its
            # fence) in, the CPU export's mapping out.
            self.assertEqual(bool(call.in_handles), name == "conv_submit_frame", name)
            self.assertEqual(bool(call.out_handles), name == "conv_get_cpu_output", name)
        create = self._call("conv_stream_create")
        self.assertEqual([(a.name, a.typename) for a in create.in_args],
                         [("desc", "struct xrt_conv_wire_stream_desc"), ("required_outputs", "uint64_t"),
                          ("legacy_mode", "uint32_t"), ("input_scale", "float")])
        acquire = self._call("conv_acquire_result")
        self.assertEqual([(a.name, a.typename) for a in acquire.out_args],
                         [("ready", "bool"), ("discontinuity", "bool"), ("lift_result", "struct ipc_lift_result"),
                          ("meta", "struct xrt_conv_wire_result")])
        text = self._generate("generate_server_c", "ipc_server_generated.c")
        for name in SPEC4_CONV_CALLS:
            self.assertIn("ipc_handle_%s(" % name, text)

    def test_conv_hint_interest_is_appended(self):
        # ADR-053 (#2005): the conversion prewarm trigger rides as its own call
        # APPENDED after the two conversion DIAG row calls (#2006), so every
        # earlier command -- every v3 lift_* message among them -- keeps its
        # enum value and wire form. One fixed-size request, no reply payload,
        # no handles.
        names = self._names_before_spec4()
        self.assertEqual(names[-7:-4], ["system_get_conv_provider_row", "system_get_conv_task_row",
                                      "conv_hint_interest"])
        call = self._call("conv_hint_interest")
        self.assertEqual([(a.name, a.typename) for a in call.in_args],
                         [("hint", "struct xrt_conv_wire_hint_interest")])
        self.assertEqual(call.out_args, [])
        self.assertFalse(call.in_handles or call.out_handles or call.varlen)
        text = self._generate("generate_server_c", "ipc_server_generated.c")
        self.assertIn("ipc_handle_conv_hint_interest(", text)

    def test_cursor_draw_request_is_appended(self):
        # XR_DXR_cursor_depth v5 over IPC (ADR-046 Phase 4): a frame's runtime-
        # drawn cursor request crosses as its own call APPENDED at the end, and
        # only on a frame that armed it - a client that never asks sends nothing.
        # The reply is the service's answer to the PREVIOUS request (#1952).
        names = self._names_before_spec4()
        self.assertEqual(names[-4], "compositor_set_cursor_draw_request")  # Amendment 3's call follows it
        call = self._call("compositor_set_cursor_draw_request")
        self.assertEqual([(a.name, a.typename) for a in call.in_args],
                         [("request", "struct ipc_cursor_draw_request")])
        self.assertEqual([(a.name, a.typename) for a in call.out_args],
                         [("outcome", "struct ipc_cursor_draw_outcome")])
        self.assertFalse(call.in_handles or call.out_handles or call.varlen)
        text = self._generate("generate_server_c", "ipc_server_generated.c")
        self.assertIn("ipc_handle_compositor_set_cursor_draw_request(", text)

    def test_cursor_measure_request_is_appended(self):
        # XR_DXR_cursor_depth v7 over IPC (ADR-046 Amendment 3 point 6): the
        # measured-disparity request crosses as its own call APPENDED at the
        # end, only on a frame whose locate armed it - a client that never
        # asks sends nothing. The reply is the service's newest finished
        # measurement (one to three frames old).
        names = self._names_before_spec4()
        self.assertEqual(names[-3], "compositor_set_cursor_measure_request")
        call = self._call("compositor_set_cursor_measure_request")
        self.assertEqual([(a.name, a.typename) for a in call.in_args],
                         [("request", "struct ipc_cursor_measure_request")])
        self.assertEqual([(a.name, a.typename) for a in call.out_args],
                         [("measurement", "struct ipc_cursor_measure_result")])
        self.assertFalse(call.in_handles or call.out_handles or call.varlen)
        text = self._generate("generate_server_c", "ipc_server_generated.c")
        self.assertIn("ipc_handle_compositor_set_cursor_measure_request(", text)

    def test_weave_submit_split_signal_is_trailing(self):
        # ADR-047 Amendment 4 addendum: weave_submit's reply APPENDS the
        # segment count of the table that very submit published, so the state
        # tracker fetches the table (weave_get_segments) only for a window
        # woven per screen. Appended like the v12 fields: every earlier field
        # keeps its offset (the u_git_tag gate refuses a skewed pair).
        # ADR-046 Amendment 1 then APPENDS lift_cursor_mask (XR_DXR_cursor_depth
        # v3: which lift rects of this submit got the runtime's depth cursor),
        # after segment_count, by the same rule.
        sub = [a.name for a in self._call("weave_submit").out_args]
        self.assertEqual(sub, ["have_output", "width", "height", "fence_value", "eyes", "segment_count",
                               "lift_cursor_mask"])
        call = self._call("weave_submit")
        self.assertEqual(call.out_args[-2].typename, "uint32_t")
        self.assertEqual(call.out_args[-1].typename, "uint32_t")
        # weave_submit_dmabuf (desktop Linux: never segmented) is untouched.
        sub = [a.name for a in self._call("weave_submit_dmabuf").out_args]
        self.assertEqual(sub, ["have_output", "width", "height", "fence_value", "eyes", "origin"])


    def test_component_reselect_is_appended(self):
        # ADR-054 N3d (#2051): the DIAG component re-select rides as its own call
        # after the two cursor calls; only #2029's DIAG conv_warm and the spec-4
        # family (appended later) follow it.
        names = self._names_before_spec4()
        self.assertEqual(names[-2], "system_request_component_reselect")
        call = self._call("system_request_component_reselect")
        self.assertEqual([(a.name, a.typename) for a in call.in_args], [("component_class", "uint32_t")])
        self.assertEqual(call.out_args, [])
        self.assertFalse(call.in_handles or call.out_handles or call.varlen)

    def test_conv_warm_diag_is_appended(self):
        # #2029: `displayxr-cli conv warm` rides its own DIAG call (the request is
        # the wire's task request; the reply is the selected row's readiness before
        # and right after). It is the last call before the spec-4 family, whose own
        # session warm is conv_request_warm (no reply payload).
        names = self._names_before_spec4()
        self.assertEqual(names[-1], "conv_warm")
        call = self._call("conv_warm")
        self.assertEqual([(a.name, a.typename) for a in call.in_args],
                         [("request", "struct xrt_conv_wire_task_request")])
        self.assertEqual([(a.name, a.typename) for a in call.out_args],
                         [("prev_state", "uint32_t"), ("state", "uint32_t")])
        self.assertFalse(call.in_handles or call.out_handles or call.varlen)
        text = self._generate("generate_server_c", "ipc_server_generated.c")
        self.assertIn("ipc_handle_conv_warm(", text)
        self.assertIn("ipc_handle_conv_request_warm(", text)

    def test_eye_positions_carry_a_target_time(self):
        # ADR-054 §4.1 / N6 Part 1 (#2144): the client's eye query names the
        # instant the eyes are wanted for (the frame's displayTime, monotonic
        # ns) instead of taking the server's "latest". The EXISTING call grew
        # an in-arg — it is not appended (its enum value is unchanged; the
        # u_git_tag gate pairs client and service, so the wire struct may
        # change shape) and nothing after it moved.
        names = [c.name for c in self.p.calls]
        call = self._call("compositor_get_predicted_eye_positions")
        self.assertEqual([(a.name, a.typename) for a in call.in_args], [("target_ns", "int64_t")])
        self.assertEqual([(a.name, a.typename) for a in call.out_args],
                         [("eye_positions", "struct xrt_eye_positions")])
        self.assertFalse(call.in_handles or call.out_handles or call.varlen)
        self.assertLess(names.index("compositor_get_predicted_eye_positions"),
                        names.index("compositor_predict_frame"))
        text = self._generate("generate_client_c", "ipc_client_generated.c")
        body = self._function_body(text, "ipc_call_compositor_get_predicted_eye_positions(")
        self.assertIn("int64_t target_ns", body)
        self.assertIn("struct xrt_eye_positions *out_eye_positions", body)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    SHARED_DIR = os.path.abspath(sys.argv.pop(1))
    unittest.main()
