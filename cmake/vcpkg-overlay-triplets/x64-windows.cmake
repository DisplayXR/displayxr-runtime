set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# NOTE: PR #253's vcpkg overlay forced `pthreads` to static so the runtime
# DLL had no pthread*.dll import. That goal turned out to be incompatible
# with already-released downstream consumers (shell v1.2.5) that
# dynamically import pthreadVC3.dll and expect it co-located with
# DisplayXRClient.dll under $INSTDIR.
#
# Reverting to default (dynamic) linkage. PR #253's primary win — the
# installer no longer mutates system PATH — is preserved: pthreadVC3.dll
# is shipped inside $INSTDIR and resolved through the standard
# exe-directory DLL search of any consumer that also lives there
# (DisplayXRClient.dll, displayxr-shell.exe, third-party workspace apps).
