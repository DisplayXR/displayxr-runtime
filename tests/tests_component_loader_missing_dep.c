// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Two test-only modules for the #2436 dependency walk.
 *
 * Built twice (Windows only). With TCL_DEP_LIBRARY it is the dependency: a
 * DLL written to an isolated directory that is never next to anything that
 * loads it and never on PATH. Without it, it is the dependent: a module that
 * STATICALLY imports the dependency, so LoadLibrary fails with error 126 and
 * the loader has to name the import it could not resolve.
 *
 * @ingroup tests
 */

#ifdef TCL_DEP_LIBRARY

__declspec(dllexport) int
dxr_test_missing_dep_value(void)
{
	return 2436;
}

#else

__declspec(dllimport) int
dxr_test_missing_dep_value(void);

//! Referenced so the linker keeps the static import.
__declspec(dllexport) int
xrtPluginNegotiate(void)
{
	return dxr_test_missing_dep_value();
}

#endif
