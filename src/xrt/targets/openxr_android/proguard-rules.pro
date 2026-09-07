# Copyright 2020, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0
# see http://developer.android.com/guide/developing/tools/proguard.html
# Trying to keep most of them in source code annotations and let Gradle do the work

# For library auto-detection in AboutLibraries
-keep class .R
-keep class **.R$* {
    <fields>;
}

# CROSS-APK CONTRACT (#1403 Option B). MiniWindowLayout's statics are called by
# reflection from ANOTHER APK — the DisplayXR Browser's browser process, whose
# XrSession lives in Chromium's GPU process and therefore has no Activity to
# measure with. A rename or a shrink here breaks a SHIPPED browser silently, at
# run time, with no build-time signal anywhere.
#
# @Keep already covers the annotated members via proguard-android-optimize.txt,
# and this build has `minifyEnabled false` today, so this rule changes nothing
# right now. It is here so that turning minification on later cannot quietly
# break the contract — the failure it prevents is invisible from inside this
# repo. Asserted on the built release APK by scripts/check_mini_window_contract.sh.
-keep class org.freedesktop.monado.auxiliary.MiniWindowLayout {
    public static <methods>;
    public static <fields>;
}

-keep class com.leia.sdk.** { *; }
-keep class com.leia.core.** { *; }
-keep class com.leia.internal.** { *; }
-keep class com.leia.headtracking.** { *; }
