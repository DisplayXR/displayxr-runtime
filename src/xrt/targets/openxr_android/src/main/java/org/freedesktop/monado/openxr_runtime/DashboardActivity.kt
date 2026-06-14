// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime diagnostics dashboard — the APK's launcher screen (#558).
 * @author David Fattal
 */
package org.freedesktop.monado.openxr_runtime

import android.content.ClipData
import android.content.ClipboardManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.Process
import android.view.View
import android.widget.Button
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import dagger.hilt.android.AndroidEntryPoint
import javax.inject.Inject
import org.freedesktop.monado.android_common.DisplayOverOtherAppsStatusFragment
import org.freedesktop.monado.android_common.VrModeStatus
import org.freedesktop.monado.auxiliary.NameAndLogoProvider
import org.json.JSONObject

/**
 * The on-device equivalent of `displayxr-cli info` + `selftest`: snapshot
 * cards (runtime / self-test / plug-in / display / advertised DP modes /
 * eye tracking) from the headless query run in the `:diag` process, plus a
 * live-service card (connected clients, workspace, active mode, tracking)
 * on the out-of-process flavor, plus the Android permission fragments
 * (VR listener, display-over-other-apps) hosted as a proper card.
 */
@AndroidEntryPoint
class DashboardActivity : AppCompatActivity() {

    @Inject lateinit var nameAndLogoProvider: NameAndLogoProvider

    private var rawJson: String? = null
    private var diagPid: Int = 0
    private var queryInFlight = false

    private val mainHandler = Handler(Looper.getMainLooper())
    private val timeoutRunnable = Runnable { onQueryTimeout() }

    private var serviceMessenger: Messenger? = null
    private val replyMessenger =
        Messenger(
            object : Handler(Looper.getMainLooper()) {
                override fun handleMessage(msg: Message) {
                    if (msg.what != DiagQueryService.MSG_RESULT) {
                        super.handleMessage(msg)
                        return
                    }
                    diagPid = msg.data.getInt(DiagQueryService.KEY_PID, 0)
                    onQueryResult(msg.data.getString(DiagQueryService.KEY_JSON))
                }
            }
        )

    private val connection =
        object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
                serviceMessenger = Messenger(binder)
                try {
                    val msg = Message.obtain(null, DiagQueryService.MSG_RUN_QUERY)
                    msg.replyTo = replyMessenger
                    serviceMessenger?.send(msg)
                } catch (e: Exception) {
                    onQueryResult(null)
                }
            }

            override fun onServiceDisconnected(name: ComponentName?) {
                // Expected: the :diag process self-kills after replying.
                serviceMessenger = null
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_dashboard)
        AppCompatDelegate.setDefaultNightMode(AppCompatDelegate.MODE_NIGHT_YES)

        findViewById<TextView>(R.id.textTitle).text = nameAndLogoProvider.getLocalizedRuntimeName()
        findViewById<ImageView>(R.id.imageLogo)
            .setImageDrawable(nameAndLogoProvider.getLogoDrawable())
        findViewById<TextView>(R.id.textFlavor).text =
            getString(
                if (BuildConfig.inProcess) R.string.diag_flavor_in_process
                else R.string.diag_flavor_out_of_process
            )

        findViewById<Button>(R.id.btnRefresh).setOnClickListener { runQuery() }
        findViewById<Button>(R.id.btnCopyJson).setOnClickListener { copyJson() }
        findViewById<Button>(R.id.btnShare).setOnClickListener { shareJson() }

        if (!BuildConfig.inProcess) {
            findViewById<View>(R.id.cardLive).visibility = View.VISIBLE
            findViewById<View>(R.id.drawOverOtherAppsFrame).visibility = View.VISIBLE
            // The service shares the activity's (default) process, so a
            // self-kill takes the whole runtime down — same behavior as the
            // legacy About screen's shutdown button.
            val shutdown = findViewById<Button>(R.id.btnShutdown)
            shutdown.visibility = View.VISIBLE
            shutdown.setOnClickListener { Process.killProcess(Process.myPid()) }
        }

        // Android status fragments — the existing android_common logic,
        // hosted in width-constrained card frames (this IS the fix for the
        // old corner-jammed overlay-permission UI).
        val tx = supportFragmentManager.beginTransaction()
        val status = VrModeStatus.detectStatus(this, applicationContext.applicationInfo.packageName)
        tx.replace(R.id.statusFrame, VrModeStatus.newInstance(status), null)
        if (!BuildConfig.inProcess) {
            tx.replace(R.id.drawOverOtherAppsFrame, DisplayOverOtherAppsStatusFragment(), null)
        }
        tx.commit()

        runQuery()
    }

    override fun onDestroy() {
        mainHandler.removeCallbacks(timeoutRunnable)
        unbindQuietly()
        super.onDestroy()
    }

    /*
     * Query plumbing.
     */

    private fun runQuery() {
        if (queryInFlight) {
            return
        }
        queryInFlight = true
        findViewById<View>(R.id.rowQueryStatus).visibility = View.VISIBLE
        findViewById<View>(R.id.progressQuery).visibility = View.VISIBLE
        findViewById<TextView>(R.id.textQueryStatus).text = getString(R.string.diag_querying)

        bindService(Intent(this, DiagQueryService::class.java), connection, BIND_AUTO_CREATE)
        mainHandler.postDelayed(timeoutRunnable, QUERY_TIMEOUT_MS)
    }

    private fun onQueryResult(json: String?) {
        mainHandler.removeCallbacks(timeoutRunnable)
        unbindQuietly()
        queryInFlight = false

        if (json.isNullOrEmpty() || json == "{}") {
            findViewById<View>(R.id.progressQuery).visibility = View.GONE
            findViewById<TextView>(R.id.textQueryStatus).text =
                getString(R.string.diag_query_failed)
            return
        }
        rawJson = json
        findViewById<View>(R.id.rowQueryStatus).visibility = View.GONE
        populate(JSONObject(json))
    }

    private fun onQueryTimeout() {
        unbindQuietly()
        queryInFlight = false
        // Same-UID kill of the wedged :diag process so Retry gets a fresh one.
        if (diagPid > 0 && diagPid != Process.myPid()) {
            Process.killProcess(diagPid)
        }
        findViewById<View>(R.id.progressQuery).visibility = View.GONE
        findViewById<TextView>(R.id.textQueryStatus).text = getString(R.string.diag_query_timeout)
    }

    private fun unbindQuietly() {
        try {
            unbindService(connection)
        } catch (e: IllegalArgumentException) {
            // not bound — fine
        }
        serviceMessenger = null
    }

    /*
     * Cards.
     */

    private fun populate(root: JSONObject) {
        val info = root.optJSONObject("info") ?: JSONObject()
        populateSelftest(root.optJSONObject("selftest"))
        populateRuntime(info)
        populatePlugin(info)
        populateDisplay(info)
        populateModes(info)
        populateEyeTracking(info)
        if (!BuildConfig.inProcess) {
            populateLive(if (root.isNull("live")) null else root.optJSONObject("live"), info)
        }
    }

    private fun populateSelftest(selftest: JSONObject?) {
        val verdictView = findViewById<TextView>(R.id.textSelftestVerdict)
        val checksView = findViewById<TextView>(R.id.textSelftestChecks)
        if (selftest == null) {
            verdictView.text = "?"
            checksView.text = ""
            return
        }
        val pass = selftest.optString("verdict") == "PASS"
        verdictView.text =
            if (pass) "PASS" else "FAIL (rc=${selftest.optInt("result_code", -1)})"
        verdictView.setTextColor(if (pass) 0xFF66BB6A.toInt() else 0xFFEF5350.toInt())

        val checks = selftest.optJSONArray("checks")
        val sb = StringBuilder()
        for (i in 0 until (checks?.length() ?: 0)) {
            val c = checks!!.getJSONObject(i)
            sb.append(if (c.optBoolean("ok")) "✓ " else "✗ ")
                .append(c.optString("name"))
                .append(" — ")
                .append(c.optString("detail"))
            if (i < checks.length() - 1) sb.append('\n')
        }
        checksView.text = sb.toString()
    }

    private fun populateRuntime(info: JSONObject) {
        val rt = info.optJSONObject("runtime") ?: JSONObject()
        findViewById<TextView>(R.id.textRuntime).text =
            buildString {
                append(rt.optString("description", "?")).append('\n')
                append("git: ").append(rt.optString("git_tag", "?")).append('\n')
                append("plug-in ABI: v").append(rt.optInt("plugin_abi_version", 0))
            }
    }

    private fun populatePlugin(info: JSONObject) {
        val view = findViewById<TextView>(R.id.textPlugin)
        val pl = if (info.isNull("plugin")) null else info.optJSONObject("plugin")
        if (pl == null) {
            view.text = "none — no vendor plug-in claimed the system"
            return
        }
        view.text =
            buildString {
                append(pl.optString("display_name", "?")).append('\n')
                append("id: ").append(pl.optString("id", "?"))
                append("  vendor: ").append(pl.optString("vendor", "?")).append('\n')
                append("version: ").append(pl.optString("version", "?"))
                append("  ABI: v").append(pl.optInt("abi_version", 0))
                if (!info.isNull("device")) {
                    append('\n').append("device: ").append(info.optString("device"))
                }
            }
    }

    private fun populateDisplay(info: JSONObject) {
        val view = findViewById<TextView>(R.id.textDisplay)
        val d = if (info.isNull("display")) null else info.optJSONObject("display")
        if (d == null) {
            view.text = "no display info"
            return
        }
        val viewer = d.optJSONObject("viewer_m") ?: JSONObject()
        val isSim = info.optJSONObject("plugin")?.optString("id") == "sim-display"
        view.text =
            buildString {
                append("physical: %.4f m × %.4f m".format(
                    d.optDouble("physical_width_m"), d.optDouble("physical_height_m")))
                append('\n')
                append("pixels:   ${d.optInt("pixel_width")} × ${d.optInt("pixel_height")}")
                if (isSim) append("  (sim: hardcoded on Android)")
                append('\n')
                append("viewer:   (%.3f, %.3f, %.3f) m".format(
                    viewer.optDouble("x"), viewer.optDouble("y"), viewer.optDouble("z")))
            }
    }

    private fun populateModes(info: JSONObject) {
        val view = findViewById<TextView>(R.id.textModes)
        val modes = info.optJSONArray("rendering_modes")
        if (modes == null || modes.length() == 0) {
            view.text = "none advertised"
            return
        }
        val sb = StringBuilder()
        for (i in 0 until modes.length()) {
            val m = modes.getJSONObject(i)
            sb.append("[${m.optInt("mode_index")}] ")
                .append(m.optString("name").padEnd(14))
                .append(" views=${m.optInt("view_count")}")
                .append(if (m.optBoolean("hardware_display_3d")) " 3D" else " 2D")
                .append(if (m.optBoolean("has_tracking")) " tracked" else "")
                .append(if (m.optBoolean("can_rotate")) " rotatable" else "")
            val vs = m.optJSONObject("view_scale")
            if (vs != null) {
                sb.append(" scale=%.2f×%.2f".format(vs.optDouble("x"), vs.optDouble("y")))
            }
            if (i < modes.length() - 1) sb.append('\n')
        }
        view.text = sb.toString()
    }

    private fun populateEyeTracking(info: JSONObject) {
        val view = findViewById<TextView>(R.id.textEyeTracking)
        val et = info.optJSONObject("display")?.optJSONObject("eye_tracking")
        if (et == null) {
            view.text = "no display info"
            return
        }
        view.text =
            buildString {
                append("supported: ").append(et.optString("supported_label", "none")).append('\n')
                append("default:   ").append(et.optString("default_label", "?"))
            }
    }

    private fun populateLive(live: JSONObject?, info: JSONObject) {
        val view = findViewById<TextView>(R.id.textLive)
        if (live == null) {
            view.text = getString(R.string.diag_live_service_not_running)
            return
        }
        val sb = StringBuilder()
        sb.append("service: running")
        if (live.has("workspace_active")) {
            sb.append("  workspace: ")
                .append(if (live.optBoolean("workspace_active")) "active" else "inactive")
        }
        val ds = live.optJSONObject("display_status")
        if (ds != null) {
            if (ds.optBoolean("mode_valid")) {
                val idx = ds.optInt("active_rendering_mode")
                sb.append('\n').append("active mode: [$idx] ${modeName(info, idx)}")
            }
            sb.append('\n').append("eye tracking: ")
            if (ds.optBoolean("tracking_valid")) {
                sb.append(if (ds.optBoolean("is_tracking")) "TRACKING" else "no lock")
            } else {
                sb.append("no DP / nothing rendering")
            }
        }
        val clients = live.optJSONArray("clients")
        var shown = 0
        for (i in 0 until (clients?.length() ?: 0)) {
            val c = clients!!.getJSONObject(i)
            val name = c.optString("name")
            if (name == DIAG_CLIENT_NAME) {
                continue // our own monitor connection
            }
            if (shown++ == 0) sb.append('\n').append("clients:")
            sb.append('\n').append("  ").append(name.ifEmpty { "?" })
                .append(" pid=${c.optInt("pid")}")
            if (c.optBoolean("primary")) sb.append(" primary")
            if (c.optBoolean("focused")) sb.append(" focused")
            else if (c.optBoolean("visible")) sb.append(" visible")
        }
        if (shown == 0) sb.append('\n').append("clients: none")
        view.text = sb.toString()
    }

    private fun modeName(info: JSONObject, idx: Int): String {
        val modes = info.optJSONArray("rendering_modes") ?: return "?"
        for (i in 0 until modes.length()) {
            val m = modes.getJSONObject(i)
            if (m.optInt("mode_index") == idx) {
                return m.optString("name", "?")
            }
        }
        return "?"
    }

    /*
     * Actions.
     */

    private fun copyJson() {
        val json = rawJson ?: return
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("DisplayXR report", prettyJson(json)))
        Toast.makeText(this, R.string.diag_copied, Toast.LENGTH_SHORT).show()
    }

    private fun shareJson() {
        val json = rawJson ?: return
        val intent =
            Intent(Intent.ACTION_SEND).apply {
                type = "text/plain"
                putExtra(Intent.EXTRA_SUBJECT, "DisplayXR runtime report")
                putExtra(Intent.EXTRA_TEXT, prettyJson(json))
            }
        startActivity(Intent.createChooser(intent, getString(R.string.diag_btn_share)))
    }

    private fun prettyJson(json: String): String =
        try {
            JSONObject(json).toString(2)
        } catch (e: Exception) {
            json
        }

    companion object {
        private const val QUERY_TIMEOUT_MS = 30_000L
        private const val DIAG_CLIENT_NAME = "displayxr-diag"
    }
}
