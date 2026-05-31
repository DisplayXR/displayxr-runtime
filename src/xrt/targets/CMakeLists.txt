# Copyright 2019-2020, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

######
# This is where we collect all of the pieces from the different parts of
# the source tree and build a complete driver or integration part.

add_subdirectory(common)

if(XRT_FEATURE_OPENXR)
	add_subdirectory(openxr)
endif()

if(XRT_MODULE_CLI)
	add_subdirectory(cli)
endif()

if(XRT_MODULE_CONTROL_PANEL)
	add_subdirectory(control_panel)
endif()

if(XRT_FEATURE_SERVICE AND XRT_FEATURE_OPENXR)
	if(ANDROID)
		add_subdirectory(service-lib)
	else()
		add_subdirectory(service)
	endif()
endif()

# DisplayXR Windows components
if(WIN32)
	# (The standalone DisplayXRSwitcher was retired in #378 — its
	# ActiveRuntime-switching function moved into `displayxr-cli runtime
	# activate` and the Control Panel.)

	# DisplayXR Shell (workspace controller) source has moved to
	# DisplayXR/displayxr-shell-pvt and ships from there. The runtime
	# discovers any installed workspace controller via the registry
	# contract; see docs/specs/runtime/workspace-controller-registration.md.

	# DisplayXR WebXR bridge host (metadata sideband for Chrome's native WebXR)
	add_subdirectory(webxr_bridge)

	# Strip any bundled vulkan-1.dll from the install bin dir. The runtime +
	# service /DELAYLOAD vulkan-1.dll, so vcpkg's applocal-deps install copies
	# it next to the exes even though the runtime target's own dependency
	# install filters it out (targets/openxr/CMakeLists.txt). A bundled loader
	# next to the exe shadows the system C:\Windows\System32\vulkan-1.dll →
	# two competing Vulkan loaders → NVIDIA ICD null-deref (issue #105). The
	# NSIS installer already excludes it; this guaranteed-last cleanup closes
	# the same gap for the _package/ dev tree (it runs after every target
	# subdir above, so it removes whatever applocal re-staged). Idempotent.
	install(CODE "
		set(_vk \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/vulkan-1.dll\")
		if(EXISTS \"\${_vk}\")
			message(STATUS \"Removing bundled vulkan-1.dll (runtime uses the system loader): \${_vk}\")
			file(REMOVE \"\${_vk}\")
		endif()
	")
endif()
