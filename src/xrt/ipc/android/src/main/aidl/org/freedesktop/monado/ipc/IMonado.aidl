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

    /*!
     * Report the client SurfaceView's on-screen rect so the compositor can anchor
     * the weave phase where the window physically sits on the panel
     * (ADR-036 D6, runtime#1033).
     *
     * A pure window MOVE on Android raises no resize (only a oneway
     * IWindow.moved) and SurfaceFlinger repositions the layer with the OLD
     * buffer, so neither the surface nor its extent tells the service where the
     * window went. The client therefore samples View.getLocationOnScreen() from
     * a Choreographer callback and pushes changes here.
     *
     * oneway on purpose: this is per-frame-ish, must never block the client's UI
     * thread, and a dropped update self-heals on the next change. (Binder oneway
     * gets half the 1 MB buffer; five ints is nothing.)
     *
     * @param x         window left edge, physical screen pixels (may be negative)
     * @param y         window top edge, physical screen pixels
     * @param w         window width in physical screen pixels
     * @param h         window height in physical screen pixels
     * @param displayId Display.getDisplayId() the rect is expressed in
     */
    oneway void updateWindowRect(int x, int y, int w, int h, int displayId);
}
