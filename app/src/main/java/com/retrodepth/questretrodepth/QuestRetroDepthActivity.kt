package com.retrodepth.questretrodepth

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.opengl.GLSurfaceView
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import org.apache.commons.compress.archivers.sevenz.SevenZFile
import java.io.File
import java.io.FileOutputStream
import java.util.Locale
import java.util.zip.ZipInputStream
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

class QuestRetroDepthActivity : QuestVrActivity() {
    private val sevenZipBufferSize = 8192
    private lateinit var glView: GLSurfaceView
    private lateinit var statusView: TextView
    private lateinit var romView: TextView
    private lateinit var runButton: Button
    private lateinit var vrButton: Button
    private lateinit var gamepadOverlay: View
    private lateinit var sharedPanelOverlay: FrameLayout
    private lateinit var sharedPanelImage: ImageView
    private lateinit var questCtrlOverlay: View
    private lateinit var scrollUpBtn: Button
    private lateinit var scrollDownBtn: Button
    private var sharedPanelGeneration = -1
    private var panelTouchStartX = 0f
    private var panelTouchStartY = 0f
    private var panelAccumDy = 0f
    private var panelDragging = false

    private val handler = Handler(Looper.getMainLooper())
    private var loadedRomFile: File? = null
    private var running = true
    private var orbitYaw = 0f
    private var orbitPitch = 0.12f
    private var panX = 0f
    private var panY = 0f
    private var zoom = 1f
    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var lastMidX = 0f
    private var lastMidY = 0f
    private var pointerMode = 0

    private var inputUp = false
    private var inputDown = false
    private var inputLeft = false
    private var inputRight = false
    private var inputA = false
    private var inputB = false
    private var inputX = false
    private var inputY = false
    private var inputL = false
    private var inputR = false
    private var inputStart = false
    private var inputSelect = false

    private val statusPoll = object : Runnable {
        override fun run() {
            statusView.text = nativeGetLastStatus()
            refreshSharedPanel()
            handler.postDelayed(this, 250L)
        }
    }

    private val scaleDetector by lazy {
        ScaleGestureDetector(this, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(detector: ScaleGestureDetector): Boolean {
                zoom = (zoom * detector.scaleFactor).coerceIn(0.55f, 2.5f)
                syncCamera()
                return true
            }
        })
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildUi())
        statusView.text = nativeStartMobile(this)
        syncCamera()
        syncMobileInput()
    }

    override fun onResume() {
        super.onResume()
        statusView.text = nativeStartMobile(this)
        glView.onResume()
        handler.post(statusPoll)
        refreshLocalRomSummary()
        nativeSetMobilePaused(!running)
        refreshSharedPanel(force = true)
    }

    override fun onPause() {
        super.onPause()
        handler.removeCallbacks(statusPoll)
        glView.onPause()
        nativeStopMobile()
    }

    override fun onBackPressed() {
        if (nativeMobileBack()) {
            refreshSharedPanel(force = true)
            return
        }
        super.onBackPressed()
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_OPEN_ROM || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        runCatching {
            loadRomFromFile(copyUriToCache(uri))
        }.onFailure {
            statusView.text = "ROM load failed\n\n${it.message ?: "Unknown error"}"
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (updateGamepadKey(keyCode, true)) return true
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent?): Boolean {
        if (updateGamepadKey(keyCode, false)) return true
        return super.onKeyUp(keyCode, event)
    }

    private fun buildUi(): View {
        val root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }

        glView = object : GLSurfaceView(this) {
            override fun onTouchEvent(event: MotionEvent): Boolean {
                scaleDetector.onTouchEvent(event)
                handleSceneTouch(event)
                return true
            }
        }.apply {
            setEGLContextClientVersion(3)
            preserveEGLContextOnPause = true
            setRenderer(object : GLSurfaceView.Renderer {
                override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
                    nativeOnMobileSurfaceCreated()
                }

                override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
                    nativeOnMobileSurfaceChanged(width, height)
                }

                override fun onDrawFrame(gl: GL10?) {
                    nativeOnMobileDrawFrame()
                }
            })
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        }
        root.addView(glView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT
        ))

        val topHud = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(12), dp(12), dp(12))
            setBackgroundColor(Color.argb(110, 0, 0, 0))
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
        val menuButton = Button(this).apply {
            text = "Menu"
            setOnClickListener {
                nativeMobileOpenMainMenu()
                refreshSharedPanel(force = true)
            }
        }
        val quickButton = Button(this).apply {
            text = "Quick Edit"
            setOnClickListener {
                nativeMobileOpenQuickEdit()
                refreshSharedPanel(force = true)
            }
        }
        vrButton = Button(this).apply {
            text = "Enter VR"
            isEnabled = supportsVrRedirect()
            alpha = if (isEnabled) 1.0f else 0.45f
            setOnClickListener {
                startActivity(Intent(this@QuestRetroDepthActivity, QuestVrActivity::class.java).apply {
                    putExtra("force_vr", true)
                })
            }
        }
        runButton = Button(this).apply {
            text = "Pause"
            setOnClickListener {
                running = !running
                syncRunButton()
                nativeSetMobilePaused(!running)
            }
        }

        controls.addView(openButton)
        controls.addView(localButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { leftMargin = dp(8) })
        controls.addView(menuButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { leftMargin = dp(8) })
        controls.addView(quickButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { leftMargin = dp(8) })
        controls.addView(vrButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { leftMargin = dp(8) })
        controls.addView(runButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { leftMargin = dp(8) })

        romView = TextView(this).apply {
            text = "ROM: none"
            textSize = 15f
            setTextColor(Color.WHITE)
            setPadding(0, dp(8), 0, 0)
        }

        statusView = TextView(this).apply {
            text = "Mobile renderer starting..."
            textSize = 13f
            setTextColor(Color.WHITE)
            setPadding(0, dp(6), 0, 0)
        }

        topHud.addView(controls)
        topHud.addView(romView)
        topHud.addView(statusView)
        root.addView(topHud, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP
        ))

        gamepadOverlay = buildGamepadOverlay()
        root.addView(gamepadOverlay, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT
        ))
        sharedPanelImage = ImageView(this).apply {
            adjustViewBounds = true
            setBackgroundColor(Color.argb(220, 6, 10, 14))
            setOnTouchListener { _, event ->
                handleSharedPanelTouch(event)
                true
            }
        }
        sharedPanelOverlay = FrameLayout(this).apply {
            setBackgroundColor(Color.argb(150, 0, 0, 0))
            visibility = View.GONE
            addView(sharedPanelImage, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER
            ).apply {
                leftMargin = dp(20)
                rightMargin = dp(20)
            })
        }
        root.addView(sharedPanelOverlay, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT
        ))
        questCtrlOverlay = buildQuestCtrlOverlay()
        root.addView(questCtrlOverlay, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT
        ))
        return root
    }

    private fun buildGamepadOverlay(): View {
        val overlay = FrameLayout(this)

        val leftPad = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            alpha = 0.82f
        }
        val up = makeHoldButton("▲") { pressed -> inputUp = pressed }
        val midRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val left = makeHoldButton("◀") { pressed -> inputLeft = pressed }
        val right = makeHoldButton("▶") { pressed -> inputRight = pressed }
        val down = makeHoldButton("▼") { pressed -> inputDown = pressed }
        midRow.addView(left)
        midRow.addView(View(this), LinearLayout.LayoutParams(dp(18), dp(18)))
        midRow.addView(right)
        leftPad.addView(up)
        leftPad.addView(midRow)
        leftPad.addView(down)
        overlay.addView(leftPad, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM or Gravity.START
        ).apply { leftMargin = dp(20); bottomMargin = dp(26) })

        val rightCluster = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            alpha = 0.82f
        }
        val topRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        topRow.addView(makeHoldButton("X") { pressed -> inputX = pressed })
        topRow.addView(View(this@QuestRetroDepthActivity), LinearLayout.LayoutParams(dp(22), dp(22)))
        topRow.addView(makeHoldButton("Y") { pressed -> inputY = pressed })
        val bottomRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        bottomRow.addView(makeHoldButton("A") { pressed -> inputA = pressed })
        bottomRow.addView(View(this@QuestRetroDepthActivity), LinearLayout.LayoutParams(dp(22), dp(22)))
        bottomRow.addView(makeHoldButton("B") { pressed -> inputB = pressed })
        rightCluster.addView(topRow)
        rightCluster.addView(bottomRow)
        overlay.addView(rightCluster, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM or Gravity.END
        ).apply { rightMargin = dp(20); bottomMargin = dp(26) })

        val shoulderRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            alpha = 0.78f
        }
        shoulderRow.addView(makeHoldButton("L") { pressed -> inputL = pressed })
        shoulderRow.addView(View(this), LinearLayout.LayoutParams(0, 0, 1f))
        shoulderRow.addView(makeHoldButton("Select") { pressed -> inputSelect = pressed })
        shoulderRow.addView(makeHoldButton("Start") { pressed -> inputStart = pressed })
        shoulderRow.addView(View(this), LinearLayout.LayoutParams(0, 0, 1f))
        shoulderRow.addView(makeHoldButton("R") { pressed -> inputR = pressed })
        overlay.addView(shoulderRow, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM
        ).apply { leftMargin = dp(20); rightMargin = dp(20); bottomMargin = dp(180) })

        return overlay
    }

    private fun buildQuestCtrlOverlay(): View {
        val overlay = FrameLayout(this)

        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.END
        }

        fun makeCtrlButton(label: String, action: () -> Unit): Button {
            return Button(this).apply {
                text = label
                alpha = 0.85f
                setBackgroundColor(Color.argb(180, 30, 30, 30))
                setTextColor(Color.WHITE)
                val sz = dp(56)
                minimumWidth = sz
                minimumHeight = sz
                setPadding(0, 0, 0, 0)
                setOnClickListener { action() }
            }
        }

        col.addView(makeCtrlButton("☰") {
            nativeMobileOpenMainMenu()
            refreshSharedPanel(force = true)
        })
        col.addView(makeCtrlButton("⚡") {
            nativeMobileOpenQuickEdit()
            refreshSharedPanel(force = true)
        })
        col.addView(makeCtrlButton("✕") {
            nativeMobileBack()
            refreshSharedPanel(force = true)
        })

        scrollUpBtn = makeCtrlButton("▲") {
            nativeMobilePanelScroll(-1)
            refreshSharedPanel(force = true)
        }.also { col.addView(it) }

        scrollDownBtn = makeCtrlButton("▼") {
            nativeMobilePanelScroll(1)
            refreshSharedPanel(force = true)
        }.also { col.addView(it) }

        scrollUpBtn.visibility = View.GONE
        scrollDownBtn.visibility = View.GONE

        overlay.addView(col, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.END or Gravity.CENTER_VERTICAL
        ).apply { rightMargin = dp(8) })

        return overlay
    }

    private fun makeHoldButton(label: String, update: (Boolean) -> Unit): Button {
        return Button(this).apply {
            text = label
            alpha = 0.78f
            setOnTouchListener { _, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN,
                    MotionEvent.ACTION_POINTER_DOWN -> {
                        update(true)
                        syncMobileInput()
                    }
                    MotionEvent.ACTION_UP,
                    MotionEvent.ACTION_POINTER_UP,
                    MotionEvent.ACTION_CANCEL -> {
                        update(false)
                        syncMobileInput()
                    }
                }
                true
            }
        }
    }

    private fun handleSceneTouch(event: MotionEvent) {
        if (sharedPanelOverlay.visibility == View.VISIBLE) return
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                pointerMode = 1
                lastTouchX = event.x
                lastTouchY = event.y
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                if (event.pointerCount >= 2) {
                    pointerMode = 2
                    lastMidX = (event.getX(0) + event.getX(1)) * 0.5f
                    lastMidY = (event.getY(0) + event.getY(1)) * 0.5f
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (pointerMode == 1 && event.pointerCount == 1 && !scaleDetector.isInProgress) {
                    val dx = event.x - lastTouchX
                    val dy = event.y - lastTouchY
                    orbitYaw += dx * 0.0065f
                    orbitPitch = (orbitPitch - dy * 0.0045f).coerceIn(-1.1f, 1.1f)
                    lastTouchX = event.x
                    lastTouchY = event.y
                    syncCamera()
                } else if (event.pointerCount >= 2) {
                    val midX = (event.getX(0) + event.getX(1)) * 0.5f
                    val midY = (event.getY(0) + event.getY(1)) * 0.5f
                    val dx = midX - lastMidX
                    val dy = midY - lastMidY
                    panX += dx / glView.width.toFloat() * 2.2f
                    panY -= dy / glView.height.toFloat() * 1.6f
                    lastMidX = midX
                    lastMidY = midY
                    syncCamera()
                }
            }
            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_CANCEL -> pointerMode = 0
        }
    }

    private fun handleSharedPanelTouch(event: MotionEvent) {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                panelTouchStartX = event.x
                panelTouchStartY = event.y
                panelAccumDy = 0f
                panelDragging = false
            }
            MotionEvent.ACTION_MOVE -> {
                val dy = event.y - panelTouchStartY
                panelAccumDy += dy
                if (kotlin.math.abs(panelAccumDy) > dp(28)) {
                    val step = if (panelAccumDy > 0f) 1 else -1
                    nativeMobilePanelScroll(step)
                    panelTouchStartY = event.y
                    panelAccumDy = 0f
                    panelDragging = true
                    refreshSharedPanel(force = true)
                }
            }
            MotionEvent.ACTION_UP -> {
                if (!panelDragging) {
                    val w = sharedPanelImage.width.coerceAtLeast(1)
                    val h = sharedPanelImage.height.coerceAtLeast(1)
                    val u = (event.x / w.toFloat()).coerceIn(0f, 1f)
                    val v = (event.y / h.toFloat()).coerceIn(0f, 1f)
                    nativeMobilePanelTap(u, v)
                    refreshSharedPanel(force = true)
                }
            }
        }
    }

    private fun refreshSharedPanel(force: Boolean = false) {
        val visible = nativeMobilePanelVisible()
        sharedPanelOverlay.visibility = if (visible) View.VISIBLE else View.GONE
        gamepadOverlay.visibility = if (visible) View.GONE else View.VISIBLE
        val scrollVis = if (visible) View.VISIBLE else View.GONE
        scrollUpBtn.visibility = scrollVis
        scrollDownBtn.visibility = scrollVis
        if (!visible) {
            sharedPanelGeneration = -1
            return
        }
        val generation = nativeMobilePanelGeneration()
        if (!force && generation == sharedPanelGeneration) return
        val widthHeight = nativeMobilePanelSize()
        if (widthHeight.size < 2 || widthHeight[0] <= 0 || widthHeight[1] <= 0) return
        val pixels = nativeMobilePanelPixels()
        if (pixels.isEmpty()) return
        val bitmap = Bitmap.createBitmap(pixels, widthHeight[0], widthHeight[1], Bitmap.Config.ARGB_8888)
        sharedPanelImage.setImageBitmap(bitmap)
        sharedPanelGeneration = generation
    }

    private fun syncRunButton() {
        runButton.text = if (running) "Pause" else "Run"
    }

    private fun syncCamera() {
        nativeSetMobileCamera(orbitYaw, orbitPitch, panX, panY, zoom)
    }

    private fun syncMobileInput() {
        nativeSetMobileInputState(
            inputUp, inputDown, inputLeft, inputRight,
            inputA, inputB, inputX, inputY,
            inputL, inputR, inputStart, inputSelect
        )
    }

    private fun updateGamepadKey(keyCode: Int, pressed: Boolean): Boolean {
        val handled = when (keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> { inputUp = pressed; true }
            KeyEvent.KEYCODE_DPAD_DOWN -> { inputDown = pressed; true }
            KeyEvent.KEYCODE_DPAD_LEFT -> { inputLeft = pressed; true }
            KeyEvent.KEYCODE_DPAD_RIGHT -> { inputRight = pressed; true }
            KeyEvent.KEYCODE_BUTTON_A -> { inputA = pressed; true }
            KeyEvent.KEYCODE_BUTTON_B -> { inputB = pressed; true }
            KeyEvent.KEYCODE_BUTTON_X -> { inputX = pressed; true }
            KeyEvent.KEYCODE_BUTTON_Y -> { inputY = pressed; true }
            KeyEvent.KEYCODE_BUTTON_L1 -> { inputL = pressed; true }
            KeyEvent.KEYCODE_BUTTON_R1 -> { inputR = pressed; true }
            KeyEvent.KEYCODE_BUTTON_START -> { inputStart = pressed; true }
            KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BACK -> { inputSelect = pressed; true }
            else -> false
        }
        if (handled) syncMobileInput()
        return handled
    }

    private fun supportsVrRedirect(): Boolean {
        val brand = Build.BRAND.lowercase(Locale.US)
        val manufacturer = Build.MANUFACTURER.lowercase(Locale.US)
        return (brand.contains("oculus") || brand.contains("meta") ||
            manufacturer.contains("oculus") || manufacturer.contains("meta")) &&
            packageManager.hasSystemFeature("android.hardware.vr.headtracking")
    }

    private fun openRomPicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        startActivityForResult(intent, REQUEST_OPEN_ROM)
    }

    private fun loadRomFromFile(file: File) {
        val prepared = prepareRomFileForLoad(file)
        loadedRomFile = prepared
        romView.text = "ROM: ${prepared.name}"
        val result = nativeLoadRom(prepared.absolutePath)
        statusView.text = result
        running = result.lowercase(Locale.US).contains("loaded")
        syncRunButton()
        nativeSetMobilePaused(!running)
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
                if (index >= 0) return cursor.getString(index)
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
                runCatching { loadRomFromFile(roms[which]) }.onFailure {
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
            romView.text = if (count > 0) "ROM: none ($count local ROMs found)" else "ROM: none"
        }
    }

    private fun localRomDirectory(): File {
        return File(getExternalFilesDir(null), "roms")
    }

    private fun isSupportedRomFile(file: File): Boolean {
        val lower = file.name.lowercase(Locale.US)
        return isSupportedRomExtension(lower) || lower.endsWith(".zip") || lower.endsWith(".7z")
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
            lower.endsWith(".smd") ||
            lower.endsWith(".gba") ||
            lower.endsWith(".gb") ||
            lower.endsWith(".gbc") ||
            lower.endsWith(".nes") ||
            lower.endsWith(".pce") ||
            lower.endsWith(".sms") ||
            lower.endsWith(".gg")
    }

    private fun sanitizeFileName(name: String): String {
        return name.replace(Regex("[^A-Za-z0-9._ -]"), "_")
    }

    private fun dp(value: Int): Int {
        return (value * resources.displayMetrics.density).toInt()
    }

    private external fun nativeStartMobile(activity: QuestRetroDepthActivity): String
    private external fun nativeStopMobile()
    private external fun nativeOnMobileSurfaceCreated()
    private external fun nativeOnMobileSurfaceChanged(width: Int, height: Int)
    private external fun nativeOnMobileDrawFrame()
    private external fun nativeSetMobileCamera(orbitYaw: Float, orbitPitch: Float, panX: Float, panY: Float, zoom: Float)
    private external fun nativeSetMobileInputState(
        up: Boolean, down: Boolean, left: Boolean, right: Boolean,
        a: Boolean, b: Boolean, x: Boolean, y: Boolean,
        l: Boolean, r: Boolean, start: Boolean, select: Boolean
    )
    private external fun nativeSetMobilePaused(paused: Boolean)
    private external fun nativeMobileOpenMainMenu()
    private external fun nativeMobileOpenQuickEdit()
    private external fun nativeMobilePanelVisible(): Boolean
    private external fun nativeMobilePanelGeneration(): Int
    private external fun nativeMobilePanelPixels(): IntArray
    private external fun nativeMobilePanelSize(): IntArray
    private external fun nativeMobilePanelTap(u: Float, v: Float)
    private external fun nativeMobilePanelScroll(delta: Int)
    private external fun nativeMobileBack(): Boolean
    private external fun nativeLoadRom(path: String): String
    private external fun nativeGetLastStatus(): String

    companion object {
        private const val REQUEST_OPEN_ROM = 1001

        init {
            System.loadLibrary("questretrodepth_native")
        }
    }
}
