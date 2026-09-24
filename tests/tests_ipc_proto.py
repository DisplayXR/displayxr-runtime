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


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    SHARED_DIR = os.path.abspath(sys.argv.pop(1))
    unittest.main()
