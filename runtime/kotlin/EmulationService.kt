package Ali.onRps4.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.os.IBinder
import androidx.core.app.NotificationCompat
import Ali.onRps4.MainActivity
import Ali.onRps4.storage.AppStorage
import Ali.onRps4.RuntimeLaunchProfileProvider
import Ali.onRps4.ServiceLocator
import Ali.onRps4.data.ParamSfoReader
import Ali.onRps4.model.RuntimeErrorCode
import Ali.onRps4.runtime.diagnostics.AppLog
import Ali.onRps4.runtime.diagnostics.SessionLog
import Ali.onRps4.runtime.compat.GameCompat
import Ali.onRps4.runtime.compat.GameCompatibility
import Ali.onRps4.runtime.config.ShadPs4ConfigManager
import Ali.onRps4.runtime.crash.CrashLedger
import Ali.onRps4.runtime.driver.DriverRegistry
import Ali.onRps4.runtime.display.WinlatorEmbeddedXServer
import Ali.onRps4.runtime.install.RuntimeInstaller
import Ali.onRps4.runtime.install.RuntimeManifest
import Ali.onRps4.runtime.input.ControllerFrameEncoder
import Ali.onRps4.runtime.input.ControllerSnapshot
import Ali.onRps4.runtime.process.Box64Mode
import Ali.onRps4.runtime.process.ExitCodeInterpreter
import Ali.onRps4.runtime.process.PerformanceGovernor
import Ali.onRps4.runtime.process.RuntimeProcessHandle
import Ali.onRps4.runtime.patch.PatchCheatManager
import Ali.onRps4.runtime.pkg.PsarcExtractor
import Ali.onRps4.runtime.process.RuntimeProcessLauncher
import Ali.onRps4.runtime.process.RuntimeProcessRequest
import Ali.onRps4.runtime.settings.ProfileScope
import Ali.onRps4.runtime.settings.RuntimeGuestBackend
import Ali.onRps4.runtime.process.RuntimeVulkanDriver
import Ali.onRps4.runtime.process.RuntimeVulkanDriverIds
import Ali.onRps4.runtime.process.VulkanDriverConfiguration
import Ali.onRps4.runtime.session.ManagedSession
import Ali.onRps4.runtime.session.ManagedSessionState
import Ali.onRps4.runtime.session.FrameTelemetryReporter
import Ali.onRps4.runtime.session.PointerInput
import com.winlator.xconnector.UnixSocketConfig
import com.winlator.xserver.Pointer
import java.io.File
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.nio.file.FileAlreadyExistsException
import java.nio.file.Path
import java.nio.file.Paths
import java.nio.file.StandardCopyOption
import java.time.Instant
import java.util.UUID
import java.util.concurrent.TimeUnit
import java.util.concurrent.Executors
import java.util.concurrent.TimeoutException
import Ali.onRps4.runtime.input.GamepadInputManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull

class EmulationService : Service() {
    private val launchProfileProvider: RuntimeLaunchProfileProvider get() = ServiceLocator.launchProfileProvider

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var sessionJob: Job? = null
    @Volatile private var process: RuntimeProcessHandle? = null
    private var logMirrorJob: Job? = null
    private var bootWatchdogJob: Job? = null
    private var internalLogPaths: List<Path> = emptyList()
    private var hiddenPluginRelativePaths: List<String> = emptyList()

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ManagedSession.ACTION_STOP -> stopSession()
            ManagedSession.ACTION_START -> {
                if (sessionJob?.isActive == true) {
                    return START_NOT_STICKY
                } else {
                    // Foreground service so the emulation process survives backgrounding
                    // and screen-off on Android 10+ (background services are killed after
                    // the grace period). Started via ContextCompat.startForegroundService.
                    if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q) {
                        startForeground(
                            NOTIFICATION_ID,
                            notification(),
                            ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK,
                        )
                    } else {
                        startForeground(NOTIFICATION_ID, notification())
                    }
                    val gameId = intent.getStringExtra(ManagedSession.EXTRA_GAME_ID).orEmpty()
                    val gamePath = intent.getStringExtra(ManagedSession.EXTRA_GAME_PATH).orEmpty()
                    val driverName = intent.getStringExtra(ManagedSession.EXTRA_VULKAN_DRIVER)
                        ?: RuntimeVulkanDriver.TURNIP_26_1_0.name
                    sessionJob = scope.launch { runSession(gameId, gamePath, RuntimeVulkanDriver.valueOf(driverName)) }
                }
            }
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        stopSession()
        scope.cancel()
        super.onDestroy()
    }

    private suspend fun runSession(gameId: String, relativePath: String, vulkanDriver: RuntimeVulkanDriver) {
        var xServer: WinlatorEmbeddedXServer? = null
        var boundSocket: LocalSocket? = null
        var serverSocket: LocalServerSocket? = null
        var clientSocket: LocalSocket? = null
        var controllerSink: ((Int, ControllerSnapshot) -> Unit)? = null
        var pointerSink: ((PointerInput) -> Unit)? = null
        var runtimeRoot: Path? = null
        val acceptExecutor = Executors.newSingleThreadExecutor()
        val controlFile = File(filesDir, "runtime-control.sock").apply { delete() }
        val logsRoot = File(filesDir, "logs").toPath()
        SessionLog.prune(logsRoot, keep = MAX_LOG_SESSIONS - 1)
        val sessionLog = SessionLog.create(
            logsRoot,
            gameId,
            Instant.now(),
            UUID.randomUUID().toString().substring(0, 8),
        )
        val outputFile = sessionLog.backendLog.toFile()
        val telemetryReporter = FrameTelemetryReporter()
        var performanceGovernor: PerformanceGovernor? = null
        sessionLog.info("Session", "start game=$gameId intentDriver=$vulkanDriver")
        AppLog.log(this, "onRps4Session", "start game=$gameId")
        GamepadInputManager.onSessionStart()
        val controllerProfiles = ServiceLocator.runtimeProfileStore.load(ProfileScope.Global).controllerSlots
        GamepadInputManager.applyProfiles(controllerProfiles)
        sessionLog.info(
            "Input",
            "applied ${controllerProfiles.size} saved controller profile(s)",
        )
        sessionLog.info(
            "Device",
            "manufacturer=${android.os.Build.MANUFACTURER} model=${android.os.Build.MODEL} " +
                "sdk=${android.os.Build.VERSION.SDK_INT} " + detectSoCInfo(),
        )
        logMemoryInfo(sessionLog)
        var pluginsHidden = false
        var sessionReachedRunning = false
        var warmupPending = false
        var hiddenPluginGameRoot: java.io.File? = null
        try {
            require(gameId.matches(Regex("[A-Za-z0-9._-]+"))) { "Invalid game id" }
            val gamesRoot = AppStorage.gamesDir().canonicalFile
            val gameRoot = File(AppStorage.baseDir(), relativePath).canonicalFile
            require(gameRoot.toPath().startsWith(gamesRoot.toPath())) { "Game path escapes app storage" }
            val eboot = File(gameRoot, "eboot.bin")
            require(eboot.isFile) { "Imported eboot.bin is missing" }
            sessionLog.info("Content", "validated game root and eboot.bin")
            logGameTrophyInfo(gameRoot, sessionLog)
            logGameContent(gameRoot, sessionLog)
            ensureGameRootPsarcs(gameRoot, sessionLog)
            val launchProfile = launchProfileProvider.resolve(gameId)
            val compat = GameCompatibility.forGame(gameId)
            sessionLog.info(
                "Config",
                "schema=${launchProfile.schemaVersion} settings=${launchProfileProvider.explicitSettingIds(launchProfile).joinToString(",")}",
            )
            if (compat.isActive) {
                sessionLog.info(
                    "Workaround",
                    "compatibility entry active for $gameId: " +
                        buildList {
                            if (compat.seedSaveFiles.isNotEmpty()) add("seed=${compat.seedSaveFiles.joinToString(",")}")
                            if (compat.hidePlugins.isNotEmpty()) add("hidePlugins=${compat.hidePlugins.joinToString(",")}")
                            if (compat.forceDriverId != null) add("driver=${compat.forceDriverId}")
                            if (compat.config.isNotEmpty()) add("config=${compat.config.keys.joinToString(",")}")
                        }.joinToString(" "),
                )
            }

            ManagedSession.update(ManagedSessionState.Preparing("runtime"))
            val installedRuntime = applyDevOverride(installRuntime(), sessionLog)
            runtimeRoot = installedRuntime
            sessionLog.info("Runtime", "installed version=${installedRuntime.fileName}")
            installedRuntime.resolve(".local/share").toFile().mkdirs()
            installedRuntime.resolve(".config").toFile().mkdirs()
            ManagedSession.update(ManagedSessionState.Preparing("display"))
            val target = withTimeout(SURFACE_TIMEOUT_MS) { ManagedSession.surface.filterNotNull().first() }
            sessionLog.info("Display", "surface=${target.width}x${target.height}")
            val socketRoot = File(filesDir, "x").apply { mkdirs() }
            // Xlib DISPLAY=:0 resolves @/tmp/.X11-unix/X0 (abstract) or
            // /tmp/.X11-unix/X0 (filesystem). Do not use bare "/X0" — that only
            // works if another process (e.g. Termux X11) owns the canonical path.
            xServer = WinlatorEmbeddedXServer(
                this,
                socketRoot,
                useAbstractXSocket = true,
                xSocketPath = UnixSocketConfig.XSERVER_PATH,
                useSharedMemoryAudio = false,
                frameGeneration = launchProfile.frameGeneration,
            )
            xServer.start(
                target.surface,
                target.width,
                target.height,
                vrMode = (launchProfile.settings["vr.display_vr"]?.value as? JsonPrimitive)?.booleanOrNull == true,
            )
            sessionLog.info("Display", "embedded X server started display=${xServer.display}")
            // Absolute pointer delivery to the guest X server; used for the
            // "auto-click dialog OK" helper (the virtual mouse was removed).
            xServer.xServer?.let { xs ->
                val sink: (PointerInput) -> Unit = { input ->
                    xs.injectPointerMove(input.x.toInt(), input.y.toInt())
                    if (input.down) xs.injectPointerButtonPress(Pointer.Button.BUTTON_LEFT)
                    if (input.up) xs.injectPointerButtonRelease(Pointer.Button.BUTTON_LEFT)
                }
                pointerSink = sink
                ManagedSession.attachPointerSink(sink)
            }

            boundSocket = LocalSocket().also {
                it.bind(LocalSocketAddress(controlFile.path, LocalSocketAddress.Namespace.FILESYSTEM))
            }
            serverSocket = LocalServerSocket(boundSocket.fileDescriptor)
            val nativeLibraryDir = Paths.get(applicationInfo.nativeLibraryDir)

            // Default guest backend is the Box64 x86_64 translation layer: it
            // has the widest game compatibility and works with both glibc and
            // APK-native (bionic) Vulkan drivers. FEX (host/shadps4-arm64-fex)
            // runs the emulator natively and is selected only when the user
            // requests it and the active Vulkan driver is glibc-native.
            val fexBinary = installedRuntime.resolve(FEX_EXECUTABLE_RELATIVE)
            val fexAvailable = java.nio.file.Files.isRegularFile(fexBinary)
            if (!fexAvailable) {
                sessionLog.warning(
                    "Runtime",
                    "Native FEX shadPS4 is missing ($fexBinary); using the Box64 backend",
                )
            }
            // FEX runs the emulator natively, so the Vulkan driver must be a
            // glibc-native ARM64 driver (host-built Turnip / custom). APK-native
            // (bionic) drivers only work under Box64 — when FEX is requested but
            // the active driver is bionic, fall back to the Box64 backend (which
            // runs that bionic driver via the APK-native box64) instead of
            // launching FEX with an unusable driver.
            val crashSuggestion = CrashLedger.suggestedDriver(filesDir, gameId)
            if (crashSuggestion != null) {
                if (driverAvailableInRuntime(installedRuntime, crashSuggestion)) {
                    sessionLog.warning(
                        "Vulkan",
                        "auto crash-fallback driver active for $gameId: $crashSuggestion " +
                            "(previous launches segfaulted on the host)",
                    )
                } else {
                    sessionLog.warning("Vulkan", "crash-fallback driver $crashSuggestion is not installed; clearing it")
                    CrashLedger.clearSuggestion(filesDir, gameId)
                }
            }
            val preferredDriverId = compat.forceDriverId ?: crashSuggestion ?: launchProfile.driverId
            if (compat.forceDriverId != null && compat.forceDriverId != launchProfile.driverId) {
                sessionLog.info("Workaround", "game $gameId forces driver=${compat.forceDriverId}")
            }
            val preferredDriverIsBionic = runCatching {
                launchProfileProvider.vulkanConfiguration(
                    launchProfile.copy(driverId = preferredDriverId),
                    installedRuntime,
                    filesDir.toPath(),
                ).box64Mode == Box64Mode.APK_NATIVE
            }.getOrDefault(false)
            val fexUsable = fexAvailable && !preferredDriverIsBionic
            if (fexAvailable && preferredDriverIsBionic) {
                sessionLog.warning(
                    "Runtime",
                    "driver=$preferredDriverId is APK-native (Box64-only); FEX needs a glibc-native driver — " +
                        "falling back to the Box64 backend",
                )
            }
            val activeDriverId = preferredDriverId
            val activeProfile = if (activeDriverId == launchProfile.driverId) {
                launchProfile
            } else {
                launchProfile.copy(driverId = activeDriverId)
            }

            val driverConfiguration = launchProfileProvider.vulkanConfiguration(
                activeProfile,
                installedRuntime,
                filesDir.toPath(),
            )
            // Backend precedence: per-title compatibility override, then the
            // user's setting (game overrides global), then the automatic default.
            // The default is the Box64 x86_64 backend (most compatible); FEX is
            // used only when requested and the native build + driver allow it.
            val autoBackend = RuntimeGuestBackend.BOX64
            val forcedBackend = compat.forceGuestBackend
            val userBackend = launchProfile.guestBackend
            val guestBackend = when {
                forcedBackend == RuntimeGuestBackend.BOX64 -> RuntimeGuestBackend.BOX64
                forcedBackend == RuntimeGuestBackend.FEX && fexUsable -> RuntimeGuestBackend.FEX
                userBackend == RuntimeGuestBackend.BOX64 -> RuntimeGuestBackend.BOX64
                userBackend == RuntimeGuestBackend.FEX && fexUsable -> RuntimeGuestBackend.FEX
                else -> RuntimeGuestBackend.BOX64
            }
            if (forcedBackend != null) {
                if (guestBackend != autoBackend) {
                    sessionLog.info("Workaround", "game $gameId forces guestBackend=${guestBackend.name.lowercase()} (auto was ${autoBackend.name.lowercase()})")
                } else if (forcedBackend != guestBackend) {
                    sessionLog.warning("Workaround", "game $gameId wants guestBackend=${forcedBackend.name.lowercase()} but it is unavailable; using ${guestBackend.name.lowercase()}")
                }
            } else if (userBackend != null && guestBackend != autoBackend) {
                sessionLog.info("Runtime", "user selection guestBackend=${guestBackend.name.lowercase()} (auto was ${autoBackend.name.lowercase()})")
            }
            sessionLog.info("Runtime", "guestBackend=${guestBackend.name.lowercase()} native=$fexUsable")
            patchGuestLibudev(installedRuntime, guestBackend, sessionLog)
            if (guestBackend == RuntimeGuestBackend.FEX) {
                runCatching { patchHostLibcStatx(installedRuntime, sessionLog) }
                    .onFailure { sessionLog.warning("Runtime", "host libc statx patch failed: ${it.message}") }
            }
            sessionLog.info(
                "Vulkan",
                "driver=${driverConfiguration.driverProfileId} box64Mode=${driverConfiguration.box64Mode}",
            )
            val runtimeHome = AppStorage.runtimeHomeDir().toPath()
            val lowRamProfile = detectLowRamProfile(sessionLog)
            warmupPending = launchProfile.boolean("gpu.shader_warmup") && shaderWarmupPending(gameId)
            ShadPs4ConfigManager.write(runtimeHome, launchProfile)
            // Enforce Android performance/correctness overrides after profile write so
            // they cannot be regressed by a stale or first-boot shadPS4 default config.
            ShadPs4ConfigManager.applyAndroidCompatibilityProfile(runtimeHome)
            // Per-title overrides win for this game only (last write).
            if (compat.config.isNotEmpty()) {
                ShadPs4ConfigManager.applyGameCompat(runtimeHome, compat.config)
            }
            seedMissingSaveData(runtimeHome, gameId, compat, sessionLog)
            seedTrophyKey(runtimeHome, sessionLog)
            val patchesRoot = runtimeHome.resolve(".local/share/shadPS4/patches").toFile()
            val autoCheats = PatchCheatManager.autoImportCheatsFromDropFolder(gameId)
            if (autoCheats.isNotEmpty()) {
                sessionLog.info("Patches", "auto-imported from Download/ps4-cheat: ${autoCheats.joinToString()}")
            }
            val bundledImported = PatchCheatManager.autoImportBundledPatches(gameId)
            if (bundledImported > 0) {
                sessionLog.info("Patches", "auto-imported $bundledImported bundled patch(es) for $gameId")
            }
            PatchCheatManager.applyToGame(gameId, patchesRoot)
                .onSuccess { applied ->
                    if (applied != null) {
                        sessionLog.info("Patches", "enabled patches/cheats applied for $gameId -> ${applied.name}")
                    }
                }
                .onFailure { error ->
                    sessionLog.warning("Patches", "could not apply patches for $gameId: ${error.message}")
                }
            sessionLog.info("Vulkan", "persistent pipeline cache enabled home=$runtimeHome")
            internalLogPaths = listOf(
                runtimeHome.resolve(".local/share/shadPS4/log/shad_log.txt"),
                installedRuntime.resolve(".local/share/shadPS4/log/shad_log.txt"),
                installedRuntime.resolve("shad_log.txt"),
            )
            logMirrorJob = startLiveLogMirror(sessionLog, internalLogPaths)
            bootWatchdogJob = startBootWatchdog(sessionLog, outputFile) { sessionReachedRunning }
            val isBox64 = guestBackend == RuntimeGuestBackend.BOX64
            val backendEnvironment = if (isBox64) {
                launchProfileProvider.box64Environment(activeProfile)
            } else {
                emptyMap()
            }
            val traceEnabled = (driverConfiguration.environment + backendEnvironment).keys
                .filter { it.startsWith("BOX64_TRACE") || it == "BOX64_DYNAREC_TRACE" }
            if (traceEnabled.isNotEmpty()) {
                sessionLog.warning(
                    "Runtime",
                    "box64 trace was enabled ($traceEnabled); forcing off — trace floods shadps4.log and stalls the guest",
                )
            }
            val lowRamEnv = if (lowRamProfile && isBox64) {
                mapOf(
                    "BOX64_DYNAREC_PURGE" to "1",
                    "BOX64_DYNAREC_PURGE_AGE" to "$BOX64_JIT_PURGE_AGE",
                )
            } else {
                emptyMap()
            }
            val box64TraceOff = if (isBox64) {
                mapOf(
                    "BOX64_TRACE" to "0",
                    "BOX64_TRACE_INIT" to "0",
                    "BOX64_TRACE_START" to "0",
                    "BOX64_DYNAREC_TRACE" to "0",
                )
            } else {
                emptyMap()
            }
            val baseEnvironment = runtimeEnvironment(installedRuntime, runtimeHome, socketRoot, xServer.display) +
                driverConfiguration.environment + backendEnvironment + box64TraceOff + lowRamEnv +
                compat.launchEnv + if (isBox64) compat.box64Env else emptyMap()
            val environment = baseEnvironment +
                mapOf("LD_PRELOAD" to nativeLibraryDir.resolve("libbachata_syscalls.so").toString())
            hiddenPluginGameRoot = gameRoot
            pluginsHidden = hideBrokenPlugins(gameRoot.toPath(), compat, sessionLog)
            val shadPs4Executable = if (guestBackend == RuntimeGuestBackend.FEX) {
                fexBinary
            } else {
                installedRuntime.resolve("bin/shadps4")
            }
            process = try {
                RuntimeProcessLauncher().launch(
                    RuntimeProcessRequest(
                        nativeLibraryDir = nativeLibraryDir,
                        runtimeRoot = installedRuntime,
                        overrideRoot = gameRoot.toPath(),
                        gamesRoot = gamesRoot.toPath(),
                        storageRoot = filesDir.toPath(),
                        shadPs4Executable = shadPs4Executable,
                        socketPath = controlFile.path,
                        environment = environment,
                        arguments = listOf("-g", eboot.path),
                        outputPath = outputFile.toPath(),
                        box64Mode = driverConfiguration.box64Mode,
                        guestBackend = guestBackend,
                    ),
                )
            } catch (launchError: Exception) {
                throw launchError
            }
            sessionLog.info("Runtime", "backend process launched")
            // CONTEXT_READY is observed by the Android server when the real packaged client
            // connects (Task 5 probe / game Vulkan init). Do not block control-socket accept.
            val acceptFuture = acceptExecutor.submit<LocalSocket> { serverSocket.accept() }
            while (clientSocket == null) {
                clientSocket = try {
                    acceptFuture.get(ACCEPT_POLL_MILLIS, TimeUnit.MILLISECONDS)
                } catch (_: TimeoutException) {
                    check(process?.isAlive == true) {
                        "shadPS4 exited before socket connect: ${process?.exitCode}"
                    }
                    null
                }
            }
            val governorPid = process?.pid
            if (governorPid != null) {
                performanceGovernor = PerformanceGovernor(governorPid).also { governor ->
                    sessionLog.info("Performance", "governor armed ${governor.summary}")
                    if (!governor.applyInitialPin()) {
                        sessionLog.warning("Performance", "governor initial pin failed (masks ineffective)")
                    }
                }
            }
            val encoder = ControllerFrameEncoder()
            val controllerOutput = clientSocket.outputStream
            val writeLock = Any()
            val inputFrames = java.util.concurrent.atomic.AtomicLong(0L)
            val lastInputLogNanos = java.util.concurrent.atomic.AtomicLong(0L)
            val sink: (Int, ControllerSnapshot) -> Unit = { slot, snapshot ->
                encoder.encode(slot, snapshot)?.let { frame ->
                    runCatching {
                        synchronized(writeLock) {
                            controllerOutput.write(frame)
                            controllerOutput.flush()
                        }
                    }
                }
                val frames = inputFrames.incrementAndGet()
                val nowNanos = System.nanoTime()
                val lastLogged = lastInputLogNanos.get()
                if (nowNanos - lastLogged >= TimeUnit.SECONDS.toNanos(1) &&
                    lastInputLogNanos.compareAndSet(lastLogged, nowNanos)
                ) {
                    sessionLog.info("Input", "controller frames $frames/s (last slot=$slot)")
                    inputFrames.set(0L)
                }
            }
            controllerSink = sink
            ManagedSession.attachControllerSlotSink(sink)
            sessionLog.info("Input", "controller transport attached")
            clientSocket.inputStream.bufferedReader().forEachLine { frame ->
                when {
                    frame == "BACHATA/1 EVENT Running" -> {
                        sessionLog.info("Session", "backend reported Running")
                        sessionReachedRunning = true
                        ManagedSession.update(ManagedSessionState.Running(gameId))
                    }
                    frame == "BACHATA/1 EVENT Frame" -> {
                        val nowNanos = System.nanoTime()
                        ManagedSession.recordPresentedFrame(nowNanos)
                        performanceGovernor?.observe(nowNanos, ManagedSession.frameTelemetry.value)?.let { action ->
                            sessionLog.info("Performance", action)
                        }
                        telemetryReporter.record(nowNanos, ManagedSession.frameTelemetry.value)?.let { sample ->
                            val governorLine = performanceGovernor?.status()?.let { " governor=$it" }.orEmpty()
                            sessionLog.info("Performance", sample.logLine() + governorLine)
                        }
                    }
                    frame.startsWith("BACHATA/1 ERROR code=") -> {
                        sessionLog.error("Backend", frame)
                        ManagedSession.update(
                            ManagedSessionState.Failed(RuntimeErrorCode.CONTENT_INVALID, frame.substringAfter("code=")),
                        )
                    }
                }
            }
            process?.waitFor(PROCESS_EXIT_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            val backendExit = process?.exitCode
            sessionLog.info("Session", "backend stopped exitCode=$backendExit")
            if (backendExit != null && backendExit != 0) {
                val backendTail = runCatching {
                    outputFile.readLines().takeLast(MAX_ERROR_LOG_LINES).joinToString("\n")
                }.getOrDefault("")
                android.util.Log.i("OnRps4Backend", "backend exit=$backendExit\n$backendTail")
                sessionLog.info("Backend", "exit=$backendExit tail=$backendTail")
                sessionLog.warning(
                    "Backend",
                    "exit=$backendExit (${ExitCodeInterpreter.describe(backendExit)})",
                )
                extractFirstFault(outputFile, internalLogPaths)?.let { sessionLog.error("Backend", "first fault: $it") }
                ExitCodeInterpreter.hint(backendExit)?.let { sessionLog.warning("Backend", it) }
                if ("bad_alloc" in backendTail) {
                    sessionLog.warning(
                        "Session",
                        "game ran out of RAM (std::bad_alloc). Close background apps and lower Display Resolution for this game.",
                    )
                }
                if (!sessionReachedRunning) {
                    sessionLog.warning(
                        "Session",
                        "guest exited before reaching Running (crash on boot). Check shadps4-internal.log for the first error.",
                    )
                }
                if (backendExit == 139) {
                    val advice = CrashLedger.recordExit(filesDir, gameId, backendExit, activeDriverId)
                    if (advice.changed) {
                        sessionLog.warning(
                            "Vulkan",
                            "recurring host segfault (${advice.consecutiveSegfaults}x); next launch will try driver=${advice.suggested}",
                        )
                    }
                }
            } else if (backendExit == 0 && sessionReachedRunning) {
                CrashLedger.clearSuggestion(filesDir, gameId)
            }
            ManagedSession.update(ManagedSessionState.Stopped(backendExit))
        } catch (_: CancellationException) {
            sessionLog.info("Session", "cancelled exitCode=${process?.exitCode}")
            ManagedSession.update(ManagedSessionState.Stopped(process?.exitCode))
        } catch (error: Exception) {
            val childOutput = runCatching { outputFile.readLines().takeLast(MAX_ERROR_LOG_LINES).joinToString(" | ") }
                .getOrDefault("")
            android.util.Log.i(
                "OnRps4Backend",
                "backend crash ${error.javaClass.simpleName}: ${error.message.orEmpty()}\n$childOutput",
            )
            val detail = listOfNotNull(error.message, childOutput.ifBlank { null }).joinToString(": ")
            sessionLog.error("Session", "${error.javaClass.simpleName}: ${error.message.orEmpty()}")
            ManagedSession.update(
                ManagedSessionState.Failed(RuntimeErrorCode.BACKEND_CRASHED, detail.ifBlank { error.javaClass.simpleName }),
            )
        } finally {
            internalLogPaths.firstOrNull { it.toFile().isFile }?.let { internalLog ->
                runCatching {
                    java.nio.file.Files.copy(
                        internalLog,
                        sessionLog.directory.resolve("shadps4-internal.log"),
                        StandardCopyOption.REPLACE_EXISTING,
                    )
                }.onFailure {
                    sessionLog.warning("Logs", "internal backend log copy failed: ${it.message.orEmpty()}")
                }
            }
            logMirrorJob?.cancel()
            logMirrorJob = null
            bootWatchdogJob?.cancel()
            bootWatchdogJob = null
            performanceGovernor?.stop()?.let { action -> sessionLog.info("Performance", action) }
            performanceGovernor = null
            exportSessionLogs(sessionLog)
            controllerSink?.let { sink ->
                repeat(4) { slot -> ManagedSession.submitController(slot, ControllerSnapshot.Neutral) }
                ManagedSession.detachControllerSlotSink(sink)
            }
            pointerSink?.let { ManagedSession.detachPointerSink(it) }
            pointerSink = null
            GamepadInputManager.onSessionEnd()
            val guestExit = process?.exitCode
            process?.destroyForcibly()
            process = null
            runCatching { clientSocket?.close() }
            runCatching { serverSocket?.close() }
            runCatching { boundSocket?.close() }
            acceptExecutor.shutdownNow()
            runCatching { xServer?.let { runBlocking { it.stop() } } }
            controlFile.delete()
            if (pluginsHidden) {
                hiddenPluginGameRoot?.let { restoreHiddenPlugins(it.toPath(), sessionLog) }
            }
            sessionLog.info("Session", "cleanup complete")
            AppLog.log(this, "onRps4Session", "ended exitCode=$guestExit exported to files/logs/")
            if (sessionReachedRunning && warmupPending) {
                runCatching { shaderWarmupMarker(gameId).parentFile?.mkdirs() }
                runCatching { shaderWarmupMarker(gameId).writeText(System.currentTimeMillis().toString()) }
                sessionLog.info("Performance", "shader warm-up complete; marked ${gameId}.done")
            }
            stopSelf()
        }
    }

    /**
     * Some games (e.g. Shovel Knight) open /savedata0/saveData.bin with a raw
     * FIO open and crash on a null handle when the file is missing on first
     * launch (guest writes to address 0x0, shadps4 terminates with exit 33).
     * A zero-length placeholder is also fatal: those games special-case size 0
     * with a fallback that stats a closed handle and then uses a garbage size.
     * Seed a non-empty zeroed placeholder in the game's SAVEDATA slot so the
     * open succeeds and the game parses it as an empty/corrupt save (fresh
     * game) instead of crashing.
     */
    private fun seedMissingSaveData(runtimeHome: Path, gameId: String, compat: GameCompat, sessionLog: SessionLog) {
        val savedataRoot = runtimeHome
            .resolve(".local/share/shadPS4/home/1000/savedata")
            .resolve(gameId)
        val slotDirs = LinkedHashSet<Path>()
        slotDirs.add(savedataRoot.resolve("SAVEDATA00"))
        if (java.nio.file.Files.isDirectory(savedataRoot)) {
            java.nio.file.Files.newDirectoryStream(savedataRoot, "SAVEDATA*").use { stream ->
                stream.forEach { slotDirs.add(it) }
            }
        }
        // Games either mount /savedata0 straight to the title dir or to the
        // SAVEDATA00 slot; seed candidate save files at BOTH levels so a raw
        // FIO open never hits a missing file (null handle -> guest exit 33,
        // e.g. Sonic Mania's autosave). Some games mount a custom save dir
        // name (not SAVEDATA00), so walk every existing subdirectory too.
        val seedTargets = buildList {
            addAll(slotDirs)
            add(savedataRoot)
            if (java.nio.file.Files.isDirectory(savedataRoot)) {
                java.nio.file.Files.walk(savedataRoot).use { stream ->
                    stream.filter { java.nio.file.Files.isDirectory(it) }.forEach { add(it) }
                }
            }
        }
        val seeded = mutableListOf<Path>()
        for (dir in seedTargets) {
            for (name in SAVE_CANDIDATE_FILES) {
                runCatching {
                    java.nio.file.Files.createDirectories(dir)
                    val save = dir.resolve(name)
                    if (!java.nio.file.Files.exists(save) || java.nio.file.Files.size(save) == 0L || isZeroFilled(save)) {
                        java.nio.file.Files.write(save, ByteArray(SEED_SAVE_PLACEHOLDER_BYTES))
                        seeded.add(save)
                    }
                }
            }
        }
        // Title-specific files the guest opens with a raw FIO open and faults on
        // a null handle when missing. CUSA47134 reads /savedata0/cw4settings.xml
        // on level transition; the missing file yields a null FILE* and the game
        // dereferences it (guest fault: Read from address 0x8) -> exit 33.
        for (dir in seedTargets) {
            for (name in compat.seedSaveFiles) {
                runCatching {
                    java.nio.file.Files.createDirectories(dir)
                    val save = dir.resolve(name)
                    if (!java.nio.file.Files.exists(save) || java.nio.file.Files.size(save) == 0L || isZeroFilled(save)) {
                        java.nio.file.Files.write(save, ByteArray(SEED_SAVE_PLACEHOLDER_BYTES))
                        seeded.add(save)
                    }
                }
            }
        }
        // Unity games (via their SaveData.prx / PlayerPrefs) call sceSaveDataGetSaveDataMemory2
        // and the bachata fork requires sce_sdmemory/memory.dat to already exist, otherwise it
        // fails ("called without save memory initialized") and the game hangs when creating a
        // new save (e.g. Silksong's New Game). Seed a zeroed save-memory region.
        runCatching {
            val memoryDir = savedataRoot.resolve("sce_sdmemory")
            java.nio.file.Files.createDirectories(memoryDir)
            val memory = memoryDir.resolve("memory.dat")
            if (!java.nio.file.Files.exists(memory) || java.nio.file.Files.size(memory) == 0L) {
                java.nio.file.Files.write(memory, ByteArray(SEED_SAVE_PLACEHOLDER_BYTES))
                seeded.add(memory)
            }
        }
        if (seeded.isNotEmpty()) {
            sessionLog.info(
                "SaveData",
                "seeded ${seeded.size} placeholder save file(s) for gameId=$gameId: " +
                    seeded.joinToString(", ") { "${it.parent.fileName}/${it.fileName}" },
            )
        }
    }

    private fun isZeroFilled(path: Path): Boolean = runCatching {
        java.nio.file.Files.readAllBytes(path).all { it == 0.toByte() }
    }.getOrDefault(false)

    // The shadPS4 bachata fork reads the trophy key from keys.json
    // (TrophyKeySet.ReleaseTrophyKey) at the UserDir before it can decrypt a
    // game's TROPHY.TRP. Without it TRP::Extract fails ("Trophy decryption key
    // is not specified"), so no trophy Xml/Icons are extracted, sceNpTrophy*
    // calls fail, and games that then invoke sceKernelDebugRaiseException on a
    // bad trophy result crash with a guest fault (e.g. CUSA02889). Seed the
    // well-known retail trophy key once so every game's trophy set can be
    // extracted.
    private fun seedTrophyKey(runtimeHome: Path, sessionLog: SessionLog) {
        runCatching {
            val keysFile = runtimeHome.resolve(".local/share/shadPS4/keys.json")
            val keyHex = "21F41A6BAD8A1D3ECA7AD586C101B7A9"
            val existing = runCatching {
                String(java.nio.file.Files.readAllBytes(keysFile))
            }.getOrNull()
            if (existing != null && existing.contains(keyHex)) return
            java.nio.file.Files.createDirectories(keysFile.parent)
            java.nio.file.Files.write(
                keysFile,
                "{\n  \"TrophyKeySet\": {\n    \"ReleaseTrophyKey\": \"$keyHex\"\n  }\n}\n".toByteArray(),
            )
            sessionLog.info("Trophy", "seeded retail trophy key into $keysFile")
        }
    }

    // Some games load plugins that crash under the bachata fork's stubbed
    // subsystems. The bachata fork stubs NP (sceNetPoolCreate/sceSslInit DUMMY)
    // and its libSceLibcInternal lacks a real __cxa_guard_acquire (CommonStub
    // always returns zero), so Unity games loading their own UnityNpToolkit2.prx
    // construct half-zeroed statics and crash with a null write (exit 33) before
    // the main menu. Hide the plugin for the session so the game boots without
    // online services; restore it afterwards.
    private fun hideBrokenPlugins(gameRoot: Path, compat: GameCompat, sessionLog: SessionLog): Boolean {
        if (compat.hidePlugins.isEmpty()) return false
        val hidden = mutableListOf<File>()
        for (relative in compat.hidePlugins) {
            val plugin = gameRoot.resolve(relative).toFile()
            if (!plugin.isFile) continue
            val disabled = File(plugin.parentFile, "${plugin.name}.disabled")
            if (disabled.exists()) continue
            if (runCatching { plugin.renameTo(disabled) }.getOrDefault(false)) {
                hidden.add(plugin)
            }
        }
        if (hidden.isNotEmpty()) {
            hiddenPluginRelativePaths = hidden.map { gameRoot.relativize(it.toPath()).toString() }
            sessionLog.warning(
                "Workaround",
                "hidden ${hidden.size} plugin(s) (${hidden.joinToString(", ") { it.name }}) — " +
                    "stubbed subsystems would null-write -> guest exit 33",
            )
            return true
        }
        return false
    }

    private fun restoreHiddenPlugins(gameRoot: Path, sessionLog: SessionLog) {
        if (hiddenPluginRelativePaths.isEmpty()) return
        val restored = mutableListOf<String>()
        for (relative in hiddenPluginRelativePaths) {
            val disabled = gameRoot.resolve("$relative.disabled").toFile()
            if (!disabled.isFile) continue
            if (runCatching { disabled.renameTo(File(disabled.parentFile, disabled.name.removeSuffix(".disabled"))) }
                    .getOrDefault(false)
            ) {
                restored.add(disabled.name.removeSuffix(".disabled"))
            }
        }
        if (restored.isNotEmpty()) {
            sessionLog.info("Workaround", "restored ${restored.size} plugin(s) (${restored.joinToString(", ")})")
        }
        hiddenPluginRelativePaths = emptyList()
    }

    private fun logGameTrophyInfo(gameRoot: File, sessionLog: SessionLog) {
        val sfo = File(gameRoot, "sce_sys/param.sfo")
        val npCommId: String? = runCatching {
            sfo.takeIf { it.isFile }?.readBytes()?.let { ParamSfoReader.parse(it).npCommId }
        }.getOrNull()
        // Trophy data can live at several locations depending on how the dump was
        // ripped: game root, sce_sys/, or sce_sys/trophy/ (PKG-style).
        val trophyPaths = listOf(
            File(gameRoot, "TROPHY.TRP"),
            File(gameRoot, "sce_sys/TROPHY.TRP"),
            File(gameRoot, "sce_sys/trophy/trophy00.trp"),
            File(gameRoot, "sce_sys/trophy/trophy01.trp"),
        )
        val trp = trophyPaths.firstOrNull { it.isFile }
        sessionLog.info(
            "Content",
            "trophies TROPHY.TRP=${trp?.let { "present(${it.length()}B)" } ?: "MISSING"} " +
                "NPCOMMID=${npCommId ?: "absent"}",
        )
        if (trp == null && npCommId == null) {
            sessionLog.warning(
                "Content",
                "trophy data incomplete (no TROPHY.TRP and no NPCOMMID): trophy-dependent games may crash at " +
                    "sceNpTrophyRegisterContext; verify the dump contains sce_sys/trophy/*.trp",
            )
        }
    }

    private fun logGameContent(gameRoot: File, sessionLog: SessionLog) {
        val top = gameRoot.listFiles()?.mapNotNull { it.name }?.sorted().orEmpty()
        sessionLog.info("Content", "gameRoot entries=${top.size} ${top.joinToString(",")}")
        val sceSys = File(gameRoot, "sce_sys").listFiles()?.mapNotNull { it.name }?.sorted().orEmpty()
        if (sceSys.isNotEmpty()) {
            sessionLog.info("Content", "sce_sys entries=${sceSys.size} ${sceSys.joinToString(",")}")
        }
        // Fox Engine games (P.T., MGSV) boot from /app0/init.lua; a dump missing it
        // hard-panics the moment the engine hands control to the (absent) scripts.
        sessionLog.info(
            "Content",
            "engine scripts init.lua=${File(gameRoot, "init.lua").isFile} " +
                "silent/start.lua=${File(gameRoot, "silent/start.lua").isFile}",
        )
    }

    /**
     * Many games keep their bulk payload inside a PSARC archive that the FIOS HLE
     * fakes but never decompresses: Unity games use /app0/archive.psarc, Spelunky
     * uses /app0/data.arc mounted at /app0/Data, etc. The HLE maps the FIOS mount
     * point to the game root host folder, so unpacking every PSARC archive into
     * the game root makes each FIOS-hosted path resolve. Idempotent: existing
     * same-size files are skipped, so a relaunch is cheap.
     */
    private fun ensureGameRootPsarcs(gameRoot: File, sessionLog: SessionLog) {
        val archives = gameRoot.listFiles()
            ?.filter { it.isFile && isPsarcArchive(it) }
            .orEmpty()
        if (archives.isEmpty()) return
        sessionLog.info("Content", "psarc extraction: found ${archives.joinToString(",") { it.name }}")
        for (archive in archives) {
            // Unity PSARC (archive.psarc) paths resolve directly under gameRoot via
            // the /app0 mount.  Non-Unity archives (e.g. Spelunky's data.arc) are
            // mounted by sceFiosArchiveMountSync at /app0/Data → gameRoot, but
            // kernel open resolves the full path via the first-match /app0 mount,
            // yielding a host path of gameRoot/Data/<manifest_rel>.  Extract there.
            val isUnityPsarc = archive.name.equals("archive.psarc", ignoreCase = true) ||
                archive.name.equals("archive_patch.psarc", ignoreCase = true)
            val destDir = if (isUnityPsarc) gameRoot else File(gameRoot, "Data")
            runCatching {
                val result = PsarcExtractor.extract(archive, destDir) { done, total, name ->
                    if (done % 128 == 0 || done == total) {
                        sessionLog.info("Content", "psarc ${archive.name}: $done/$total $name")
                    }
                }
                if (result.message != null) {
                    sessionLog.warning("Content", "psarc ${archive.name}: ${result.message}")
                } else {
                    sessionLog.info(
                        "Content",
                        "psarc ${archive.name}: extracted=${result.filesExtracted} " +
                            "bytes=${result.bytesWritten} skippedExisting=${result.skippedExisting} " +
                            "total=${result.totalFiles}",
                    )
                }
            }.onFailure { error ->
                sessionLog.warning("Content", "psarc ${archive.name} extraction failed: ${error.message}")
            }
        }
    }

    private fun isPsarcArchive(file: File): Boolean {
        if (!file.name.endsWith(".arc", ignoreCase = true) &&
            !file.name.endsWith(".psarc", ignoreCase = true)
        ) {
            return false
        }
        return runCatching {
            val magic = ByteArray(4)
            java.io.DataInputStream(file.inputStream()).use { stream ->
                stream.readFully(magic)
            }
            String(magic, Charsets.US_ASCII) == "PSAR"
        }.getOrDefault(false)
    }

    private fun detectLowRamProfile(sessionLog: SessionLog): Boolean {
        val am = getSystemService(Service.ACTIVITY_SERVICE) as? android.app.ActivityManager ?: return false
        val info = android.app.ActivityManager.MemoryInfo()
        am.getMemoryInfo(info)
        val availMb = (info.availMem / 1024 / 1024).toInt()
        if (availMb >= LOW_RAM_THRESHOLD_MB) return false
        sessionLog.warning(
            "Memory",
            "low RAM ($availMb MB free < $LOW_RAM_THRESHOLD_MB MB): box64 JIT purge age=$BOX64_JIT_PURGE_AGE",
        )
        return true
    }

    private fun shaderWarmupMarker(gameId: String): File = File(filesDir, "warmup").resolve("$gameId.done")

    /** True while the game has never reached Running with the warm-up cap applied. */
    private fun shaderWarmupPending(gameId: String): Boolean = !shaderWarmupMarker(gameId).isFile

    private fun detectSoCInfo(): String {
        val hardware = android.os.Build.HARDWARE
        val board = android.os.Build.BOARD
        val cpuInfoLine = runCatching {
            java.io.File("/proc/cpuinfo").useLines { lines ->
                lines.take(64).firstOrNull { it.startsWith("Hardware", ignoreCase = true) }
                    ?.substringAfter(":")?.trim()
            }
        }.getOrNull()
        return buildList {
            if (hardware.isNotBlank()) add("socHardware=$hardware")
            if (board.isNotBlank()) add("board=$board")
            if (!cpuInfoLine.isNullOrBlank()) add("cpu=$cpuInfoLine")
        }.joinToString(" ")
    }

    private fun logMemoryInfo(sessionLog: SessionLog) {
        val am = getSystemService(Service.ACTIVITY_SERVICE) as? android.app.ActivityManager ?: return
        val info = android.app.ActivityManager.MemoryInfo()
        am.getMemoryInfo(info)
        val totalMb = info.totalMem / 1024 / 1024
        val availMb = info.availMem / 1024 / 1024
        sessionLog.info("Device", "ramTotal=${totalMb}MB ramAvailable=${availMb}MB")
        if (availMb < 1024) {
            sessionLog.warning(
                "Device",
                "low RAM ($availMb MB free): close background apps and/or lower Resolution Scale for this game to avoid OOM crashes",
            )
        }
    }

    /**
     * shadps4 hard-links libudev.so.1 (DT_NEEDED) and its systemd-derived code calls statx()
     * and name_to_handle_at(), which box64 forwards as native arm64 syscalls. Android 10 app
     * seccomp (arm64_app policy) does not whitelist them, so the process is killed with exit 159
     * (SIGSYS) before returning.
     *
     * For **guest (x86_64) libudev**: We swap in a patched guest libudev whose statx@plt and
     * name_to_handle_at@plt call sites are redirected to a stub that sets errno = ENOSYS and
     * returns -1; systemd's own fallbacks then retry via openat/readlink/sysfs (all whitelisted),
     * so udev keeps working. The patched binary is shipped in assets/runtime-patches (see
     * tools/patch_libudev.py) and is applied even when the MD5 doesn't match a known version
     * (matching Python tool behavior — any unrecognized libc gets patched with a warning).
     *
     * For **host (ARM64) libudev**: Required by FEX as a DT_NEEDED dependency. Instead of hiding
     * it, we patch [host/libc.so.6] at runtime via [patchHostLibcStatx] to replace the statx and
     * name_to_handle_at SVC instructions with -ENOSYS returns, so the kernel never sees them.
     *
     * For Box64, the host libudev is hidden (renamed to *.disabled-statx) so the guest never
     * loads it — the guest uses the patched x86_64 version in lib/x86_64-linux-gnu/.
     */
    private fun patchGuestLibudev(runtimeRoot: Path, guestBackend: RuntimeGuestBackend, sessionLog: SessionLog) {
        val patched = runCatching {
            assets.open(LIBUDEV_PATCH_ASSET).use { it.readBytes() }
        }.getOrElse {
            sessionLog.warning("Runtime", "libudev patch asset unavailable: ${it.message}")
            return
        }
        val guestDir = runtimeRoot.resolve("lib/x86_64-linux-gnu").toFile()
        val guestNames = listOf("libudev.so.1.7.10", "libudev.so.1", "libudev.so")
        var patchedCount = 0
        guestNames.forEach { name ->
            val original = File(guestDir, name)
            val disabled = File(guestDir, name + LIBUDEV_DISABLED_SUFFIX)
            if (disabled.isFile && !original.exists() && disabled.renameTo(original)) {
                sessionLog.info("Runtime", "libudev restored: lib/x86_64-linux-gnu/$name")
            }
            if (!original.isFile) {
                sessionLog.warning("Runtime", "libudev not present: lib/x86_64-linux-gnu/$name")
                return@forEach
            }
            val md5 = original.readBytes().md5Hex()
            when {
                md5 == LIBUDEV_PATCHED_MD5 -> Unit
                md5 == LIBUDEV_UNPATCHED_MD5 -> {
                    original.writeBytes(patched)
                    patchedCount++
                    sessionLog.info("Runtime", "libudev patched (statx/name_to_handle_at -> ENOSYS): $name")
                }
                md5 in LIBUDEV_STALE_PATCHED_MD5S -> {
                    original.writeBytes(patched)
                    patchedCount++
                    sessionLog.info("Runtime", "libudev re-patched (replaced stale stub): $name")
                }
                else -> {
                    original.writeBytes(patched)
                    patchedCount++
                    sessionLog.warning(
                        "Runtime",
                        "libudev patched (unknown MD5 $md5, statx/name_to_handle_at -> ENOSYS): $name",
                    )
                }
            }
        }
        val hostDir = runtimeRoot.resolve("host").toFile()
        if (guestBackend == RuntimeGuestBackend.BOX64) {
            var hostHidden = 0
            hostDir.listFiles { f ->
                f.isFile && f.name.startsWith("libudev.so") && !f.name.endsWith(LIBUDEV_DISABLED_SUFFIX)
            }.orEmpty().forEach { file ->
                if (file.renameTo(File(file.parentFile, file.name + LIBUDEV_DISABLED_SUFFIX))) {
                    hostHidden++
                }
            }
            sessionLog.info("Runtime", "libudev patch done patched=$patchedCount hostHidden=$hostHidden")
        } else {
            var hostRestored = 0
            hostDir.listFiles { f ->
                f.isFile && f.name.startsWith("libudev.so") && f.name.endsWith(LIBUDEV_DISABLED_SUFFIX)
            }.orEmpty().forEach { file ->
                val target = File(file.parentFile, file.name.removeSuffix(LIBUDEV_DISABLED_SUFFIX))
                if (!target.exists() && file.renameTo(target)) {
                    hostRestored++
                }
            }
            sessionLog.info("Runtime", "libudev patch done patched=$patchedCount hostRestored=$hostRestored")
        }
    }

    /**
     * Binary-patch the ARM64 host [host/libc.so.6] so that the `statx` (291) and
     * `name_to_handle_at` (264) syscall wrappers can never reach the kernel.  Android 10 app
     * seccomp (`arm64_app` policy) does not whitelist these syscalls and kills the process with
     * exit 159 (SIGSYS) when they trap.
     *
     * At build time `tools/patch-runtime-statx.py` does the same patch at fixed offsets, but
     * different runtime builds or glibc versions shift those offsets.  This method uses
     * **pattern scanning**: it looks for `mov x8, #N` immediately followed by `svc #0` for
     * each blocked syscall number and replaces the `svc` with `mov x0, #-38` (-ENOSYS).
     * Glibc's own statx wrapper retries via `__fstatat64` on ENOSYS, so statx() keeps working
     * transparently.
     */
    private fun patchHostLibcStatx(runtimeRoot: Path, sessionLog: SessionLog) {
        val libc = runtimeRoot.resolve("host/libc.so.6").toFile()
        if (!libc.isFile) {
            sessionLog.warning("Runtime", "host/libc.so.6 not found, skipping statx/name_to_handle_at patch")
            return
        }
        val data = libc.readBytes()
        if (data.size < 8) return

        // mov x8, #<N> ; svc #0  -- AArch64 little-endian bytes
        val movSvcTargets = listOf(
            // (syscall_number, mov_x8_encoding[4], description)
            Triple(291, byteArrayOf(0x68.toByte(), 0x24, 0x80.toByte(), 0xD2.toByte()), "statx"),
            Triple(264, byteArrayOf(0x08, 0x21, 0x80.toByte(), 0xD2.toByte()), "name_to_handle_at"),
        )
        // mov x0, #-38  (ENOSYS)  -- replaces the svc #0
        val movX0Enosys = byteArrayOf(0xA0.toByte(), 0x04, 0x80.toByte(), 0x92.toByte())

        var patched = 0
        for (i in 4 until data.size - 4) {
            if (!isSvc0(data, i)) continue
            for ((_, movEnc, name) in movSvcTargets) {
                if (data[i - 4] == movEnc[0] && data[i - 3] == movEnc[1] &&
                    data[i - 2] == movEnc[2] && data[i - 1] == movEnc[3]
                ) {
                    data[i] = movX0Enosys[0]
                    data[i + 1] = movX0Enosys[1]
                    data[i + 2] = movX0Enosys[2]
                    data[i + 3] = movX0Enosys[3]
                    patched++
                    sessionLog.info("Runtime", "host libc patched: $name (svc @ $i -> mov x0,#-38)")
                }
            }
        }
        if (patched > 0) {
            libc.writeBytes(data)
            sessionLog.info("Runtime", "host libc statx/name_to_handle_at patch done: $patched site(s)")
        } else {
            sessionLog.info("Runtime", "host libc statx/name_to_handle_at: no matching patterns (already patched or different glibc)")
        }
    }

    /** Returns true if the 4 bytes at [pos] encode `svc #0` (0xD4000001 in little-endian). */
    private fun isSvc0(data: ByteArray, pos: Int): Boolean =
        data[pos] == 0x01.toByte() && data[pos + 1] == 0x00.toByte() &&
            data[pos + 2] == 0x00.toByte() && data[pos + 3] == 0xD4.toByte()

    private fun ByteArray.md5Hex(): String =
        MessageDigest.getInstance("MD5").digest(this).joinToString("") { "%02x".format(it.toInt() and 0xFF) }

    /** First evidence-bearing line (guest fault, signal, assertion, OOM) in the backend output. */
    private fun extractFirstFault(output: File, internalLogs: List<Path>): String? {
        val sources = listOf(output.toPath()) + internalLogs
        for (source in sources) {
            if (!java.nio.file.Files.isRegularFile(source)) continue
            val found = runCatching {
                java.nio.file.Files.newBufferedReader(source).useLines { lines ->
                    lines.firstOrNull { line ->
                        val trimmed = line.trim()
                        trimmed.isNotBlank() && FAULT_PATTERNS.any { it in trimmed }
                    }?.trim()?.take(300)
                }
            }.getOrNull()
            if (found != null) return found
        }
        return null
    }

    /**
     * Whether an auto-fallback driver is actually usable with this runtime.
     * The system driver is always available; Turnip ids must be installed in the
     * driver registry (filesDir/vulkan-drivers/installed).
     */
    private fun driverAvailableInRuntime(runtimeRoot: Path, driverId: String): Boolean {
        if (driverId == RuntimeVulkanDriverIds.SYSTEM) return true
        return runCatching {
            DriverRegistry(File(filesDir, VULKAN_DRIVERS_DIR).toPath()).resolve(driverId) != null
        }.getOrDefault(false)
    }

    private fun installRuntime(): Path {
        val installRoot = AppStorage.runtimeDir().toPath()
        if (!Ali.onRps4.BuildConfig.DOWNLOAD_RUNTIME) {
            val manifest = assets.open("runtime/manifest.json").bufferedReader().use {
                Json { ignoreUnknownKeys = true }.decodeFromString<RuntimeManifest>(it.readText())
            }
            val target = installRoot.resolve(manifest.runtimeVersion)
            if (target.toFile().isDirectory) return target
            return assets.open("runtime/runtime.zip").use { bundle ->
                RuntimeInstaller(installRoot).install(bundle, manifest).getOrElse { error ->
                    if (error is FileAlreadyExistsException && target.toFile().isDirectory) target else throw error
                }
            }
        }
        val installedDir = installRoot.toFile().listFiles()?.firstOrNull { it.isDirectory && it.name.startsWith("box64-") }
        if (installedDir != null) return installedDir.toPath()
        throw IllegalStateException("Runtime not installed")
    }

    /**
     * Dev override: if [getExternalFilesDir]("runtime-dev") contains any files
     * (e.g. a freshly built host/shadps4-arm64-fex dropped in by the local build
     * pipeline), overlay them onto a scratch copy of the installed runtime and
     * run from that. The scratch is only rebuilt when the override changes, so a
     * normal install boots straight from the installed runtime.
     */
    private fun applyDevOverride(installedRuntime: Path, sessionLog: SessionLog): Path {
        val devRoot = getExternalFilesDir("runtime-dev") ?: return installedRuntime
        val devFiles = devRoot.listFiles() ?: return installedRuntime
        if (devFiles.isEmpty()) return installedRuntime
        val stamp = buildString {
            append(installedRuntime.fileName.toString())
            devFiles.sortedBy { it.name }.forEach {
                append('|').append(it.name).append(':').append(it.lastModified()).append(':').append(it.length())
            }
        }
        val scratch = File(cacheDir, "dev-runtime")
        val stampFile = File(scratch, ".dev-override-stamp")
        val current = try {
            stampFile.readText()
        } catch (_: Exception) {
            ""
        }
        if (current != stamp) {
            sessionLog.info("Runtime", "applying dev override from ${devRoot.absolutePath}")
            scratch.deleteRecursively()
            installedRuntime.toFile().copyRecursively(scratch, overwrite = true)
            devRoot.copyRecursively(scratch, overwrite = true)
            stampFile.writeText(stamp)
        }
        return scratch.toPath()
    }

    private fun runtimeEnvironment(runtimeRoot: Path, runtimeHome: Path, socketRoot: File, display: String): Map<String, String> {
        val ldLibraryPath = listOf(
            runtimeRoot.resolve("lib/x86_64-linux-gnu").toFile(),
            runtimeRoot.resolve("host").toFile(),
            runtimeRoot.resolve("lib64").toFile(),
            File("/system/lib64"),
            File("/system/lib"),
            File("/vendor/lib64"),
            File("/vendor/lib"),
        ).filter { it.exists() }.joinToString(":")
        return mapOf(
            "HOME" to runtimeHome.toString(),
            "BOX64_PATH" to runtimeRoot.resolve("bin").toString(),
            "BOX64_LOG" to "1",
            "BOX64_SHOWBT" to "1",
            "BOX64_ROLLING_LOG" to "0",
            "BOX64_LOAD_ADDR" to "0x6000000000",
            "BOX64_PREFER_WRAPPED" to "1",
            "BOX64_DYNAREC_CALLRET" to "1",
            "BOX64_LD_LIBRARY_PATH" to "${runtimeRoot.resolve("lib/x86_64-linux-gnu")}:${runtimeRoot.resolve("lib64")}",
            "BOX64_EMULATED_LIBS" to EMULATED_LIBRARIES,
            "LD_LIBRARY_PATH" to ldLibraryPath,
            "BACHATA_ALSA_SOCKET" to File(socketRoot, UnixSocketConfig.ALSA_SERVER_PATH).path,
            "DISPLAY" to display,
            "SDL_VIDEODRIVER" to "x11",
            "SDL_JOYSTICK_DISABLE_UDEV" to "1",
            "SDL_HIDAPI_JOYSTICK_DISABLE_UDEV" to "1",
            "SDL_AUDIODRIVER" to "dummy",
            "XKB_CONFIG_ROOT" to runtimeRoot.resolve("usr/share/X11/xkb").toString(),
            "TMPDIR" to cacheDir.path,
            "XDG_CACHE_HOME" to File(cacheDir, "xdg").apply { mkdirs() }.path,
            "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
        )
    }

    private fun stopSession() {
        process?.destroy()
        sessionJob?.cancel()
        sessionJob = null
    }

    private fun startLiveLogMirror(sessionLog: SessionLog, extraSources: List<Path> = emptyList()): Job? {
        val externalRoot = getExternalFilesDir("logs") ?: return null
        val targetDir = File(externalRoot, sessionLog.directory.fileName.toString()).apply { mkdirs() }
        return scope.launch {
            val offsets = mutableMapOf<String, Long>()
            val logs = listOf(sessionLog.applicationLog, sessionLog.backendLog) + extraSources
            while (isActive) {
                logs.forEach { src ->
                    val name = src.fileName.toString()
                    if (!java.nio.file.Files.isRegularFile(src)) return@forEach
                    try {
                        val size = java.nio.file.Files.size(src)
                        val off = (offsets[name] ?: 0L).coerceAtMost(size)
                        if (size > off) {
                            val chunk = ByteArray(((size - off).coerceAtMost(64L * 1024)).toInt())
                            RandomAccessFile(src.toFile(), "r").use { raf ->
                                raf.seek(off)
                                val n = raf.read(chunk)
                                if (n > 0) {
                                    RandomAccessFile(File(targetDir, name), "rw").use { out ->
                                        out.seek(out.length())
                                        out.write(chunk, 0, n)
                                    }
                                    offsets[name] = off + n
                                }
                            }
                        }
                    } catch (_: Exception) {
                        // Source may be truncated mid-copy (child still writing); retry next tick.
                    }
                }
                delay(250)
            }
        }
    }

    private fun startBootWatchdog(sessionLog: SessionLog, outputFile: java.io.File, isRunning: () -> Boolean): Job? =
        scope.launch {
            val startNanos = System.nanoTime()
            var stallLogged = false
            while (isActive) {
                delay(BOOT_WATCHDOG_TICK_MILLIS)
                val proc = process
                if (proc == null || !proc.isAlive || isRunning()) return@launch
                val elapsedSec = TimeUnit.NANOSECONDS.toSeconds(System.nanoTime() - startNanos)
                val outputSize = runCatching { outputFile.length() }.getOrDefault(0L)
                val outputAgeSec = runCatching {
                    (System.currentTimeMillis() - outputFile.lastModified()).coerceAtLeast(0L) / 1000L
                }.getOrDefault(Long.MAX_VALUE)
                val stalled = outputSize > 0L && outputAgeSec >= BOOT_WATCHDOG_STALL_SECONDS
                if (stalled) {
                    if (!stallLogged) {
                        stallLogged = true
                        sessionLog.warning(
                            "Boot",
                            "possible boot stall: backend alive ${elapsedSec}s but shadps4 wrote nothing for ${outputAgeSec}s. " +
                                "If the game never reaches Running it is likely spinning in guest module init (e.g. libSceNgs2).",
                        )
                    }
                } else {
                    sessionLog.info(
                        "Boot",
                        "alive ${elapsedSec}s, still booting, last shadps4 output ${outputAgeSec}s ago (log=${outputSize}b)",
                    )
                }
            }
        }

    private fun exportSessionLogs(sessionLog: SessionLog) {
        val externalRoot = getExternalFilesDir("logs") ?: return
        runCatching {
            val targetDir = File(externalRoot, sessionLog.directory.fileName.toString())
            targetDir.deleteRecursively()
            java.nio.file.Files.walk(sessionLog.directory).use { paths ->
                paths.forEach { source ->
                    val target = targetDir.toPath().resolve(sessionLog.directory.relativize(source))
                    if (java.nio.file.Files.isDirectory(source)) {
                        java.nio.file.Files.createDirectories(target)
                    } else {
                        java.nio.file.Files.createDirectories(target.parent)
                        java.nio.file.Files.copy(source, target, StandardCopyOption.REPLACE_EXISTING)
                    }
                }
            }
            android.util.Log.i("OnRps4Logs", "session logs exported to $targetDir")
        }.onFailure {
            sessionLog.warning("Logs", "session log export failed: ${it.message.orEmpty()}")
        }
    }

    private fun createNotificationChannel() {
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Emulation", NotificationManager.IMPORTANCE_LOW),
        )
    }

    private fun notification(): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val stop = PendingIntent.getService(
            this, 1,
            Intent(ManagedSession.ACTION_STOP).setClassName(packageName, ManagedSession.SERVICE_CLASS),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle("OnRps4 emulation")
            .setContentText("Game session running")
            .setContentIntent(open)
            .addAction(0, "Stop", stop)
            .setOngoing(true)
            .build()
    }

    private companion object {
        const val CHANNEL_ID = "emulation"
        const val NOTIFICATION_ID = 41
        const val SURFACE_TIMEOUT_MS = 30_000L
        const val PROCESS_EXIT_TIMEOUT_SECONDS = 5L
        const val ACCEPT_POLL_MILLIS = 250L
        const val MAX_ERROR_LOG_LINES = 20
        const val BOOT_WATCHDOG_TICK_MILLIS = 20_000L
        const val BOOT_WATCHDOG_STALL_SECONDS = 60L
        // Lines that carry crash evidence in shad_log.txt / backend output.
        val FAULT_PATTERNS = listOf(
            "Unhandled", "SIGSEGV", "Segmentation", "SIGABRT", "terminate",
            "FATAL", "bad_alloc", "assertion failed", "Assertion", "Guest fault",
        )
        const val SEED_SAVE_PLACEHOLDER_BYTES = 65536
        // Games also read their input/options configuration straight from the
        // save root (e.g. Super Meat Boy opens /savedata0/inputconfigs.dat with a
        // raw FIO open and crashes on a null handle when it is missing, right
        // after scePadOpen/scePadClose). Seed those as well so the open succeeds
        // and the game falls back to default bindings instead of faulting.
        val SAVE_CANDIDATE_FILES = listOf(
            "saveData.bin", "game.sav", "savedata.bin", "data.bin", "slot0.sav",
            "inputconfigs.dat", "options.ini", "Options.bin",
        )
        const val MAX_LOG_SESSIONS = 10
        const val LOW_RAM_THRESHOLD_MB = 1536
        const val BOX64_JIT_PURGE_AGE = 1024
        // Native ARM64 shadPS4 (FEX backend). The launcher requires it to live in
        // the runtime's host/ dir, co-located with the ARM64 glibc + host libs
        // (name matches the runtime packager's deployment path).
        const val FEX_EXECUTABLE_RELATIVE = "host/shadps4-arm64-fex"
        const val VULKAN_DRIVERS_DIR = "vulkan-drivers/installed"
        const val EMULATED_LIBRARIES = "libSDL2-2.0.so.0:libudev.so.1:libuuid.so.1"
        const val LIBUDEV_DISABLED_SUFFIX = ".disabled-statx"
        const val LIBUDEV_PATCH_ASSET = "runtime-patches/libudev.so.1.7.10"
        const val LIBUDEV_UNPATCHED_MD5 = "bd1d94b43f77252bb1d3775bfb138384"
        const val LIBUDEV_PATCHED_MD5 = "6f2e32ecb1eceb5dc7fa9152a9da9596"
        val LIBUDEV_STALE_PATCHED_MD5S = setOf(
            "108ebd530b6a4eee3712be3204c3f953", // statx-only ENOSYS stub
            "5199b17b16153bff6b6f2446b88ab038", // buggy arg-swap thunk
        )
    }
}
