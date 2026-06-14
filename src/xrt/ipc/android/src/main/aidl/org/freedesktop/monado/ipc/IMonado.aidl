// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to bootstrap the Monado IPC connection.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup ipc_android
 */

package org.freedesktop.monado.ipc;

import android.os.ParcelFileDescriptor;
import android.view.Surface;

interface IMonado {
    /*!
     * Pass one side of the socket pair to the service to set up the IPC.
     */
    void connect(in ParcelFileDescriptor parcelFileDescriptor);

    /*!
     * Provide the surface we inject into the activity, back to the service.
     */
    void passAppSurface(in Surface surface);

    /*!
     * Asking service whether it has the capbility to draw over other apps or not.
     */
    boolean canDrawOverOtherApps();

    /*!
     * Tell the service the previously passed app surface was destroyed (e.g. the
     * client backgrounded behind a file picker), so the compositor stops
     * presenting into the dead BufferQueue and tears its VkSurfaceKHR down.
     * The client passes the replacement via passAppSurface on resume. #528
     */
    void clearAppSurface();
}
