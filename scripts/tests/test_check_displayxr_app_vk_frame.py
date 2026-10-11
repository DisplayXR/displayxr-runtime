#!/usr/bin/env python3
"""Unit tests for the INV-4.10 Vulkan frame-submit discipline check in
scripts/check_displayxr_app.py.

Hermetic: each case writes a tiny synthetic app tree to a temp dir and lints it.

The positive shapes are the ones the in-tree cubes shipped with (runtime #1917,
displayxr-demo-modelviewer#170): Android's record_atlas() allocated a command
buffer, submitted, vkQueueWaitIdle'd and freed it every frame; the Windows /
Linux renderers submit with a fence and immediately wait that same fence. On an
Adreno 740 that serialisation parks the GPU at ~1/3 clock. The negative shape is
dxr::VkFrameRing's (displayxr-common common/vk_frame_ring.h): N slots, one
submit per frame, the only wait is on the slot from N frames ago.

    python3 scripts/tests/test_check_displayxr_app_vk_frame.py
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_displayxr_app as lint  # noqa: E402

# The cube_handle_vk_android record_atlas() shape (per-frame alloc + drain + free).
ANDROID_DRAIN = """
bool record_atlas(uint32_t image_idx, const XrView *views, uint32_t view_count)
{
	VkCommandBufferAllocateInfo ai = {};
	ai.commandPool = g_app_cmd_pool;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(g_vk_device, &ai, &cmd) != VK_SUCCESS) {
		return false;
	}
	vkBeginCommandBuffer(cmd, &bi);
	vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
	for (uint32_t v = 0; v < view_count; ++v) {
		vkCmdDraw(cmd, 36, 1, 0, 0);
	}
	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);
	VkResult res = vkQueueSubmit(g_vk_queue, 1, &si, VK_NULL_HANDLE);
	if (res == VK_SUCCESS) {
		vkQueueWaitIdle(g_vk_queue);
	}
	vkFreeCommandBuffers(g_vk_device, g_app_cmd_pool, 1, &cmd);
	return res == VK_SUCCESS;
}
"""

# The cube_*_vk_win / linux RenderScene() shape (submit, then wait that fence).
DESKTOP_WAIT_OWN_FENCE = """
void RenderScene(VkRenderer& renderer, uint32_t imageIndex)
{
    vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &renderer.frameFence);
    vkCmdBeginRenderPass(renderer.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdDrawIndexed(renderer.commandBuffer, 36, 1, 0, 0, 0);
    vkCmdEndRenderPass(renderer.commandBuffer);
    VkResult submitResult = vkQueueSubmit(renderer.graphicsQueue, 1, &submitInfo, renderer.frameFence);
    if (submitResult != VK_SUCCESS) return;
    VkResult waitResult = vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
}
"""

# The sanctioned shape: dxr::VkFrameRing (no drain, one submit, no wait on it).
FRAME_RING = """
#include "vk_frame_ring.h"
static dxr::VkFrameRing g_ring;
void RenderFrame(const XrView *views, uint32_t viewCount)
{
    VkCommandBuffer cmd = g_ring.beginFrame();
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    for (uint32_t i = 0; i < viewCount; ++i) {
        if (i > 0) dxr::VkFrameRing::tileBarrier(cmd);
        vkCmdDraw(cmd, 36, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
    VkResult r = g_ring.endFrame();
    (void)r;
}
"""

# A hand-rolled ring: wait the slot from N frames ago, submit with this slot's
# fence, never wait it. Must NOT fire (b) or the single-fence INFO (d).
HAND_RING = """
void RenderScene(Renderer& r)
{
    Slot& s = r.slots[r.cur];
    vkWaitForFences(r.device, 1, &r.fences[r.cur], VK_TRUE, UINT64_MAX);
    vkResetCommandBuffer(r.cmds[r.cur], 0);
    vkCmdDraw(r.cmds[r.cur], 3, 1, 0, 0);
    vkQueueSubmit(r.queue, 1, &si, r.fences[r.cur]);
    r.cur = (r.cur + 1) % kFramesInFlight;
}
"""

# A one-off texture upload: alloc + submit + drain + free, no draw, no frame
# call, not a render/draw/frame/present name — the allowed exception.
ONE_OFF_UPLOAD = """
static bool UploadTexture(VkDevice device, VkQueue queue, VkCommandPool pool, VkImage img)
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &ai, &cmd);
    vkCmdCopyBufferToImage(cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
    return true;
}
"""

EXEMPT_ON_SUBMIT = ANDROID_DRAIN.replace(
    "VkResult res = vkQueueSubmit(g_vk_queue, 1, &si, VK_NULL_HANDLE);",
    "VkResult res = vkQueueSubmit(g_vk_queue, 1, &si, VK_NULL_HANDLE); "
    "// dxr-lint: allow-blocking-submit capture readback test",
)
EXEMPT_ON_HEADER = DESKTOP_WAIT_OWN_FENCE.replace(
    "void RenderScene(VkRenderer& renderer, uint32_t imageIndex)",
    "void RenderScene(VkRenderer& renderer, uint32_t imageIndex) "
    "// dxr-lint: allow-blocking-submit single-shot screenshot tool",
)

# A per-frame drain in a frame function whose name says so but which records no
# draw itself (it calls helpers) — the name heuristic must still catch it.
NAMED_FRAME_NO_DRAW = """
void PresentFrame(App& app)
{
    RecordEverything(app.cmd);
    vkQueueSubmit(app.queue, 1, &si, VK_NULL_HANDLE);
    vkDeviceWaitIdle(app.device);
}
"""

# A function named ...Renderer (an init function) is NOT judged by name.
INIT_RENDERER = ONE_OFF_UPLOAD.replace("UploadTexture", "InitializeVulkanRenderer")


def lint_tree(files: dict[str, str]) -> list:
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        for rel, text in files.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text)
        findings: list = []
        lint.check_vk_frame_discipline(root, findings)
        return [f for f in findings if f.rule == "INV-4.10"]


def msgs(found):
    return [f"{f.level}:{f.line}:{f.msg.split(' — ')[0]}" for f in found]


class Inv410(unittest.TestCase):
    def test_android_drain_and_alloc_flagged(self):
        got = lint_tree({"src/main/cpp/main.cpp": ANDROID_DRAIN})
        self.assertEqual(len(got), 2, msgs(got))
        self.assertTrue(all(f.level == lint.WARN for f in got))
        joined = " ".join(f.msg for f in got)
        self.assertIn("vkQueueWaitIdle", joined)
        self.assertIn("vkAllocateCommandBuffers + vkFreeCommandBuffers", joined)
        self.assertIn("record_atlas", joined)

    def test_wait_on_just_submitted_fence_flagged(self):
        got = lint_tree({"vk_renderer.cpp": DESKTOP_WAIT_OWN_FENCE})
        self.assertEqual(len(got), 1, msgs(got))
        self.assertEqual(got[0].level, lint.WARN)
        self.assertIn("renderer.frameFence", got[0].msg)
        self.assertEqual(got[0].line, 11)  # the wait AFTER the submit, not the one before

    def test_frame_ring_clean(self):
        self.assertEqual(lint_tree({"main.cpp": FRAME_RING}), [])

    def test_hand_rolled_ring_clean(self):
        self.assertEqual(lint_tree({"main.cpp": HAND_RING}), [])

    def test_one_off_upload_not_flagged(self):
        self.assertEqual(lint_tree({"main.cpp": ONE_OFF_UPLOAD}), [])
        self.assertEqual(lint_tree({"main.cpp": INIT_RENDERER}), [])

    def test_exemption_on_submit_line(self):
        self.assertEqual(lint_tree({"main.cpp": EXEMPT_ON_SUBMIT}), [])

    def test_exemption_on_function_header(self):
        self.assertEqual(lint_tree({"main.cpp": EXEMPT_ON_HEADER}), [])

    def test_frame_name_without_draw_flagged(self):
        got = lint_tree({"main.cpp": NAMED_FRAME_NO_DRAW})
        self.assertEqual(len(got), 1, msgs(got))
        self.assertIn("vkDeviceWaitIdle", got[0].msg)

    def test_single_fence_top_of_frame_is_info(self):
        one_in_flight = DESKTOP_WAIT_OWN_FENCE.replace(
            "    VkResult waitResult = vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);\n", "")
        got = lint_tree({"main.mm": one_in_flight})
        self.assertEqual(len(got), 1, msgs(got))
        self.assertEqual(got[0].level, lint.INFO)
        self.assertIn("one frame in flight", got[0].msg)

    def test_commented_out_drain_ignored(self):
        src = ANDROID_DRAIN.replace("\t\tvkQueueWaitIdle(g_vk_queue);", "\t\t// vkQueueWaitIdle(g_vk_queue);")
        got = lint_tree({"main.cpp": src})
        self.assertEqual(len(got), 1, msgs(got))  # only the alloc/free churn remains
        self.assertIn("vkAllocateCommandBuffers", got[0].msg)

    def test_lazy_alloc_without_free_clean(self):
        # Allocate-once-on-first-use, reset every frame: no churn, no finding.
        src = """
void RenderFrame(App& a)
{
    if (a.cmd == VK_NULL_HANDLE) vkAllocateCommandBuffers(a.device, &ai, &a.cmd);
    vkResetCommandBuffer(a.cmd, 0);
    vkCmdDraw(a.cmd, 3, 1, 0, 0);
    vkQueueSubmit(a.queue, 1, &si, a.fences[a.slot]);
}
"""
        self.assertEqual(lint_tree({"main.cpp": src}), [])

    def test_headers_and_jvm_not_scanned(self):
        self.assertEqual(lint_tree({"renderer.h": ANDROID_DRAIN, "Main.java": ANDROID_DRAIN}), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
