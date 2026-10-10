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
        # dashboard phase 7's re-probe request, phase 8's launch settings).
        later = ("system_request_display_reprobe", "system_reload_service_config", "system_workspace_launch",
                 "system_workspace_hotkey_suspend", "weave_get_segments")
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
        # 8's three calls and #1884's weave_get_segments follow it).
        names = [c.name for c in self.p.calls]
        self.assertEqual(names[-5], "system_request_display_reprobe")
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
        # weave_get_segments follows them.
        names = [c.name for c in self.p.calls]
        self.assertEqual(names[-4:-1], ["system_reload_service_config", "system_workspace_launch",
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
        # own call, APPENDED at the end — weave_submit's reply is untouched, so
        # a caller that does not chain XrWeaveOutputRectPartsDXR sends exactly
        # the pre-v19 messages. The table is the per-segment views' struct.
        names = [c.name for c in self.p.calls]
        self.assertEqual(names[-1], "weave_get_segments")
        call = self._call("weave_get_segments")
        self.assertEqual(call.in_args, [])
        self.assertEqual([(a.name, a.typename) for a in call.out_args],
                         [("metrics", "struct xrt_segment_metrics")])
        self.assertFalse(call.in_handles or call.out_handles or call.varlen)
        sub = [a.name for a in self._call("weave_submit").out_args]
        self.assertEqual(sub, ["have_output", "width", "height", "fence_value", "eyes"])
        text = self._generate("generate_server_c", "ipc_server_generated.c")
        self.assertIn("ipc_handle_weave_get_segments(", text)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    SHARED_DIR = os.path.abspath(sys.argv.pop(1))
    unittest.main()
