package com.retrodepth.questretrodepth

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import org.apache.commons.compress.archivers.sevenz.SevenZFile
import java.io.File
import java.io.FileOutputStream
import java.util.Locale
import java.util.zip.ZipInputStream

class QuestRetroDepthActivity : Activity() {
    private val sevenZipBufferSize = 8192
    private lateinit var imageView: ImageView
    private lateinit var statusView: TextView
    private lateinit var romView: TextView
    private lateinit var runButton: Button

    private val handler = Handler(Looper.getMainLooper())
    private var frameBitmap: Bitmap? = null
    private var framePixels: IntArray = IntArray(0)
    private var loadedRomFile: File? = null
    private var running = false

    private val frameRunnable = object : Runnable {
        override fun run() {
            if (!running) return
            if (nativeStepFrame()) {
                refreshFrameBitmap()
                handler.postDelayed(this, 16L)
            } else {
                running = false
                syncRunButton()
                statusView.text = nativeGetLastStatus()
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // On Quest, this activity has the VR intent category so Quest shows its loading screen
        // waiting for OpenXR frames — but this is a 2-D activity that never submits any.
        // Redirect immediately to QuestVrActivity, which owns the OpenXR loop.
        startActivity(Intent(this, QuestVrActivity::class.java))
        finish()
    }

    override fun onPause() {
        super.onPause()
        stopRunning()
    }

    override fun onResume() {
        super.onResume()
        refreshLocalRomSummary()
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_OPEN_ROM || resultCode != RESULT_OK) return

        val uri = data?.data ?: return
        runCatching {
            stopRunning()
            loadRomFromFile(copyUriToCache(uri))
        }.onFailure {
            statusView.text = "ROM load failed\n\n${it.message ?: "Unknown error"}"
        }
    }

    private fun buildUi(): LinearLayout {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
            setPadding(dp(16), dp(16), dp(16), dp(16))
        }

        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }

        val openButton = Button(this).apply {
            text = "Open ROM"
            setOnClickListener { openRomPicker() }
        }

        val localButton = Button(this).apply {
            text = "Local ROMs"
            setOnClickListener { openLocalRomChooser() }
        }

        val vrButton = Button(this).apply {
            text = "Enter VR"
            setOnClickListener {
                startActivity(Intent(this@QuestRetroDepthActivity, QuestVrActivity::class.java))
            }
        }

        runButton = Button(this).apply {
            text = "Run"
            setOnClickListener {
                if (running) {
                    stopRunning()
                } else if (loadedRomFile != null) {
                    running = true
                    syncRunButton()
                    handler.post(frameRunnable)
                }
            }
        }

        val stepButton = Button(this).apply {
            text = "Step"
            setOnClickListener {
                stopRunning()
                if (nativeStepFrame()) {
                    refreshFrameBitmap()
                    statusView.text = nativeGetLastStatus()
                }
            }
        }

        controls.addView(openButton)
        controls.addView(localButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { leftMargin = dp(12) })
        controls.addView(vrButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { leftMargin = dp(12) })
        controls.addView(runButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { leftMargin = dp(12) })
        controls.addView(stepButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { leftMargin = dp(12) })

        romView = TextView(this).apply {
            text = "ROM: none"
            textSize = 16f
            setTextColor(Color.WHITE)
            setPadding(0, dp(12), 0, dp(12))
        }

        imageView = ImageView(this).apply {
            setBackgroundColor(Color.rgb(16, 16, 16))
            adjustViewBounds = true
            scaleType = ImageView.ScaleType.FIT_CENTER
        }

        val imageFrame = FrameLayout(this).apply {
            setBackgroundColor(Color.rgb(8, 8, 8))
            addView(imageView, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
                Gravity.CENTER
            ))
        }

        statusView = TextView(this).apply {
            textSize = 15f
            setTextColor(Color.WHITE)
            setPadding(0, dp(12), 0, 0)
        }

        root.addView(controls)
        root.addView(romView)
        root.addView(imageFrame, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1f
        ))
        root.addView(statusView)

        return root
    }

    private fun openRomPicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        startActivityForResult(intent, REQUEST_OPEN_ROM)
    }

    private fun stopRunning() {
        running = false
        handler.removeCallbacks(frameRunnable)
        syncRunButton()
    }

    private fun syncRunButton() {
        runButton.text = if (running) "Pause" else "Run"
    }

    private fun refreshFrameBitmap() {
        val width = nativeGetFrameWidth()
        val height = nativeGetFrameHeight()
        if (width <= 0 || height <= 0) return

        val pixelCount = width * height
        if (framePixels.size != pixelCount) {
            framePixels = IntArray(pixelCount)
        }
        if (!nativeCopyFramePixels(framePixels)) return

        if (frameBitmap == null || frameBitmap?.width != width || frameBitmap?.height != height) {
            frameBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        }

        frameBitmap?.setPixels(framePixels, 0, width, 0, 0, width, height)
        imageView.setImageBitmap(frameBitmap)
    }

    private fun loadRomFromFile(file: File) {
        val prepared = prepareRomFileForLoad(file)
        loadedRomFile = prepared
        romView.text = "ROM: ${prepared.name}"
        val result = nativeLoadRom(prepared.absolutePath)
        statusView.text = result
        if (result.lowercase(Locale.US).contains("loaded")) {
            repeat(2) { nativeStepFrame() }
            refreshFrameBitmap()
            running = true
            syncRunButton()
            handler.post(frameRunnable)
        }
    }

    private fun copyUriToCache(uri: Uri): File {
        val displayName = queryDisplayName(uri)
        val cacheDir = File(cacheDir, "roms").apply { mkdirs() }
        val target = File(cacheDir, sanitizeFileName(displayName))

        contentResolver.openInputStream(uri)?.use { input ->
            if (displayName.lowercase(Locale.US).endsWith(".zip")) {
                return extractZipRomToFile(input.readBytes(), target)
            } else {
                FileOutputStream(target).use { output -> input.copyTo(output) }
            }
        } ?: error("Unable to open selected document")

        return target
    }

    private fun prepareRomFileForLoad(file: File): File {
        val extension = file.extension.lowercase(Locale.US)
        if (extension == "zip") {
            val targetHint = File(File(cacheDir, "roms").apply { mkdirs() }, file.name)
            return extractZipRomToFile(file.readBytes(), targetHint)
        }
        if (extension == "7z") {
            val targetDir = File(cacheDir, "roms").apply { mkdirs() }
            return extract7zRomToFile(file, targetDir)
        }
        return file
    }

    private fun extractZipRomToFile(zipBytes: ByteArray, targetHint: File): File {
        ZipInputStream(zipBytes.inputStream()).use { zip ->
            var entry = zip.nextEntry
            while (entry != null) {
                if (!entry.isDirectory) {
                    val lower = entry.name.lowercase(Locale.US)
                    if (isSupportedRomExtension(lower)) {
                        val outName = sanitizeFileName(entry.name.substringAfterLast('/'))
                        val outFile = File(targetHint.parentFile, outName)
                        FileOutputStream(outFile).use { output -> zip.copyTo(output) }
                        return outFile
                    }
                }
                entry = zip.nextEntry
            }
        }
        error("ZIP does not contain a supported ROM")
    }

    private fun extract7zRomToFile(archiveFile: File, targetDir: File): File {
        SevenZFile(archiveFile).use { sevenZ ->
            var entry = sevenZ.nextEntry
            val buffer = ByteArray(sevenZipBufferSize)
            while (entry != null) {
                if (!entry.isDirectory) {
                    val lower = entry.name.lowercase(Locale.US)
                    if (isSupportedRomExtension(lower)) {
                        val outName = sanitizeFileName(entry.name.substringAfterLast('/').substringAfterLast('\\'))
                        val outFile = File(targetDir, outName)
                        FileOutputStream(outFile).use { output ->
                            var remaining = entry.size
                            while (remaining > 0) {
                                val toRead = minOf(buffer.size.toLong(), remaining).toInt()
                                val read = sevenZ.read(buffer, 0, toRead)
                                if (read <= 0) break
                                output.write(buffer, 0, read)
                                remaining -= read.toLong()
                            }
                        }
                        return outFile
                    }
                }
                entry = sevenZ.nextEntry
            }
        }
        error("7z does not contain a supported ROM")
    }

    private fun queryDisplayName(uri: Uri): String {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (index >= 0) {
                    return cursor.getString(index)
                }
            }
        }
        return uri.lastPathSegment ?: "selected_rom.sfc"
    }

    private fun openLocalRomChooser() {
        val roms = localRomDirectory()
            .listFiles()
            ?.filter { it.isFile && isSupportedRomFile(it) }
            ?.sortedBy { it.name.lowercase(Locale.US) }
            .orEmpty()

        if (roms.isEmpty()) {
            statusView.text = "No local ROMs found.\n\nExpected folder:\n${localRomDirectory().absolutePath}"
            return
        }

        val labels = roms.map { it.name }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("Local ROMs")
            .setItems(labels) { _, which ->
                runCatching {
                    stopRunning()
                    loadRomFromFile(roms[which])
                }.onFailure {
                    statusView.text = "ROM load failed\n\n${it.message ?: "Unknown error"}"
                }
            }
            .show()
    }

    private fun refreshLocalRomSummary() {
        val count = localRomDirectory()
            .listFiles()
            ?.count { it.isFile && isSupportedRomFile(it) }
            ?: 0

        if (loadedRomFile == null) {
            romView.text = if (count > 0) {
                "ROM: none (${count} local ROMs found)"
            } else {
                "ROM: none"
            }
        }
    }

    private fun localRomDirectory(): File {
        return File(getExternalFilesDir(null), "roms")
    }

    private fun isSupportedRomFile(file: File): Boolean {
        val lower = file.name.lowercase(Locale.US)
        return isSupportedRomExtension(lower) ||
            lower.endsWith(".zip") ||
            lower.endsWith(".7z")
    }

    private fun isSupportedRomExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".smc") ||
            lower.endsWith(".sfc") ||
            lower.endsWith(".fig") ||
            lower.endsWith(".swc") ||
            lower.endsWith(".md") ||
            lower.endsWith(".bin") ||
            lower.endsWith(".gen") ||
            lower.endsWith(".smd")
    }

    private fun sanitizeFileName(name: String): String {
        return name.replace(Regex("[^A-Za-z0-9._ -]"), "_")
    }

    private fun dp(value: Int): Int {
        return (value * resources.displayMetrics.density).toInt()
    }

    private external fun nativeGetBuildInfo(): String
    private external fun nativeLoadRom(path: String): String
    private external fun nativeStepFrame(): Boolean
    private external fun nativeGetFrameWidth(): Int
    private external fun nativeGetFrameHeight(): Int
    private external fun nativeCopyFramePixels(outPixels: IntArray): Boolean
    private external fun nativeGetLastStatus(): String

    companion object {
        private const val REQUEST_OPEN_ROM = 1001

        init {
            System.loadLibrary("questretrodepth_native")
        }
    }
}
