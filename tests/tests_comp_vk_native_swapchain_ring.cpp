// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Vulkan native swapchain's acquire/wait/release lifecycle (#1504).
 *
 * CTS `Swapchains` ("Acquiring all swapchain images") and `SwapchainsAcquire`
 * both acquire EVERY image before releasing any, then run repeated full
 * acquire->wait->release passes. On `-Graphics vulkan` the second acquire
 * returned `XR_ERROR_RUNTIME_FAILURE`: the compositor picked the next index as
 * `(last_released_index + 1) % image_count`, and with nothing released yet that
 * handed index 0 out twice, which the state tracker rejected as a "non-ready
 * image".
 *
 * These cases drive the REAL bookkeeping
 * (@ref comp_vk_native_swapchain_ring.h, compiled into the compositor) through
 * the state tracker's Vulkan sequence, which is the part that made the defect
 * visible: `oxr_swapchain_vk.c` sets `WAIT_IN_ACQUIRE`, so
 * `xrAcquireSwapchainImage` acquires AND waits before returning, leaving the
 * compositor no single "currently acquired" slot to reason from. No VkDevice is
 * needed — the ring is pure index bookkeeping, and creating a headless device
 * in ctest would make the check depend on an ICD being installed.
 */

#include "vk_native/comp_vk_native_swapchain_ring.h"

#include "catch_amalgamated.hpp"

#include <vector>

namespace {

//! What `xrAcquire/Wait/ReleaseSwapchainImage` returned, in OpenXR terms.
enum class Res
{
	Success,
	CallOrderInvalid,
	RuntimeFailure,
};

/*!
 * A faithful stand-in for the state tracker's Vulkan swapchain path:
 * @ref oxr_swapchain_common_acquire + @ref vk_implicit_acquire_image +
 * @ref vk_implicit_wait_image + @ref oxr_swapchain_common_release, calling the
 * production ring underneath. Everything above the ring is copied from the
 * state tracker; the ring is the code under test.
 */
class OxrVkSwapchain
{
public:
	explicit OxrVkSwapchain(uint32_t image_count) : m_image_count(image_count), m_ready(image_count, true)
	{
		comp_vk_native_swapchain_ring_init(&m_ring, image_count);
	}

	//! xrAcquireSwapchainImage on the Vulkan path (acquires, then waits).
	Res
	acquire(uint32_t *out_index)
	{
		// oxr_swapchain_common_acquire: bound the outstanding count first.
		if (m_acquired_num >= m_image_count) {
			return Res::CallOrderInvalid;
		}

		uint32_t index = UINT32_MAX;
		if (comp_vk_native_swapchain_ring_acquire(&m_ring, &index) != XRT_SUCCESS) {
			return Res::RuntimeFailure;
		}

		// The check that actually fired on main: a re-handed-out index is not
		// READY, because the app still holds it.
		if (index >= m_image_count || !m_ready[index]) {
			return Res::RuntimeFailure;
		}
		m_ready[index] = false;

		m_acquired_num++;
		m_fifo.push_back(index);

		// WAIT_IN_ACQUIRE: oxr_swapchain_vk.c waits here, not in xrWait.
		if (comp_vk_native_swapchain_ring_wait(&m_ring, index) != XRT_SUCCESS) {
			return Res::RuntimeFailure;
		}

		*out_index = index;
		return Res::Success;
	}

	//! xrWaitSwapchainImage on the Vulkan path (pops the acquire FIFO only).
	Res
	wait()
	{
		if (m_inflight >= 0) {
			// oxr_swapchain_verify_wait_state: one image in flight at a time.
			return Res::CallOrderInvalid;
		}
		if (m_fifo.empty()) {
			return Res::CallOrderInvalid;
		}
		m_inflight = static_cast<int32_t>(m_fifo.front());
		m_fifo.erase(m_fifo.begin());
		return Res::Success;
	}

	//! xrReleaseSwapchainImage.
	Res
	release()
	{
		if (m_inflight < 0) {
			return Res::CallOrderInvalid;
		}
		uint32_t index = static_cast<uint32_t>(m_inflight);
		m_inflight = -1;

		if (comp_vk_native_swapchain_ring_release(&m_ring, index) != XRT_SUCCESS) {
			return Res::RuntimeFailure;
		}

		m_acquired_num--;
		m_ready[index] = true;
		return Res::Success;
	}

	uint32_t
	image_count() const
	{
		return m_image_count;
	}

	uint32_t
	outstanding() const
	{
		return comp_vk_native_swapchain_ring_outstanding(&m_ring);
	}

private:
	uint32_t m_image_count;
	uint32_t m_acquired_num = 0;
	int32_t m_inflight = -1;
	std::vector<bool> m_ready;
	std::vector<uint32_t> m_fifo;
	struct comp_vk_native_swapchain_ring m_ring{};
};

} // namespace


TEST_CASE("vk_native_swapchain_ring: acquires every image before any release")
{
	// CTS test_Swapchains.cpp:196 "Acquiring all swapchain images", and the
	// first half of each SwapchainsAcquire pass. 3 is what the compositor
	// creates; sweep the neighbours so the fix is not a 3-specific accident.
	const uint32_t image_count = GENERATE(1u, 2u, 3u, 4u, 8u);
	CAPTURE(image_count);

	OxrVkSwapchain sc(image_count);

	std::vector<uint32_t> indices;
	for (uint32_t i = 0; i < image_count; ++i) {
		CAPTURE(i);
		uint32_t index = UINT32_MAX;
		REQUIRE(sc.acquire(&index) == Res::Success);
		REQUIRE(index < image_count);
		// Every acquire must yield a DISTINCT index while all are outstanding.
		for (uint32_t prev : indices) {
			REQUIRE(index != prev);
		}
		indices.push_back(index);
	}
	REQUIRE(sc.outstanding() == image_count);

	// "An extra acquire once we acquired all should be XR_ERROR_CALL_ORDER_INVALID"
	uint32_t extra = UINT32_MAX;
	REQUIRE(sc.acquire(&extra) == Res::CallOrderInvalid);

	// "Wait then release all the images in turn"
	for (uint32_t i = 0; i < image_count; ++i) {
		CAPTURE(i);
		REQUIRE(sc.wait() == Res::Success);
		// "Another wait should fail with XR_ERROR_CALL_ORDER_INVALID."
		REQUIRE(sc.wait() == Res::CallOrderInvalid);
		REQUIRE(sc.release() == Res::Success);
	}
	REQUIRE(sc.outstanding() == 0);
}

TEST_CASE("vk_native_swapchain_ring: repeated full acquire/wait/release passes")
{
	// CTS SwapchainsAcquire, test_Swapchains.cpp:751 — ten passes, each
	// acquiring all N and then waiting/releasing all N. On main this failed at
	// i == 1 of the very first pass.
	const uint32_t image_count = GENERATE(1u, 2u, 3u, 8u);
	CAPTURE(image_count);

	OxrVkSwapchain sc(image_count);

	for (int pass = 0; pass < 10; ++pass) {
		CAPTURE(pass);

		std::vector<uint32_t> indices;
		for (uint32_t i = 0; i < image_count; ++i) {
			CAPTURE(i);
			uint32_t index = UINT32_MAX;
			REQUIRE(sc.acquire(&index) == Res::Success);
			for (uint32_t prev : indices) {
				REQUIRE(index != prev);
			}
			indices.push_back(index);
		}

		for (uint32_t i = 0; i < image_count; ++i) {
			CAPTURE(i);
			REQUIRE(sc.wait() == Res::Success);
			REQUIRE(sc.release() == Res::Success);
		}
		REQUIRE(sc.outstanding() == 0);
	}
}

TEST_CASE("vk_native_swapchain_ring: one-at-a-time steady state round-robins")
{
	// The normal app loop, which worked before and must keep working: each
	// frame acquires exactly one image, and consecutive frames must not reuse
	// the same image (that is what triple buffering buys).
	OxrVkSwapchain sc(3);

	uint32_t previous = UINT32_MAX;
	for (int frame = 0; frame < 12; ++frame) {
		CAPTURE(frame);
		uint32_t index = UINT32_MAX;
		REQUIRE(sc.acquire(&index) == Res::Success);
		REQUIRE(index != previous);
		REQUIRE(sc.wait() == Res::Success);
		REQUIRE(sc.release() == Res::Success);
		previous = index;
	}
}

TEST_CASE("vk_native_swapchain_ring: partial overlap, the real double-buffered app")
{
	// Acquire two, release one, acquire another — the shape a pipelined app
	// actually produces, and the one a last-released-index scheme gets wrong
	// even when it never fills the ring.
	OxrVkSwapchain sc(3);

	uint32_t a = UINT32_MAX, b = UINT32_MAX, c = UINT32_MAX;
	REQUIRE(sc.acquire(&a) == Res::Success);
	REQUIRE(sc.acquire(&b) == Res::Success);
	REQUIRE(a != b);

	REQUIRE(sc.wait() == Res::Success); // a
	REQUIRE(sc.release() == Res::Success);

	REQUIRE(sc.acquire(&c) == Res::Success);
	REQUIRE(c != b);
	REQUIRE(sc.outstanding() == 2);

	REQUIRE(sc.wait() == Res::Success); // b
	REQUIRE(sc.release() == Res::Success);
	REQUIRE(sc.wait() == Res::Success); // c
	REQUIRE(sc.release() == Res::Success);
	REQUIRE(sc.outstanding() == 0);
}

TEST_CASE("vk_native_swapchain_ring: rejects out-of-cycle wait and release")
{
	// The ring's own guards, below the state tracker: a wait on an image that
	// was never acquired, and a release of one that was never waited on, must
	// be refused rather than corrupt the free pool. XRT_ERROR_NO_IMAGE_AVAILABLE
	// and not XRT_ERROR_IPC_FAILURE, which would mark the session lost.
	struct comp_vk_native_swapchain_ring ring{};
	comp_vk_native_swapchain_ring_init(&ring, 3);

	REQUIRE(comp_vk_native_swapchain_ring_wait(&ring, 0) == XRT_ERROR_NO_IMAGE_AVAILABLE);
	REQUIRE(comp_vk_native_swapchain_ring_release(&ring, 0) == XRT_ERROR_NO_IMAGE_AVAILABLE);

	uint32_t index = UINT32_MAX;
	REQUIRE(comp_vk_native_swapchain_ring_acquire(&ring, &index) == XRT_SUCCESS);
	REQUIRE(index == 0);

	// Out of range, and a release before the wait.
	REQUIRE(comp_vk_native_swapchain_ring_wait(&ring, 99) == XRT_ERROR_NO_IMAGE_AVAILABLE);
	REQUIRE(comp_vk_native_swapchain_ring_release(&ring, 0) == XRT_ERROR_NO_IMAGE_AVAILABLE);

	REQUIRE(comp_vk_native_swapchain_ring_wait(&ring, 0) == XRT_SUCCESS);
	// Waiting twice on the same image is no longer legal either.
	REQUIRE(comp_vk_native_swapchain_ring_wait(&ring, 0) == XRT_ERROR_NO_IMAGE_AVAILABLE);
	REQUIRE(comp_vk_native_swapchain_ring_release(&ring, 0) == XRT_SUCCESS);
	REQUIRE(comp_vk_native_swapchain_ring_outstanding(&ring) == 0);
}

TEST_CASE("vk_native_swapchain_ring: the pre-fix rule really did hand out a duplicate")
{
	// Negative control, so the cases above cannot quietly pass against a
	// reverted fix. This is the exact arithmetic the compositor used before
	// #1504 -- next index = (last_released_index + 1) % image_count, seeded to
	// image_count - 1 so the first acquire yields 0 -- and it is enough on its
	// own to see the defect: with nothing released, two acquires in a row give
	// the SAME index, which the state tracker then rejects as a non-ready image
	// and reports as XR_ERROR_RUNTIME_FAILURE.
	const uint32_t image_count = 3;
	uint32_t last_released_index = image_count - 1;

	uint32_t first = (last_released_index + 1) % image_count;
	uint32_t second = (last_released_index + 1) % image_count; // no release between
	REQUIRE(first == 0);
	REQUIRE(second == first); // <-- the bug

	// The fixed ring, same sequence, hands out two different images.
	struct comp_vk_native_swapchain_ring ring{};
	comp_vk_native_swapchain_ring_init(&ring, image_count);
	uint32_t a = UINT32_MAX, b = UINT32_MAX;
	REQUIRE(comp_vk_native_swapchain_ring_acquire(&ring, &a) == XRT_SUCCESS);
	REQUIRE(comp_vk_native_swapchain_ring_acquire(&ring, &b) == XRT_SUCCESS);
	REQUIRE(a == 0);
	REQUIRE(b != a);
}

TEST_CASE("vk_native_swapchain_ring: exhaustion is NO_IMAGE_AVAILABLE, never session-lost")
{
	struct comp_vk_native_swapchain_ring ring{};
	comp_vk_native_swapchain_ring_init(&ring, 2);

	uint32_t a = UINT32_MAX, b = UINT32_MAX, c = UINT32_MAX;
	REQUIRE(comp_vk_native_swapchain_ring_acquire(&ring, &a) == XRT_SUCCESS);
	REQUIRE(comp_vk_native_swapchain_ring_acquire(&ring, &b) == XRT_SUCCESS);
	REQUIRE(a != b);
	REQUIRE(comp_vk_native_swapchain_ring_acquire(&ring, &c) == XRT_ERROR_NO_IMAGE_AVAILABLE);
	REQUIRE(comp_vk_native_swapchain_ring_acquire(&ring, &c) != XRT_ERROR_IPC_FAILURE);
}
