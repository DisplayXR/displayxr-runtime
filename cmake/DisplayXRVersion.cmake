# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# The runtime's version, derived in ONE place for every platform (#1790).
#
# Everything that reports or packages a version reads it from here:
# project(VERSION) -> xrGetInstanceProperties runtimeVersion (u_version_*), the
# git tag in runtimeName / `displayxr-cli info` / the IPC client<->service gate
# (u_git_tag), the Windows installer, the macOS .pkg and the Linux .deb.
#
# Before #1790 CMakeLists.txt hard-coded VERSION 1.6.0 and only the Windows
# tag build patched it, so Linux, macOS, Android and every untagged build
# reported 1.6.0. Apps gate features on runtimeVersion (the Unity plug-in's
# format-honest swapchain choice needs >= 2.21.x), so the wrong number changed
# behaviour, not just a log line.
#
# Sources, first match wins:
#   1. DXR_RUNTIME_VERSION_OVERRIDE (-D or environment): X.Y.Z, vX.Y.Z or a
#      `git describe` string. For packagers building a tree that has no git
#      metadata and no export-subst'ed .git_archival.txt.
#   2. git describe --tags --long --match 'v[0-9]*' over the checkout: the
#      nearest canonical vX.Y.Z tag reachable from HEAD.
#   3. .git_archival.txt, which `git archive` (and so GitHub's "Source code"
#      tarballs) fills in via export-subst (.gitattributes).
#   4. versions.json "runtime": the last release, recorded on main by
#      build-windows.yml's BumpVersionsJsonOnTag right after each tag. Every
#      tree that carries it contains that release, so it is a lower bound (a
#      shallow tagless clone, a plain copy of the sources). WARNs.
#   (none of these -> FATAL_ERROR; never a made-up number.)
#
# The scheme. The NUMBER is always the nearest release tag: exactly vX.Y.Z on
# the tag, and the same X.Y.Z on every commit after it until the next tag. A
# dev build therefore compares EQUAL to the release it descends from (never
# lower, so a feature gate that the release passes, the dev build passes too)
# and never claims a release it is not (no X.Y.Z+1 that may never exist). What
# marks it as a dev build is the string next to the number:
#   tag build       2.51.1    v2.51.1              package 2.51.1
#   3 commits after 2.51.1    v2.51.1-3-g1a2b3c4d5 package 2.51.1+3.g1a2b3c4d5
#   versions.json   2.51.1    v2.51.1-dev[-g<sha>] package 2.51.1+dev[.g<sha>]
# The package string sorts >= the release under dpkg and equal under SemVer
# (build metadata), and is what installers/packages are NAMED after. Values
# that other tools parse as a version (the Windows registry Version, ARP
# DisplayVersion, VIProductVersion) stay the plain number.
#
# Outputs (PARENT_SCOPE of dxr_runtime_version_resolve()):
#   DXR_RUNTIME_VERSION            X.Y.Z (project(VERSION))
#   DXR_RUNTIME_VERSION_MAJOR/_MINOR/_PATCH
#   DXR_RUNTIME_VERSION_DESCRIBE   vX.Y.Z[...] (u_git_tag; the string apps see)
#   DXR_RUNTIME_VERSION_PACKAGE    X.Y.Z or X.Y.Z+<n>.g<sha> (installer names)
#   DXR_RUNTIME_VERSION_IS_RELEASE 1 exactly on a vX.Y.Z tag, else 0
#   DXR_RUNTIME_VERSION_SOURCE     override | git | archive | versions.json
#
# Script mode prints the same values as KEY=VALUE lines (or writes them to
# -DOUT=<file>), for tooling that needs the version without configuring:
#   cmake -P cmake/DisplayXRVersion.cmake

get_filename_component(_DXR_VERSION_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Canonical release tags only (the /release skill's ^v[0-9]+\.[0-9]+\.[0-9]+$):
# --match keeps the v-prefixed ones, --exclude drops any with a character other
# than a digit or a dot after the v (an rc, a -moved bookmark tag, ...).
set(_DXR_VERSION_TAG_MATCH "v[0-9]*")
set(_DXR_VERSION_TAG_EXCLUDE "v*[!0-9.]*")

# Parse X.Y.Z, vX.Y.Z or vX.Y.Z-<n>-g<sha> into <prefix>_OK/_MAJOR/_MINOR/_PATCH
# and, for the describe form, _COMMITS/_HASH (empty when absent).
function(_dxr_version_parse _str _prefix)
	set(_ok 0)
	set(_commits "")
	set(_hash "")
	if("${_str}" MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9]+)-g([0-9a-f]+))?$")
		set(_ok 1)
		set(_major "${CMAKE_MATCH_1}")
		set(_minor "${CMAKE_MATCH_2}")
		set(_patch "${CMAKE_MATCH_3}")
		if(NOT "${CMAKE_MATCH_4}" STREQUAL "")
			set(_commits "${CMAKE_MATCH_5}")
			set(_hash "${CMAKE_MATCH_6}")
		endif()
		# u_version_* are uint16_t; XrVersion's major/minor are 16 bits.
		foreach(_part IN ITEMS _major _minor _patch)
			if(${${_part}} GREATER 65535)
				set(_ok 0)
			endif()
		endforeach()
	endif()
	set(${_prefix}_OK
	    ${_ok}
	    PARENT_SCOPE)
	set(${_prefix}_MAJOR
	    "${_major}"
	    PARENT_SCOPE)
	set(${_prefix}_MINOR
	    "${_minor}"
	    PARENT_SCOPE)
	set(${_prefix}_PATCH
	    "${_patch}"
	    PARENT_SCOPE)
	set(${_prefix}_COMMITS
	    "${_commits}"
	    PARENT_SCOPE)
	set(${_prefix}_HASH
	    "${_hash}"
	    PARENT_SCOPE)
endfunction()

# The checkout's `git describe`, or empty. Also returns HEAD's short hash when
# git works at all (a shallow tagless clone still knows its commit).
function(_dxr_version_from_git _desc_var _hash_var _why_var)
	set(${_desc_var}
	    ""
	    PARENT_SCOPE)
	set(${_hash_var}
	    ""
	    PARENT_SCOPE)
	set(${_why_var}
	    ""
	    PARENT_SCOPE)
	if(NOT GIT_EXECUTABLE)
		find_package(Git QUIET)
	endif()
	if(NOT GIT_EXECUTABLE)
		set(${_why_var}
		    "git not found"
		    PARENT_SCOPE)
		return()
	endif()
	# Only OUR repository: a source tree unpacked inside some other checkout
	# must not be versioned after that checkout's tags. --show-prefix is the
	# root's path below the work tree's top: empty exactly when the root IS
	# the top (a plain clone, a worktree, a submodule, a FetchContent clone).
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" rev-parse --show-prefix
		WORKING_DIRECTORY "${_DXR_VERSION_ROOT}"
		RESULT_VARIABLE _res
		OUTPUT_VARIABLE _prefix
		ERROR_VARIABLE _err
		OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
	if(NOT _res EQUAL 0)
		if(_err MATCHES "not a git repository")
			set(_err "no .git")
		endif()
		set(${_why_var}
		    "not a usable git checkout (${_err})"
		    PARENT_SCOPE)
		return()
	endif()
	if(NOT "${_prefix}" STREQUAL "")
		set(${_why_var}
		    "the sources sit inside another git checkout (at ${_prefix})"
		    PARENT_SCOPE)
		return()
	endif()
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" rev-parse --short=9 HEAD
		WORKING_DIRECTORY "${_DXR_VERSION_ROOT}"
		RESULT_VARIABLE _res
		OUTPUT_VARIABLE _hash
		ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(_res EQUAL 0)
		set(${_hash_var}
		    "${_hash}"
		    PARENT_SCOPE)
	endif()
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" describe --tags --long --match "${_DXR_VERSION_TAG_MATCH}" --exclude
		        "${_DXR_VERSION_TAG_EXCLUDE}" HEAD
		WORKING_DIRECTORY "${_DXR_VERSION_ROOT}"
		RESULT_VARIABLE _res
		OUTPUT_VARIABLE _desc
		ERROR_VARIABLE _err
		OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
	if(NOT _res EQUAL 0)
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" rev-parse --is-shallow-repository
			WORKING_DIRECTORY "${_DXR_VERSION_ROOT}"
			OUTPUT_VARIABLE _shallow
			ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
		if(_shallow STREQUAL "true")
			set(_err "shallow clone with no vX.Y.Z tag in reach -- `git fetch --unshallow --tags` (CI: actions/checkout fetch-depth: 0)")
		endif()
		set(${_why_var}
		    "git describe found no vX.Y.Z tag: ${_err}"
		    PARENT_SCOPE)
		return()
	endif()
	set(${_desc_var}
	    "${_desc}"
	    PARENT_SCOPE)
endfunction()

function(dxr_runtime_version_resolve)
	set(_source "")
	set(_hash "")
	set(_notes "")

	# 1. Explicit override.
	set(_override "${DXR_RUNTIME_VERSION_OVERRIDE}")
	if("${_override}" STREQUAL "" AND DEFINED ENV{DXR_RUNTIME_VERSION_OVERRIDE})
		set(_override "$ENV{DXR_RUNTIME_VERSION_OVERRIDE}")
	endif()
	if(NOT "${_override}" STREQUAL "")
		_dxr_version_parse("${_override}" _v)
		if(NOT _v_OK)
			message(
				FATAL_ERROR
					"DXR_RUNTIME_VERSION_OVERRIDE='${_override}' is not X.Y.Z, vX.Y.Z or vX.Y.Z-<n>-g<sha> (#1790)."
				)
		endif()
		set(_source "override")
	endif()

	# 2. git.
	if(_source STREQUAL "")
		if(NOT CMAKE_SCRIPT_MODE_FILE)
			# Re-run the configure when HEAD or its branch ref moves, so an
			# incremental build never keeps the previous commit's version.
			# (A tag added to the commit already checked out needs a re-run
			# of cmake; CI and /release always configure fresh.)
			include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GetGitRevisionDescription.cmake")
			get_git_head_revision(_dxr_refspec _dxr_head)
		endif()
		_dxr_version_from_git(_desc _hash _why)
		if(NOT "${_desc}" STREQUAL "")
			_dxr_version_parse("${_desc}" _v)
			if(_v_OK)
				set(_source "git")
			else()
				list(APPEND _notes "git describe gave '${_desc}', not vX.Y.Z-<n>-g<sha>")
			endif()
		elseif(NOT "${_why}" STREQUAL "")
			list(APPEND _notes "${_why}")
		endif()
	endif()

	# 3. .git_archival.txt (export-subst by `git archive`).
	if(_source STREQUAL "" AND EXISTS "${_DXR_VERSION_ROOT}/.git_archival.txt")
		file(STRINGS "${_DXR_VERSION_ROOT}/.git_archival.txt" _lines)
		foreach(_line IN LISTS _lines)
			if(_line MATCHES "^describe-name: *(.*)$")
				set(_desc "${CMAKE_MATCH_1}")
				string(STRIP "${_desc}" _desc)
				# Unsubstituted ($Format:...$ in a checkout) or empty (no tag
				# in reach when the archive was made): not a version.
				if(NOT _desc MATCHES "Format:" AND NOT "${_desc}" STREQUAL "")
					_dxr_version_parse("${_desc}" _v)
					if(_v_OK)
						set(_source "archive")
					else()
						list(APPEND _notes ".git_archival.txt names '${_desc}', not a vX.Y.Z describe")
					endif()
				endif()
			endif()
		endforeach()
	endif()

	# 4. versions.json: the last release, a lower bound.
	if(_source STREQUAL "" AND EXISTS "${_DXR_VERSION_ROOT}/versions.json")
		file(READ "${_DXR_VERSION_ROOT}/versions.json" _json)
		string(
			JSON
			_rel
			ERROR_VARIABLE
			_json_err
			GET
			"${_json}"
			runtime)
		if(NOT _json_err AND "${_rel}" MATCHES "^v?[0-9]+\\.[0-9]+\\.[0-9]+$")
			_dxr_version_parse("${_rel}" _v)
			if(_v_OK)
				set(_source "versions.json")
			endif()
		endif()
	endif()

	if(_source STREQUAL "")
		string(REPLACE ";" "; " _notes "${_notes}")
		message(
			FATAL_ERROR
				"Cannot determine the DisplayXR runtime version (#1790): ${_notes}. "
				"Build from a git checkout that has its vX.Y.Z tags, from a `git archive` / GitHub source tarball, "
				"or pass -DDXR_RUNTIME_VERSION_OVERRIDE=X.Y.Z.")
	endif()

	set(_ver "${_v_MAJOR}.${_v_MINOR}.${_v_PATCH}")
	if(_source STREQUAL "versions.json")
		set(_is_release 0)
		set(_describe "v${_ver}-dev")
		set(_package "${_ver}+dev")
		if(NOT "${_hash}" STREQUAL "")
			string(APPEND _describe "-g${_hash}")
			string(APPEND _package ".g${_hash}")
		endif()
	elseif("${_v_COMMITS}" STREQUAL "" OR "${_v_COMMITS}" STREQUAL "0")
		set(_is_release 1)
		set(_describe "v${_ver}")
		set(_package "${_ver}")
	else()
		set(_is_release 0)
		set(_describe "v${_ver}-${_v_COMMITS}-g${_v_HASH}")
		set(_package "${_ver}+${_v_COMMITS}.g${_v_HASH}")
	endif()

	set(DXR_RUNTIME_VERSION
	    "${_ver}"
	    PARENT_SCOPE)
	set(DXR_RUNTIME_VERSION_MAJOR
	    "${_v_MAJOR}"
	    PARENT_SCOPE)
	set(DXR_RUNTIME_VERSION_MINOR
	    "${_v_MINOR}"
	    PARENT_SCOPE)
	set(DXR_RUNTIME_VERSION_PATCH
	    "${_v_PATCH}"
	    PARENT_SCOPE)
	set(DXR_RUNTIME_VERSION_DESCRIBE
	    "${_describe}"
	    PARENT_SCOPE)
	set(DXR_RUNTIME_VERSION_PACKAGE
	    "${_package}"
	    PARENT_SCOPE)
	set(DXR_RUNTIME_VERSION_IS_RELEASE
	    "${_is_release}"
	    PARENT_SCOPE)
	set(DXR_RUNTIME_VERSION_SOURCE
	    "${_source}"
	    PARENT_SCOPE)

	if(_source STREQUAL "versions.json")
		string(REPLACE ";" "; " _notes "${_notes}")
		message(
			WARNING
				"DisplayXR runtime version ${_ver} comes from versions.json (the last release this tree contains), "
				"not from git: ${_notes}. It reports as '${_describe}'. For the exact version build from a checkout "
				"with its vX.Y.Z tags (#1790).")
	endif()
endfunction()

# KEY=VALUE lines, as printed by script mode and written next to the build.
function(dxr_runtime_version_lines _var)
	set(_out "")
	foreach(
		_k IN
		ITEMS DXR_RUNTIME_VERSION
		      DXR_RUNTIME_VERSION_MAJOR
		      DXR_RUNTIME_VERSION_MINOR
		      DXR_RUNTIME_VERSION_PATCH
		      DXR_RUNTIME_VERSION_DESCRIBE
		      DXR_RUNTIME_VERSION_PACKAGE
		      DXR_RUNTIME_VERSION_IS_RELEASE
		      DXR_RUNTIME_VERSION_SOURCE)
		string(APPEND _out "${_k}=${${_k}}\n")
	endforeach()
	set(${_var}
	    "${_out}"
	    PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
	dxr_runtime_version_resolve()
	dxr_runtime_version_lines(_dxr_lines)
	if(DEFINED OUT)
		file(WRITE "${OUT}" "${_dxr_lines}")
	else()
		# message() goes to stderr in script mode; stdout is for the values.
		string(REGEX REPLACE "\n$" "" _dxr_lines "${_dxr_lines}")
		string(REPLACE "\n" ";" _dxr_lines "${_dxr_lines}")
		foreach(_dxr_line IN LISTS _dxr_lines)
			execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${_dxr_line}")
		endforeach()
	endif()
endif()
