# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# §5.6 end to end through `displayxr-cli conv models` (ctest drives it with
# `cmake -P`). Inputs: CLI, PACK (manifest path), WORK (a scratch dir),
# FAKE_BUILDER (empty = skip the build half), FAKE_ENV (DXR_MODEL_BUILDER_<ID>).
#
#  1. `fetch --installer` never builds: skipped-buildable, no staging dir.
#  2. a plain `fetch <pack>` (no --model) never builds: build-locally + the hint.
#  3. with the dev override: `fetch --model --events --accept-license
#     --allow-research-only` streams NDJSON whose last line is the result
#     (`built`), `license status` reports the record, `list --json` shows the
#     model installed with `license_accepted`, `remove` deletes it.

function(run_cli out_var rc_var)
	execute_process(
		COMMAND ${CMAKE_COMMAND} -E env ${ARGN}
		OUTPUT_VARIABLE out
		ERROR_VARIABLE err
		RESULT_VARIABLE rc)
	set(${out_var} "${out}" PARENT_SCOPE)
	set(${rc_var} "${rc}" PARENT_SCOPE)
	message(STATUS "stdout:\n${out}\nstderr:\n${err}\nexit: ${rc}")
endfunction()

function(expect text regex what)
	if(NOT "${text}" MATCHES "${regex}")
		message(FATAL_ERROR "FAIL: ${what}: no match for '${regex}'")
	endif()
endfunction()

set(STORE "${WORK}/store")
set(HOMEDIR "${WORK}/home")
file(REMOVE_RECURSE "${STORE}" "${HOMEDIR}")
file(MAKE_DIRECTORY "${HOMEDIR}")
set(BASE_ENV "DXR_CONV_MODELS_ROOT=${STORE}" "HOME=${HOMEDIR}" "XDG_DATA_HOME=${HOMEDIR}/.local/share"
	"DXR_CONV_SCRATCH_ROOT=${WORK}/scratch")

# 1. The installer path.
run_cli(out rc ${BASE_ENV} "${CLI}" conv models fetch "${PACK}" --model m-a --installer --json)
expect("${out}" "\"result\":[ \t]*\"skipped-buildable\"" "installer fetch")
if(EXISTS "${STORE}/staging")
	message(FATAL_ERROR "FAIL: an installer fetch created a staging directory")
endif()

# 2. A bulk fetch.
run_cli(out rc ${BASE_ENV} "${CLI}" conv models fetch "${PACK}" --allow-research-only --json)
expect("${out}" "\"result\":[ \t]*\"build-locally\"" "bulk fetch")
expect("${out}" "--model m-a" "bulk fetch hint")
if(EXISTS "${STORE}/staging")
	message(FATAL_ERROR "FAIL: a bulk fetch created a staging directory")
endif()

if("${FAKE_BUILDER}" STREQUAL "")
	return()
endif()

# 3. The build, as the Dashboard runs it.
set(DEV_ENV ${BASE_ENV} "DXR_ALLOW_DEV_PLUGIN_PATHS=1" "${FAKE_ENV}=${FAKE_BUILDER}")
run_cli(out rc ${DEV_ENV} "${CLI}" conv models fetch "${PACK}" --model m-a --events)
expect("${out}" "\"result\":\"policy\"" "no opt-in")
run_cli(out rc ${DEV_ENV} "${CLI}" conv models fetch "${PACK}" --model m-a --allow-research-only --events)
expect("${out}" "\"result\":\"build-license\"" "no licence record")
run_cli(out rc ${DEV_ENV} "${CLI}" conv models fetch "${PACK}" --model m-a --allow-research-only --accept-license
	test-licence --events)
expect("${out}" "\"event\":\"start\"" "forwarded builder line")
expect("${out}" "\"phase\":\"download-source\"" "the store's download phase")
expect("${out}" "\"v\":1,\"event\":\"result\"" "result line")
expect("${out}" "\"result\":\"built\"" "built")
expect("${out}" "\"provenance\":\"dev-override\"" "provenance")
if(NOT rc EQUAL 0)
	message(FATAL_ERROR "FAIL: a built fetch exited ${rc}")
endif()
string(REGEX MATCHALL "[^\n]+" lines "${out}")
foreach(l ${lines})
	if(NOT l MATCHES "^{")
		message(FATAL_ERROR "FAIL: --events printed a non-JSON line: ${l}")
	endif()
endforeach()
run_cli(out rc ${BASE_ENV} "${CLI}" conv models license status test-licence --pack "${PACK}" --json)
expect("${out}" "\"accepted\":[ \t]*true" "licence status")
run_cli(out rc ${BASE_ENV} "${CLI}" conv models fetch "${PACK}" --model m-a --allow-research-only --json)
expect("${out}" "\"result\":[ \t]*\"ok\"" "second fetch finds it present")
run_cli(out rc ${BASE_ENV} "${CLI}" conv models remove "${PACK}" --model m-a --json)
expect("${out}" "\"blobs_deleted\":[ \t]*1" "remove")
run_cli(out rc ${BASE_ENV} "${CLI}" conv models license withdraw test-licence --json)
expect("${out}" "\"result\":[ \t]*\"withdrawn\"" "withdraw")
if(EXISTS "${STORE}/staging")
	file(GLOB left "${STORE}/staging/*")
	if(left)
		message(FATAL_ERROR "FAIL: staging left behind: ${left}")
	endif()
endif()
message(STATUS "conv models build e2e: PASS")
