// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The native swapchains' acquire/wait/release lifecycle (#1504, #1513).
 *
 * CTS `Swapchains` ("Acquiring all swapchain images") and `SwapchainsAcquire`
 * both acquire EVERY image before releasing any, then run repeated full
 * acquire->wait->release passes. On `-Graphics vulkan` the second acquire
 * returned `XR_ERROR_RUNTIME_FAILURE`: the compositor picked the next index as
 * `(last_released_index + 1) % image_count`, and with nothing released yet that
 * handed index 0 out twice, which the state tracker rejected as a "non-ready
 * image". #1513 found the identical arithmetic in the OpenGL and Metal
 * compositors, so the bookkeeping now lives in util/ and all three share it.
 *
 * These cases drive the REAL bookkeeping
 * (@ref comp_swapchain_ring.h, compiled into every native compositor) through
 * BOTH state-tracker sequences, because they differ in the way that made the
 * defect visible:
 *
 * - Vulkan: `oxr_swapchain_vk.c` sets `WAIT_IN_ACQUIRE`, so
 *   `xrAcquireSwapchainImage` acquires AND waits before returning, leaving the
 *   compositor no single "currently acquired" slot to reason from.
 * - OpenGL / Metal: `oxr_swapchain_gl.c` and `oxr_swapchain_metal.c` take the
 *   plain `oxr_swapchain.c` path, so the backend sees acquire, then wait, then
 *   release as three separate calls — and never a release without a preceding
 *   wait, because `oxr_swapchain_common_release` refuses when `inflight.index`
 *   is unset. Both orders must work against one ring.
 *
 * No graphics device is needed — the ring is pure index bookkeeping, and
 * creating a headless device in ctest would make the check depend on an
 * installed ICD.
 */

#include "util/comp_swapchain_ring.h"

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
	explicit OxrVkSwapchain(uint32_t image_count, bool is_static = false)
	    : m_image_count(image_count), m_is_static(is_static), m_ready(image_count, true)
	{
		comp_swapchain_ring_init(&m_ring, image_count);
	}

	//! xrAcquireSwapchainImage on the Vulkan path (acquires, then waits).
	Res
	acquire(uint32_t *out_index)
	{
		// oxr_swapchain_common_acquire: bound the outstanding count first.
		if (m_acquired_num >= m_image_count) {
			return Res::CallOrderInvalid;
		}

		// oxr_swapchain.c:192 -- a static swapchain may be acquired once, ever.
		if (m_is_static && (m_released_yes || !m_ready[0])) {
			return Res::CallOrderInvalid;
		}

		uint32_t index = UINT32_MAX;
		if (comp_swapchain_ring_acquire(&m_ring, &index) != XRT_SUCCESS) {
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
		if (comp_swapchain_ring_wait(&m_ring, index) != XRT_SUCCESS) {
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

		if (comp_swapchain_ring_release(&m_ring, index) != XRT_SUCCESS) {
			return Res::RuntimeFailure;
		}

		m_acquired_num--;
		m_ready[index] = true;
		m_released_yes = true;
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
		return comp_swapchain_ring_outstanding(&m_ring);
	}

private:
	uint32_t m_image_count;
	bool m_is_static = false;
	bool m_released_yes = false;
	uint32_t m_acquired_num = 0;
	int32_t m_inflight = -1;
	std::vector<bool> m_ready;
	std::vector<uint32_t> m_fifo;
	struct comp_swapchain_ring m_ring{};
};

/*!
 * The OTHER state-tracker sequence: the plain `oxr_swapchain.c` path that
 * OpenGL (`oxr_swapchain_gl.c`) and Metal (`oxr_swapchain_metal.c`) take. No
 * WAIT_IN_ACQUIRE — the backend sees three separate calls, so the ring must
 * survive `acquire` … `acquire` … `wait` … `release` as well as the Vulkan
 * shape. Modelled on @ref oxr_swapchain_common_acquire, @ref
 * oxr_swapchain_verify_wait_state + @ref oxr_swapchain_common_wait and @ref
 * implicit_release_image + @ref oxr_swapchain_common_release.
 */
class OxrGlSwapchain
{
public:
	explicit OxrGlSwapchain(uint32_t image_count, bool is_static = false)
	    : m_image_count(image_count), m_is_static(is_static), m_ready(image_count, true)
	{
		comp_swapchain_ring_init(&m_ring, image_count);
	}

	//! xrAcquireSwapchainImage: the backend acquire ONLY, no wait.
	Res
	acquire(uint32_t *out_index)
	{
		if (m_acquired_num >= m_image_count) {
			return Res::CallOrderInvalid;
		}
		if (m_is_static && (m_released_yes || !m_ready[0])) {
			return Res::CallOrderInvalid;
		}

		uint32_t index = UINT32_MAX;
		if (comp_swapchain_ring_acquire(&m_ring, &index) != XRT_SUCCESS) {
			return Res::RuntimeFailure;
		}

		if (index >= m_image_count || !m_ready[index]) {
			return Res::RuntimeFailure;
		}
		m_ready[index] = false;

		m_acquired_num++;
		m_fifo.push_back(index);

		*out_index = index;
		return Res::Success;
	}

	//! xrWaitSwapchainImage: peek the FIFO, call the backend wait, pop.
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

		uint32_t index = m_fifo.front();
		if (comp_swapchain_ring_wait(&m_ring, index) != XRT_SUCCESS) {
			return Res::RuntimeFailure;
		}
		m_fifo.erase(m_fifo.begin());
		m_inflight = static_cast<int32_t>(index);
		return Res::Success;
	}

	//! xrReleaseSwapchainImage.
	Res
	release()
	{
		if (m_inflight < 0) {
			// implicit_release_image: "No swapchain images waited on".
			return Res::CallOrderInvalid;
		}
		uint32_t index = static_cast<uint32_t>(m_inflight);
		m_inflight = -1;

		if (comp_swapchain_ring_release(&m_ring, index) != XRT_SUCCESS) {
			return Res::RuntimeFailure;
		}

		m_acquired_num--;
		m_ready[index] = true;
		m_released_yes = true;
		return Res::Success;
	}

	uint32_t
	outstanding() const
	{
		return comp_swapchain_ring_outstanding(&m_ring);
	}

private:
	uint32_t m_image_count;
	bool m_is_static = false;
	bool m_released_yes = false;
	uint32_t m_acquired_num = 0;
	int32_t m_inflight = -1;
	std::vector<bool> m_ready;
	std::vector<uint32_t> m_fifo;
	struct comp_swapchain_ring m_ring{};
};

} // namespace


TEST_CASE("swapchain_ring(vk): acquires every image before any release")
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

TEST_CASE("swapchain_ring(vk): repeated full acquire/wait/release passes")
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

TEST_CASE("swapchain_ring(vk): one-at-a-time steady state round-robins")
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

TEST_CASE("swapchain_ring(vk): partial overlap, the real double-buffered app")
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

TEST_CASE("swapchain_ring(vk): rejects out-of-cycle wait and release")
{
	// The ring's own guards, below the state tracker: a wait on an image that
	// was never acquired, and a release of one that was never waited on, must
	// be refused rather than corrupt the free pool. XRT_ERROR_NO_IMAGE_AVAILABLE
	// and not XRT_ERROR_IPC_FAILURE, which would mark the session lost.
	struct comp_swapchain_ring ring{};
	comp_swapchain_ring_init(&ring, 3);

	REQUIRE(comp_swapchain_ring_wait(&ring, 0) == XRT_ERROR_NO_IMAGE_AVAILABLE);
	REQUIRE(comp_swapchain_ring_release(&ring, 0) == XRT_ERROR_NO_IMAGE_AVAILABLE);

	uint32_t index = UINT32_MAX;
	REQUIRE(comp_swapchain_ring_acquire(&ring, &index) == XRT_SUCCESS);
	REQUIRE(index == 0);

	// Out of range, and a release before the wait.
	REQUIRE(comp_swapchain_ring_wait(&ring, 99) == XRT_ERROR_NO_IMAGE_AVAILABLE);
	REQUIRE(comp_swapchain_ring_release(&ring, 0) == XRT_ERROR_NO_IMAGE_AVAILABLE);

	REQUIRE(comp_swapchain_ring_wait(&ring, 0) == XRT_SUCCESS);
	// Waiting twice on the same image is no longer legal either.
	REQUIRE(comp_swapchain_ring_wait(&ring, 0) == XRT_ERROR_NO_IMAGE_AVAILABLE);
	REQUIRE(comp_swapchain_ring_release(&ring, 0) == XRT_SUCCESS);
	REQUIRE(comp_swapchain_ring_outstanding(&ring) == 0);
}

TEST_CASE("comp_swapchain_image_count: static swapchains get exactly one image")
{
	// The second half of #1504. The compositor hardcoded 3, so
	// xrEnumerateSwapchainImages promised three images for a
	// XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT swapchain that the state tracker's
	// single-acquire rule would only ever hand out one of -- CTS
	// test_Swapchains.cpp:196, "Non-default create flags", one failure per
	// format. D3D11 and D3D12 already did this; Vulkan did not.
	REQUIRE(comp_swapchain_image_count(XRT_SWAPCHAIN_CREATE_STATIC_IMAGE, 3) == 1);

	// Every other flag combination stays triple buffered.
	REQUIRE(comp_swapchain_image_count((enum xrt_swapchain_create_flags)0, 3) == 3);
	REQUIRE(comp_swapchain_image_count(XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT, 3) == 3);
	REQUIRE(comp_swapchain_image_count((enum xrt_swapchain_create_flags)(XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT |
	                                                                     XRT_SWAPCHAIN_CREATE_STATIC_IMAGE),
	                                   3) == 1);

	// Never past the fixed-size image/memory/view arrays.
	REQUIRE(comp_swapchain_image_count((enum xrt_swapchain_create_flags)0, 3) <= COMP_SWAPCHAIN_MAX_IMAGES);
}

TEST_CASE("swapchain_ring(vk): the static swapchain's one image, acquired once")
{
	// CTS test_Swapchains.cpp:196 then :229-232 for a static swapchain: the
	// acquire-all loop runs exactly once, the extra acquire is refused, the one
	// image waits and releases, and the post-release acquire is refused again
	// because a static swapchain may be acquired once for its whole lifetime.
	const uint32_t image_count = comp_swapchain_image_count(XRT_SWAPCHAIN_CREATE_STATIC_IMAGE, 3);
	REQUIRE(image_count == 1);

	OxrVkSwapchain sc(image_count, /* is_static */ true);

	uint32_t index = UINT32_MAX;
	REQUIRE(sc.acquire(&index) == Res::Success);
	REQUIRE(index == 0);

	// "An extra acquire once we acquired all should be XR_ERROR_CALL_ORDER_INVALID"
	uint32_t extra = UINT32_MAX;
	REQUIRE(sc.acquire(&extra) == Res::CallOrderInvalid);

	REQUIRE(sc.wait() == Res::Success);
	REQUIRE(sc.wait() == Res::CallOrderInvalid);
	REQUIRE(sc.release() == Res::Success);
	REQUIRE(sc.outstanding() == 0);

	// ":229 In the case of XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT we must only
	// allow a single acquire" -- the image is free again, but the swapchain is
	// spent.
	REQUIRE(sc.acquire(&extra) == Res::CallOrderInvalid);
}

TEST_CASE("swapchain_ring(vk): the pre-fix rule really did hand out a duplicate")
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
	struct comp_swapchain_ring ring{};
	comp_swapchain_ring_init(&ring, image_count);
	uint32_t a = UINT32_MAX, b = UINT32_MAX;
	REQUIRE(comp_swapchain_ring_acquire(&ring, &a) == XRT_SUCCESS);
	REQUIRE(comp_swapchain_ring_acquire(&ring, &b) == XRT_SUCCESS);
	REQUIRE(a == 0);
	REQUIRE(b != a);
}

TEST_CASE("swapchain_ring(vk): exhaustion is NO_IMAGE_AVAILABLE, never session-lost")
{
	struct comp_swapchain_ring ring{};
	comp_swapchain_ring_init(&ring, 2);

	uint32_t a = UINT32_MAX, b = UINT32_MAX, c = UINT32_MAX;
	REQUIRE(comp_swapchain_ring_acquire(&ring, &a) == XRT_SUCCESS);
	REQUIRE(comp_swapchain_ring_acquire(&ring, &b) == XRT_SUCCESS);
	REQUIRE(a != b);
	REQUIRE(comp_swapchain_ring_acquire(&ring, &c) == XRT_ERROR_NO_IMAGE_AVAILABLE);
	REQUIRE(comp_swapchain_ring_acquire(&ring, &c) != XRT_ERROR_IPC_FAILURE);
}


/*
 *
 * The OpenGL / Metal state-tracker sequence (#1513).
 *
 */

TEST_CASE("swapchain_ring(gl): acquires every image before any release")
{
	// The same CTS step ("Acquiring all swapchain images") on -Graphics opengl,
	// but reaching the backend as acquire-only calls. This is what
	// gl_swapchain_acquire_image got wrong in exactly the Vulkan way: nothing
	// released yet, so (last_released_index + 1) % image_count repeated index 1.
	const uint32_t image_count = GENERATE(1u, 2u, 3u, 4u, 8u);
	CAPTURE(image_count);

	OxrGlSwapchain sc(image_count);

	std::vector<uint32_t> indices;
	for (uint32_t i = 0; i < image_count; ++i) {
		CAPTURE(i);
		uint32_t index = UINT32_MAX;
		REQUIRE(sc.acquire(&index) == Res::Success);
		REQUIRE(index < image_count);
		for (uint32_t prev : indices) {
			REQUIRE(index != prev);
		}
		indices.push_back(index);
	}
	REQUIRE(sc.outstanding() == image_count);

	uint32_t extra = UINT32_MAX;
	REQUIRE(sc.acquire(&extra) == Res::CallOrderInvalid);

	// Wait and release in turn -- FIFO order, so the same order they came out.
	for (uint32_t i = 0; i < image_count; ++i) {
		CAPTURE(i);
		REQUIRE(sc.wait() == Res::Success);
		REQUIRE(sc.wait() == Res::CallOrderInvalid);
		REQUIRE(sc.release() == Res::Success);
	}
	REQUIRE(sc.outstanding() == 0);
}

TEST_CASE("swapchain_ring(gl): repeated full acquire/wait/release passes")
{
	// CTS SwapchainsAcquire on -Graphics opengl: ten passes of acquire-all then
	// wait/release-all. On main this failed at i == 1 of the first pass.
	const uint32_t image_count = GENERATE(1u, 2u, 3u, 8u);
	CAPTURE(image_count);

	OxrGlSwapchain sc(image_count);

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

TEST_CASE("swapchain_ring(gl): one-at-a-time steady state round-robins")
{
	// The ordinary GL app loop, which worked before and must keep working:
	// one image per frame, and consecutive frames must not reuse the same one.
	OxrGlSwapchain sc(3);

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

TEST_CASE("swapchain_ring(gl): acquire, acquire, wait, release interleave")
{
	// The shape only the non-WAIT_IN_ACQUIRE path can produce: two images
	// ACQUIRED at once with neither waited. The ring has to hold two images in
	// the ACQUIRED state and then accept the wait for the FIFO-front one.
	OxrGlSwapchain sc(3);

	uint32_t a = UINT32_MAX, b = UINT32_MAX, c = UINT32_MAX;
	REQUIRE(sc.acquire(&a) == Res::Success);
	REQUIRE(sc.acquire(&b) == Res::Success);
	REQUIRE(a != b);
	REQUIRE(sc.outstanding() == 2);

	REQUIRE(sc.wait() == Res::Success); // a
	REQUIRE(sc.release() == Res::Success);

	REQUIRE(sc.acquire(&c) == Res::Success);
	REQUIRE(c != b);

	REQUIRE(sc.wait() == Res::Success); // b
	REQUIRE(sc.release() == Res::Success);
	REQUIRE(sc.wait() == Res::Success); // c
	REQUIRE(sc.release() == Res::Success);
	REQUIRE(sc.outstanding() == 0);
}

TEST_CASE("swapchain_ring(gl): release without a wait never reaches the ring")
{
	// Why the ring may stay strict (WAITED-only release) for the OpenGL and
	// Metal backends too, even though their old release_image accepted
	// anything: oxr_swapchain.c's implicit_release_image refuses with
	// CALL_ORDER_INVALID while inflight.index is unset, so a release without a
	// preceding wait never gets as far as the backend.
	OxrGlSwapchain sc(3);

	REQUIRE(sc.release() == Res::CallOrderInvalid);

	uint32_t a = UINT32_MAX;
	REQUIRE(sc.acquire(&a) == Res::Success);
	REQUIRE(sc.release() == Res::CallOrderInvalid); // acquired, not waited
	REQUIRE(sc.outstanding() == 1);

	REQUIRE(sc.wait() == Res::Success);
	REQUIRE(sc.release() == Res::Success);
	REQUIRE(sc.outstanding() == 0);

	// And directly at the ring: an ACQUIRED image is not releasable.
	struct comp_swapchain_ring ring{};
	comp_swapchain_ring_init(&ring, 3);
	uint32_t index = UINT32_MAX;
	REQUIRE(comp_swapchain_ring_acquire(&ring, &index) == XRT_SUCCESS);
	REQUIRE(comp_swapchain_ring_release(&ring, index) == XRT_ERROR_NO_IMAGE_AVAILABLE);
	REQUIRE(comp_swapchain_ring_outstanding(&ring) == 1);
}

TEST_CASE("swapchain_ring(gl): the static swapchain's one image, acquired once")
{
	// CTS Swapchains "Non-default create flags" on -Graphics opengl. The GL and
	// Metal compositors hardcoded image_count = 3 at BOTH the create site and
	// get_swapchain_create_properties, so a XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT
	// swapchain advertised three images the state tracker would hand out one
	// of -- CALL_ORDER_INVALID on the second acquire, once per format.
	const uint32_t image_count = comp_swapchain_image_count(XRT_SWAPCHAIN_CREATE_STATIC_IMAGE, 3);
	REQUIRE(image_count == 1);

	OxrGlSwapchain sc(image_count, /* is_static */ true);

	uint32_t index = UINT32_MAX;
	REQUIRE(sc.acquire(&index) == Res::Success);
	REQUIRE(index == 0);

	uint32_t extra = UINT32_MAX;
	REQUIRE(sc.acquire(&extra) == Res::CallOrderInvalid);

	REQUIRE(sc.wait() == Res::Success);
	REQUIRE(sc.wait() == Res::CallOrderInvalid);
	REQUIRE(sc.release() == Res::Success);
	REQUIRE(sc.outstanding() == 0);

	// A static swapchain is spent after one acquire.
	REQUIRE(sc.acquire(&extra) == Res::CallOrderInvalid);
}

TEST_CASE("comp_swapchain_image_count: the default count is the caller's, and is clamped")
{
	// #1513 lifted the helper out of vk_native and gave it the backend's own
	// default, because nothing guarantees every compositor triple buffers
	// forever. The static rule outranks the default, and the result can never
	// exceed the fixed-size image arrays nor be zero.
	REQUIRE(comp_swapchain_image_count((enum xrt_swapchain_create_flags)0, 2) == 2);
	REQUIRE(comp_swapchain_image_count((enum xrt_swapchain_create_flags)0, 8) == 8);
	REQUIRE(comp_swapchain_image_count(XRT_SWAPCHAIN_CREATE_STATIC_IMAGE, 8) == 1);
	REQUIRE(comp_swapchain_image_count((enum xrt_swapchain_create_flags)0, 99) == COMP_SWAPCHAIN_MAX_IMAGES);
	REQUIRE(comp_swapchain_image_count((enum xrt_swapchain_create_flags)0, 0) == 1);

	// The bound is the one the xrt_swapchain_* images[] arrays are sized to.
	REQUIRE(COMP_SWAPCHAIN_MAX_IMAGES == XRT_MAX_SWAPCHAIN_IMAGES);
}

TEST_CASE("swapchain_ring(gl): the pre-fix GL rule really did hand out a duplicate")
{
	// Negative control for the OpenGL/Metal half, so these cases cannot quietly
	// pass against a reverted fix. This is comp_gl_compositor.cpp:1733 and
	// comp_metal_compositor.m:1130 verbatim -- next = (last_released_index + 1)
	// % image_count, seeded to 0 (GL) so the first acquire yields 1 and the
	// second yields 1 again.
	const uint32_t image_count = 3;
	uint32_t last_released_index = 0; // what gl_compositor_create_swapchain set

	uint32_t first = (last_released_index + 1) % image_count;
	uint32_t second = (last_released_index + 1) % image_count; // no release between
	REQUIRE(first == 1);
	REQUIRE(second == first); // <-- the bug

	struct comp_swapchain_ring ring{};
	comp_swapchain_ring_init(&ring, image_count);
	uint32_t a = UINT32_MAX, b = UINT32_MAX;
	REQUIRE(comp_swapchain_ring_acquire(&ring, &a) == XRT_SUCCESS);
	REQUIRE(comp_swapchain_ring_acquire(&ring, &b) == XRT_SUCCESS);
	REQUIRE(a == 0);
	REQUIRE(b != a);
}
