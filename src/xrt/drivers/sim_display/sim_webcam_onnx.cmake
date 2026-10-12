# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# #1855: the opt-in ONNX face estimator for sim_display webcam eye tracking
# (-DXRT_SIM_WEBCAM_ESTIMATOR=onnx). Included from src/xrt/drivers/CMakeLists.txt
# only when that option is set, so the default and release builds fetch nothing,
# ship no new library and keep the stub estimator.
#
# Fetches, pinned by URL + SHA256, into ${CMAKE_BINARY_DIR}/_deps/sim_webcam/:
#   - the ONNX Runtime prebuilt CPU package (MIT) for this platform; only its
#     headers, its runtime library and its LICENSE are extracted;
#   - the YuNet face detector (opencv_zoo, MIT), dynamic-input ONNX export.
# Nothing is committed. The plug-in loads the library at run time by full path,
# so it never links against it (sim_display_face_estimator_onnx.c).
#
# Sets for the caller:
#   SIM_WEBCAM_ESTIMATOR_SOURCE   the estimator .c to compile
#   SIM_ORT_INCLUDE_DIR           onnxruntime_c_api.h lives here
#   SIM_ORT_LIB_FILE / _NAME      the runtime library (path / file name next to the plug-in)
#   SIM_WEBCAM_MODEL_FILE / _NAME the model (path / file name next to the plug-in)
# and defines sim_webcam_onnx_configure(<target>).

set(SIM_ORT_VERSION "1.30.0")
set(_sim_ort_base "https://github.com/microsoft/onnxruntime/releases/download/v${SIM_ORT_VERSION}")

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
	set(_sim_ort_pkg "onnxruntime-win-x64-${SIM_ORT_VERSION}")
	set(_sim_ort_ext "zip")
	set(_sim_ort_sha256 "c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949")
	set(SIM_ORT_LIB_NAME "onnxruntime.dll")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
	set(_sim_ort_pkg "onnxruntime-linux-x64-${SIM_ORT_VERSION}")
	set(_sim_ort_ext "tgz")
	set(_sim_ort_sha256 "a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd")
	set(SIM_ORT_LIB_NAME "libonnxruntime.so.${SIM_ORT_VERSION}")
elseif(APPLE AND NOT IOS AND (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")))
	set(_sim_ort_pkg "onnxruntime-osx-arm64-${SIM_ORT_VERSION}")
	set(_sim_ort_ext "tgz")
	set(_sim_ort_sha256 "6ebb5062a934537c352937821f9fe9718e7de1a2db1122a93dd363ffd53a7012")
	set(SIM_ORT_LIB_NAME "libonnxruntime.${SIM_ORT_VERSION}.dylib")
else()
	message(FATAL_ERROR
		"XRT_SIM_WEBCAM_ESTIMATOR=onnx: no pinned ONNX Runtime package for this platform "
		"(${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}). Supported: Windows x64, Linux x64, macOS arm64. "
		"Use XRT_SIM_WEBCAM_ESTIMATOR=stub.")
endif()

# YuNet, the dynamic-shape export (symbolic height/width), pinned to the
# opencv_zoo commit that added it. Licence: MIT (models/face_detection_yunet/LICENSE).
set(_sim_model_commit "26cc381e4d2594bb9f47a26eb8fd96c94a13660d")
set(SIM_WEBCAM_MODEL_NAME "face_detection_yunet_2026may.onnx")
set(_sim_model_url
	"https://github.com/opencv/opencv_zoo/raw/${_sim_model_commit}/models/face_detection_yunet/${SIM_WEBCAM_MODEL_NAME}")
set(_sim_model_sha256 "ebafce4e3c118d6554634be5c27ab333b4c047a9a8c3faf1d7cf93101c22f0f0")

set(_sim_dir "${CMAKE_BINARY_DIR}/_deps/sim_webcam")
set(_sim_ort_archive "${_sim_dir}/${_sim_ort_pkg}.${_sim_ort_ext}")
set(_sim_ort_root "${_sim_dir}/${_sim_ort_pkg}")
set(SIM_ORT_INCLUDE_DIR "${_sim_ort_root}/include")
set(SIM_ORT_LIB_FILE "${_sim_ort_root}/lib/${SIM_ORT_LIB_NAME}")
set(SIM_WEBCAM_MODEL_FILE "${_sim_dir}/${SIM_WEBCAM_MODEL_NAME}")

function(_sim_webcam_download url sha256 dest)
	if(EXISTS "${dest}")
		file(SHA256 "${dest}" _have)
		if(_have STREQUAL sha256)
			return()
		endif()
	endif()
	message(STATUS "sim_display webcam (#1855): fetching ${url}")
	file(DOWNLOAD "${url}" "${dest}" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS _st)
	list(GET _st 0 _code)
	if(NOT _code EQUAL 0)
		file(REMOVE "${dest}")
		message(FATAL_ERROR "sim_display webcam (#1855): download of ${url} failed: ${_st}")
	endif()
endfunction()

_sim_webcam_download("${_sim_ort_base}/${_sim_ort_pkg}.${_sim_ort_ext}" "${_sim_ort_sha256}" "${_sim_ort_archive}")
_sim_webcam_download("${_sim_model_url}" "${_sim_model_sha256}" "${SIM_WEBCAM_MODEL_FILE}")

if(NOT EXISTS "${SIM_ORT_LIB_FILE}" OR NOT EXISTS "${SIM_ORT_INCLUDE_DIR}/onnxruntime_c_api.h")
	# Headers + the one library + LICENSE only: the Windows zip also carries a
	# ~400 MB PDB that nothing here needs.
	file(ARCHIVE_EXTRACT
		INPUT "${_sim_ort_archive}"
		DESTINATION "${_sim_dir}"
		PATTERNS "*/include/*" "*/lib/${SIM_ORT_LIB_NAME}" "*/LICENSE"
		)
	if(NOT EXISTS "${SIM_ORT_LIB_FILE}")
		message(FATAL_ERROR "sim_display webcam (#1855): ${SIM_ORT_LIB_NAME} not found in ${_sim_ort_archive}")
	endif()
endif()

set(SIM_WEBCAM_ESTIMATOR_SOURCE sim_display/sim_display_face_estimator_onnx.c)
message(STATUS "sim_display webcam (#1855): face estimator = YuNet via ONNX Runtime ${SIM_ORT_VERSION}")

# Compile flags for a target that builds SIM_WEBCAM_ESTIMATOR_SOURCE.
function(sim_webcam_onnx_configure tgt)
	target_include_directories(${tgt} PRIVATE "${SIM_ORT_INCLUDE_DIR}")
	target_compile_definitions(
		${tgt} PRIVATE SIM_ORT_LIB_NAME="${SIM_ORT_LIB_NAME}" SIM_WEBCAM_MODEL_NAME="${SIM_WEBCAM_MODEL_NAME}"
		)
	if(NOT WIN32)
		target_link_libraries(${tgt} PRIVATE ${CMAKE_DL_LIBS})
	endif()
endfunction()
