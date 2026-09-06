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
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.TextView
import org.apache.commons.compress.archivers.sevenz.SevenZFile
import java.io.File
import java.io.FileOutputStream
import java.util.Locale
import java.util.zip.ZipInputStream

// Update this manually for each public build shown in the main menu.
private const val MAIN_MENU_BUILD_LABEL = "b009 - Trigger Happy"

open class QuestVrActivity : Activity() {
    @Volatile private var activeUiTheme = 0

    // The theme pass is deliberately an overlay: it never changes panel geometry or text flow.
    // Classic (0) is a no-op, preserving the established renderer exactly.
    fun setUiThemeId(themeId: Int) {
        activeUiTheme = themeId.coerceIn(0, 3)
    }

    private fun finishThemedPanel(canvas: android.graphics.Canvas, width: Int, height: Int) {
        val theme = activeUiTheme
        if (theme == 0) return
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)
        val accent = when (theme) {
            1 -> Color.rgb(55, 220, 190)
            2 -> Color.rgb(190, 135, 255)
            else -> Color.rgb(255, 165, 55)
        }
        val wash = when (theme) {
            1 -> Color.argb(18, 0, 110, 125)
            2 -> Color.argb(16, 120, 70, 180)
            else -> Color.argb(14, 150, 45, 25)
        }
        paint.color = wash
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)
        paint.color = Color.argb(220, Color.red(accent), Color.green(accent), Color.blue(accent))
        canvas.drawRect(0f, 0f, width.toFloat(), 5f, paint)
        paint.style = android.graphics.Paint.Style.STROKE
        paint.strokeWidth = 3f
        paint.color = Color.argb(130, Color.red(accent), Color.green(accent), Color.blue(accent))
        canvas.drawRect(2f, 2f, width - 2f, height - 2f, paint)
        paint.style = android.graphics.Paint.Style.FILL
    }

    private lateinit var statusView: TextView
    private val handler = Handler(Looper.getMainLooper())
    private var vrStarted = false
    @Volatile private var quickPresetRenameDialogOpen = false

    private var lastSavedRomFilename = ""
    private val prefs by lazy { getSharedPreferences("qrd_prefs", MODE_PRIVATE) }

    // BT gamepad state — digital keys and hat/stick axes tracked separately so they
    // can't cancel each other out (e.g. a centered stick won't clear a held dpad key).
    private var btKeyUp = false; private var btKeyDown = false
    private var btKeyLeft = false; private var btKeyRight = false
    private var btAxisUp = false; private var btAxisDown = false
    private var btAxisLeft = false; private var btAxisRight = false
    private var btA = false; private var btB = false
    private var btX = false; private var btY = false
    private var btL = false; private var btR = false
    private var btStart = false; private var btSelect = false

    private fun syncBtInput() = nativeSetBtInputState(
        btKeyUp || btAxisUp, btKeyDown || btAxisDown,
        btKeyLeft || btAxisLeft, btKeyRight || btAxisRight,
        btA, btB, btX, btY, btL, btR, btStart, btSelect
    )

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (updateBtKey(keyCode, true)) return true
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent?): Boolean {
        if (updateBtKey(keyCode, false)) return true
        return super.onKeyUp(keyCode, event)
    }

    private fun updateBtKey(keyCode: Int, pressed: Boolean): Boolean {
        val handled = when (keyCode) {
            KeyEvent.KEYCODE_DPAD_UP              -> { btKeyUp    = pressed; true }
            KeyEvent.KEYCODE_DPAD_DOWN            -> { btKeyDown  = pressed; true }
            KeyEvent.KEYCODE_DPAD_LEFT            -> { btKeyLeft  = pressed; true }
            KeyEvent.KEYCODE_DPAD_RIGHT           -> { btKeyRight = pressed; true }
            KeyEvent.KEYCODE_BUTTON_A             -> { btA        = pressed; true }
            KeyEvent.KEYCODE_BUTTON_B             -> { btB        = pressed; true }
            KeyEvent.KEYCODE_BUTTON_X             -> { btX        = pressed; true }
            KeyEvent.KEYCODE_BUTTON_Y             -> { btY        = pressed; true }
            KeyEvent.KEYCODE_BUTTON_L1            -> { btL        = pressed; true }
            KeyEvent.KEYCODE_BUTTON_R1            -> { btR        = pressed; true }
            KeyEvent.KEYCODE_BUTTON_L2            -> { btL        = pressed; true }
            KeyEvent.KEYCODE_BUTTON_R2            -> { btR        = pressed; true }
            KeyEvent.KEYCODE_BUTTON_START         -> { btStart    = pressed; true }
            KeyEvent.KEYCODE_BUTTON_SELECT,
            KeyEvent.KEYCODE_BUTTON_MODE          -> { btSelect   = pressed; true }
            else -> false
        }
        if (handled) syncBtInput()
        return handled
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        val src = event.source
        if (src and InputDevice.SOURCE_JOYSTICK == 0 &&
            src and InputDevice.SOURCE_GAMEPAD  == 0) {
            return super.onGenericMotionEvent(event)
        }
        val deadzone = 0.5f
        val hatX  = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hatY  = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        val stickX = event.getAxisValue(MotionEvent.AXIS_X)
        val stickY = event.getAxisValue(MotionEvent.AXIS_Y)
        btAxisLeft  = hatX < -deadzone || stickX < -deadzone
        btAxisRight = hatX >  deadzone || stickX >  deadzone
        btAxisUp    = hatY < -deadzone || stickY < -deadzone
        btAxisDown  = hatY >  deadzone || stickY >  deadzone
        syncBtInput()
        return true
    }

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
        if (javaClass != QuestVrActivity::class.java) return

        // The 2D Android UI is deliberately not shown. Everything the app does
        // lives in the OpenXR shell, and this window is only the surface the XR
        // session is created from — but the compositor still flashes it up
        // whenever the session starts or is torn down (ROM load, calibration),
        // which read as a stray "old menu" appearing in the headset. A plain
        // black window has nothing to flash. statusView is still created (the
        // status poll and the startup ROM loader write to it, and native holds
        // the JNI entry points for the presets), it is simply never attached to
        // the view hierarchy.
        statusView = TextView(this).apply {
            textSize = 16f
            setTextColor(Color.WHITE)
            text = "Starting OpenXR shell..."
        }

        setContentView(FrameLayout(this).apply { setBackgroundColor(Color.BLACK) })
    }

    override fun onResume() {
        super.onResume()
        if (javaClass != QuestVrActivity::class.java) return
        // Require external storage access before starting VR so getRomDirectory() can
        // create /storage/emulated/0/QuestRetroDepth/ and find ROMs inside it.
        if (!hasStoragePermission()) {
            requestStoragePermission()
            return  // don't start VR yet — wait for permission
        }
        if (!vrStarted) {
            vrStarted = true
            val startupPrefs = readSaveAutomationPrefs()
            // Only an explicit debug/CLI ROM request may auto-load at startup.
            // Do not force Metal Slug (or any other ROM) on ordinary launches:
            // that leaves a previous game active while we are trying to browse
            // or inspect another system.
            val devRomName = intent.getStringExtra("rom")
            val anyRomCandidate = findStartupRomCandidate(devRomName)
            val forceLoad = devRomName != null
            val startupCandidate = if (forceLoad || startupPrefs.loadLastRomEnabled) anyRomCandidate else null
            val openMenuOnStartup = startupCandidate == null
            statusView.text = nativeStartVr(
                this,
                openMenuOnStartup,
                startupPrefs.autosaveIntervalSeconds,
                startupPrefs.loadLastSaveEnabled
            )
            nativeSetHomebrewFeed(selectedHomebrewFeedIndex())
            if ((forceLoad || startupPrefs.loadLastRomEnabled) && startupCandidate != null) {
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

    private fun findStartupRomCandidate(overrideName: String? = null): File? {
        val romDir = File(getRomDirectory())
        Log.i("QuestRetroDepthXR", "findStartupRomCandidate: dir=${romDir.absolutePath} exists=${romDir.exists()}")

        // An explicit override (the "rom" intent extra, used for debug/CLI
        // launches like debug_rom.bat) needs a full recursive search --
        // MAME ROMs live two levels deep (roms/mame/<family>/name.zip), past
        // what the shallow scan below covers. Kept as a separate path (not
        // just widening the shallow scan) so the no-override startup case --
        // "load whatever was last played, or alphabetically first" -- keeps
        // its existing shallow/fast behavior across ~2000+ files.
        if (overrideName != null) {
            val match = romDir.walkTopDown()
                .firstOrNull { it.isFile && it.name == overrideName && isSupportedOrArchiveFile(it) }
            Log.i("QuestRetroDepthXR", "findStartupRomCandidate: recursive lookup for '$overrideName' -> ${match?.absolutePath}")
            if (match != null) return match
            // Fall through to the shallow scan/fallback below if not found,
            // same as before this override existed.
        }

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

        // Intent extra "rom" overrides pref; then fall back to last played; then first alphabetically.
        val lastName = (overrideName ?: prefs.getString("last_rom_path", null))
            ?.substringAfterLast('/')
        return if (lastName != null) {
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
        // Keep the original system folder as a backend hint. The extracted
        // file lives under app cache, so passing only romFile.absolutePath
        // makes nativeLoadRom fall back to MAME instead of FCEUmm for NES.
        val result = nativeLoadRom(romFile.absolutePath, candidate.absolutePath)
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
        var loadLastRomEnabled = true
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
                    "load_last_rom" -> loadLastRomEnabled = value == "1"
                }
            }
        }
        return SaveAutomationPrefs(autosaveIntervalSeconds, loadLastSaveEnabled, loadLastRomEnabled)
    }

    /** If the file is a raw ROM, return it as-is. If it's an archive, extract the ROM inside. */
    private fun prepareRomFile(file: File): File {
        val lower = file.name.lowercase(Locale.US)
        val path = file.absolutePath.lowercase(Locale.US)
        // MAME always needs the whole zip (it reads multiple named ROM chip
        // files out of it directly), never a single extracted file. Without
        // this guard, a Neo Geo zip whose internal chip files happen to be
        // named like "201-c1.bin" gets misidentified by extractZipRom's
        // extension sniffing as a Genesis ROM (".bin") and only that one
        // chip file gets extracted -- silently feeding MAME a corrupt
        // single-file "ROM" instead of the real multi-file romset.
        if (path.contains("/roms/mame/") || path.contains("\\roms\\mame\\")) return file
        val cacheDir = File(cacheDir, "roms").apply { mkdirs() }
        return when {
            lower.endsWith(".zip") -> extractZipRom(file, cacheDir, preferredRomFamily(file))
            lower.endsWith(".7z")  -> extract7zRom(file, cacheDir, preferredRomFamily(file))
            else                   -> file
        }
    }

    private fun extractZipRom(archive: File, targetDir: File, preferredFamily: RomFamily?): File {
        val cache = preparedArchiveCache(archive, targetDir)
        findPreparedRom(cache, preferredFamily)?.let {
            nativeRomPreparationProgress(archive.absolutePath, 100, 1, 1, it.name)
            return it
        }
        cache.mkdirs()
        ZipInputStream(archive.inputStream()).use { zip ->
            var entry = zip.nextEntry
            var fallback: File? = null
            while (entry != null) {
                if (!entry.isDirectory && isSupportedRomExtension(entry.name)) {
                    val entryName = entry.name.substringAfterLast('/')
                    val out = File(cache, sanitize(entryName))
                    out.delete()
                    val expected = entry.size
                    var copied = 0L
                    var lastPercent = -1
                    FileOutputStream(out).use {
                        val buf = ByteArray(8192)
                        while (true) {
                            val n = zip.read(buf)
                            if (n <= 0) break
                            it.write(buf, 0, n)
                            copied += n
                            if (expected > 0) {
                                val percent = ((copied * 100L) / expected).toInt().coerceIn(0, 100)
                                if (percent != lastPercent) {
                                    lastPercent = percent
                                    nativeRomPreparationProgress(archive.absolutePath, percent, 1, 1, entryName)
                                }
                            }
                        }
                    }
                    if (preferredFamily == null || romFamilyForName(entry.name) == preferredFamily) {
                        markPreparedArchive(cache)
                        return out
                    }
                    if (fallback == null) fallback = out
                }
                entry = zip.nextEntry
            }
            fallback?.let {
                markPreparedArchive(cache)
                nativeRomPreparationProgress(archive.absolutePath, 100, 1, 1, it.name)
                return it
            }
        }
        error("ZIP contains no supported ROM: ${archive.name}")
    }

    private fun extract7zRom(archive: File, targetDir: File, preferredFamily: RomFamily?): File {
        val cache = preparedArchiveCache(archive, targetDir)
        findPreparedRom(cache, preferredFamily)?.let {
            nativeRomPreparationProgress(archive.absolutePath, 100, 1, 1, it.name)
            return it
        }
        cache.mkdirs()
        // Saturn (.cue+.bin) and Dreamcast (.gdi+track files) are multi-file
        // disc images: the manifest (.cue/.gdi) references sibling track
        // files by name that don't themselves match isSupportedRomExtension
        // (e.g. plain .bin), so the single-matched-entry early return below
        // would extract only the manifest and silently leave every track
        // missing -- the core then hangs/fails trying to read a disc that's
        // mostly absent on disk. For these families, extract every entry in
        // the archive unconditionally; one 7z here is always exactly one
        // disc, so there's nothing else in it to skip.
        val extractWholeArchive = preferredFamily == RomFamily.Saturn ||
            preferredFamily == RomFamily.Psx ||
            preferredFamily == RomFamily.Dreamcast
        // 7z headers hold the full entry list up front, so this is a cheap
        // metadata-only pass (no decompression) -- lets the progress UI show
        // "file N/total" instead of the percent looping back to 0 per file
        // with no indication anything besides the current file exists.
        val fileTotal = if (extractWholeArchive) {
            SevenZFile(archive).use { it.entries.count { e -> !e.isDirectory } }
        } else 1
        var fileIndex = 0
        SevenZFile(archive).use { sz ->
            val buf = ByteArray(8192)
            var entry = sz.nextEntry
            var fallback: File? = null
            var manifest: File? = null
            while (entry != null) {
                if (!entry.isDirectory &&
                    (extractWholeArchive || isSupportedRomExtension(entry.name))) {
                    fileIndex++
                    val entryName = entry.name.substringAfterLast('/').substringAfterLast('\\')
                    // Multi-file disc images (Saturn .cue+.bin, Dreamcast .gdi+track)
                    // reference sibling files by their exact original name inside the
                    // manifest text itself -- sanitizing the on-disk filename here would
                    // desync it from what the manifest says to open, so the core's file
                    // opens would silently fail (ENOENT) even though the file exists.
                    val out = File(cache, if (extractWholeArchive) entryName else sanitize(entryName))
                    out.delete()
                    val expected = entry.size
                    var copied = 0L
                    var lastPercent = -1
                    FileOutputStream(out).use { fos ->
                        var rem = expected
                        while (rem > 0) {
                            val n = sz.read(buf, 0, minOf(buf.size.toLong(), rem).toInt())
                            if (n <= 0) break
                            fos.write(buf, 0, n)
                            rem -= n
                            copied += n
                            if (expected > 0) {
                                val percent = ((copied * 100L) / expected).toInt().coerceIn(0, 100)
                                if (percent != lastPercent) {
                                    lastPercent = percent
                                    nativeRomPreparationProgress(
                                        archive.absolutePath, percent, fileIndex, fileTotal, entryName)
                                }
                            }
                        }
                    }
                    if (lastPercent != 100) {
                        nativeRomPreparationProgress(
                            archive.absolutePath, 100, fileIndex, fileTotal, entryName)
                    }
                    if (isSupportedRomExtension(entry.name)) {
                        if (preferredFamily == null || romFamilyForName(entry.name) == preferredFamily) {
                            if (extractWholeArchive) manifest = out
                            else { markPreparedArchive(cache); return out }
                        } else if (fallback == null) {
                            fallback = out
                        }
                    }
                }
                entry = sz.nextEntry
            }
            manifest?.let {
                markPreparedArchive(cache)
                nativeRomPreparationProgress(archive.absolutePath, 100, fileTotal, fileTotal, it.name)
                return it
            }
            fallback?.let {
                markPreparedArchive(cache)
                nativeRomPreparationProgress(archive.absolutePath, 100, fileTotal, fileTotal, it.name)
                return it
            }
        }
        error("7z contains no supported ROM: ${archive.name}")
    }

    private fun preparedArchiveCache(archive: File, targetDir: File): File {
        // The source path plus size/mtime is stable across launches and avoids
        // decompressing the same archive again. A new archive revision gets a
        // different directory automatically.
        val key = "${archive.absolutePath}|${archive.length()}|${archive.lastModified()}"
        return File(targetDir, "qrd_extract_${Integer.toHexString(key.hashCode())}")
    }

    private fun findPreparedRom(cache: File, preferredFamily: RomFamily?): File? {
        if (!File(cache, ".complete").isFile) return null
        val files = cache.listFiles()?.filter { it.isFile && it.name != ".complete" &&
            isSupportedRomExtension(it.name) } ?: return null
        return files.firstOrNull {
            preferredFamily == null ||
                romFamilyForName(it.name) == preferredFamily ||
                (preferredFamily == RomFamily.Psx && isPsxExtension(it.name))
        }
            ?: files.firstOrNull()
    }

    private fun markPreparedArchive(cache: File) {
        File(cache, ".complete").writeText("ready\n")
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

    private fun isDsExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".nds")
    }

    private fun isSaturnExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".cue") || lower.endsWith(".iso") || lower.endsWith(".chd")
    }

    private fun isPsxExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".cue") || lower.endsWith(".iso") || lower.endsWith(".chd") ||
               lower.endsWith(".img") || lower.endsWith(".mds") || lower.endsWith(".pbp")
    }

    private fun isDreamcastExtension(name: String): Boolean {
        val lower = name.lowercase(Locale.US)
        return lower.endsWith(".gdi") || lower.endsWith(".cdi")
    }

    private fun isSupportedRomExtension(name: String): Boolean {
        return isSnesExtension(name) || isGenesisExtension(name) || isNesExtension(name) ||
               isGbExtension(name) || isGbaExtension(name) || isGgExtension(name) ||
               isPceExtension(name) || is32xExtension(name) || isAtari2600Extension(name) ||
               isDsExtension(name) || isSaturnExtension(name) || isPsxExtension(name) ||
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
            path.contains("/roms/ds/") || path.contains("\\roms\\ds\\") -> RomFamily.Ds
            path.contains("/roms/saturn/") || path.contains("\\roms\\saturn\\") -> RomFamily.Saturn
            path.contains("/roms/psx/") || path.contains("\\roms\\psx\\") ||
                path.contains("/roms/ps1/") || path.contains("\\roms\\ps1\\") -> RomFamily.Psx
            path.contains("/roms/dreamcast/") || path.contains("\\roms\\dreamcast\\") -> RomFamily.Dreamcast
            else -> romFamilyForName(file.name)
        }
    }

    private fun sanitize(name: String) = name.replace(Regex("[^A-Za-z0-9._ -]"), "_")

    // --- Background music -------------------------------------------------
    // Random track from assets/bgm/, played while browsing menus. Called from
    // native (openxr_shell.cpp) via edge-detected transitions on m_menu_open
    // and the live-preview hover state, so these fire exactly once per real
    // transition regardless of which code path caused it.
    // User preference (Audio > Music > Background Music in the new menu; the
    // real persisted value lives in VrState::bgm_enabled — this mirrors it so
    // the automatic bgmEnterMenu() edge-trigger below can respect it without
    // a native round-trip on every menu-open). Defaults true so behavior is
    // unchanged for anyone who's never touched the new toggle.
    private var bgmEnabled = true
    // User-controlled menu-music volume (0..1), independent of ROM/game audio
    // volume. Real persisted value lives in VrState::bgm_volume; every
    // "fade to audible" target below uses this instead of a hardcoded 1f.
    private var bgmUserVolume = 0.5f
    private var bgmPlayer: android.media.MediaPlayer? = null
    private var bgmTargetVolume = 1f
    private val bgmFadeRunnable = object : Runnable {
        override fun run() {
            val p = bgmPlayer ?: return
            val cur = bgmCurrentVolume
            val step = bgmFadeStep
            val next = if (bgmTargetVolume > cur) (cur + step).coerceAtMost(bgmTargetVolume)
                       else (cur - step).coerceAtLeast(bgmTargetVolume)
            bgmCurrentVolume = next
            try { p.setVolume(next, next) } catch (_: Exception) {}
            if (next != bgmTargetVolume) {
                handler.postDelayed(this, 30)
            } else if (next <= 0f && bgmStopWhenSilent) {
                try { p.stop(); p.release() } catch (_: Exception) {}
                bgmPlayer = null
            }
        }
    }
    private var bgmCurrentVolume = 0f
    private var bgmFadeStep = 0f
    private var bgmStopWhenSilent = false

    // Bgm-driven haptics (Visualizer -> nativeSetBgmRms) removed: it was suspected of
    // interfering with ROM loading, and wasn't working anyway. Bgm playback itself is
    // unaffected — only the controller-rumble tie-in was pulled out.

    private fun bgmFadeTo(target: Float, durationMs: Long, stopWhenSilent: Boolean) {
        handler.removeCallbacks(bgmFadeRunnable)
        bgmTargetVolume = target
        bgmStopWhenSilent = stopWhenSilent
        val steps = (durationMs / 30L).coerceAtLeast(1)
        bgmFadeStep = kotlin.math.abs(target - bgmCurrentVolume) / steps
        if (bgmFadeStep <= 0f) bgmFadeStep = 1f
        handler.post(bgmFadeRunnable)
    }

    // Called from the new menu's Background Music checkbox (Audio > Music) —
    // that checkbox lives inside the menu, which is only reachable while
    // menu music should already be playing, so this can safely just replay
    // the same track-pick-and-fade-in bgmEnterMenu() does rather than needing
    // separate "resume" logic.
    fun bgmEnable() {
        bgmEnabled = true
        bgmEnterMenu()
    }

    // Called from the new menu's Background Music checkbox, and once at
    // startup (nativeStartVr) if the loaded VrState::bgm_enabled was false,
    // so a saved "off" preference is respected before any automatic
    // bgmEnterMenu() edge-trigger could start playback.
    fun bgmDisable() {
        bgmEnabled = false
        bgmStopImmediately()
    }

    // Entering the menu system: pick a random track and slow-fade it in (~3s).
    fun bgmEnterMenu() {
        if (!bgmEnabled) return
        try {
            val tracks = assets.list("bgm")?.filter { it.endsWith(".ogg") } ?: emptyList()
            if (tracks.isEmpty()) return
            if (bgmPlayer == null) {
                val name = tracks[(Math.random() * tracks.size).toInt()]
                val afd = assets.openFd("bgm/$name")
                val p = android.media.MediaPlayer()
                p.setDataSource(afd.fileDescriptor, afd.startOffset, afd.length)
                afd.close()
                p.isLooping = true
                p.setVolume(0f, 0f)
                p.prepare()
                p.start()
                bgmPlayer = p
                bgmCurrentVolume = 0f
            }
            bgmFadeTo(bgmUserVolume, 3000L, stopWhenSilent = false)
        } catch (e: Exception) {
            Log.w("QuestVrActivity", "bgmEnterMenu failed: ${e.message}")
        }
    }

    // Leaving the menu system (ROM launched, or back to a running game): quick fade-out + stop.
    fun bgmExitMenu() {
        if (bgmPlayer == null) return
        bgmFadeTo(0f, 300L, stopWhenSilent = true)
    }

    // Hovering a ROM for live preview: the preview's own audio starts essentially
    // instantly (request_live() on the native side), so a slow duck fade left bgm
    // audibly overlapping it for the whole fade window — duck near-instantly instead.
    fun bgmDuck() {
        if (bgmPlayer == null) return
        bgmFadeTo(0f, 40L, stopWhenSilent = false)
    }

    // Live preview hover ended: slow fade back in (~3s).
    fun bgmUnduck() {
        if (bgmPlayer == null) return
        bgmFadeTo(bgmUserVolume, 3000L, stopWhenSilent = false)
    }

    // Called from the new menu's BG Music Volume slider (Audio > Music) —
    // updates the live-playing track immediately (skipping the fade) as long
    // as music is meant to be audible right now; if it's mid-duck/exit-fade
    // toward silence, just remember the new target for the next fade-in.
    fun bgmSetVolume(v: Float) {
        bgmUserVolume = v.coerceIn(0f, 1f)
        val p = bgmPlayer ?: return
        if (bgmTargetVolume > 0f) {
            handler.removeCallbacks(bgmFadeRunnable)
            bgmTargetVolume = bgmUserVolume
            bgmCurrentVolume = bgmUserVolume
            try { p.setVolume(bgmUserVolume, bgmUserVolume) } catch (_: Exception) {}
        }
    }

    private fun bgmStopImmediately() {
        handler.removeCallbacks(bgmFadeRunnable)
        try { bgmPlayer?.stop(); bgmPlayer?.release() } catch (_: Exception) {}
        bgmPlayer = null
    }

    // Called from native (off the UI thread — this JNI call runs on the native/XR thread,
    // not posted through the Looper) right before loading a ROM, so bgm's MediaPlayer isn't
    // still holding an audio session when the ROM's own AAudio stream opens. MUST post to the
    // main Handler rather than touching bgmPlayer directly here: the fade runnable also calls
    // MediaPlayer methods from the main thread, and MediaPlayer is not safe to call
    // concurrently from two threads — doing so here previously deadlocked ROM loading.
    fun bgmStopImmediate() {
        handler.post { bgmStopImmediately() }
    }

    override fun onPause() {
        handler.removeCallbacks(statusPoll)
        super.onPause()
    }

    override fun onDestroy() {
        bgmStopImmediately()
        if (javaClass != QuestVrActivity::class.java) {
            super.onDestroy()
            return
        }
        nativeStopVr()
        vrStarted = false
        super.onDestroy()
    }

    private external fun nativeSetBtInputState(
        up: Boolean, down: Boolean, left: Boolean, right: Boolean,
        a: Boolean, b: Boolean, x: Boolean, y: Boolean,
        l: Boolean, r: Boolean, start: Boolean, select: Boolean
    )
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
    // Called by the shared ZIP/7z preparation path while a foreground ROM is
    // being unpacked. The XR shelf renders the progress; this callback is a
    // no-op for background thumbnail preparation.
    // fileIndex/fileTotal/fileName let a multi-file disc image (Saturn cue+bin,
    // Dreamcast gdi+tracks) show which of the N files is currently extracting
    // instead of the percent looping back to 0 for each one with no context.
    // Single-file archives just pass (1, 1, entryName).
    private external fun nativeRomPreparationProgress(
        path: String, percent: Int, fileIndex: Int, fileTotal: Int, fileName: String)
    private external fun nativeGetLastLoadedRomFilename(): String
    private external fun nativeApplyStateCode(code: String): Boolean
    private external fun nativeOpenMainMenu()
    private external fun nativeOpenHomebrew()
    private external fun nativeSubmitQuickPresetName(kind: Int, slot: Int, name: String)
    private external fun nativeCancelQuickPresetName(kind: Int, slot: Int)
    private external fun nativeSubmitRomSearch(text: String)
    private external fun nativeCancelRomSearch()
    private external fun nativeHomebrewDataReady()
    private external fun nativeHomebrewDownloadComplete(entryIdx: Int)
    private external fun nativeSetHomebrewFeed(idx: Int)

    @Volatile private var romSearchDialogOpen = false

    // ROM search (new ImGui Library tab): ImGui's own text fields have no real
    // OS keyboard behind them (input here is a laser+trigger, not a physical
    // keyboard), so the search box triggers this real Android dialog instead —
    // Quest overlays its system keyboard automatically for any focused
    // EditText, same as showQuickPresetRenameDialog already relies on.
    fun showRomSearchDialog(currentText: String) {
        // NOTE: confirmed on-device this dialog never actually renders while
        // an immersive OpenXR session is running (it's a plain 2D Android
        // window; the VR compositor doesn't composite it) — kept only for the
        // JNI plumbing (nativeSubmitRomSearch/nativeCancelRomSearch); the
        // Library tab now uses a real in-VR ImGui keyboard instead
        // (draw_rom_search_keyboard() in openxr_shell.cpp).
        runOnUiThread {
            if (isFinishing || (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1 && isDestroyed)) {
                nativeCancelRomSearch()
                return@runOnUiThread
            }
            if (romSearchDialogOpen) {
                nativeCancelRomSearch()
                return@runOnUiThread
            }
            romSearchDialogOpen = true
            val input = EditText(this).apply {
                setText(currentText)
                setSelection(text.length)
                setTextColor(Color.WHITE)
                setHintTextColor(Color.GRAY)
                hint = "ROM name"
            }
            try {
                AlertDialog.Builder(this)
                    .setTitle("Search ROMs")
                    .setView(input)
                    .setCancelable(true)
                    .setPositiveButton("Search") { _, _ ->
                        romSearchDialogOpen = false
                        nativeSubmitRomSearch(input.text?.toString() ?: "")
                    }
                    .setNegativeButton("Cancel") { _, _ ->
                        romSearchDialogOpen = false
                        nativeCancelRomSearch()
                    }
                    .setOnCancelListener {
                        romSearchDialogOpen = false
                        nativeCancelRomSearch()
                    }
                    .show()
            } catch (e: Exception) {
                romSearchDialogOpen = false
                nativeCancelRomSearch()
            }
        }
    }

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
        finishThemedPanel(canvas, width, height)
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
        layerSideColor: IntArray, // 0=Ori 1=Black 2=White 3=Red 4=Green 5=Blue, per layer
        grabbed: Int,           // row being dragged (-1 = none)
        dropTarget: Int,        // row laser is pointing at while dragging (-1 = none)
        depthValues: FloatArray, // depth_meters per layer in display order
        depthSelectedRow: Int,  // row in depth-edit mode (-1 = none)
        width: Int,
        height: Int,
        frozen: Boolean,        // frozen state for play/pause button
        autoDupLabel: String,
        filterLabel: String,
        showFilter: Boolean,
        geometryModeLabels: Array<String>, // "BOX"/"FLR"/"CEIL"/"SYM" per layer, display order
        thicknessValues: FloatArray,       // box_thickness_meters per layer; 0 = auto
        layerModeLabel: String             // Neo Geo only: composition-mode label, "" elsewhere
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

        // Layer-composition mode toggle pill (Neo Geo only, PanelRole::LayerModeToggle
        // hit region is {0.66..1.00, 0..titleV} -- see make_layers_layout() in panel_layout.cpp).
        if (layerModeLabel.isNotEmpty()) {
            val pillLeft = width * 0.66f
            paint.color = android.graphics.Color.argb(255, 70, 100, 150)
            canvas.drawRect(pillLeft, 8f, width - 8f, 80f, paint)
            paint.color = android.graphics.Color.WHITE
            paint.textSize = 30f
            paint.textAlign = android.graphics.Paint.Align.CENTER
            canvas.drawText(layerModeLabel, (pillLeft + width - 8f) / 2f, 52f, paint)
            paint.textAlign = android.graphics.Paint.Align.LEFT
        }

        val titleH = 88f
        val n      = layerNames.size
        val trailingRows = 3 + if (showFilter) 1 else 0  // play/pause + auto-dup + reset-depths + optional filter
        // Each layer takes 2 slots now: the name/visibility/ambilight row, plus a compact second
        // row for the real-geometry mode toggle and distance/thickness +/- controls. Trailing
        // rows take 2 slots each too, so they stay the same absolute height as before this split.
        val totalSlots = n * 2 + trailingRows * 2
        if (n == 0) {
            paint.color = android.graphics.Color.argb(160, 150, 150, 160)
            paint.textSize = 36f
            canvas.drawText("No layers", 14f, titleH + 72f, paint)
        }

        val rowH = ((height - titleH) / totalSlots).toFloat().coerceAtLeast(28f)
        paint.textSize = rowH * 0.46f

        val isDragging = (grabbed >= 0)

        // Compute depth display scale: map depth_meters → 0–10 using min/max of the array
        val depthMin = if (depthValues.isNotEmpty()) depthValues.min() else 0.9f
        val depthMax = if (depthValues.isNotEmpty()) depthValues.max() else 2.0f
        val depthRange = if (depthMax > depthMin) depthMax - depthMin else 1.0f

        fun depthDisplay(depthMeters: Float): String {
            val t = (depthMeters - depthMin) / depthRange
            val d = (1.0f - t) * 10.0f
            return "%.1f".format(d.coerceIn(0.0f, 10.0f))
        }

        // Render layer rows (indices 0 to n-1)
        for (i in 0 until n) {
            val y          = titleH + i * 2 * rowH
            val enabled    = if (i < layerEnabled.size) layerEnabled[i] else true
            val isGrabbed  = (i == grabbed)
            val isDepthSel = (i == depthSelectedRow)

            // Row background
            when {
                isDepthSel -> {
                    paint.color = android.graphics.Color.argb(120, 40, 100, 60)
                    canvas.drawRect(0f, y, width * 0.60f, y + rowH, paint)
                    if (i % 2 == 0) {
                        paint.color = android.graphics.Color.argb(70, 40, 40, 65)
                        canvas.drawRect(width * 0.60f, y, width.toFloat(), y + rowH, paint)
                    }
                }
                isGrabbed -> {
                    paint.color = android.graphics.Color.argb(100, 60, 120, 220)
                    canvas.drawRect(2f, y + 1f, width - 2f, y + rowH - 1f, paint)
                }
                i % 2 == 0 -> {
                    paint.color = android.graphics.Color.argb(70, 40, 40, 65)
                    canvas.drawRect(0f, y, width.toFloat(), y + rowH, paint)
                }
            }

            // Depth value in left zone
            val depthStr = if (i < depthValues.size) depthDisplay(depthValues[i]) else ""
            paint.textSize = rowH * 0.38f
            paint.color = if (isDepthSel) android.graphics.Color.argb(255, 100, 240, 140)
                          else android.graphics.Color.argb(160, 140, 200, 160)
            canvas.drawText(depthStr, 8f, y + rowH * 0.65f, paint)
            paint.textSize = rowH * 0.46f

            // Layer name
            paint.color = when {
                isGrabbed   -> android.graphics.Color.argb(100, 180, 190, 220)
                !enabled    -> android.graphics.Color.argb(110, 150, 150, 160)
                else        -> android.graphics.Color.argb(220, 200, 210, 230)
            }
            canvas.save()
            canvas.clipRect(width * 0.22f, y, width * 0.58f, y + rowH)
            canvas.drawText(layerNames[i], width * 0.22f, y + rowH * 0.70f, paint)
            canvas.restore()

            // Visibility toggle, ambilight toggle, and side-color-override swatch
            if (!isGrabbed) {
                val visX   = width * 0.60f
                val ambX   = width * 0.80f
                val sideX  = width * 0.90f
                val ambiEnabled = if (i < layerAmbilight.size) layerAmbilight[i] else true
                val sideMode = if (i < layerSideColor.size) layerSideColor[i] else 0

                paint.color = if (enabled) android.graphics.Color.argb(200, 80, 200, 100)
                              else android.graphics.Color.argb(120, 180, 60, 60)
                canvas.drawRoundRect(visX, y + rowH * 0.18f, ambX - 4f, y + rowH * 0.82f, 6f, 6f, paint)
                paint.color = android.graphics.Color.WHITE
                paint.textSize = rowH * 0.36f
                canvas.drawText(if (enabled) "ON" else "OFF", visX + 4f, y + rowH * 0.68f, paint)

                paint.color = if (ambiEnabled) android.graphics.Color.argb(200, 255, 170, 50)
                              else android.graphics.Color.argb(100, 100, 100, 100)
                canvas.drawRoundRect(ambX, y + rowH * 0.18f, sideX - 4f, y + rowH * 0.82f, 6f, 6f, paint)
                paint.color = android.graphics.Color.WHITE
                paint.textSize = rowH * 0.30f
                canvas.drawText("AMB", ambX + 3f, y + rowH * 0.68f, paint)

                // Side-color swatch: solid fill in the chosen color, or a hollow outline
                // labeled "ORI" when left at the original (texture-sampled) behaviour.
                val swLabel = when (sideMode) {
                    1 -> "BLK"; 2 -> "WHT"; 3 -> "RED"; 4 -> "GRN"; 5 -> "BLU"; else -> "ORI"
                }
                if (sideMode == 0) {
                    paint.style = android.graphics.Paint.Style.STROKE
                    paint.strokeWidth = 2f
                    paint.color = android.graphics.Color.argb(160, 170, 170, 180)
                    canvas.drawRoundRect(sideX, y + rowH * 0.18f, width - 4f, y + rowH * 0.82f, 6f, 6f, paint)
                    paint.style = android.graphics.Paint.Style.FILL
                    paint.color = android.graphics.Color.argb(200, 170, 170, 180)
                } else {
                    paint.color = when (sideMode) {
                        1 -> android.graphics.Color.argb(220, 20, 20, 20)
                        2 -> android.graphics.Color.argb(220, 235, 235, 235)
                        3 -> android.graphics.Color.argb(220, 230, 60, 60)
                        4 -> android.graphics.Color.argb(220, 70, 210, 100)
                        else -> android.graphics.Color.argb(220, 70, 140, 235)
                    }
                    canvas.drawRoundRect(sideX, y + rowH * 0.18f, width - 4f, y + rowH * 0.82f, 6f, 6f, paint)
                    paint.color = if (sideMode == 2) android.graphics.Color.BLACK else android.graphics.Color.WHITE
                }
                canvas.drawText(swLabel, sideX + 3f, y + rowH * 0.68f, paint)

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

            // Secondary row: real-geometry mode toggle + distance/thickness +/- controls.
            // Hidden while dragging this row, matching the visibility/ambilight buttons above.
            if (!isGrabbed) {
                val y2 = y + rowH
                paint.color = android.graphics.Color.argb(55, 30, 30, 50)
                canvas.drawRect(0f, y2, width.toFloat(), y2 + rowH, paint)

                val geomLabel = if (i < geometryModeLabels.size) geometryModeLabels[i] else "BOX"
                val thickVal  = if (i < thicknessValues.size) thicknessValues[i] else 0f
                val thickLabel = when {
                    geomLabel == "SPLTF" || geomLabel == "SPLTC" -> "${thickVal.toInt()}px"
                    thickVal <= 0.0001f -> "AUTO"
                    else                -> "%.2f".format(thickVal)
                }

                paint.textSize = rowH * 0.40f

                // MODE button (0.00-0.28)
                paint.color = android.graphics.Color.argb(190, 90, 90, 160)
                canvas.drawRoundRect(4f, y2 + rowH * 0.12f, width * 0.28f - 4f, y2 + rowH * 0.88f, 6f, 6f, paint)
                paint.color = android.graphics.Color.WHITE
                canvas.drawText(geomLabel, 10f, y2 + rowH * 0.66f, paint)

                // DIST -/label/+ (0.30-0.68)
                fun stepBtn(x0: Float, x1: Float, label: String) {
                    paint.color = android.graphics.Color.argb(150, 70, 90, 130)
                    canvas.drawRoundRect(x0, y2 + rowH * 0.12f, x1, y2 + rowH * 0.88f, 5f, 5f, paint)
                    paint.color = android.graphics.Color.WHITE
                    val w = paint.measureText(label)
                    canvas.drawText(label, x0 + (x1 - x0 - w) / 2f, y2 + rowH * 0.66f, paint)
                }
                stepBtn(width * 0.30f, width * 0.36f, "-")
                paint.color = android.graphics.Color.argb(200, 150, 210, 170)
                val distMeters = if (i < depthValues.size) depthValues[i] else 0f
                val distLabel = "%.2fm".format(distMeters)
                val distW = paint.measureText(distLabel)
                canvas.drawText(distLabel, width * 0.36f + (width * 0.26f - distW) / 2f, y2 + rowH * 0.66f, paint)
                stepBtn(width * 0.62f, width * 0.68f, "+")

                // THICK -/label/+ (0.70-0.98)
                stepBtn(width * 0.70f, width * 0.76f, "-")
                paint.color = android.graphics.Color.argb(200, 210, 180, 150)
                val thickW = paint.measureText(thickLabel)
                canvas.drawText(thickLabel, width * 0.76f + (width * 0.16f - thickW) / 2f, y2 + rowH * 0.66f, paint)
                stepBtn(width * 0.92f, width * 0.98f, "+")

                paint.textSize = rowH * 0.46f
            }
        }

        // Trailing rows each take 2 slots (matching make_layers_layout's trailing-row sizing),
        // so their absolute height on screen is unchanged from before the per-layer row split.
        val rowH2 = rowH * 2f
        val trailingBase = titleH + n * 2 * rowH

        // Play/Pause button as last row (index n)
        val pauseY = trailingBase
        val isPauseHovered = (dropTarget == n)  // use dropTarget for hover state

        // Row background
        paint.color = if (isPauseHovered) android.graphics.Color.argb(100, 100, 140, 200)
                      else android.graphics.Color.argb(70, 40, 40, 65)
        canvas.drawRect(0f, pauseY, width.toFloat(), pauseY + rowH2, paint)

        // Play/Pause icon and text
        paint.color = if (frozen) android.graphics.Color.argb(220, 80, 160, 255)  // blue when paused
                      else android.graphics.Color.argb(220, 60, 180, 80)   // green when playing
        val iconLabel = if (frozen) "\u25B6 PLAY" else "\u275A\u275A PAUSED"
        paint.textSize = rowH2 * 0.50f
        val labelW = paint.measureText(iconLabel)
        val labelX = (width - labelW) / 2
        canvas.drawText(iconLabel, labelX, pauseY + rowH2 * 0.68f, paint)

        // Auto duplication row after play/pause (index n + 1)
        val autoDupY = trailingBase + rowH2
        val isAutoDupHovered = (dropTarget == n + 1)
        paint.color = if (isAutoDupHovered) android.graphics.Color.argb(100, 100, 140, 200)
                      else android.graphics.Color.argb(70, 40, 40, 65)
        canvas.drawRect(0f, autoDupY, width.toFloat(), autoDupY + rowH2, paint)

        paint.textSize = rowH2 * 0.42f
        paint.color = android.graphics.Color.argb(220, 210, 220, 240)
        canvas.drawText("AUTO DUP", 16f, autoDupY + rowH2 * 0.68f, paint)

        paint.color = if (autoDupLabel == "OFF") android.graphics.Color.argb(180, 160, 170, 180)
                      else android.graphics.Color.argb(220, 255, 205, 100)
        val autoDupW = paint.measureText(autoDupLabel)
        canvas.drawText(autoDupLabel, width - autoDupW - 16f, autoDupY + rowH2 * 0.68f, paint)

        // Reset depths row (index n + 2)
        val resetY = trailingBase + rowH2 * 2f
        val isResetHovered = (dropTarget == n + 2)
        paint.color = if (isResetHovered) android.graphics.Color.argb(120, 60, 130, 80)
                      else android.graphics.Color.argb(70, 30, 55, 40)
        canvas.drawRect(0f, resetY, width.toFloat(), resetY + rowH2, paint)
        paint.textSize = rowH2 * 0.42f
        paint.color = android.graphics.Color.argb(200, 120, 220, 140)
        val resetLabel = "RESET DEPTHS"
        val resetW = paint.measureText(resetLabel)
        canvas.drawText(resetLabel, (width - resetW) / 2f, resetY + rowH2 * 0.68f, paint)

        if (showFilter) {
            // Layer filter row (index n + 3)
            val filterY = trailingBase + rowH2 * 3f
            val isFilterHovered = (dropTarget == n + 3)
            paint.color = if (isFilterHovered) android.graphics.Color.argb(100, 100, 140, 200)
                          else android.graphics.Color.argb(70, 40, 40, 65)
            canvas.drawRect(0f, filterY, width.toFloat(), filterY + rowH2, paint)

            paint.textSize = rowH2 * 0.42f
            paint.color = android.graphics.Color.argb(220, 210, 220, 240)
            canvas.drawText("LAYER FILTER", 16f, filterY + rowH2 * 0.68f, paint)

            paint.color = android.graphics.Color.argb(220, 120, 210, 255)
            val filterW = paint.measureText(filterLabel)
            canvas.drawText(filterLabel, width - filterW - 16f, filterY + rowH2 * 0.68f, paint)
        }

        val pixels = IntArray(width * height)
        finishThemedPanel(canvas, width, height)
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
        finishThemedPanel(canvas, width, height)
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

            // Draw separator before action buttons
            if (i == 23) {
                paint.color = android.graphics.Color.argb(120, 100, 130, 200)
                paint.strokeWidth = 2f
                paint.style = android.graphics.Paint.Style.STROKE
                canvas.drawLine(8f, y + 1f, width - 8f, y + 1f, paint)
                paint.style = android.graphics.Paint.Style.FILL
                paint.strokeWidth = 0f
            }

            if (isAction) {
                // Full-width action buttons
                paint.color = when {
                    names.getOrNull(i)?.startsWith("Calibrate") == true -> android.graphics.Color.argb(175, 155, 80, 35)
                    names.getOrNull(i)?.startsWith("Reset") == true -> android.graphics.Color.argb(170, 160, 30, 30)
                    names.getOrNull(i)?.startsWith("Save") == true -> android.graphics.Color.argb(170, 35, 140, 65)
                    names.getOrNull(i)?.startsWith("Load") == true -> android.graphics.Color.argb(170, 35, 95, 180)
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
                val disabledHint = if (names.getOrNull(i)?.startsWith("Calibrate") == true)
                    "ENABLE LIGHTGUN FIRST" else "LOAD A ROM FIRST"
                canvas.drawText(disabledHint, 18f, y + rowH * 0.82f, paint)
            } else if (i == 18) {
                // Side Panels: compact fixed buttons; Themes is intentionally not in this bar.
                val labels = arrayOf("OFF", "HELP", "SETT", "PERF", "BGC")
                val btnCount = labels.size
                val btnWidth = width.toFloat() / btnCount
                for (b in 0 until btnCount) {
                    val bx0 = b * btnWidth
                    val bx1 = (b + 1) * btnWidth
                    val active = (labels[b] == value)
                    paint.color = if (active) android.graphics.Color.argb(210, 50, 180, 90)
                                  else android.graphics.Color.argb(120, 60, 65, 85)
                    canvas.drawRoundRect(bx0 + 3f, y + rowH * 0.18f, bx1 - 3f, y + rowH * 0.82f, 8f, 8f, paint)
                    paint.color = android.graphics.Color.WHITE
                    paint.textSize = rowH * 0.26f
                    val tw = paint.measureText(labels[b])
                    canvas.drawText(labels[b], bx0 + (btnWidth - tw) / 2f, y + rowH * 0.62f, paint)
                }
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
        finishThemedPanel(canvas, width, height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    fun renderSidePanelBarBitmap(
        labels: Array<String>,
        activeMode: Int,
        hoveredId: Int,
        width: Int,
        height: Int
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint  = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        // Always-visible mode-select bar: dim (25%) until the laser is hovering it, full opacity
        // while hovered — the whole bar fades as one unit via saveLayerAlpha, not per-element.
        val hovered = hoveredId >= 0
        val opacity = if (hovered) 255 else 64 // 64/255 ~= 25%
        canvas.saveLayerAlpha(0f, 0f, width.toFloat(), height.toFloat(), opacity)

        paint.color = android.graphics.Color.argb(235, 12, 14, 24)
        canvas.drawRoundRect(0f, 0f, width.toFloat(), height.toFloat(), 16f, 16f, paint)

        val btnCount = labels.size
        val btnWidth = width.toFloat() / btnCount
        for (b in 0 until btnCount) {
            val bx0 = b * btnWidth
            val bx1 = (b + 1) * btnWidth
            val active = (b == activeMode)
            val isHoveredBtn = (b == hoveredId)
            paint.color = when {
                active -> android.graphics.Color.argb(230, 50, 180, 90)
                isHoveredBtn -> android.graphics.Color.argb(200, 90, 100, 130)
                else -> android.graphics.Color.argb(150, 55, 60, 80)
            }
            canvas.drawRoundRect(bx0 + 4f, 6f, bx1 - 4f, height - 6f, 10f, 10f, paint)
            paint.color = android.graphics.Color.WHITE
            paint.textSize = height * 0.30f
            val tw = paint.measureText(labels[b])
            canvas.drawText(labels[b], bx0 + (btnWidth - tw) / 2f, height * 0.65f, paint)
        }

        canvas.restore()

        val pixels = IntArray(width * height)
        finishThemedPanel(canvas, width, height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    fun renderBgColorPanelBitmap(
        colors: IntArray, // [0-7]=solid, [8-15]=gradient top, [16-23]=gradient bottom
        activeIndex: Int,
        hoveredId: Int,
        width: Int,
        height: Int
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint  = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        paint.color = android.graphics.Color.argb(235, 12, 14, 24)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        val titleH = 88f
        paint.color = android.graphics.Color.argb(255, 25, 20, 40)
        canvas.drawRect(0f, 0f, width.toFloat(), titleH, paint)
        paint.color = android.graphics.Color.WHITE
        paint.textSize = 40f
        paint.isFakeBoldText = true
        canvas.drawText("Background Color", 12f, 58f, paint)
        paint.isFakeBoldText = false

        val cols = 4
        val rows = 4
        val cellW = width.toFloat() / cols
        val cellH = (height - titleH) / rows

        for (id in 0 until 16) {
            val r = id / cols
            val c = id % cols
            val x0 = c * cellW
            val y0 = titleH + r * cellH
            val x1 = x0 + cellW
            val y1 = y0 + cellH
            val pad = 6f

            if (id < 8) {
                paint.shader = null
                paint.color = colors[id]
            } else {
                val gi = id - 8
                paint.shader = android.graphics.LinearGradient(
                    0f, y0 + pad, 0f, y1 - pad,
                    colors[8 + gi], colors[16 + gi],
                    android.graphics.Shader.TileMode.CLAMP
                )
            }
            canvas.drawRoundRect(x0 + pad, y0 + pad, x1 - pad, y1 - pad, 10f, 10f, paint)
            paint.shader = null

            // Border: green when active, light when hovered, subtle otherwise.
            val borderColor = when {
                id == activeIndex -> android.graphics.Color.argb(255, 60, 220, 110)
                id == hoveredId   -> android.graphics.Color.argb(255, 220, 220, 220)
                else              -> android.graphics.Color.argb(120, 80, 80, 90)
            }
            val borderWidth = if (id == activeIndex) 6f else 2f
            paint.style = android.graphics.Paint.Style.STROKE
            paint.strokeWidth = borderWidth
            paint.color = borderColor
            canvas.drawRoundRect(x0 + pad, y0 + pad, x1 - pad, y1 - pad, 10f, 10f, paint)
            paint.style = android.graphics.Paint.Style.FILL
        }

        val pixels = IntArray(width * height)
        finishThemedPanel(canvas, width, height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    // Called from the XR thread to render the lateral UI theme picker.
    // The four cards intentionally match the C++ make_themes_layout() UV regions.
    fun renderThemesPanelBitmap(
        @Suppress("UNUSED_PARAMETER") labels: Array<String>,
        activeTheme: Int,
        hoveredId: Int,
        width: Int,
        height: Int
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)
        paint.color = android.graphics.Color.rgb(10, 12, 22)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        val titleH = 88f
        paint.color = android.graphics.Color.rgb(34, 42, 76)
        canvas.drawRect(0f, 0f, width.toFloat(), titleH, paint)
        paint.color = Color.WHITE
        paint.textSize = 42f
        paint.isFakeBoldText = true
        canvas.drawText("UI Theme", 18f, 58f, paint)
        paint.isFakeBoldText = false

        val names = arrayOf("CLASSIC", "PREMIUM", "GLASS", "ARCADE")
        val fills = intArrayOf(
            Color.rgb(32, 44, 76), Color.rgb(15, 58, 78),
            Color.rgb(52, 43, 78), Color.rgb(96, 35, 48)
        )
        val accents = intArrayOf(
            Color.rgb(150, 190, 255), Color.rgb(60, 220, 190),
            Color.rgb(190, 130, 255), Color.rgb(255, 170, 55)
        )
        val gapU = 18f
        val gapV = 18f
        val cellW = (width - gapU * 3f) * 0.5f
        val cellH = (height - titleH - gapV * 3f) * 0.5f
        for (id in 0 until 4) {
            val col = id % 2
            val row = id / 2
            val x0 = gapU + col * (cellW + gapU)
            val y0 = titleH + gapV + row * (cellH + gapV)
            val x1 = x0 + cellW
            val y1 = y0 + cellH
            val selected = id == activeTheme
            val hovered = id == hoveredId
            paint.color = if (hovered) Color.argb(255, 64, 90, 130) else Color.rgb(24, 28, 46)
            canvas.drawRoundRect(x0, y0, x1, y1, 16f, 16f, paint)
            paint.color = fills[id]
            canvas.drawRoundRect(x0 + 8f, y0 + 8f, x1 - 8f, y0 + cellH * 0.52f, 11f, 11f, paint)
            paint.color = accents[id]
            canvas.drawRect(x0 + 8f, y0 + cellH * 0.52f, x1 - 8f, y0 + cellH * 0.58f, paint)
            paint.color = Color.WHITE
            paint.textSize = 27f
            paint.isFakeBoldText = true
            canvas.drawText(names[id], x0 + 18f, y0 + cellH * 0.76f, paint)
            paint.isFakeBoldText = false
            paint.textSize = 21f
            paint.color = Color.argb(210, 205, 215, 235)
            canvas.drawText(if (selected) "ACTIVE" else "SELECT", x0 + 18f, y0 + cellH * 0.90f, paint)
            paint.style = android.graphics.Paint.Style.STROKE
            paint.strokeWidth = if (selected) 6f else 2f
            paint.color = if (selected) accents[id] else Color.argb(120, 120, 130, 160)
            canvas.drawRoundRect(x0, y0, x1, y1, 16f, 16f, paint)
            paint.style = android.graphics.Paint.Style.FILL
        }

        val pixels = IntArray(width * height)
        finishThemedPanel(canvas, width, height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    fun renderDashboardLeftPanelBitmap(
        rowNames: Array<String>,
        rowValues: Array<String>,
        hoveredRow: Int,
        width: Int,
        height: Int
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        // Background
        paint.color = android.graphics.Color.argb(235, 12, 14, 24)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        // Title bar
        paint.color = android.graphics.Color.argb(255, 20, 50, 40)
        canvas.drawRect(0f, 0f, width.toFloat(), 88f, paint)
        paint.color = android.graphics.Color.WHITE
        paint.textSize = 44f
        paint.isFakeBoldText = true
        canvas.drawText("Dashboard", 12f, 60f, paint)
        paint.isFakeBoldText = false

        val titleH = 88f
        val n = rowNames.size
        val rowH = if (n > 0) ((height - titleH) / n).toFloat().coerceIn(52f, 96f) else 72f
        val btnW = width * 0.20f  // left/right button zones

        for (i in 0 until n) {
            val y = titleH + i * rowH
            val name = if (i < rowNames.size) rowNames[i] else ""
            val value = if (i < rowValues.size) rowValues[i] else ""

            // Row background (alternating)
            if (i % 2 == 0) {
                paint.color = android.graphics.Color.argb(60, 35, 40, 60)
                canvas.drawRect(0f, y, width.toFloat(), y + rowH, paint)
            }

            paint.textSize = rowH * 0.44f

            // Row name on left
            paint.color = android.graphics.Color.argb(215, 190, 200, 220)
            canvas.drawText(name, 12f, y + rowH * 0.68f, paint)

            // Value in center
            paint.color = android.graphics.Color.argb(200, 140, 200, 140)
            paint.textSize = rowH * 0.42f
            val tw = paint.measureText(value)
            canvas.drawText(value, width / 2f - tw / 2f, y + rowH * 0.68f, paint)

            // Left minus button (static color)
            paint.color = android.graphics.Color.argb(120, 70, 40, 40)
            canvas.drawRoundRect(2f, y + rowH * 0.18f, btnW - 2f, y + rowH * 0.82f, 6f, 6f, paint)
            paint.color = android.graphics.Color.WHITE
            paint.textSize = rowH * 0.54f
            canvas.drawText("-", btnW * 0.25f, y + rowH * 0.70f, paint)

            // Right plus button (static color)
            paint.color = android.graphics.Color.argb(120, 40, 100, 70)
            canvas.drawRoundRect(width - btnW + 2f, y + rowH * 0.18f, width - 4f, y + rowH * 0.82f, 6f, 6f, paint)
            paint.color = android.graphics.Color.WHITE
            paint.textSize = rowH * 0.54f
            canvas.drawText("+", width - btnW + btnW * 0.22f, y + rowH * 0.70f, paint)

            // Hover highlight (will be overlaid by GL quad, but draw the row rect for reference)
            if (i == hoveredRow) {
                paint.style = android.graphics.Paint.Style.STROKE
                paint.strokeWidth = 2f
                paint.color = android.graphics.Color.argb(150, 120, 200, 200)
                canvas.drawRect(4f, y + 2f, width - 4f, y + rowH - 2f, paint)
                paint.style = android.graphics.Paint.Style.FILL
            }
        }

        val pixels = IntArray(width * height)
        finishThemedPanel(canvas, width, height)
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
        finishThemedPanel(canvas, width, height)
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
        finishThemedPanel(canvas, width, height)
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
        finishThemedPanel(canvas, width, height)
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
        finishThemedPanel(canvas, width, height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
    }

    // Called from C++ XR thread to extract a zip/7z archive and return the ROM path inside.
    fun prepareRomFileForNative(rawPath: String): String =
        runCatching { prepareRomFile(File(rawPath)).absolutePath }.getOrElse { rawPath }

    // Called from C++ (Wipe Settings) to delete every cached extracted-archive
    // directory under cacheDir/roms. Also the fix for an archive that only
    // partially extracted (e.g. app killed mid-extraction, or the sibling-file
    // bug for multi-file disc images fixed alongside this) -- markPreparedArchive
    // has no way to detect that later, so a full wipe is the reliable recovery.
    fun clearExtractedRomCache() {
        val dir = File(cacheDir, "roms")
        runCatching { dir.deleteRecursively() }
        dir.mkdirs()
    }

    // Called from C++ (Danger Zone) to show a live count/size for the
    // extracted-archive cache before wiping it. Returns "count|bytes".
    fun extractedRomCacheStats(): String {
        val dir = File(cacheDir, "roms")
        var count = 0
        var bytes = 0L
        runCatching {
            dir.walkTopDown().forEach { f -> if (f.isFile) { count++; bytes += f.length() } }
        }
        return "$count|$bytes"
    }

    // -----------------------------------------------------------------------
    // Called from C++ XR thread to render the main menu panel texture.
    // Returns ARGB_8888 pixel array of size width×height.
    // menuItems: labels for each menu option. hoveredRow: row under laser (-1=none).
    // romName is retained in the JNI method signature; the title bar shows the
    // manually maintained build label instead of the currently loaded ROM.
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
        paint.textSize = 48f
        paint.isFakeBoldText = true
        var titleX = 14f
        val titleY = titleBarTop + 60f
        for (ch in "RetroDepth") {
            paint.color = when (ch) {
                'R' -> android.graphics.Color.rgb(235, 60, 60)
                'D' -> android.graphics.Color.rgb(70, 220, 100)
                else -> android.graphics.Color.WHITE
            }
            val s = ch.toString()
            canvas.drawText(s, titleX, titleY, paint)
            titleX += paint.measureText(s)
        }
        paint.isFakeBoldText = false

        // Build label display (right side of title bar).
        paint.textSize = 26f
        paint.color = android.graphics.Color.argb(200, 120, 210, 255)
        val tw = paint.measureText(MAIN_MENU_BUILD_LABEL)
        canvas.drawText(MAIN_MENU_BUILD_LABEL, width - tw - 12f, titleBarTop + 56f, paint)

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
        finishThemedPanel(canvas, width, height)
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
    // Uses /storage/emulated/0/QuestRetroDepth/roms with one official folder
    // per QRD backend. MAME's verified layered families get convenient
    // subfolders too; arbitrary folders are still allowed and route to MAME.
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

    private fun createRomSubfolders(base: File, includeMameProfiles: Boolean = true) {
        val names = mutableListOf(
            "pce", "snes", "genesis", "sms", "nes", "gb", "gg", "gba", "gbc", "saturn", "psx"
        )
        if (includeMameProfiles) names += listOf(
            "mame", "mame/neogeo", "mame/cps", "mame/konami",
            "mame/sega16b", "mame/dec0", "mame/gp9001", "mame/full_frame"
        )
        for (name in names) {
            File(base, name).mkdirs()
        }
    }

    // Called from C++ to get the settings directory path (created if needed).
    // Uses /storage/emulated/0/QuestRetroDepth/config so settings survive reinstall and
    // are accessible via USB. Falls back to app-private storage if unavailable.
    fun getSettingsDirectory(): String {
        val dir = java.io.File(Environment.getExternalStorageDirectory(), "QuestRetroDepth/config")
        if (dir.exists() || dir.mkdirs()) {
            return dir.absolutePath
        }
        return java.io.File(getExternalFilesDir(null), "settings").apply {
            mkdirs()
        }.absolutePath
    }

    fun getRumbleDirectory(): String {
        val dir = java.io.File(Environment.getExternalStorageDirectory(), "QuestRetroDepth/rumble")
        if (dir.exists() || dir.mkdirs()) {
            createRomSubfolders(dir, includeMameProfiles = false)
            return dir.absolutePath
        }
        return java.io.File(getExternalFilesDir(null), "rumble").apply {
            mkdirs()
            createRomSubfolders(this, includeMameProfiles = false)
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

    private val supportedHomebrewSystems = setOf("nes", "gb", "gbc", "gba", "sms", "gg", "snes", "genesis", "pce", "psx")

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

    fun readCreditsAsset(): String {
        return try {
            assets.open("credits.txt").bufferedReader().use { it.readText() }
        } catch (e: Exception) {
            Log.e("Credits", "Failed to read credits.txt: ${e.message}")
            ""
        }
    }

    // Same asset-backed pattern as readCreditsAsset(): the Help tab's text
    // lives in assets/help.txt so it can be reworded without touching C++.
    fun readHelpAsset(): String {
        return try {
            assets.open("help.txt").bufferedReader().use { it.readText() }
        } catch (e: Exception) {
            Log.e("Help", "Failed to read help.txt: ${e.message}")
            ""
        }
    }

    fun creditsOpenLink(url: String) {
        if (url.isNotBlank()) {
            try {
                startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
                // Leaving the VR session to open a browser leaves this activity
                // suspended in the background with no way back into VR from the
                // headset UI, so close it cleanly instead of hanging — same as
                // pressing Exit. The user just relaunches the app afterward.
                exitApp()
            } catch (e: Exception) {
                Log.e("Credits", "Failed to open $url: ${e.message}")
            }
        }
    }

    fun renderCreditsPanelBitmap(
        names: Array<String>,
        details: Array<String>,
        hasLink: BooleanArray,
        isHeader: BooleanArray,
        hovered: Int,
        hasMoreUp: Boolean,
        hasMoreDown: Boolean,
        width: Int,
        height: Int
    ): IntArray {
        val bmp = android.graphics.Bitmap.createBitmap(
            width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

        paint.color = android.graphics.Color.argb(240, 12, 12, 22)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)

        val titleBarBottom = 88f
        paint.color = android.graphics.Color.argb(255, 30, 50, 90)
        canvas.drawRect(0f, 0f, width.toFloat(), titleBarBottom, paint)
        paint.color = android.graphics.Color.WHITE
        paint.textSize = 48f
        paint.isFakeBoldText = true
        canvas.drawText("Credits", 14f, 60f, paint)
        paint.isFakeBoldText = false

        if (hasMoreUp) {
            paint.textSize = 28f
            paint.color = android.graphics.Color.argb(220, 200, 210, 230)
            canvas.drawText("^", width - 50f, 56f, paint)
        }
        if (hasMoreDown) {
            paint.textSize = 28f
            paint.color = android.graphics.Color.argb(220, 200, 210, 230)
            canvas.drawText("v", width - 90f, 56f, paint)
        }

        // names.size visible rows + one appended "Back" row
        val rowCount = names.size + 1
        val rowH = (height - titleBarBottom) / rowCount

        for (i in 0 until rowCount) {
            val y = titleBarBottom + i * rowH
            val isBack = i == names.size

            if (isBack) {
                if (i == hovered) {
                    paint.color = android.graphics.Color.argb(90, 90, 160, 255)
                    canvas.drawRect(0f, y, width.toFloat(), y + rowH, paint)
                }
                paint.textSize = 34f
                paint.color = android.graphics.Color.argb(230, 200, 210, 230)
                paint.isFakeBoldText = true
                canvas.drawText("< Back", 20f, y + rowH * 0.62f, paint)
                paint.isFakeBoldText = false
                continue
            }

            if (isHeader.getOrElse(i) { false }) {
                // Section header: distinct background, no detail line, not
                // clickable (hasLink is false for these), centered-ish text.
                paint.color = android.graphics.Color.argb(255, 34, 46, 68)
                canvas.drawRect(0f, y, width.toFloat(), y + rowH, paint)
                paint.textSize = 30f
                paint.color = android.graphics.Color.argb(255, 190, 210, 255)
                paint.isFakeBoldText = true
                canvas.drawText(names[i].uppercase(), 20f, y + rowH * 0.66f, paint)
                paint.isFakeBoldText = false
                continue
            }

            if (i % 2 == 0) {
                paint.color = android.graphics.Color.argb(60, 35, 40, 60)
                canvas.drawRect(0f, y, width.toFloat(), y + rowH, paint)
            }
            if (i == hovered) {
                paint.color = android.graphics.Color.argb(90, 90, 160, 255)
                canvas.drawRect(0f, y, width.toFloat(), y + rowH, paint)
            }

            paint.color = if (hasLink.getOrElse(i) { false })
                android.graphics.Color.argb(255, 140, 200, 255)
                else android.graphics.Color.argb(255, 225, 225, 235)
            paint.isFakeBoldText = true
            // Shrink to fit instead of letting a long entry (e.g. a creator
            // note) run off the panel edge by a few pixels.
            var nameTextSize = 32f
            paint.textSize = nameTextSize
            val maxNameWidth = width - 40f
            while (paint.measureText(names[i]) > maxNameWidth && nameTextSize > 18f) {
                nameTextSize -= 1f
                paint.textSize = nameTextSize
            }
            canvas.drawText(names[i], 20f, y + rowH * 0.42f, paint)
            paint.isFakeBoldText = false

            paint.textSize = 24f
            paint.color = android.graphics.Color.argb(200, 170, 175, 190)
            canvas.drawText(details.getOrElse(i) { "" }, 20f, y + rowH * 0.72f, paint)
        }

        val pixels = IntArray(width * height)
        bmp.getPixels(pixels, 0, width, 0, 0, width, height)
        bmp.recycle()
        return pixels
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
        finishThemedPanel(canvas, width, height)
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
        finishThemedPanel(canvas, width, height)
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
        val loadLastSaveEnabled: Boolean = true,
        // Separate from loadLastSaveEnabled (which auto-loads a ROM's most
        // recent save-state slot once it's open) -- this one gates whether
        // onResume() auto-boots the last-played ROM at APK launch at all.
        val loadLastRomEnabled: Boolean = true
    )

    private enum class RomFamily {
        Snes, Genesis, Nes, Gb, Gba, Gg, Pce, Sega32x, Atari2600, Ds, Saturn, Psx, Dreamcast
    }
}
