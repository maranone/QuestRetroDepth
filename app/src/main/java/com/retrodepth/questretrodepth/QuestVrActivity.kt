package com.retrodepth.questretrodepth

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import android.view.Gravity
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import org.apache.commons.compress.archivers.sevenz.SevenZFile
import java.io.File
import java.io.FileOutputStream
import java.util.Locale
import java.util.zip.ZipInputStream

class QuestVrActivity : Activity() {
    private lateinit var statusView: TextView
    private val handler = Handler(Looper.getMainLooper())
    private var vrStarted = false

    private var lastSavedRomFilename = ""
    private val prefs by lazy { getSharedPreferences("qrd_prefs", MODE_PRIVATE) }

    private val statusPoll = object : Runnable {
        override fun run() {
            statusView.text = nativeGetVrStatus()
            // Persist the last ROM loaded from the browser panel
            val fn = nativeGetLastLoadedRomFilename()
            if (fn.isNotEmpty() && fn != lastSavedRomFilename) {
                lastSavedRomFilename = fn
                prefs.edit().putString("last_rom_path", fn).apply()
            }
            handler.postDelayed(this, 250L)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(24, 24, 24, 24)
        }

        statusView = TextView(this).apply {
            textSize = 16f
            setTextColor(Color.WHITE)
            text = "Starting OpenXR shell..."
        }

        val randomizeBtn = Button(this).apply {
            text = "✦ RANDOMIZE"
            setOnClickListener { nativeRandomize() }
        }

        val presetRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
        }
        for (i in 0 until 5) {
            val btn = Button(this).apply {
                text = "P${i + 1}"
                textSize = 12f
                setOnClickListener { nativeLoadPreset(i) }
                setOnLongClickListener { nativeSavePreset(i); true }
            }
            presetRow.addView(btn, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        }

        val presetHint = TextView(this).apply {
            text = "Tap preset to load  |  Long-press to save"
            textSize = 11f
            setTextColor(Color.GRAY)
            gravity = Gravity.CENTER
        }


        root.addView(randomizeBtn, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT))
        root.addView(presetRow, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT))
        root.addView(presetHint, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT))
        root.addView(statusView, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = 16 })

        val wrapper = FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
            addView(root, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP))
        }
        setContentView(wrapper)
    }

    override fun onResume() {
        super.onResume()
        // Require external storage access before starting VR so getRomDirectory() can
        // create /storage/emulated/0/QuestRetroDepth/ and find ROMs inside it.
        if (!hasStoragePermission()) {
            requestStoragePermission()
            return  // don't start VR yet — wait for permission
        }
        if (!vrStarted) {
            vrStarted = true
            statusView.text = nativeStartVr(this)
            if (ENABLE_STARTUP_AUTO_LOAD) {
                autoLoadFirstRom()
            }
        }
        handler.post(statusPoll)
    }

    // Called when the user returns from the API 30+ "All Files Access" settings screen.
    // onResume() won't fire in that case, so we re-check here.
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus && !vrStarted) onResume()
    }

    private fun hasStoragePermission(): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Environment.isExternalStorageManager()
        } else {
            checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
                android.content.pm.PackageManager.PERMISSION_GRANTED
        }
    }

    private fun requestStoragePermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            startActivity(Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:$packageName")))
        } else {
            requestPermissions(arrayOf(
                android.Manifest.permission.READ_EXTERNAL_STORAGE,
                android.Manifest.permission.WRITE_EXTERNAL_STORAGE), 1001)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>,
                                            grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 1001) onResume()
    }

    private fun autoLoadFirstRom() {
        val lastRomPath = prefs.getString("last_rom_path", null)

        val romDir = File(getRomDirectory())
        Log.i("QuestRetroDepthXR", "autoLoadFirstRom: dir=${romDir.absolutePath} exists=${romDir.exists()}")

        // Collect all ROM/archive files from romDir and one level of subdirectories.
        val allFiles = mutableListOf<File>()
        romDir.listFiles()?.forEach { entry ->
            if (entry.isFile && isSupportedOrArchiveFile(entry)) {
                allFiles += entry
            } else if (entry.isDirectory) {
                entry.listFiles()?.filter { it.isFile && isSupportedOrArchiveFile(it) }
                    ?.let { allFiles += it }
            }
        }
        Log.i("QuestRetroDepthXR", "autoLoadFirstRom: found ${allFiles.size} ROM(s)")

        // Prefer the last successfully played ROM (stored as filename only); fall back to first alphabetically.
        val candidate = if (lastRomPath != null) {
            val lastName = lastRomPath.substringAfterLast('/')
            allFiles.firstOrNull { it.name == lastName }
                ?: allFiles.minByOrNull { it.name.lowercase(Locale.US) }
        } else {
            allFiles.minByOrNull { it.name.lowercase(Locale.US) }
        }

        Log.i("QuestRetroDepthXR", "autoLoadFirstRom: chosen=${candidate?.name ?: "none"} (last=$lastRomPath)")
        if (candidate == null) return

        val romFile = runCatching { prepareRomFile(candidate) }.getOrElse {
            Log.e("QuestRetroDepthXR", "autoLoadFirstRom: extraction failed", it)
            statusView.text = "ROM extract failed: ${it.message}"
            return
        }

        Log.i("QuestRetroDepthXR", "autoLoadFirstRom: loading ${romFile.absolutePath}")
        val result = nativeLoadRom(romFile.absolutePath)
        Log.i("QuestRetroDepthXR", "autoLoadFirstRom: result=$result")
        statusView.text = result
        if (!result.startsWith("ROM load failed")) {
            lastSavedRomFilename = candidate.name
            prefs.edit().putString("last_rom_path", candidate.name).apply()
        }
    }

    /** If the file is a raw ROM, return it as-is. If it's an archive, extract the ROM inside. */
    private fun prepareRomFile(file: File): File {
        val lower = file.name.lowercase(Locale.US)
        val cacheDir = File(cacheDir, "roms").apply { mkdirs() }
        return when {
            lower.endsWith(".zip") -> extractZipRom(file, cacheDir, preferredRomFamily(file))
            lower.endsWith(".7z")  -> extract7zRom(file, cacheDir, preferredRomFamily(file))
            else                   -> file
        }
    }

    private fun extractZipRom(archive: File, targetDir: File, preferredFamily: RomFamily?): File {
        ZipInputStream(archive.inputStream()).use { zip ->
            var entry = zip.nextEntry
            var fallback: File? = null
            while (entry != null) {
                if (!entry.isDirectory && isSupportedRomExtension(entry.name)) {
                    val out = File(targetDir, sanitize(entry.name.substringAfterLast('/')))
                    FileOutputStream(out).use { zip.copyTo(it) }
                    if (preferredFamily == null || romFamilyForName(entry.name) == preferredFamily) {
                        return out
                    }
                    if (fallback == null) fallback = out
                }
                entry = zip.nextEntry
            }
            fallback?.let { return it }
        }
        error("ZIP contains no supported ROM: ${archive.name}")
    }

    private fun extract7zRom(archive: File, targetDir: File, preferredFamily: RomFamily?): File {
        SevenZFile(archive).use { sz ->
            val buf = ByteArray(8192)
            var entry = sz.nextEntry
            var fallback: File? = null
            while (entry != null) {
                if (!entry.isDirectory && isSupportedRomExtension(entry.name)) {
                    val out = File(targetDir, sanitize(
                        entry.name.substringAfterLast('/').substringAfterLast('\\')))
                    FileOutputStream(out).use { fos ->
                        var rem = entry.size
                        while (rem > 0) {
                            val n = sz.read(buf, 0, minOf(buf.size.toLong(), rem).toInt())
                            if (n <= 0) break
                            fos.write(buf, 0, n)
                            rem -= n
                        }
                    }
                    if (preferredFamily == null || romFamilyForName(entry.name) == preferredFamily) {
                        return out
                    }
                    if (fallback == null) fallback = out
                }
                entry = sz.nextEntry
            }
            fallback?.let { return it }
        }
        error("7z contains no supported ROM: ${archive.name}")
    }

    private fun isSnesExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".smc") || lower.endsWith(".sfc") ||
               lower.endsWith(".fig") || lower.endsWith(".swc")
    }

    private fun isGenesisExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".md") || lower.endsWith(".bin") ||
               lower.endsWith(".gen") || lower.endsWith(".smd")
    }

    private fun isSupportedRomExtension(name: String): Boolean {
        return isSnesExtension(name) || isGenesisExtension(name)
    }

    private fun isSupportedOrArchiveFile(file: File): Boolean {
        val lower = file.name.lowercase(Locale.US)
        return isSupportedRomExtension(lower) || lower.endsWith(".zip") || lower.endsWith(".7z")
    }

    private fun romFamilyForName(name: String): RomFamily? = when {
        isSnesExtension(name) -> RomFamily.Snes
        isGenesisExtension(name) -> RomFamily.Genesis
        else -> null
    }

    private fun preferredRomFamily(file: File): RomFamily? {
        val path = file.absolutePath.lowercase(Locale.US)
        return when {
            path.contains("/roms/snes/") || path.contains("\\roms\\snes\\") -> RomFamily.Snes
            path.contains("/roms/genesis/") || path.contains("\\roms\\genesis\\") -> RomFamily.Genesis
            else -> romFamilyForName(file.name)
        }
    }

    private fun sanitize(name: String) = name.replace(Regex("[^A-Za-z0-9._ -]"), "_")

    override fun onPause() {
        handler.removeCallbacks(statusPoll)
        super.onPause()
    }

    override fun onDestroy() {
        nativeStopVr()
        vrStarted = false
        super.onDestroy()
    }

    private external fun nativeStartVr(activity: Activity): String
    private external fun nativeGetVrStatus(): String
    private external fun nativeStopVr()
    private external fun nativeRandomize()
    private external fun nativeLoadPreset(idx: Int)
    private external fun nativeSavePreset(idx: Int)
    private external fun nativeGetVrStateSummary(): String
    private external fun nativeLoadRom(path: String): String
    private external fun nativeGetLastLoadedRomFilename(): String
    private external fun nativeApplyStateCode(code: String): Boolean

    // -----------------------------------------------------------------------
    // Called from C++ GL thread to render the ROM browser panel texture.
    // Returns ARGB_8888 pixel array of size width×height.
    // -----------------------------------------------------------------------
    fun renderRomPanelBitmap(
        romNames: Array<String>,
        isDir: BooleanArray,
        hoveredIdx: Int,
        width: Int,
        height: Int,
        hasMoreUp: Boolean,
        hasMoreDown: Boolean
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(
            width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        // Panel background
        paint.color = android.graphics.Color.argb(235, 12, 12, 22)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        // Title bar
        paint.color = android.graphics.Color.argb(255, 28, 45, 78)
        canvas.drawRect(0f, 0f, width.toFloat(), 88f, paint)
        paint.color = android.graphics.Color.WHITE
        paint.textSize = 48f
        paint.isFakeBoldText = true
        canvas.drawText("ROM Browser", 14f, 60f, paint)
        paint.isFakeBoldText = false

        // Scroll arrows
        if (hasMoreUp) {
            paint.color = android.graphics.Color.argb(200, 160, 200, 255)
            paint.textSize = 40f
            canvas.drawText("▲ scroll up", width - 260f, 60f, paint)
        }

        val titleH = 88f
        val rowH = if (romNames.isEmpty()) (height - titleH)
                   else ((height - titleH) / romNames.size.coerceAtLeast(1)).toFloat()

        if (romNames.isEmpty()) {
            paint.color = android.graphics.Color.argb(180, 150, 150, 160)
            paint.textSize = 40f
            canvas.drawText("No ROMs found in roms/ root", 14f, titleH + 80f, paint)
        } else {
            paint.textSize = rowH.coerceIn(28f, 56f) * 0.58f
            for (i in romNames.indices) {
                val y = titleH + i * rowH
                val entryIsDir = i < isDir.size && isDir[i]
                // Alternating row backgrounds only (hover highlight is a separate GL quad)
                if (i % 2 == 0) {
                    paint.color = android.graphics.Color.argb(80, 35, 35, 55)
                    canvas.drawRect(0f, y, width.toFloat(), y + rowH, paint)
                }
                // Folder icon prefix
                val label = if (entryIsDir) "\uD83D\uDCC1 ${romNames[i]}" else romNames[i]
                paint.color = when {
                    entryIsDir -> android.graphics.Color.argb(215, 140, 220, 200)
                    else       -> android.graphics.Color.argb(215, 190, 195, 210)
                }
                canvas.drawText(label, 14f, y + rowH * 0.68f, paint)
            }
        }

        // "more below" hint
        if (hasMoreDown) {
            paint.color = android.graphics.Color.argb(200, 160, 200, 255)
            paint.textSize = 36f
            canvas.drawText("▼ more", width / 2f - 60f, height - 12f, paint)
        }

        val pixels = IntArray(width * height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    // -----------------------------------------------------------------------
    // Called from C++ GL thread — renders the layer order panel.
    // layerNames are in display order (top = nearest). grabbed = dragged row (-1 = none).
    // frozen: true = paused (game frozen), false = playing.
    // Returns ARGB_8888 pixels, width×height.
    // -----------------------------------------------------------------------
    fun renderLayerPanelBitmap(
        layerNames: Array<String>,
        layerEnabled: BooleanArray,
        layerAmbilight: BooleanArray,
        grabbed: Int,       // row being dragged (-1 = none)
        dropTarget: Int,   // row laser is pointing at while dragging (-1 = none)
        width: Int,
        height: Int,
        frozen: Boolean,   // frozen state for play/pause button
        autoDupLabel: String,
        filterLabel: String
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint  = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        // Background
        paint.color = android.graphics.Color.argb(235, 14, 14, 24)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        // Title bar
        paint.color = android.graphics.Color.argb(255, 30, 50, 90)
        canvas.drawRect(0f, 0f, width.toFloat(), 88f, paint)
        paint.color = android.graphics.Color.WHITE
        paint.textSize = 44f
        paint.isFakeBoldText = true
        canvas.drawText("Layers  (near→far)", 12f, 60f, paint)
        paint.isFakeBoldText = false

        val titleH = 88f
        val n      = layerNames.size
        val totalRows = n + 3  // layers + play/pause + auto-dup + filter row
        if (n == 0) {
            paint.color = android.graphics.Color.argb(160, 150, 150, 160)
            paint.textSize = 36f
            canvas.drawText("No layers", 14f, titleH + 72f, paint)
        }

        val rowH = ((height - titleH) / totalRows).toFloat().coerceAtLeast(56f)
        paint.textSize = rowH * 0.46f

        val isDragging = (grabbed >= 0)

        // Render layer rows (indices 0 to n-1)
        for (i in 0 until n) {
            val y       = titleH + i * rowH
            val enabled = if (i < layerEnabled.size) layerEnabled[i] else true
            val isGrabbed = (i == grabbed)

            // Row background
            when {
                isGrabbed -> {
                    paint.color = android.graphics.Color.argb(100, 60, 120, 220)
                    canvas.drawRect(2f, y + 1f, width - 2f, y + rowH - 1f, paint)
                }
                i % 2 == 0 -> {
                    paint.color = android.graphics.Color.argb(70, 40, 40, 65)
                    canvas.drawRect(0f, y, width.toFloat(), y + rowH, paint)
                }
            }

            // Drag handle (≡)
            paint.color = android.graphics.Color.argb(if (isGrabbed) 60 else 120, 180, 180, 200)
            canvas.drawText("≡", 8f, y + rowH * 0.70f, paint)

            // Layer name
            paint.color = when {
                isGrabbed   -> android.graphics.Color.argb(100, 180, 190, 220)
                !enabled    -> android.graphics.Color.argb(110, 150, 150, 160)
                else        -> android.graphics.Color.argb(220, 200, 210, 230)
            }
            canvas.save()
            canvas.clipRect(32f, y, width * 0.58f, y + rowH)
            canvas.drawText(layerNames[i], 32f, y + rowH * 0.70f, paint)
            canvas.restore()

            // Visibility toggle and ambilight toggle
            if (!isGrabbed) {
                val visX  = width * 0.60f
                val ambX  = width * 0.80f
                val ambiEnabled = if (i < layerAmbilight.size) layerAmbilight[i] else true

                paint.color = if (enabled) android.graphics.Color.argb(200, 80, 200, 100)
                              else android.graphics.Color.argb(120, 180, 60, 60)
                canvas.drawRoundRect(visX, y + rowH * 0.18f, ambX - 4f, y + rowH * 0.82f, 6f, 6f, paint)
                paint.color = android.graphics.Color.WHITE
                paint.textSize = rowH * 0.36f
                canvas.drawText(if (enabled) "ON" else "OFF", visX + 4f, y + rowH * 0.68f, paint)

                paint.color = if (ambiEnabled) android.graphics.Color.argb(200, 255, 170, 50)
                              else android.graphics.Color.argb(100, 100, 100, 100)
                canvas.drawRoundRect(ambX, y + rowH * 0.18f, width - 4f, y + rowH * 0.82f, 6f, 6f, paint)
                paint.color = android.graphics.Color.WHITE
                canvas.drawText("AMB", ambX + 4f, y + rowH * 0.68f, paint)

                paint.textSize = rowH * 0.46f
            }

            // Drop-target indicator
            if (isDragging && i == dropTarget && dropTarget != grabbed) {
                paint.color = android.graphics.Color.argb(255, 80, 200, 255)
                paint.strokeWidth = 3f
                paint.style = android.graphics.Paint.Style.STROKE
                canvas.drawLine(4f, y, width - 4f, y, paint)
                paint.style = android.graphics.Paint.Style.FILL
                paint.strokeWidth = 0f
            }
        }

        // Play/Pause button as last row (index n)
        val pauseY = titleH + n * rowH
        val isPauseHovered = (dropTarget == n)  // use dropTarget for hover state

        // Row background
        paint.color = if (isPauseHovered) android.graphics.Color.argb(100, 100, 140, 200)
                      else android.graphics.Color.argb(70, 40, 40, 65)
        canvas.drawRect(0f, pauseY, width.toFloat(), pauseY + rowH, paint)

        // Play/Pause icon and text
        paint.color = if (frozen) android.graphics.Color.argb(220, 80, 160, 255)  // blue when paused
                      else android.graphics.Color.argb(220, 60, 180, 80)   // green when playing
        val iconLabel = if (frozen) "\u25B6 PLAY" else "\u275A\u275A PAUSED"
        paint.textSize = rowH * 0.50f
        val labelW = paint.measureText(iconLabel)
        val labelX = (width - labelW) / 2
        canvas.drawText(iconLabel, labelX, pauseY + rowH * 0.68f, paint)

        // Auto duplication row after play/pause (index n + 1)
        val autoDupY = titleH + (n + 1) * rowH
        val isAutoDupHovered = (dropTarget == n + 1)
        paint.color = if (isAutoDupHovered) android.graphics.Color.argb(100, 100, 140, 200)
                      else android.graphics.Color.argb(70, 40, 40, 65)
        canvas.drawRect(0f, autoDupY, width.toFloat(), autoDupY + rowH, paint)

        paint.textSize = rowH * 0.42f
        paint.color = android.graphics.Color.argb(220, 210, 220, 240)
        canvas.drawText("AUTO DUP", 16f, autoDupY + rowH * 0.68f, paint)

        paint.color = if (autoDupLabel == "OFF") android.graphics.Color.argb(180, 160, 170, 180)
                      else android.graphics.Color.argb(220, 255, 205, 100)
        val autoDupW = paint.measureText(autoDupLabel)
        canvas.drawText(autoDupLabel, width - autoDupW - 16f, autoDupY + rowH * 0.68f, paint)

        // Layer filter row after auto-dup (index n + 2)
        val filterY = titleH + (n + 2) * rowH
        val isFilterHovered = (dropTarget == n + 2)
        paint.color = if (isFilterHovered) android.graphics.Color.argb(100, 100, 140, 200)
                      else android.graphics.Color.argb(70, 40, 40, 65)
        canvas.drawRect(0f, filterY, width.toFloat(), filterY + rowH, paint)

        paint.textSize = rowH * 0.42f
        paint.color = android.graphics.Color.argb(220, 210, 220, 240)
        canvas.drawText("LAYER FILTER", 16f, filterY + rowH * 0.68f, paint)

        paint.color = android.graphics.Color.argb(220, 120, 210, 255)
        val filterW = paint.measureText(filterLabel)
        canvas.drawText(filterLabel, width - filterW - 16f, filterY + rowH * 0.68f, paint)

        val pixels = IntArray(width * height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    // -----------------------------------------------------------------------
    // Called from C++ GL thread — renders the settings panel.
    // names/values/isBoolean are parallel arrays.
    // hoveredRow / area: 0=none, 1=minus, 2=plus.
    // Returns ARGB_8888 pixels, width×height.
    // -----------------------------------------------------------------------
    fun renderSettingsPanelBitmap(
        names: Array<String>,
        values: Array<String>,
        isBoolean: BooleanArray,
        hoveredRow: Int,
        area: Int,
        width: Int,
        height: Int,
        shareCode: String
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint  = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        // Background
        paint.color = android.graphics.Color.argb(235, 12, 14, 24)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        // Title bar
        paint.color = android.graphics.Color.argb(255, 30, 60, 50)
        canvas.drawRect(0f, 0f, width.toFloat(), 88f, paint)
        paint.color = android.graphics.Color.WHITE
        paint.textSize = 44f
        paint.isFakeBoldText = true
        canvas.drawText("Settings", 12f, 60f, paint)
        paint.isFakeBoldText = false
        // Share code (right side of title bar, monospace-style)
        if (shareCode.isNotEmpty()) {
            paint.textSize = 28f
            paint.color = android.graphics.Color.argb(200, 140, 230, 180)
            val codeLabel = "CODE: $shareCode"
            val tw = paint.measureText(codeLabel)
            canvas.drawText(codeLabel, width - tw - 8f, 56f, paint)
        }

        val titleH  = 88f
        val n       = names.size
        val rowH    = if (n > 0) ((height - titleH) / n).toFloat().coerceIn(52f, 96f) else 72f
        val btnW    = width * 0.20f  // left/right button zones

        for (i in 0 until n) {
            val y         = titleH + i * rowH
            val isBool    = if (i < isBoolean.size) isBoolean[i] else false
            val value     = if (i < values.size) values[i] else ""
            val isOn      = (value == "ON")

            // Row background (alternating rows only; hover highlight is a separate GL quad)
            if (i % 2 == 0) {
                paint.color = android.graphics.Color.argb(60, 35, 40, 60)
                canvas.drawRect(0f, y, width.toFloat(), y + rowH, paint)
            }

            paint.textSize = rowH * 0.44f

            val isAction = (value == "ACTION")

            // Draw separator before action buttons (row 10) and before ctrlmap button (row 15)
            if (i == 10 || i == 15) {
                paint.color = android.graphics.Color.argb(120, 100, 130, 200)
                paint.strokeWidth = 2f
                paint.style = android.graphics.Paint.Style.STROKE
                canvas.drawLine(8f, y + 1f, width - 8f, y + 1f, paint)
                paint.style = android.graphics.Paint.Style.FILL
                paint.strokeWidth = 0f
            }

            if (isAction) {
                // Full-width action button; ctrlmap button (row 15) uses purple tint
                val isCtrlMap = (i == 15)
                paint.color = if (isCtrlMap) android.graphics.Color.argb(160, 60, 35, 130)
                              else           android.graphics.Color.argb(140, 40,  70, 120)
                canvas.drawRoundRect(6f, y + rowH * 0.12f, width - 6f, y + rowH * 0.88f, 8f, 8f, paint)
                paint.color = if (isCtrlMap) android.graphics.Color.argb(255, 200, 160, 255)
                              else android.graphics.Color.WHITE
                paint.textSize = rowH * 0.46f
                canvas.drawText(if (i < names.size) names[i] else "", 18f, y + rowH * 0.68f, paint)
            } else if (isBool) {
                // Name on left
                paint.color = android.graphics.Color.argb(215, 190, 200, 220)
                canvas.drawText(if (i < names.size) names[i] else "", 12f, y + rowH * 0.68f, paint)
                // ON/OFF badge on right
                val badgeX = width * 0.68f
                paint.color = if (isOn) android.graphics.Color.argb(210, 50, 180, 90)
                              else      android.graphics.Color.argb(150, 160, 50, 50)
                canvas.drawRoundRect(badgeX, y + rowH * 0.18f, width - 8f, y + rowH * 0.82f, 8f, 8f, paint)
                paint.color = android.graphics.Color.WHITE
                paint.textSize = rowH * 0.40f
                canvas.drawText(value, badgeX + 8f, y + rowH * 0.68f, paint)
            } else {
                // [−] name [value] [+] layout
                // Left minus button (static color — hover highlight is a GL quad)
                paint.color = android.graphics.Color.argb(120, 100, 60, 50)
                canvas.drawRoundRect(4f, y + rowH * 0.18f, btnW - 2f, y + rowH * 0.82f, 6f, 6f, paint)
                paint.color = android.graphics.Color.WHITE
                paint.textSize = rowH * 0.54f
                canvas.drawText("−", btnW * 0.22f, y + rowH * 0.70f, paint)

                // Name + value (centre)
                paint.textSize = rowH * 0.40f
                paint.color    = android.graphics.Color.argb(215, 190, 200, 220)
                canvas.drawText(if (i < names.size) names[i] else "", btnW + 6f, y + rowH * 0.52f, paint)
                paint.color = android.graphics.Color.argb(255, 120, 210, 255)
                canvas.drawText(value, btnW + 6f, y + rowH * 0.88f, paint)

                // Right plus button (static color — hover highlight is a GL quad)
                paint.color = android.graphics.Color.argb(120, 40, 100, 70)
                canvas.drawRoundRect(width - btnW + 2f, y + rowH * 0.18f, width - 4f, y + rowH * 0.82f, 6f, 6f, paint)
                paint.color = android.graphics.Color.WHITE
                paint.textSize = rowH * 0.54f
                canvas.drawText("+", width - btnW + btnW * 0.22f, y + rowH * 0.70f, paint)
            }
        }

        val pixels = IntArray(width * height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    // -----------------------------------------------------------------------
    // Called from C++ GL thread — renders the code-input panel (above ROM browser).
    // currentInput: chars typed so far. hoveredKey: 0-35=alphanum, 36=⌫, -1=none.
    // Layout: title area (current code) + 4 key rows:
    //   Title: current share code (large) + Enter Code label
    //   Row 0: 0-9   (keys 0-9)
    //   Row 1: A-J   (keys 10-19)
    //   Row 2: K-T   (keys 20-29)
    //   Row 3: U-Z+⌫ (keys 30-36; cols 0-5=U-Z, col 6=⌫)
    // -----------------------------------------------------------------------
    fun renderCodePanelBitmap(
        currentInput: String,
        currentShareCode: String,
        hoveredKey: Int,
        width: Int,
        height: Int
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        // Background
        paint.color = android.graphics.Color.argb(235, 10, 16, 28)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        // Title area (80 px) - shows current share code + label
        val titleH = 80f
        paint.color = android.graphics.Color.argb(255, 20, 50, 80)
        canvas.drawRect(0f, 0f, width.toFloat(), titleH, paint)

        // Current share code (large, left side)
        paint.color = android.graphics.Color.argb(255, 100, 200, 255)
        paint.textSize = 36f
        paint.isFakeBoldText = true
        val codeDisplay = if (currentShareCode.isNotEmpty()) currentShareCode.take(16) else "(no code)"
        canvas.drawText(codeDisplay, 12f, 32f, paint)
        paint.isFakeBoldText = false

        // "Enter Code" label (right side)
        paint.color = android.graphics.Color.argb(200, 180, 180, 180)
        paint.textSize = 20f
        canvas.drawText("Type to enter:", width - 160f, 28f, paint)

        // Input buffer display (right side of title bar)
        val displayStr = if (currentInput.isEmpty()) "______" else currentInput
        paint.textSize = 28f
        paint.color = android.graphics.Color.argb(255, 120, 230, 160)
        val tw = paint.measureText(displayStr)
        canvas.drawText(displayStr, width - tw - 12f, 64f, paint)

        // Key grid
        val cols = 10
        val rows = 4
        val keyAreaH = height - titleH
        val keyH = keyAreaH / rows
        val keyW = width.toFloat() / cols
        val keys = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ⌫"

        for (row in 0 until rows) {
            val y = titleH + row * keyH
            val colCount = if (row < 3) cols else 7
            for (col in 0 until colCount) {
                val keyIdx = when (row) {
                    0    -> col
                    1    -> 10 + col
                    2    -> 20 + col
                    else -> if (col < 6) 30 + col else 36
                }
                val isBackspace = (keyIdx == 36)
                val x = col * keyW

                paint.color = when {
                    isBackspace  -> android.graphics.Color.argb(140, 140, 50, 50)
                    row % 2 == 0 -> android.graphics.Color.argb(100, 30, 50, 80)
                    else         -> android.graphics.Color.argb(70, 20, 35, 60)
                }
                canvas.drawRoundRect(x + 2f, y + 2f, x + keyW - 2f, y + keyH - 2f, 6f, 6f, paint)

                val label = keys[keyIdx].toString()
                paint.color = android.graphics.Color.argb(210, 190, 205, 225)
                paint.textSize = keyH * 0.52f
                val lw = paint.measureText(label)
                canvas.drawText(label, x + (keyW - lw) * 0.5f, y + keyH * 0.68f, paint)
            }
        }

        val pixels = IntArray(width * height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    // -----------------------------------------------------------------------
    // Called from C++ GL thread — renders the controller mapping panel.
    // buttonNames: 12 emulated button names. questNames: 12 current Quest bindings.
    // hoveredRow: row under laser (-1=none). selectedRow: row being remapped (-1=none).
    // Bottom 6 rows after the 12 button rows are action buttons:
    //   Reset, Load Game, Load Global, Save Game, Save Global, Back
    // Returns ARGB_8888 pixels, width×height.
    // -----------------------------------------------------------------------
    fun renderCtrlMapPanelBitmap(
        title: String,
        buttonNames: Array<String>,
        questNames: Array<String>,
        hoveredRow: Int,
        selectedRow: Int,
        width: Int,
        height: Int
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint  = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        // Background
        paint.color = android.graphics.Color.argb(235, 14, 12, 24)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        // Title bar
        val titleH = 88f
        paint.color = android.graphics.Color.argb(255, 50, 30, 80)
        canvas.drawRect(0f, 0f, width.toFloat(), titleH, paint)
        paint.color = android.graphics.Color.WHITE
        paint.textSize = 44f
        paint.isFakeBoldText = true
        canvas.drawText(title, 12f, 60f, paint)
        paint.isFakeBoldText = false

        val hint = if (selectedRow >= 0) "Use stick to change binding" else "Tap a button to remap"
        paint.textSize = 26f
        paint.color = android.graphics.Color.argb(180, 160, 200, 255)
        val hw = paint.measureText(hint)
        canvas.drawText(hint, width - hw - 8f, 56f, paint)

        val n = buttonNames.size // 12
        val actionLabels = arrayOf("Reset Defaults", "Load Game", "Load Global", "Save Game", "Save Global", "← Back")
        val totalRows = n + actionLabels.size
        val rowH = ((height - titleH) / totalRows).toFloat().coerceIn(44f, 80f)

        for (i in 0 until totalRows) {
            val y       = titleH + i * rowH

            if (i < n) {
                // Emulated button mapping row
                val isSelected = (i == selectedRow)

                // Row background (selected or alternating; hover highlight is a separate GL quad)
                val bgColor = when {
                    isSelected -> android.graphics.Color.argb(200, 60, 30, 120)
                    i % 2 == 0 -> android.graphics.Color.argb(60, 35, 35, 55)
                    else       -> 0
                }
                if (bgColor != 0) {
                    paint.color = bgColor
                    canvas.drawRect(0f, y, width.toFloat(), y + rowH, paint)
                }

                // Selection accent bar
                if (isSelected) {
                    paint.color = android.graphics.Color.argb(255, 140, 80, 255)
                    canvas.drawRect(0f, y, 5f, y + rowH, paint)
                }

                paint.textSize = rowH * 0.44f

                // Emulated button name (left column, ~40% width)
                paint.color = if (isSelected) android.graphics.Color.argb(255, 200, 160, 255)
                              else android.graphics.Color.argb(215, 190, 200, 220)
                canvas.drawText(if (i < buttonNames.size) buttonNames[i] else "", 14f, y + rowH * 0.68f, paint)

                // Arrow separator
                paint.color = android.graphics.Color.argb(120, 160, 160, 180)
                paint.textSize = rowH * 0.36f
                canvas.drawText("→", width * 0.42f, y + rowH * 0.68f, paint)

                // Quest binding (right column, highlight if selected)
                paint.textSize = rowH * 0.44f
                val bindingColor = if (isSelected) android.graphics.Color.argb(255, 100, 230, 255)
                                   else android.graphics.Color.argb(220, 90, 200, 255)
                paint.color = bindingColor
                canvas.drawText(if (i < questNames.size) questNames[i] else "---", width * 0.50f, y + rowH * 0.68f, paint)

                // If selected: show cycle hint with ◄►
                if (isSelected) {
                    paint.textSize = rowH * 0.34f
                    paint.color = android.graphics.Color.argb(160, 200, 200, 200)
                    canvas.drawText("◄ stick ►", width - 130f, y + rowH * 0.68f, paint)
                }
            } else {
                // Action button row
                val actionIdx = i - n
                val label = if (actionIdx < actionLabels.size) actionLabels[actionIdx] else ""
                val isBack = (actionIdx == actionLabels.size - 1)

                // Separator before action rows
                if (actionIdx == 0) {
                    paint.color = android.graphics.Color.argb(120, 100, 130, 200)
                    paint.strokeWidth = 2f
                    paint.style = android.graphics.Paint.Style.STROKE
                    canvas.drawLine(8f, y + 1f, width - 8f, y + 1f, paint)
                    paint.style = android.graphics.Paint.Style.FILL
                    paint.strokeWidth = 0f
                }

                val btnColor = if (isBack) android.graphics.Color.argb(120, 35, 55, 45)
                               else        android.graphics.Color.argb(140, 40, 70, 120)
                paint.color = btnColor
                canvas.drawRoundRect(6f, y + rowH * 0.12f, width - 6f, y + rowH * 0.88f, 8f, 8f, paint)
                paint.color = android.graphics.Color.WHITE
                paint.textSize = rowH * 0.44f
                canvas.drawText(label, 18f, y + rowH * 0.68f, paint)
            }
        }

        val pixels = IntArray(width * height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    // Called from C++ XR thread to extract a zip/7z archive and return the ROM path inside.
    fun prepareRomFileForNative(rawPath: String): String =
        runCatching { prepareRomFile(File(rawPath)).absolutePath }.getOrElse { rawPath }

    // -----------------------------------------------------------------------
    // Called from C++ XR thread to render the main menu panel texture.
    // Returns ARGB_8888 pixel array of size width×height.
    // menuItems: labels for each menu option. hoveredRow: row under laser (-1=none).
    // romName: name of currently loaded ROM (empty = none).
    // -----------------------------------------------------------------------
    fun renderMainMenuPanelBitmap(
        menuItems: Array<String>,
        hoveredRow: Int,
        width: Int,
        height: Int,
        romName: String
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(
            width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        // Panel background
        paint.color = android.graphics.Color.argb(240, 12, 12, 22)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        // Title bar
        val titleBarTop = 0f
        val titleBarBottom = titleBarTop + 88f
        paint.color = android.graphics.Color.argb(255, 30, 50, 90)
        canvas.drawRect(0f, titleBarTop, width.toFloat(), titleBarBottom, paint)
        paint.color = android.graphics.Color.WHITE
        paint.textSize = 48f
        paint.isFakeBoldText = true
        canvas.drawText("RetroDepth", 14f, titleBarTop + 60f, paint)
        paint.isFakeBoldText = false

        // ROM name display (right side of title bar)
        if (romName.isNotEmpty()) {
            paint.textSize = 26f
            paint.color = android.graphics.Color.argb(200, 120, 210, 255)
            val tw = paint.measureText(romName)
            canvas.drawText(romName, width - tw - 12f, titleBarTop + 56f, paint)
        }

        val titleH = titleBarBottom
        val n = menuItems.size
        val rowH = ((height - titleH) / n).toFloat()

        for (i in menuItems.indices) {
            val y = titleH + i * rowH

            // Row background (alternating rows only; hover is a GL quad)
            if (i % 2 == 0) {
                paint.color = android.graphics.Color.argb(60, 35, 40, 60)
                canvas.drawRect(0f, y, width.toFloat(), y + rowH, paint)
            }

            // Separator before "Stop Emulation" and "Exit" (last two items)
            if (i == n - 2) {
                paint.color = android.graphics.Color.argb(120, 100, 130, 200)
                paint.strokeWidth = 2f
                paint.style = android.graphics.Paint.Style.STROKE
                canvas.drawLine(8f, y + 1f, width - 8f, y + 1f, paint)
                paint.style = android.graphics.Paint.Style.FILL
                paint.strokeWidth = 0f
            }

            paint.textSize = rowH * 0.48f

            // Icon prefix for each item
            val icon = when (i) {
                0 -> "\uD83C\uDFAE "  // Open ROM
                1 -> "\u2699\uFE0F "   // Settings
                2 -> "\uD83D\uDCE6 "   // Layers
                3 -> "\uD83C\uDFAE "   // Mappings
                4 -> "\uD83D\uDD22 "   // View/Enter Code
                5 -> "\u23F9\uFE0F "   // Stop Emulation
                6 -> "\u274C "         // Exit
                else -> ""
            }

            val label = icon + menuItems[i]
            paint.color = android.graphics.Color.argb(220, 200, 210, 230)
            canvas.drawText(label, 20f, y + rowH * 0.68f, paint)
        }

        val pixels = IntArray(width * height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    // Called from C++ XR thread to exit the app
    fun exitApp() {
        handler.post {
            vrStarted = false
            nativeStopVr()
            finishAndRemoveTask()
        }
    }

    // Called from C++ to get the ROM root path.
    // Uses /storage/emulated/0/QuestRetroDepth/roms with `snes/` and `genesis/` subfolders.
    // Survives app uninstall/reinstall. Falls back to app-private storage if unavailable.
    fun getRomDirectory(): String {
        val dir = java.io.File(Environment.getExternalStorageDirectory(), "QuestRetroDepth/roms")
        if (dir.exists() || dir.mkdirs()) {
            File(dir, "snes").mkdirs()
            File(dir, "genesis").mkdirs()
            return dir.absolutePath
        }
        return java.io.File(getExternalFilesDir(null), "roms").apply {
            mkdirs()
            File(this, "snes").mkdirs()
            File(this, "genesis").mkdirs()
        }.absolutePath
    }

    // Called from C++ to get the settings directory path (created if needed).
    // Uses /storage/emulated/0/QuestRetroDepth/config so settings survive reinstall and
    // are accessible via USB. Falls back to app-private storage if unavailable.
    fun getSettingsDirectory(): String {
        val dir = java.io.File(Environment.getExternalStorageDirectory(), "QuestRetroDepth/config")
        if (dir.exists() || dir.mkdirs()) return dir.absolutePath
        return java.io.File(getExternalFilesDir(null), "settings").apply { mkdirs() }.absolutePath
    }

    companion object {
        private const val ENABLE_STARTUP_AUTO_LOAD = false
        init { System.loadLibrary("questretrodepth_native") }
    }

    private enum class RomFamily {
        Snes,
        Genesis
    }
}
