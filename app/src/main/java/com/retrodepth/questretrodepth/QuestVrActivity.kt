package com.retrodepth.questretrodepth

import android.app.Activity
import android.app.AlertDialog
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
import android.widget.EditText
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
    @Volatile private var quickPresetRenameDialogOpen = false

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
        if (!intent.getBooleanExtra("force_vr", false) && !supportsVrRuntime()) {
            startActivity(Intent(this, QuestRetroDepthActivity::class.java))
            finish()
            return
        }

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

    private fun supportsVrRuntime(): Boolean {
        val brand = Build.BRAND.lowercase(Locale.US)
        val manufacturer = Build.MANUFACTURER.lowercase(Locale.US)
        return (brand.contains("oculus") || brand.contains("meta") ||
            manufacturer.contains("oculus") || manufacturer.contains("meta")) &&
            packageManager.hasSystemFeature("android.hardware.vr.headtracking")
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
            val startupPrefs = readSaveAutomationPrefs()
            val anyRomCandidate = findStartupRomCandidate()
            val startupCandidate = if (startupPrefs.loadLastSaveEnabled) anyRomCandidate else null
            val openMenuOnStartup = !startupPrefs.loadLastSaveEnabled || startupCandidate == null
            val openHomebrewOnStartup =
                anyRomCandidate == null && !prefs.getBoolean(PREF_HOME_BREW_ONBOARDING_DONE, false)
            statusView.text = nativeStartVr(
                this,
                openMenuOnStartup,
                startupPrefs.autosaveIntervalSeconds,
                startupPrefs.loadLastSaveEnabled
            )
            nativeSetHomebrewFeed(selectedHomebrewFeedIndex())
            if (openHomebrewOnStartup) {
                prefs.edit().putBoolean(PREF_HOME_BREW_ONBOARDING_DONE, true).apply()
                nativeOpenHomebrew()
            }
            if (startupPrefs.loadLastSaveEnabled && startupCandidate != null) {
                autoLoadStartupRom(startupCandidate)
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

    private fun findStartupRomCandidate(): File? {
        val lastRomPath = prefs.getString("last_rom_path", null)
        val romDir = File(getRomDirectory())
        Log.i("QuestRetroDepthXR", "findStartupRomCandidate: dir=${romDir.absolutePath} exists=${romDir.exists()}")

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
        Log.i("QuestRetroDepthXR", "findStartupRomCandidate: found ${allFiles.size} ROM(s)")

        // Prefer the last successfully played ROM (stored as filename only); fall back to first alphabetically.
        return if (lastRomPath != null) {
            val lastName = lastRomPath.substringAfterLast('/')
            allFiles.firstOrNull { it.name == lastName }
                ?: allFiles.minByOrNull { it.name.lowercase(Locale.US) }
        } else {
            allFiles.minByOrNull { it.name.lowercase(Locale.US) }
        }
    }

    private fun autoLoadStartupRom(candidate: File) {
        Log.i("QuestRetroDepthXR", "autoLoadStartupRom: chosen=${candidate.name}")

        val romFile = runCatching { prepareRomFile(candidate) }.getOrElse {
            Log.e("QuestRetroDepthXR", "autoLoadStartupRom: extraction failed", it)
            statusView.text = "ROM extract failed: ${it.message}"
            nativeOpenMainMenu()
            return
        }

        Log.i("QuestRetroDepthXR", "autoLoadStartupRom: loading ${romFile.absolutePath}")
        val result = nativeLoadRom(romFile.absolutePath, candidate.name)
        Log.i("QuestRetroDepthXR", "autoLoadStartupRom: result=$result")
        statusView.text = result
        if (!result.startsWith("ROM load failed")) {
            lastSavedRomFilename = candidate.name
            prefs.edit().putString("last_rom_path", candidate.name).apply()
        } else {
            nativeOpenMainMenu()
        }
    }

    private fun readSaveAutomationPrefs(): SaveAutomationPrefs {
        val file = File(getSettingsDirectory(), SAVE_AUTOMATION_FILE_NAME)
        if (!file.isFile) return SaveAutomationPrefs()

        var autosaveIntervalSeconds = 30
        var loadLastSaveEnabled = true
        runCatching {
            file.forEachLine { raw ->
                val line = raw.trim()
                if (line.isEmpty() || line.startsWith("#")) return@forEachLine
                val sep = line.indexOf('=')
                if (sep <= 0) return@forEachLine
                val key = line.substring(0, sep).trim()
                val value = line.substring(sep + 1).trim()
                when (key) {
                    "autosave_interval_seconds" -> {
                        autosaveIntervalSeconds = value.toIntOrNull()
                            ?.takeIf { it in VALID_AUTOSAVE_INTERVALS }
                            ?: autosaveIntervalSeconds
                    }
                    "load_last_save" -> loadLastSaveEnabled = value == "1"
                }
            }
        }
        return SaveAutomationPrefs(autosaveIntervalSeconds, loadLastSaveEnabled)
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

    private fun isNesExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".nes") || lower.endsWith(".unf") || lower.endsWith(".unif")
    }

    private fun isGbExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".gb") || lower.endsWith(".gbc")
    }

    private fun isGbaExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".gba")
    }

    private fun isGgExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".gg") || lower.endsWith(".sms")
    }

    private fun isPceExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".pce") || lower.endsWith(".sgx")
    }

    private fun is32xExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".32x")
    }

    private fun isAtari2600Extension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".a26")
    }

    private fun isN64Extension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".z64") || lower.endsWith(".n64") || lower.endsWith(".v64")
    }

    private fun isDsExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".nds")
    }

    private fun isSaturnExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".cue") || lower.endsWith(".iso") || lower.endsWith(".chd")
    }

    private fun isDreamcastExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".gdi") || lower.endsWith(".cdi")
    }

    private fun isSupportedRomExtension(name: String): Boolean {
        return isSnesExtension(name) || isGenesisExtension(name) || isNesExtension(name) ||
               isGbExtension(name) || isGbaExtension(name) || isGgExtension(name) ||
               isPceExtension(name) || is32xExtension(name) || isAtari2600Extension(name) ||
               isN64Extension(name) || isDsExtension(name) || isSaturnExtension(name) ||
               isDreamcastExtension(name)
    }

    private fun isSupportedOrArchiveFile(file: File): Boolean {
        val lower = file.name.lowercase(Locale.US)
        return isSupportedRomExtension(lower) || lower.endsWith(".zip") || lower.endsWith(".7z")
    }

    private fun romFamilyForName(name: String): RomFamily? = when {
        isSnesExtension(name) -> RomFamily.Snes
        isGenesisExtension(name) -> RomFamily.Genesis
        isNesExtension(name) -> RomFamily.Nes
        isGbExtension(name) -> RomFamily.Gb
        isGbaExtension(name) -> RomFamily.Gba
        isGgExtension(name) -> RomFamily.Gg
        isPceExtension(name) -> RomFamily.Pce
        is32xExtension(name) -> RomFamily.Sega32x
        isAtari2600Extension(name) -> RomFamily.Atari2600
        isN64Extension(name) -> RomFamily.N64
        isDsExtension(name) -> RomFamily.Ds
        isSaturnExtension(name) -> RomFamily.Saturn
        isDreamcastExtension(name) -> RomFamily.Dreamcast
        else -> null
    }

    private fun preferredRomFamily(file: File): RomFamily? {
        val path = file.absolutePath.lowercase(Locale.US)
        return when {
            path.contains("/roms/snes/") || path.contains("\\roms\\snes\\") -> RomFamily.Snes
            path.contains("/roms/genesis/") || path.contains("\\roms\\genesis\\") -> RomFamily.Genesis
            path.contains("/roms/nes/") || path.contains("\\roms\\nes\\") -> RomFamily.Nes
            path.contains("/roms/gb/") || path.contains("\\roms\\gb\\") -> RomFamily.Gb
            path.contains("/roms/gba/") || path.contains("\\roms\\gba\\") -> RomFamily.Gba
            path.contains("/roms/gg/") || path.contains("\\roms\\gg\\") -> RomFamily.Gg
            path.contains("/roms/pce/") || path.contains("\\roms\\pce\\") -> RomFamily.Pce
            path.contains("/roms/32x/") || path.contains("\\roms\\32x\\") -> RomFamily.Sega32x
            path.contains("/roms/atari2600/") || path.contains("\\roms\\atari2600\\") -> RomFamily.Atari2600
            path.contains("/roms/n64/") || path.contains("\\roms\\n64\\") -> RomFamily.N64
            path.contains("/roms/ds/") || path.contains("\\roms\\ds\\") -> RomFamily.Ds
            path.contains("/roms/saturn/") || path.contains("\\roms\\saturn\\") -> RomFamily.Saturn
            path.contains("/roms/dreamcast/") || path.contains("\\roms\\dreamcast\\") -> RomFamily.Dreamcast
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

    private external fun nativeStartVr(
        activity: Activity,
        openMenuOnStartup: Boolean,
        autosaveIntervalSeconds: Int,
        loadLastSaveEnabled: Boolean
    ): String
    private external fun nativeGetVrStatus(): String
    private external fun nativeStopVr()
    private external fun nativeRandomize()
    private external fun nativeLoadPreset(idx: Int)
    private external fun nativeSavePreset(idx: Int)
    private external fun nativeGetVrStateSummary(): String
    private external fun nativeLoadRom(path: String, sourceName: String): String
    private external fun nativeGetLastLoadedRomFilename(): String
    private external fun nativeApplyStateCode(code: String): Boolean
    private external fun nativeOpenMainMenu()
    private external fun nativeOpenHomebrew()
    private external fun nativeSubmitQuickPresetName(kind: Int, slot: Int, name: String)
    private external fun nativeCancelQuickPresetName(kind: Int, slot: Int)
    private external fun nativeHomebrewDataReady()
    private external fun nativeHomebrewDownloadComplete(entryIdx: Int)
    private external fun nativeSetHomebrewFeed(idx: Int)

    fun showQuickPresetRenameDialog(kind: Int, slot: Int, currentName: String) {
        runOnUiThread {
            if (isFinishing || (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1 && isDestroyed)) {
                nativeCancelQuickPresetName(kind, slot)
                return@runOnUiThread
            }
            if (quickPresetRenameDialogOpen) {
                nativeCancelQuickPresetName(kind, slot)
                return@runOnUiThread
            }
            quickPresetRenameDialogOpen = true
            val input = EditText(this).apply {
                setText(currentName)
                setSelection(text.length)
                setTextColor(Color.WHITE)
                setHintTextColor(Color.GRAY)
                hint = if (kind == 0) "Settings name" else "Layers name"
            }
            AlertDialog.Builder(this)
                .setTitle(if (kind == 0) "Rename Settings Preset" else "Rename Layer Preset")
                .setView(input)
                .setCancelable(true)
                .setPositiveButton("Save") { _, _ ->
                    quickPresetRenameDialogOpen = false
                    nativeSubmitQuickPresetName(kind, slot, input.text?.toString() ?: "")
                }
                .setNegativeButton("Cancel") { _, _ ->
                    quickPresetRenameDialogOpen = false
                    nativeCancelQuickPresetName(kind, slot)
                }
                .setOnCancelListener {
                    quickPresetRenameDialogOpen = false
                    nativeCancelQuickPresetName(kind, slot)
                }
                .show()
        }
    }

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
        filterLabel: String,
        showFilter: Boolean
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
        val totalRows = n + 2 + if (showFilter) 1 else 0  // layers + play/pause + auto-dup + optional filter row
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

        if (showFilter) {
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
        }

        val pixels = IntArray(width * height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    fun renderQuickEditPanelBitmap(
        settingsNames: Array<String>,
        layerNames: Array<String>,
        layerEnabled: BooleanArray,
        hoveredSettingsLoad: Int,
        hoveredSettingsSave: Int,
        hoveredLayersLoad: Int,
        hoveredLayersSave: Int,
        hoveredAction: Int,
        width: Int,
        height: Int
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        paint.color = android.graphics.Color.argb(238, 10, 18, 12)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        val titleH = 96f
        paint.color = android.graphics.Color.argb(255, 36, 98, 56)
        canvas.drawRect(0f, 0f, width.toFloat(), titleH, paint)
        paint.color = android.graphics.Color.WHITE
        paint.textSize = 50f
        paint.isFakeBoldText = true
        canvas.drawText("Quick Edit", 20f, 62f, paint)
        paint.isFakeBoldText = false
        paint.textSize = 24f
        paint.color = android.graphics.Color.argb(210, 202, 240, 214)
        canvas.drawText("Pick presets, then press left thumbstick again to close", 20f, 88f, paint)

        val gap = 24f
        val columnW = (width - gap * 3f) / 2f
        val leftX = gap
        val rightX = leftX + columnW + gap
        val sectionTop = titleH + 32f
        val sectionBottom = height - 24f

        fun drawSection(
            x: Float,
            label: String,
            items: List<String>,
            enabled: List<Boolean>,
            actionLabels: List<String>,
            accent: Int,
            hoveredLoad: Int,
            hoveredSave: Int,
            actionBase: Int
        ) {
            paint.color = accent
            canvas.drawRoundRect(x, sectionTop, x + columnW, sectionTop + 64f, 14f, 14f, paint)
            paint.color = android.graphics.Color.WHITE
            paint.textSize = 38f
            paint.isFakeBoldText = true
            canvas.drawText(label, x + 18f, sectionTop + 43f, paint)
            paint.isFakeBoldText = false

            val totalRows = items.size + actionLabels.size
            val rowGap = 12f
            val startY = sectionTop + 84f
            val rowH = ((sectionBottom - startY) - rowGap * (totalRows - 1)) / totalRows.toFloat()

            fun actionFillColor(actionLabel: String): Int = when (actionLabel) {
                "Reset Presets" -> android.graphics.Color.argb(170, 122, 52, 44)
                "Manual Edit", "Manual Layers" -> android.graphics.Color.argb(165, 112, 84, 30)
                "Open Settings" -> android.graphics.Color.argb(165, 34, 82, 124)
                else -> android.graphics.Color.argb(150, 28, 86, 70)
            }

            for (i in items.indices) {
                val y0 = startY + i * (rowH + rowGap)
                val y1 = y0 + rowH
                val isEnabled = enabled.getOrElse(i) { true }
                paint.color = if (isEnabled) {
                    android.graphics.Color.argb(165, 36, 62, 44)
                } else {
                    android.graphics.Color.argb(96, 42, 52, 46)
                }
                canvas.drawRoundRect(x, y0, x + columnW, y1, 14f, 14f, paint)
                if (i == hoveredLoad) {
                    paint.style = android.graphics.Paint.Style.STROKE
                    paint.strokeWidth = 5f
                    paint.color = android.graphics.Color.argb(235, 196, 255, 222)
                    canvas.drawRoundRect(x + 2f, y0 + 2f, x + columnW - 2f, y1 - 2f, 14f, 14f, paint)
                    paint.style = android.graphics.Paint.Style.FILL
                    paint.strokeWidth = 0f
                }
                paint.color = if (isEnabled) android.graphics.Color.WHITE else android.graphics.Color.argb(170, 168, 160, 150)
                paint.textSize = rowH * 0.34f
                canvas.save()
                canvas.clipRect(x + 12f, y0, x + columnW - 12f, y1)
                canvas.drawText(items[i], x + 18f, y0 + rowH * 0.62f, paint)
                canvas.restore()
                if (!isEnabled) {
                    paint.color = android.graphics.Color.argb(150, 162, 184, 166)
                    paint.textSize = rowH * 0.18f
                    canvas.drawText("NOT AVAILABLE FOR THIS LAYER SET", x + 18f, y0 + rowH * 0.84f, paint)
                }
            }

            for (i in actionLabels.indices) {
                val row = items.size + i
                val y0 = startY + row * (rowH + rowGap)
                val y1 = y0 + rowH
                paint.color = actionFillColor(actionLabels[i])
                canvas.drawRoundRect(x, y0, x + columnW, y1, 14f, 14f, paint)
                if (hoveredAction == actionBase + i) {
                    paint.style = android.graphics.Paint.Style.STROKE
                    paint.strokeWidth = 5f
                    paint.color = android.graphics.Color.argb(235, 196, 255, 222)
                    canvas.drawRoundRect(x + 2f, y0 + 2f, x + columnW - 2f, y1 - 2f, 14f, 14f, paint)
                    paint.style = android.graphics.Paint.Style.FILL
                    paint.strokeWidth = 0f
                }
                paint.color = android.graphics.Color.WHITE
                paint.textSize = rowH * 0.32f
                canvas.drawText(actionLabels[i], x + 18f, y0 + rowH * 0.62f, paint)
            }
        }

        drawSection(
            leftX,
            "Settings",
            settingsNames.toList(),
            List(settingsNames.size) { true },
            listOf("Reset Presets", "Manual Edit", "Open Settings"),
            android.graphics.Color.argb(255, 52, 118, 70),
            hoveredSettingsLoad,
            hoveredSettingsSave,
            0
        )
        drawSection(
            rightX,
            "Layers",
            layerNames.toList(),
            layerEnabled.map { it },
            listOf("Reset Presets", "Manual Layers"),
            android.graphics.Color.argb(255, 30, 96, 86),
            hoveredLayersLoad,
            hoveredLayersSave,
            3
        )

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
            val isDisabledAction = (value == "DISABLED")

            // Draw separator before action buttons (row 14)
            if (i == 14) {
                paint.color = android.graphics.Color.argb(120, 100, 130, 200)
                paint.strokeWidth = 2f
                paint.style = android.graphics.Paint.Style.STROKE
                canvas.drawLine(8f, y + 1f, width - 8f, y + 1f, paint)
                paint.style = android.graphics.Paint.Style.FILL
                paint.strokeWidth = 0f
            }

            if (isAction) {
                // Full-width action buttons
                paint.color = when (i) {
                    14 -> android.graphics.Color.argb(170, 160, 30, 30)
                    15 -> android.graphics.Color.argb(170, 35, 140, 65)
                    16 -> android.graphics.Color.argb(170, 15, 85, 40)
                    17 -> android.graphics.Color.argb(170, 35, 95, 180)
                    18 -> android.graphics.Color.argb(170, 20, 50, 130)
                    19 -> android.graphics.Color.argb(120, 35, 55, 45)
                    else -> android.graphics.Color.argb(140, 40, 70, 120)
                }
                canvas.drawRoundRect(6f, y + rowH * 0.12f, width - 6f, y + rowH * 0.88f, 8f, 8f, paint)
                paint.color = android.graphics.Color.WHITE
                paint.textSize = rowH * 0.46f
                canvas.drawText(if (i < names.size) names[i] else "", 18f, y + rowH * 0.68f, paint)
            } else if (isDisabledAction) {
                paint.color = android.graphics.Color.argb(95, 58, 60, 68)
                canvas.drawRoundRect(6f, y + rowH * 0.12f, width - 6f, y + rowH * 0.88f, 8f, 8f, paint)
                paint.color = android.graphics.Color.argb(175, 168, 174, 188)
                paint.textSize = rowH * 0.42f
                canvas.drawText(if (i < names.size) names[i] else "", 18f, y + rowH * 0.56f, paint)
                paint.color = android.graphics.Color.argb(145, 138, 146, 160)
                paint.textSize = rowH * 0.22f
                canvas.drawText("LOAD A ROM FIRST", 18f, y + rowH * 0.82f, paint)
            } else if (isBool) {
                // Name on left
                paint.color = android.graphics.Color.argb(215, 190, 200, 220)
                canvas.drawText(if (i < names.size) names[i] else "", 12f, y + rowH * 0.68f, paint)
                // ON/OFF or status badge on right
                val badgeX = width * 0.68f
                paint.color = when {
                    value == "ON" -> android.graphics.Color.argb(210, 50, 180, 90)
                    value == "OFF" -> android.graphics.Color.argb(150, 160, 50, 50)
                    value == "USER OK" -> android.graphics.Color.argb(210, 45, 135, 210)
                    value == "BUNDLED" -> android.graphics.Color.argb(210, 70, 120, 205)
                    value == "USER BROKEN" || value == "USER BROKEN -> BUNDLED" ->
                        android.graphics.Color.argb(210, 185, 105, 35)
                    else -> android.graphics.Color.argb(170, 95, 95, 120)
                }
                canvas.drawRoundRect(badgeX, y + rowH * 0.18f, width - 8f, y + rowH * 0.82f, 8f, 8f, paint)
                paint.color = android.graphics.Color.WHITE
                paint.textSize = if (value.length > 10) rowH * 0.28f else rowH * 0.34f
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

    fun renderSaveStatePanelBitmap(
        romName: String,
        loadLabels: Array<String>,
        loadEnabled: BooleanArray,
        saveLabels: Array<String>,
        autosaveLabel: String,
        autoloadLabel: String,
        hoveredCell: Int,
        width: Int,
        height: Int
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        paint.color = android.graphics.Color.argb(238, 10, 12, 22)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        val titleH = 88f
        paint.color = android.graphics.Color.argb(255, 28, 62, 92)
        canvas.drawRect(0f, 0f, width.toFloat(), titleH, paint)
        paint.color = android.graphics.Color.WHITE
        paint.textSize = 46f
        paint.isFakeBoldText = true
        canvas.drawText("Save States", 16f, 60f, paint)
        paint.isFakeBoldText = false
        val hasActiveRom = romName.isNotEmpty()
        if (hasActiveRom) {
            paint.textSize = 26f
            paint.color = android.graphics.Color.argb(210, 120, 210, 255)
            val tw = paint.measureText(romName)
            canvas.drawText(romName, width - tw - 12f, 56f, paint)
        } else {
            paint.textSize = 24f
            paint.color = android.graphics.Color.argb(180, 150, 158, 176)
            val note = "No ROM loaded"
            val tw = paint.measureText(note)
            canvas.drawText(note, width - tw - 12f, 56f, paint)
        }

        val top = titleH + 24f
        val bottom = height - 24f
        val totalRows = 4
        val rowH = (bottom - top) / totalRows.toFloat()
        val colW = width / 3f

        for (row in 0 until 2) {
            for (col in 0 until 3) {
                val idx = row * 3 + col
                val x0 = col * colW + 12f
                val x1 = (col + 1) * colW - 12f
                val y0 = top + row * rowH + 10f
                val y1 = top + (row + 1) * rowH - 10f
                val isLoad = row == 0
                val enabled = if (!hasActiveRom) {
                    false
                } else if (isLoad) {
                    idx < loadEnabled.size && loadEnabled[idx]
                } else {
                    true
                }
                val label = if (isLoad) {
                    if (idx < loadLabels.size) loadLabels[idx] else "LOAD ${idx + 1}"
                } else {
                    val sidx = idx - 3
                    if (sidx in saveLabels.indices) saveLabels[sidx] else "SAVE ${sidx + 1}"
                }

                paint.color = when {
                    isLoad && enabled -> android.graphics.Color.argb(170, 26, 96, 144)
                    isLoad -> android.graphics.Color.argb(110, 42, 48, 64)
                    enabled -> android.graphics.Color.argb(180, 34, 124, 66)
                    else -> android.graphics.Color.argb(110, 48, 54, 60)
                }
                canvas.drawRoundRect(x0, y0, x1, y1, 18f, 18f, paint)

                if (idx == hoveredCell) {
                    paint.style = android.graphics.Paint.Style.STROKE
                    paint.strokeWidth = 4f
                    paint.color = android.graphics.Color.argb(210, 180, 220, 255)
                    canvas.drawRoundRect(x0 + 2f, y0 + 2f, x1 - 2f, y1 - 2f, 16f, 16f, paint)
                    paint.style = android.graphics.Paint.Style.FILL
                    paint.strokeWidth = 0f
                }

                paint.textAlign = android.graphics.Paint.Align.CENTER
                paint.color = if (!enabled) {
                    android.graphics.Color.argb(170, 170, 176, 190)
                } else {
                    android.graphics.Color.WHITE
                }
                paint.textSize = if (label.length > 14) rowH * 0.18f else rowH * 0.22f
                canvas.drawText(label, (x0 + x1) * 0.5f, y0 + (y1 - y0) * 0.60f, paint)

                paint.textSize = rowH * 0.14f
                paint.color = if (isLoad) {
                    if (enabled) android.graphics.Color.argb(190, 170, 225, 255)
                    else android.graphics.Color.argb(150, 138, 146, 160)
                } else if (enabled) {
                    android.graphics.Color.argb(190, 180, 240, 195)
                } else {
                    android.graphics.Color.argb(150, 138, 146, 160)
                }
                canvas.drawText(if (isLoad) "LOAD" else "SAVE", (x0 + x1) * 0.5f, y0 + (y1 - y0) * 0.82f, paint)
                paint.textAlign = android.graphics.Paint.Align.LEFT
            }
        }

        val optionLabels = arrayOf(
            "Autosave Every",
            "Load Last Save"
        )
        val optionValues = arrayOf(autosaveLabel, autoloadLabel)
        for (row in 0 until 2) {
            val cellId = 6 + row
            val x0 = 12f
            val x1 = width - 12f
            val y0 = top + (row + 2) * rowH + 10f
            val y1 = top + (row + 3) * rowH - 10f

            paint.color = android.graphics.Color.argb(170, 42, 54, 88)
            canvas.drawRoundRect(x0, y0, x1, y1, 18f, 18f, paint)

            if (hoveredCell == cellId) {
                paint.style = android.graphics.Paint.Style.STROKE
                paint.strokeWidth = 4f
                paint.color = android.graphics.Color.argb(210, 180, 220, 255)
                canvas.drawRoundRect(x0 + 2f, y0 + 2f, x1 - 2f, y1 - 2f, 16f, 16f, paint)
                paint.style = android.graphics.Paint.Style.FILL
                paint.strokeWidth = 0f
            }

            paint.color = android.graphics.Color.argb(215, 196, 208, 228)
            paint.textSize = rowH * 0.22f
            canvas.drawText(optionLabels[row], x0 + 24f, y0 + (y1 - y0) * 0.45f, paint)

            paint.textAlign = android.graphics.Paint.Align.RIGHT
            paint.color = android.graphics.Color.argb(230, 120, 210, 255)
            paint.textSize = rowH * 0.26f
            canvas.drawText(optionValues[row], x1 - 24f, y0 + (y1 - y0) * 0.58f, paint)

            paint.textAlign = android.graphics.Paint.Align.LEFT
            paint.color = android.graphics.Color.argb(165, 170, 196, 220)
            paint.textSize = rowH * 0.14f
            canvas.drawText("TRIGGER TO CYCLE", x0 + 24f, y0 + (y1 - y0) * 0.78f, paint)
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
        mode: Int,
        currentInput: String,
        secondaryText: String,
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

        if (mode == 1) {
            paint.color = android.graphics.Color.argb(210, 235, 220, 160)
            paint.textSize = 26f
            paint.isFakeBoldText = true
            canvas.drawText("Cancel", 18f, 50f, paint)
            val saveLabel = "Save"
            val saveW = paint.measureText(saveLabel)
            canvas.drawText(saveLabel, width - saveW - 18f, 50f, paint)
            paint.color = android.graphics.Color.argb(190, 180, 210, 245)
            paint.textSize = 20f
            paint.isFakeBoldText = false
            val slotLabel = if (secondaryText.isNotEmpty()) secondaryText else "Preset Name"
            val slotW = paint.measureText(slotLabel)
            canvas.drawText(slotLabel, (width - slotW) * 0.5f, 26f, paint)
            val spaceLabel = "Space"
            val spaceW = paint.measureText(spaceLabel)
            canvas.drawText(spaceLabel, (width - spaceW) * 0.5f, 54f, paint)
            val displayStr = if (currentInput.isEmpty()) "Type preset name" else currentInput.take(24)
            paint.textSize = 30f
            paint.color = android.graphics.Color.argb(255, 120, 230, 160)
            val inputW = paint.measureText(displayStr)
            canvas.drawText(displayStr, (width - inputW) * 0.5f, 76f, paint)
        } else {
            paint.color = android.graphics.Color.argb(255, 100, 200, 255)
            paint.textSize = 36f
            paint.isFakeBoldText = true
            val codeDisplay = if (secondaryText.isNotEmpty()) secondaryText.take(16) else "(no code)"
            canvas.drawText(codeDisplay, 12f, 32f, paint)
            paint.isFakeBoldText = false

            paint.color = android.graphics.Color.argb(200, 180, 180, 180)
            paint.textSize = 20f
            canvas.drawText("Type to enter:", width - 160f, 28f, paint)

            val displayStr = if (currentInput.isEmpty()) "______" else currentInput
            paint.textSize = 28f
            paint.color = android.graphics.Color.argb(255, 120, 230, 160)
            val tw = paint.measureText(displayStr)
            canvas.drawText(displayStr, width - tw - 12f, 64f, paint)
        }

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

    // -----------------------------------------------------------------------
    // Called from C++ XR thread — renders passive side help panels.
    // inputLabels/actionLabels are parallel arrays generated from native control metadata.
    // Returns ARGB_8888 pixels, width×height.
    // -----------------------------------------------------------------------
    fun renderHelpPanelBitmap(
        title: String,
        inputLabels: Array<String>,
        actionLabels: Array<String>,
        width: Int,
        height: Int
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(
            width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        fun wrapText(text: String, maxWidth: Float): List<String> {
            if (text.isEmpty()) return listOf("")
            val words = text.split(' ')
            val lines = mutableListOf<String>()
            var line = ""
            for (word in words) {
                val candidate = if (line.isEmpty()) word else "$line $word"
                if (paint.measureText(candidate) <= maxWidth || line.isEmpty()) {
                    line = candidate
                } else {
                    lines.add(line)
                    line = word
                }
            }
            if (line.isNotEmpty()) lines.add(line)
            return lines
        }

        paint.color = android.graphics.Color.argb(230, 10, 12, 18)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        val titleH = 86f
        paint.color = android.graphics.Color.argb(255, 34, 48, 62)
        canvas.drawRect(0f, 0f, width.toFloat(), titleH, paint)

        paint.color = android.graphics.Color.WHITE
        paint.textSize = 42f
        paint.isFakeBoldText = true
        canvas.drawText(title, 18f, 58f, paint)
        paint.isFakeBoldText = false

        val count = minOf(inputLabels.size, actionLabels.size)
        var y = titleH + 36f
        val marginX = 24f
        val maxTextW = width - marginX * 2f
        val actionSize = if (count > 12) 25f else 28f
        val inputSize = if (count > 12) 23f else 25f

        for (i in 0 until count) {
            if (y > height - 36f) break

            if (i > 0) {
                paint.color = android.graphics.Color.argb(70, 120, 140, 160)
                canvas.drawRect(marginX, y - 17f, width - marginX, y - 15f, paint)
            }

            paint.textSize = inputSize
            paint.color = android.graphics.Color.argb(230, 115, 215, 245)
            paint.isFakeBoldText = true
            val input = inputLabels[i]
            val inputLines = wrapText(input, maxTextW)
            for (line in inputLines) {
                if (y > height - 30f) break
                canvas.drawText(line, marginX, y, paint)
                y += inputSize + 4f
            }
            paint.isFakeBoldText = false

            paint.textSize = actionSize
            paint.color = android.graphics.Color.argb(225, 218, 224, 232)
            val actionLines = wrapText(actionLabels[i], maxTextW)
            for (line in actionLines) {
                if (y > height - 30f) break
                canvas.drawText(line, marginX, y, paint)
                y += actionSize + 5f
            }
            y += 16f
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

            // Separator before "Exit"
            if (i == n - 1) {
                paint.color = android.graphics.Color.argb(120, 100, 130, 200)
                paint.strokeWidth = 2f
                paint.style = android.graphics.Paint.Style.STROKE
                canvas.drawLine(8f, y + 1f, width - 8f, y + 1f, paint)
                paint.style = android.graphics.Paint.Style.FILL
                paint.strokeWidth = 0f
            }

            paint.textSize = rowH * 0.48f

            val label = menuItems[i]
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
            createRomSubfolders(dir)
            return dir.absolutePath
        }
        return java.io.File(getExternalFilesDir(null), "roms").apply {
            mkdirs()
            createRomSubfolders(this)
        }.absolutePath
    }

    private fun createRomSubfolders(base: File) {
        for (name in listOf("snes", "genesis", "nes", "gb", "gba", "gg", "pce", "32x",
                            "atari2600", "n64", "ds", "saturn", "dreamcast", "arcade")) {
            File(base, name).mkdirs()
        }
    }

    // Called from C++ to get the settings directory path (created if needed).
    // Uses /storage/emulated/0/QuestRetroDepth/config so settings survive reinstall and
    // are accessible via USB. Falls back to app-private storage if unavailable.
    fun getSettingsDirectory(): String {
        val dir = java.io.File(Environment.getExternalStorageDirectory(), "QuestRetroDepth/config")
        if (dir.exists() || dir.mkdirs()) return dir.absolutePath
        return java.io.File(getExternalFilesDir(null), "settings").apply { mkdirs() }.absolutePath
    }

    fun getRumbleDirectory(): String {
        val dir = java.io.File(Environment.getExternalStorageDirectory(), "QuestRetroDepth/rumble")
        if (dir.exists() || dir.mkdirs()) {
            createRomSubfolders(dir)
            return dir.absolutePath
        }
        return java.io.File(getExternalFilesDir(null), "rumble").apply {
            mkdirs()
            createRomSubfolders(this)
        }.absolutePath
    }

    // -----------------------------------------------------------------------
    // Homebrew Manager
    // -----------------------------------------------------------------------

    data class HomebrewEntry(
        val name: String,
        val author: String,
        val license: String,
        val website: String,
        val download: String,
        val system: String,
        val filename: String = "",
        val licenseUrl: String = "",
        val source: String = "",
        val sourceEntryUrl: String = "",
        val distributionMode: String = "official",
        val mirrorAllowed: Boolean = false,
        val notes: String = ""
    )

    data class HomebrewFeedSource(
        val fileName: String,
        val name: String,
        val url: String
    )

    private var hwEntries: List<HomebrewEntry> = emptyList()
    private var hwFeeds: List<HomebrewFeedSource> = fallbackHomebrewFeeds()
    private var hasValidatedRemoteHomebrewFeeds = false
    private val hwDownloaded = mutableSetOf<String>()

    private val supportedHomebrewSystems = setOf("nes", "gb", "gbc", "gba", "sms", "gg", "snes", "genesis", "pce")

    private fun fallbackHomebrewFeeds(): List<HomebrewFeedSource> = listOf(
        HomebrewFeedSource(
            "all_homebrew.json",
            "All Systems",
            "https://raw.githubusercontent.com/maranone/QuestRetroDepth/main/homebrew/all_homebrew.json"
        ),
        HomebrewFeedSource(
            "featured_homebrew.json",
            "Featured",
            "https://raw.githubusercontent.com/maranone/QuestRetroDepth/main/homebrew/featured_homebrew.json"
        ),
        HomebrewFeedSource(
            "ghb_curated.json",
            "GHB",
            "https://raw.githubusercontent.com/maranone/QuestRetroDepth/main/homebrew/ghb_curated.json"
        )
    )

    private fun fetchHttpText(url: String, accept: String? = null): String {
        val conn = java.net.URL(url).openConnection() as java.net.HttpURLConnection
        try {
            conn.connectTimeout = 10_000
            conn.readTimeout = 15_000
            conn.setRequestProperty("User-Agent", "QuestRetroDepth")
            if (!accept.isNullOrBlank()) {
                conn.setRequestProperty("Accept", accept)
            }
            conn.connect()
            return conn.inputStream.bufferedReader().use { it.readText() }
        } finally {
            conn.disconnect()
        }
    }

    private fun parseHomebrewCatalogDisplayName(fileName: String, body: String): String {
        return runCatching {
            val obj = org.json.JSONObject(body)
            obj.optString("feed").trim().ifBlank { fileName.removeSuffix(".json") }
        }.getOrDefault(fileName.removeSuffix(".json"))
    }

    private fun isValidHomebrewCatalog(body: String): Boolean {
        return runCatching {
            val obj = org.json.JSONObject(body)
            obj.optJSONArray("roms") != null
        }.getOrDefault(false)
    }

    private fun refreshHomebrewFeeds(): List<HomebrewFeedSource> {
        return runCatching {
            val body = fetchHttpText(HOME_BREW_GITHUB_API_URL, "application/vnd.github+json")

            val parsed = mutableListOf<HomebrewFeedSource>()
            val arr = org.json.JSONArray(body)
            for (i in 0 until arr.length()) {
                val item = arr.optJSONObject(i) ?: continue
                if (!item.optString("type").equals("file", ignoreCase = true)) continue
                val fileName = item.optString("name").trim()
                val downloadUrl = item.optString("download_url").trim()
                if (!fileName.endsWith(".json", ignoreCase = true)) continue
                if (downloadUrl.isBlank()) continue
                val catalogBody = runCatching { fetchHttpText(downloadUrl, "application/json") }.getOrNull() ?: continue
                if (!isValidHomebrewCatalog(catalogBody)) continue
                parsed.add(
                    HomebrewFeedSource(
                        fileName = fileName,
                        name = parseHomebrewCatalogDisplayName(fileName, catalogBody),
                        url = downloadUrl
                    )
                )
            }
            if (parsed.isNotEmpty()) {
                hwFeeds = parsed.sortedBy { it.name.lowercase(Locale.US) }
                hasValidatedRemoteHomebrewFeeds = true
            } else if (!hasValidatedRemoteHomebrewFeeds) {
                hwFeeds = fallbackHomebrewFeeds()
            }
            hwFeeds
        }.getOrElse {
            Log.e("Homebrew", "Feed directory fetch failed: ${it.message}")
            if (!hasValidatedRemoteHomebrewFeeds || hwFeeds.isEmpty()) {
                hwFeeds = fallbackHomebrewFeeds()
            }
            hwFeeds
        }
    }

    private fun selectedHomebrewFeedIndex(feeds: List<HomebrewFeedSource> = if (hwFeeds.isEmpty()) fallbackHomebrewFeeds() else hwFeeds): Int {
        val savedFileName = prefs.getString(PREF_HOME_BREW_FEED_FILE_NAME, null)
        val byFileName = if (!savedFileName.isNullOrBlank()) feeds.indexOfFirst { it.fileName == savedFileName } else -1
        if (byFileName >= 0) return byFileName
        val savedIndex = prefs.getInt(PREF_HOME_BREW_FEED_INDEX, 0)
        return savedIndex.coerceIn(0, maxOf(0, feeds.size - 1))
    }

    private fun persistHomebrewFeedSelection(feed: HomebrewFeedSource, index: Int) {
        prefs.edit()
            .putString(PREF_HOME_BREW_FEED_FILE_NAME, feed.fileName)
            .putInt(PREF_HOME_BREW_FEED_INDEX, index)
            .apply()
    }

    private fun hwFeedName(feedIdx: Int): String {
        val feeds = if (hwFeeds.isEmpty()) fallbackHomebrewFeeds() else hwFeeds
        return feeds.getOrNull(feedIdx)?.name ?: feeds.firstOrNull()?.name ?: "Homebrew"
    }

    fun showHomebrewFeedDialog(currentFeedIdx: Int) {
        Thread {
            val feeds = refreshHomebrewFeeds()
            runOnUiThread {
                if (isFinishing || (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1 && isDestroyed)) {
                    return@runOnUiThread
                }
                if (feeds.isEmpty()) return@runOnUiThread
                val labels = feeds.map { it.name }.toTypedArray()
                val initial = selectedHomebrewFeedIndex(feeds).coerceIn(0, feeds.size - 1)
                AlertDialog.Builder(this)
                    .setTitle("Select Homebrew Feed")
                    .setSingleChoiceItems(labels, initial) { dialog, which ->
                        persistHomebrewFeedSelection(feeds[which], which)
                        nativeSetHomebrewFeed(which)
                        homebrewFetchFeed(which)
                        dialog.dismiss()
                    }
                    .setNegativeButton("Cancel", null)
                    .show()
            }
        }.start()
    }

    private fun normalizeHomebrewSystem(raw: String): String {
        val normalized = raw.trim().lowercase(Locale.US)
        return if (normalized in supportedHomebrewSystems) normalized else ""
    }

    private fun parseContentDispositionFilename(header: String?): String {
        if (header.isNullOrBlank()) return ""
        val starMatch = Regex("""filename\*\s*=\s*([^']*)''([^;]+)""", RegexOption.IGNORE_CASE).find(header)
        if (starMatch != null) {
            val encoded = starMatch.groupValues.getOrElse(2) { "" }
            return runCatching { java.net.URLDecoder.decode(encoded, "UTF-8") }.getOrDefault(encoded)
        }
        val plainMatch = Regex("""filename\s*=\s*"?([^\";]+)"?""", RegexOption.IGNORE_CASE).find(header)
        return plainMatch?.groupValues?.getOrElse(1) { "" } ?: ""
    }

    private fun inferredFilenameFromUrl(url: String): String {
        val clean = url.substringBefore('#').substringBefore('?').substringAfterLast('/')
        return clean
    }

    private fun resolvedHomebrewFilename(
        entry: HomebrewEntry,
        responseFilename: String = "",
        finalUrl: String = entry.download
    ): String {
        val candidates = listOf(
            entry.filename,
            responseFilename,
            inferredFilenameFromUrl(finalUrl),
            inferredFilenameFromUrl(entry.download)
        )
        for (candidate in candidates) {
            val sanitized = sanitize(candidate.trim())
            if (sanitized.isNotBlank() && isSupportedOrArchiveFile(File(sanitized))) {
                return sanitized
            }
        }
        return ""
    }

    private fun hwFile(entry: HomebrewEntry): File =
        File(
            getRomDirectory(),
            "${entry.system}/${resolvedHomebrewFilename(entry).ifBlank { "__invalid_homebrew__" }}"
        )

    fun isHomebrewDownloaded(entryIdx: Int): Boolean {
        val list = hwEntries
        if (entryIdx < 0 || entryIdx >= list.size) return false
        return hwFile(list[entryIdx]).exists()
    }

    fun homebrewFetchFeed(feedIdx: Int) {
        Thread {
            try {
                val feeds = refreshHomebrewFeeds()
                val url = feeds.getOrNull(feedIdx)?.url ?: return@Thread
                val body = fetchHttpText(url, "application/json")
                val entries = mutableListOf<HomebrewEntry>()
                val obj = org.json.JSONObject(body)
                val roms = obj.optJSONArray("roms") ?: org.json.JSONArray()
                for (i in 0 until roms.length()) {
                    val r = roms.getJSONObject(i)
                    val system = normalizeHomebrewSystem(r.optString("system"))
                    val download = r.optString("download").trim()
                    if (system.isEmpty() || download.isEmpty()) continue
                    entries.add(HomebrewEntry(
                        name     = r.optString("name"),
                        author   = r.optString("author"),
                        license  = r.optString("license"),
                        website  = r.optString("website"),
                        download = download,
                        system   = system,
                        filename = sanitize(r.optString("filename")),
                        licenseUrl = r.optString("license_url"),
                        source = r.optString("source"),
                        sourceEntryUrl = r.optString("source_entry_url"),
                        distributionMode = r.optString("distribution_mode", "official"),
                        mirrorAllowed = r.optBoolean("mirror_allowed", false),
                        notes = r.optString("notes")
                    ))
                }
                hwEntries = entries
                feeds.getOrNull(feedIdx)?.let { persistHomebrewFeedSelection(it, feedIdx) }
                hwDownloaded.clear()
                for (e in entries) { if (hwFile(e).exists()) hwDownloaded.add(e.download) }
            } catch (e: Exception) {
                Log.e("Homebrew", "Fetch failed: ${e.message}")
            }
            runOnUiThread { nativeHomebrewDataReady() }
        }.start()
    }

    fun homebrewDownload(entryIdx: Int) {
        val list = hwEntries
        if (entryIdx < 0 || entryIdx >= list.size) return
        val entry = list[entryIdx]
        Thread {
            try {
                val conn = java.net.URL(entry.download).openConnection() as java.net.HttpURLConnection
                try {
                    conn.connectTimeout = 15_000
                    conn.readTimeout = 60_000
                    conn.instanceFollowRedirects = true
                    conn.setRequestProperty("User-Agent", "QuestRetroDepth")
                    conn.connect()
                    val responseFilename = parseContentDispositionFilename(conn.getHeaderField("Content-Disposition"))
                    val resolvedFilename = resolvedHomebrewFilename(entry, responseFilename, conn.url.toString())
                    if (resolvedFilename.isBlank()) {
                        throw IllegalStateException("Unsupported or missing filename for ${entry.name}")
                    }
                    val dest = File(getRomDirectory(), "${entry.system}/${resolvedFilename}")
                    dest.parentFile?.mkdirs()
                    conn.inputStream.use { inp ->
                        FileOutputStream(dest).use { out -> inp.copyTo(out) }
                    }
                    hwDownloaded.add(entry.download)
                } finally {
                    conn.disconnect()
                }
            } catch (e: Exception) {
                Log.e("Homebrew", "Download failed: ${e.message}")
            }
            runOnUiThread { nativeHomebrewDownloadComplete(entryIdx) }
        }.start()
    }

    fun homebrewDelete(entryIdx: Int) {
        val list = hwEntries
        if (entryIdx < 0 || entryIdx >= list.size) return
        val entry = list[entryIdx]
        hwFile(entry).delete()
        hwDownloaded.remove(entry.download)
        nativeHomebrewDataReady()
    }

    fun homebrewOpenWebsite(entryIdx: Int) {
        val list = hwEntries
        if (entryIdx < 0 || entryIdx >= list.size) return
        val url = list[entryIdx].website.ifBlank { list[entryIdx].sourceEntryUrl }
        if (url.isNotBlank()) {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        }
    }

    fun renderHomebrewListBitmap(
        hovered: Int, scroll: Int, feedIdx: Int, loading: Boolean, width: Int, height: Int
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        paint.color = android.graphics.Color.argb(240, 12, 12, 22)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        val titleH = 88f
        paint.color = android.graphics.Color.argb(255, 20, 50, 90)
        canvas.drawRect(0f, 0f, width.toFloat(), titleH, paint)

        val feedName = hwFeedName(feedIdx)
        paint.color = android.graphics.Color.WHITE
        paint.textSize = 38f
        paint.typeface = android.graphics.Typeface.DEFAULT_BOLD
        paint.textAlign = android.graphics.Paint.Align.CENTER
        canvas.drawText("◀  $feedName  ▶", width / 2f, titleH - 22f, paint)

        val entries = hwEntries
        val totalRows = entries.size + 2
        val rowH = ((height - titleH) / totalRows).coerceIn(44f, 80f)

        if (loading) {
            paint.color = android.graphics.Color.argb(200, 0, 200, 255)
            paint.textSize = 34f
            paint.typeface = android.graphics.Typeface.DEFAULT
            paint.textAlign = android.graphics.Paint.Align.CENTER
            canvas.drawText("Loading…", width / 2f, titleH + rowH + rowH / 2f, paint)
        } else {
            for (i in 1 until totalRows - 1) {
                val entryIdx = scroll + i - 1
                if (entryIdx < 0 || entryIdx >= entries.size) continue
                val entry = entries[entryIdx]
                val y0 = titleH + i * rowH
                val downloaded = hwDownloaded.contains(entry.download)

                if (i - 1 == hovered - 1) {
                    paint.color = android.graphics.Color.argb(80, 0, 180, 255)
                    canvas.drawRect(0f, y0, width.toFloat(), y0 + rowH, paint)
                }

                val sysTag = "[${entry.system.uppercase().take(4)}]"
                paint.textSize = 28f
                paint.typeface = android.graphics.Typeface.DEFAULT_BOLD
                paint.textAlign = android.graphics.Paint.Align.LEFT
                paint.color = android.graphics.Color.argb(200, 100, 200, 255)
                canvas.drawText(sysTag, 16f, y0 + rowH * 0.62f, paint)

                paint.color = android.graphics.Color.WHITE
                paint.textSize = 30f
                canvas.drawText(entry.name, 100f, y0 + rowH * 0.62f, paint)

                val indicator = if (downloaded) "✓" else "↓"
                paint.color = if (downloaded) android.graphics.Color.argb(255, 80, 220, 80)
                              else android.graphics.Color.argb(200, 180, 180, 180)
                paint.textAlign = android.graphics.Paint.Align.RIGHT
                canvas.drawText(indicator, width - 16f, y0 + rowH * 0.62f, paint)
            }
        }

        // Feed toggle row (row 0)
        val y0feed = titleH
        if (hovered == 0) {
            paint.color = android.graphics.Color.argb(80, 0, 180, 255)
            canvas.drawRect(0f, y0feed, width.toFloat(), y0feed + rowH, paint)
        }
        paint.color = android.graphics.Color.argb(220, 160, 220, 255)
        paint.textSize = 28f
        paint.textAlign = android.graphics.Paint.Align.CENTER
        paint.typeface = android.graphics.Typeface.DEFAULT
        canvas.drawText("Select Feed", width / 2f, y0feed + rowH * 0.65f, paint)

        // Back row (last row)
        val yBack = titleH + (totalRows - 1) * rowH
        if (hovered == totalRows - 1) {
            paint.color = android.graphics.Color.argb(80, 0, 180, 255)
            canvas.drawRect(0f, yBack, width.toFloat(), yBack + rowH, paint)
        }
        paint.color = android.graphics.Color.argb(220, 160, 160, 255)
        paint.textSize = 28f
        paint.textAlign = android.graphics.Paint.Align.CENTER
        canvas.drawText("← Back", width / 2f, yBack + rowH * 0.65f, paint)

        val pixels = IntArray(width * height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    fun renderHomebrewDetailBitmap(
        entryIdx: Int, isDownloading: Boolean, width: Int, height: Int
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        paint.color = android.graphics.Color.argb(240, 12, 12, 22)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        val entries = hwEntries
        val entry = entries.getOrNull(entryIdx)

        val titleH = 88f
        paint.color = android.graphics.Color.argb(255, 20, 50, 90)
        canvas.drawRect(0f, 0f, width.toFloat(), titleH, paint)

        paint.color = android.graphics.Color.WHITE
        paint.textSize = 36f
        paint.typeface = android.graphics.Typeface.DEFAULT_BOLD
        paint.textAlign = android.graphics.Paint.Align.CENTER
        canvas.drawText(entry?.name ?: "—", width / 2f, titleH - 22f, paint)

        val rowH = (height - titleH) / 4f
        val rows = listOf("← Back", "", "Open Website", "")

        if (entry != null) {
            // Info block
            val infoY = titleH + rowH * 0.15f
            paint.textAlign = android.graphics.Paint.Align.LEFT
            paint.textSize = 26f
            paint.typeface = android.graphics.Typeface.DEFAULT
            paint.color = android.graphics.Color.argb(200, 160, 200, 255)
            canvas.drawText("Author: ${entry.author}", 24f, infoY + 30f, paint)
            canvas.drawText("License: ${entry.license}", 24f, infoY + 64f, paint)
            canvas.drawText("System: ${entry.system.uppercase()}", 24f, infoY + 98f, paint)
            val mode = if (entry.distributionMode.equals("mirror", ignoreCase = true)) "Mirror"
                       else "Official"
            canvas.drawText("Source: ${entry.source.ifBlank { "Manual" }}", 24f, infoY + 132f, paint)
            canvas.drawText("Delivery: $mode", 24f, infoY + 166f, paint)
            paint.textSize = 20f
            paint.color = android.graphics.Color.argb(180, 120, 160, 220)
            val website = entry.website.ifBlank { entry.sourceEntryUrl }.take(52)
            canvas.drawText(website, 24f, infoY + 198f, paint)

            // Download/Delete button (row 1)
            val downloaded = hwDownloaded.contains(entry.download)
            val btnY1 = titleH + rowH
            val btnLabel = when {
                isDownloading -> "Downloading…"
                downloaded    -> "Delete"
                else          -> "Download"
            }
            val btnColor = when {
                isDownloading -> android.graphics.Color.argb(200, 80, 80, 80)
                downloaded    -> android.graphics.Color.argb(220, 180, 40, 40)
                else          -> android.graphics.Color.argb(220, 0, 160, 200)
            }
            paint.color = btnColor
            canvas.drawRoundRect(
                android.graphics.RectF(24f, btnY1 + 8f, width - 24f, btnY1 + rowH - 8f),
                16f, 16f, paint)
            paint.color = android.graphics.Color.WHITE
            paint.textSize = 32f
            paint.typeface = android.graphics.Typeface.DEFAULT_BOLD
            paint.textAlign = android.graphics.Paint.Align.CENTER
            canvas.drawText(btnLabel, width / 2f, btnY1 + rowH * 0.62f, paint)

            // Open Website button (row 2)
            val btnY2 = titleH + rowH * 2f
            paint.color = android.graphics.Color.argb(220, 30, 80, 160)
            canvas.drawRoundRect(
                android.graphics.RectF(24f, btnY2 + 8f, width - 24f, btnY2 + rowH - 8f),
                16f, 16f, paint)
            paint.color = android.graphics.Color.WHITE
            paint.textSize = 30f
            paint.typeface = android.graphics.Typeface.DEFAULT_BOLD
            paint.textAlign = android.graphics.Paint.Align.CENTER
            canvas.drawText("Open Website", width / 2f, btnY2 + rowH * 0.62f, paint)
        }

        // Back button (row 3)
        val btnY3 = titleH + rowH * 3f
        paint.color = android.graphics.Color.argb(180, 40, 40, 70)
        canvas.drawRoundRect(
            android.graphics.RectF(24f, btnY3 + 8f, width - 24f, btnY3 + rowH - 8f),
            16f, 16f, paint)
        paint.color = android.graphics.Color.argb(220, 160, 160, 255)
        paint.textSize = 30f
        paint.typeface = android.graphics.Typeface.DEFAULT_BOLD
        paint.textAlign = android.graphics.Paint.Align.CENTER
        canvas.drawText("← Back", width / 2f, btnY3 + rowH * 0.62f, paint)

        val pixels = IntArray(width * height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    companion object {
        private const val SAVE_AUTOMATION_FILE_NAME = "save_automation.ini"
        private const val PREF_HOME_BREW_FEED_INDEX = "homebrew_feed_index"
        private const val PREF_HOME_BREW_FEED_FILE_NAME = "homebrew_feed_file_name"
        private const val PREF_HOME_BREW_ONBOARDING_DONE = "homebrew_onboarding_done"
        private const val HOME_BREW_GITHUB_API_URL =
            "https://api.github.com/repos/maranone/QuestRetroDepth/contents/homebrew?ref=main"
        private val VALID_AUTOSAVE_INTERVALS = setOf(0, 5, 30, 60, 300)
        init { System.loadLibrary("questretrodepth_native") }
    }

    private data class SaveAutomationPrefs(
        val autosaveIntervalSeconds: Int = 30,
        val loadLastSaveEnabled: Boolean = true
    )

    private enum class RomFamily {
        Snes, Genesis, Nes, Gb, Gba, Gg, Pce, Sega32x, Atari2600, N64, Ds, Saturn, Dreamcast
    }
}
