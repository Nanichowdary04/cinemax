package app.cinemax.mobile.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import app.cinemax.mobile.AppState
import app.cinemax.mobile.Cinemax
import app.cinemax.mobile.Detected
import app.cinemax.mobile.EngineMode
import app.cinemax.mobile.FullEngineState
import app.cinemax.mobile.OutputKind
import app.cinemax.mobile.audio.EQ_FREQS
import app.cinemax.mobile.audio.EarTest
import app.cinemax.mobile.head.HeadStatus
import kotlinx.coroutines.delay
import app.cinemax.mobile.audio.ListeningMode
import app.cinemax.mobile.audio.PresetId
import kotlin.math.abs
import kotlin.math.roundToInt

// ---------------------------------------------------------------------------- palette
private val Bg = Color(0xFF0A0A0B)
private val Surface = Color(0xFF141416)
private val Surface2 = Color(0xFF1B1B1E)
private val Line = Color(0x1FFFFFFF)
private val Text1 = Color(0xFFF2F2F0)
private val Text2 = Color(0x9EFFFFFF)
private val Text3 = Color(0x66FFFFFF)
private val Accent = Color(0xFFFF6A1A)
private val AccentSoft = Color(0x29FF6A1A)

@Composable
fun CinemaxApp(
    onStartFullEngine: () -> Unit,
    onRequestSpectrum: () -> Unit,
    hasSpectrumPermission: () -> Boolean,
    onOpenBatterySettings: () -> Unit,
    onOpenSoundSettings: () -> Unit,
    onRequestCamera: () -> Unit,
    hasCameraPermission: () -> Boolean,
    askEngine: Boolean = false,
    onEngineAnswer: (Boolean) -> Unit = {},
) {
    MaterialTheme(colorScheme = darkColorScheme(background = Bg, surface = Surface, primary = Accent, onSurface = Text1)) {
        val state by Cinemax.state.collectAsState()
        var tab by rememberSaveable { mutableIntStateOf(0) }
        var intro by rememberSaveable { mutableStateOf(true) }
        LaunchedEffect(Unit) { delay(3000); intro = false }   // opening screen: 3 s (tap to skip)
      Box(Modifier.fillMaxSize()) {

        Scaffold(
            containerColor = Bg,
            bottomBar = {
                NavigationBar(containerColor = Color(0xFF0E0E10), tonalElevation = 0.dp) {
                    listOf("Home", "Equalizer", "Effects", "TV", "Settings").forEachIndexed { i, label ->
                        NavigationBarItem(
                            selected = tab == i,
                            onClick = { tab = i },
                            icon = { NavIcon(i, tab == i) },
                            label = { Text(label, fontSize = 11.sp, letterSpacing = 0.5.sp) },
                            colors = NavigationBarItemDefaults.colors(
                                selectedTextColor = Text1, unselectedTextColor = Text3,
                                indicatorColor = AccentSoft,
                            ),
                        )
                    }
                }
            },
        ) { pad ->
            Box(Modifier.fillMaxSize().background(Brush.verticalGradient(listOf(Color(0xFF121214), Bg))).padding(pad)) {
                when (tab) {
                    0 -> HomeScreen(state, onStartFullEngine, onOpenSoundSettings, onRequestSpectrum, hasSpectrumPermission)
                    1 -> EqualizerScreen(state)
                    2 -> EffectsScreen(state, onRequestCamera, hasCameraPermission)
                    3 -> TvScreen(state)
                    else -> SettingsScreen(state, onStartFullEngine, onOpenBatterySettings, onOpenSoundSettings)
                }
            }
        }
        AnimatedVisibility(visible = intro, enter = fadeIn(tween(1)), exit = fadeOut(tween(450))) { Intro { intro = false } }
        if (askEngine && !intro) EngineAskDialog(onEngineAnswer)
      }
    }
}

/** Asked once the app is open, before Android's own screen-share prompt: what it is for, and a way to say no. */
@Composable
private fun EngineAskDialog(onAnswer: (Boolean) -> Unit) {
    androidx.compose.material3.AlertDialog(
        onDismissRequest = { onAnswer(false) },
        containerColor = Surface2,
        title = { Text("Start the CINEMAX engine?", color = Text1, fontSize = 18.sp, fontWeight = FontWeight.SemiBold) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("To improve the sound of other apps - YouTube, music, games - Android needs your OK to let CINEMAX hear them.",
                    color = Text2, fontSize = 14.sp, lineHeight = 19.sp)
                Text("Android calls this \"sharing your screen\". CINEMAX only uses the sound: nothing is seen, recorded or saved, and you can stop it any time.",
                    color = Text2, fontSize = 14.sp, lineHeight = 19.sp)
            }
        },
        confirmButton = {
            Box(Modifier.clip(RoundedCornerShape(10.dp)).background(Accent).clickable { onAnswer(true) }.padding(horizontal = 18.dp, vertical = 10.dp)) {
                Text("Continue", color = Color.Black, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
            }
        },
        dismissButton = {
            Box(Modifier.clip(RoundedCornerShape(10.dp)).clickable { onAnswer(false) }.padding(horizontal = 14.dp, vertical = 10.dp)) {
                Text("Not now", color = Text2, fontSize = 14.sp, fontWeight = FontWeight.Medium)
            }
        },
    )
}

/** Opening screen on every start: the logo and who made CINEMAX. Tap to skip. */
@Composable
private fun Intro(onSkip: () -> Unit) {
    Column(
        Modifier.fillMaxSize().background(Brush.verticalGradient(listOf(Color(0xFF16110E), Bg)))
            .pointerInput(Unit) { detectTapGestures { onSkip() } },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        LogoMark(Modifier.size(96.dp))
        Spacer(Modifier.height(22.dp))
        Text("CINEMAX", color = Text1, fontSize = 34.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 10.sp)
        Spacer(Modifier.height(6.dp))
        Text("Cinematic sound for your phone", color = Text2, fontSize = 13.sp, letterSpacing = 1.sp)
        Spacer(Modifier.height(64.dp))
        Text("DEVELOPED BY", color = Text3, fontSize = 11.sp, letterSpacing = 4.sp)
        Spacer(Modifier.height(8.dp))
        Text("NANI CHOWDARY", color = Accent, fontSize = 20.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 5.sp)
    }
}

// ---------------------------------------------------------------------------- Home
@Composable
private fun HomeScreen(state: AppState, onStartFullEngine: () -> Unit, onOpenSoundSettings: () -> Unit, onRequestSpectrum: () -> Unit, hasSpectrumPermission: () -> Boolean) {
    val p = state.params
    ScreenColumn {
        Row(verticalAlignment = Alignment.CenterVertically) {
            LogoMark(Modifier.size(30.dp))
            Spacer(Modifier.width(10.dp))
            Column {
                Text("CINEMAX", color = Text1, fontSize = 20.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 5.sp)
                Text("by Nani Chowdary", color = Accent, fontSize = 11.sp, fontWeight = FontWeight.Medium, letterSpacing = 1.5.sp)
            }
            Spacer(Modifier.weight(1f))
            PowerSwitch(state.enabled) {
                Cinemax.setEnabled(it)
                if (it && state.engineMode == EngineMode.FULL) onStartFullEngine()
            }
        }

        StatusLine(state)
        EngineBanner(state, onStartFullEngine)
        MakerEffectsWarning(state, onOpenSoundSettings)
        ModeSwitch(state)
        DolbyCard(state)

        Card {
            Eyebrow(if (state.enabled) "LIVE OUTPUT" else "OUTPUT")
            Text(p.preset.title, color = Text1, fontSize = 28.sp, fontWeight = FontWeight.Light, letterSpacing = 4.sp)
            Text(p.preset.tagline, color = Text2, fontSize = 13.sp)
            Spacer(Modifier.height(14.dp))
            SpectrumView(state.enabled, onRequestSpectrum, hasSpectrumPermission)
            Spacer(Modifier.height(12.dp))
            CompareButton(state.enabled, state.comparing)
        }

        Eyebrow("PRESETS")
        val tiles = listOf(PresetId.CINEMA, PresetId.IMAX, PresetId.DOLBY_STYLE, PresetId.THEATRE, PresetId.CONCERT_HALL,
            PresetId.MUSIC, PresetId.DEEP_BASS, PresetId.GAMING, PresetId.DIALOGUE, PresetId.NIGHT, PresetId.FLAT)
        tiles.chunked(2).forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                row.forEach { id -> PresetTile(id, p.preset == id, Modifier.weight(1f)) }
                if (row.size == 1) Spacer(Modifier.weight(1f))
            }
        }

        Card {
            Eyebrow("QUICK CONTROLS")
            ParamSlider("Bass boost", p.bassDb, 0f..12f, { "+%.1f dB".format(it) }) { v -> Cinemax.editParams { it.copy(bassDb = v) } }
            ParamSlider("Surround", p.surround, 0f..1f, { pct(it) }) { v -> Cinemax.editParams { it.copy(surround = v) } }
            ParamSlider("Room / hall", p.room, 0f..1f, { pct(it) }) { v -> Cinemax.editParams { it.copy(room = v) } }
        }
    }
}

/** Press and hold: every CINEMAX effect switches off so the original sound plays; release to hear CINEMAX again. */
@Composable
private fun CompareButton(enabled: Boolean, comparing: Boolean) {
    val shape = RoundedCornerShape(12.dp)
    Box(
        Modifier.fillMaxWidth().clip(shape).background(if (comparing) Surface2 else AccentSoft)
            .border(1.dp, if (comparing) Line else Accent.copy(alpha = 0.55f), shape)
            .pointerInput(enabled) {
                if (!enabled) return@pointerInput
                detectTapGestures(onPress = {
                    Cinemax.setCompare(true)
                    try { tryAwaitRelease() } finally { Cinemax.setCompare(false) }
                })
            }
            .padding(vertical = 13.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            when {
                !enabled -> "Turn CINEMAX on to compare"
                comparing -> "ORIGINAL SOUND · release for CINEMAX"
                else -> "Hold to hear the original"
            },
            color = if (comparing) Text2 else Text1, fontSize = 13.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 1.sp,
        )
    }
}


@Composable
private fun StatusLine(state: AppState) {
    val e = state.engine
    val (dot, text) = when {
        !state.enabled -> Text3 to "Off · phone sound untouched"
        state.comparing -> Text3 to "Original sound (comparing)"
        state.fullRunning && state.videoPassThrough -> Accent to "Video · Android effects, no delay · ${state.outputName}"
        state.fullRunning -> Accent to "CINEMAX engine · ${state.outputName}"
        e.anyActive -> Accent to "${if (state.engineMode == EngineMode.FULL) "Basic effects" else "Android effects"} · ${state.outputName}"
        else -> Color(0xFFFFB020) to "Starting…"
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(8.dp).clip(CircleShape).background(dot))
        Spacer(Modifier.width(8.dp))
        Text(text, color = Text2, fontSize = 13.sp)
    }
    if (state.enabled && e.notes.isNotEmpty())
        Text(e.notes.joinToString("\n"), color = Color(0xFFFFB020), fontSize = 12.sp)
}

/** Samsung's own effects run after CINEMAX and change its sound: say so, with a shortcut. */
@Composable
private fun MakerEffectsWarning(state: AppState, onOpenSoundSettings: () -> Unit) {
    if (!state.enabled || state.makerEffects.isEmpty()) return
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(Color(0xFF1E1A10))
            .border(1.dp, Color(0x99FFB020), RoundedCornerShape(16.dp)).padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text("${state.makerEffects.joinToString(" + ")} ${if (state.makerEffects.size == 1) "is" else "are"} changing CINEMAX's sound",
            color = Text1, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
        Text("Samsung processes the sound again after CINEMAX (Dolby adds its own 3D effect and automatic EQ), so it won't sound like your laptop. Turn them off in Sound quality and effects.",
            color = Text2, fontSize = 12.sp, lineHeight = 16.sp)
        Box(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).border(1.dp, Color(0x99FFB020), RoundedCornerShape(12.dp))
                .clickable { onOpenSoundSettings() }.padding(vertical = 11.dp),
            contentAlignment = Alignment.Center,
        ) { Text("Open Sound quality and effects", color = Text1, fontSize = 13.sp, fontWeight = FontWeight.SemiBold) }
    }
}

/** Full-engine state on Home: start button when it isn't running, live facts when it is. */
@Composable
private fun EngineBanner(state: AppState, onStartFullEngine: () -> Unit) {
    if (!state.enabled || state.engineMode != EngineMode.FULL) return
    val f = state.full
    if (state.fullRunning) {
        val st = f.stats
        val parts = mutableListOf<String>()
        if (st != null) {
            parts += "+${st.addedDelayMs} ms delay"
            if (st.playing && state.smartTone) parts += "Smart tone bass %+.1f / treble %+.1f dB".format(st.smartBassDb, st.smartTrebleDb)
            if (st.processedApps.isNotEmpty()) parts += "processing ${st.processedApps.joinToString()}"
            if (st.unprocessedApps.isNotEmpty()) parts += "${st.unprocessedApps.joinToString()} blocks CINEMAX"
        }
        if (parts.isNotEmpty()) Text(parts.joinToString(" · "), color = Text3, fontSize = 12.sp, lineHeight = 16.sp)
        if (!state.muteSetupDone)
            Text("One-time USB setup missing: the original sound can't be muted yet, so you may hear both.",
                color = Color(0xFFFFB020), fontSize = 12.sp, lineHeight = 16.sp)
        return
    }
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(Color(0xFF1C1512))
            .border(1.dp, Accent.copy(alpha = 0.5f), RoundedCornerShape(16.dp)).padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text(if (f.status == FullEngineState.Status.STARTING) "Starting the CINEMAX engine…" else "The CINEMAX engine is not running",
            color = Text1, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
        Text(f.message?.let { "$it. " }.orEmpty() +
            "Until it runs you hear basic Android effects. Android asks you to allow recording or casting each time it starts; CINEMAX only uses the sound, never the screen.",
            color = Text2, fontSize = 12.sp, lineHeight = 16.sp)
        if (f.status != FullEngineState.Status.STARTING)
            Box(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Accent).clickable { onStartFullEngine() }.padding(vertical = 12.dp),
                contentAlignment = Alignment.Center,
            ) { Text("Start CINEMAX engine", color = Color.Black, fontSize = 14.sp, fontWeight = FontWeight.SemiBold) }
    }
}

@Composable
private fun ModeSwitch(state: AppState) {
    Column {
        Row(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Surface).border(1.dp, Line, RoundedCornerShape(14.dp)).padding(4.dp),
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            listOf(ListeningMode.MUSIC to "Music", ListeningMode.MOVIES to "Movies", ListeningMode.AUTO to "Auto").forEach { (m, label) ->
                val active = state.mode == m
                val bg by animateColorAsState(if (active) AccentSoft else Color.Transparent, tween(180), label = "mode")
                Box(
                    Modifier.weight(1f).clip(RoundedCornerShape(10.dp)).background(bg)
                        .then(if (active) Modifier.border(1.dp, Accent.copy(alpha = 0.5f), RoundedCornerShape(10.dp)) else Modifier)
                        .clickable(enabled = state.enabled) { Cinemax.setMode(m) }.padding(vertical = 11.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(label, color = if (active) Text1 else Text2, fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
                }
            }
        }
        val hint = when (state.mode) {
            ListeningMode.AUTO -> when (state.detected) {
                Detected.MOVIE -> "Auto · the playing app reports a film / video"
                Detected.MUSIC -> "Auto · the playing app reports music"
                Detected.UNKNOWN -> "Auto · waiting for something to play"
            }
            ListeningMode.MUSIC -> "Music mode · ${state.musicPreset.title}"
            ListeningMode.MOVIES -> "Movies mode · ${state.moviePreset.title}"
            ListeningMode.MANUAL -> "Manual preset (tap a mode to switch back)"
        }
        Text(hint, color = if (state.mode == ListeningMode.AUTO) Color(0xFFFFB080) else Text3, fontSize = 12.sp,
            modifier = Modifier.padding(top = 6.dp, start = 4.dp))
    }
}

@Composable
private fun SpectrumView(enabled: Boolean, onRequest: () -> Unit, hasPermission: () -> Boolean) {
    val bands by Cinemax.spectrum.collectAsState()
    Box(Modifier.fillMaxWidth().height(120.dp).clip(RoundedCornerShape(12.dp)).background(Color(0xFF0C0C0E)), contentAlignment = Alignment.Center) {
        if (bands.isEmpty()) {
            if (!hasPermission())
                Text("Tap to show the live spectrum\n(needs audio permission, nothing is recorded)", color = Text2, fontSize = 12.sp,
                    textAlign = TextAlign.Center, modifier = Modifier.clickable { onRequest() }.padding(12.dp))
            else
                Text(if (enabled) "Play something to see the live spectrum" else "Turn CINEMAX on", color = Text3, fontSize = 12.sp)
        } else {
            Canvas(Modifier.fillMaxSize().padding(horizontal = 8.dp, vertical = 10.dp)) {
                val n = bands.size
                val gap = 3.dp.toPx()
                val w = (size.width - gap * (n - 1)) / n
                for (i in 0 until n) {
                    val h = (bands[i] * size.height).coerceAtLeast(2f)
                    drawRoundRect(
                        brush = Brush.verticalGradient(listOf(Color(0xFFEDEDED), Color(0x33FFFFFF)), startY = size.height - h, endY = size.height),
                        topLeft = Offset(i * (w + gap), size.height - h), size = Size(w, h), cornerRadius = CornerRadius(2f, 2f),
                    )
                }
            }
        }
    }
}

@Composable
private fun PresetTile(id: PresetId, active: Boolean, modifier: Modifier) {
    val border by animateColorAsState(if (active) Accent else Line, tween(180), label = "tile")
    Column(
        modifier.clip(RoundedCornerShape(16.dp)).background(if (active) Color(0xFF1C1512) else Surface)
            .border(BorderStroke(1.dp, border), RoundedCornerShape(16.dp))
            .clickable { Cinemax.applyPreset(id) }.padding(14.dp),
    ) {
        Text(id.title, color = Text1, fontSize = 13.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 2.sp, maxLines = 1)
        Spacer(Modifier.height(4.dp))
        Text(id.tagline, color = Text2, fontSize = 11.sp, lineHeight = 14.sp, maxLines = 3)
        Spacer(Modifier.height(6.dp))
        Text(if (active) "ACTIVE" else "SELECT", color = if (active) Accent else Text3, fontSize = 10.sp, letterSpacing = 2.sp)
    }
}

// ---------------------------------------------------------------------------- Equalizer
@Composable
private fun EqualizerScreen(state: AppState) {
    val p = state.params
    ScreenColumn {
        Title("10-BAND", "Equalizer")
        Card {
            ParamSlider("Preamp", p.preampDb, -12f..6f, { db(it) }) { v -> Cinemax.editParams { it.copy(preampDb = v) } }
        }
        Card {
            EQ_FREQS.forEachIndexed { i, f ->
                val label = if (f >= 1000) "${f / 1000} kHz" else "$f Hz"
                ParamSlider(label, p.eq[i], -12f..12f, { db(it) }) { v ->
                    Cinemax.editParams { it.copy(eq = it.eq.toMutableList().also { l -> l[i] = (v * 2).roundToInt() / 2f }) }
                }
            }
        }
        OutlineButton("Flatten all bands") { Cinemax.editParams { it.copy(eq = List(10) { 0f }, preampDb = 0f) } }
    }
}

// ---------------------------------------------------------------------------- Effects
@Composable
private fun EffectsScreen(state: AppState, onRequestCamera: () -> Unit, hasCameraPermission: () -> Boolean) {
    val p = state.params
    ScreenColumn {
        Title("PROCESSING", "Sound Effects")
        RealEarsCard(state, onRequestCamera, hasCameraPermission)
        Card {
            Eyebrow("SMART EQ")
            ToggleRow("Smart EQ", "Listens to each song or film and slowly evens out its tone toward the target below (at most +5 / −4 dB per band). CINEMAX engine only.",
                p.ieqOn) { v -> Cinemax.editParams { it.copy(ieqOn = v) } }
            if (p.ieqOn) {
                Segmented(listOf("Balanced", "Warm", "Detailed"), p.ieqProfile) { i -> Cinemax.editParams { it.copy(ieqProfile = i) } }
                ParamSlider("Amount", p.ieqAmount, 0f..1f, { pct(it) }) { v -> Cinemax.editParams { it.copy(ieqAmount = v) } }
            }
        }
        Card {
            Eyebrow("BASS")
            ParamSlider("Bass boost", p.bassDb, 0f..12f, { db(it) }) { v -> Cinemax.editParams { it.copy(bassDb = v) } }
            ParamSlider("Sub-bass", p.subBassDb, -6f..9f, { db(it) }) { v -> Cinemax.editParams { it.copy(subBassDb = v) } }
            Note("Bass protection limits the low end automatically so speakers don't distort.")
        }
        Card {
            Eyebrow("DIALOGUE")
            ParamSlider("Voice clarity", p.dialogue, 0f..1f, { pct(it) }) { v -> Cinemax.editParams { it.copy(dialogue = v) } }
        }
        Card {
            Eyebrow("SURROUND & SPACE")
            ParamSlider("Virtual surround", p.surround, 0f..1f, { pct(it) }) { v -> Cinemax.editParams { it.copy(surround = v) } }
            ToggleRow("Atmos-style 7.1.4", "Splits the sound into 11 virtual speakers: voices on a real centre, ambience around and above you, " +
                "film surround decoded. CINEMAX engine; Android's effects use their own surround.", p.immersive) { v -> Cinemax.editParams { it.copy(immersive = v) } }
            ParamSlider("Room ambience", p.room, 0f..1f, { pct(it) }) { v -> Cinemax.editParams { it.copy(room = v) } }
            ParamSlider("Venue", p.venue, 0f..1f, { v ->
                val name = if (v < 0.34f) "Theatre" else if (v < 0.67f) "Opera house" else "Concert hall"
                "$name · %.1f s".format(0.45f + 1.65f * v)
            }) { v -> Cinemax.editParams { it.copy(venue = v) } }
            Note("Virtual surround works on headphones and earbuds (wired, Bluetooth, USB). The phone speaker gets the room ambience and EQ only; a TV or soundbar gets surround sized to your room.")
        }
        Card {
            Eyebrow("DYNAMICS")
            ParamSlider("Compression", p.compression, 0f..1f, { pct(it) }) { v -> Cinemax.editParams { it.copy(compression = v) } }
            ToggleRow("Lift quiet parts", "Raises soft dialogue and quiet scenes.", p.loudness) { v -> Cinemax.editParams { it.copy(loudness = v) } }
            ToggleRow("Volume-aware bass", "Keeps the sound full at low volume: adds back the bass (up to +9 dB) and top your ears lose when it's quiet. Nothing near full volume.",
                p.volumeAware) { v -> Cinemax.editParams { it.copy(volumeAware = v) } }
            ParamSlider("Limiter ceiling", p.ceilingDb, -6f..-0.1f, { "%.1f dBFS".format(it) }) { v -> Cinemax.editParams { it.copy(ceilingDb = v) } }
        }
    }
}

// ---------------------------------------------------------------------------- Settings
@Composable
private fun SettingsScreen(state: AppState, onStartFullEngine: () -> Unit, onOpenBatterySettings: () -> Unit, onOpenSoundSettings: () -> Unit) {
    ScreenColumn {
        Title("PREFERENCES", "Settings")
        Card {
            Eyebrow("SOUND ENGINE")
            EngineChoice(state.engineMode) {
                Cinemax.setEngineMode(it)
                if (it == EngineMode.FULL && state.enabled) onStartFullEngine()
            }
            if (state.engineMode == EngineMode.FULL) {
                Note("The same CINEMAX engine as the laptop: 3D surround with height and real ears, concert-hall reverb, bass enhancer, " +
                    "Smart tone, Smart EQ and loudness control. Android lets CINEMAX hear other apps through its screen-capture " +
                    "permission (audio only; nothing is recorded or stored), then mutes the original. " +
                    "Apps that block capture (Spotify, Netflix, Prime Video and others) play unprocessed.")
                ToggleRow("Smart tone", "Automatic bass & treble for each song or film (at most +5 / -4 dB).", state.smartTone) { Cinemax.setSmartTone(it) }
                ToggleRow("CINEMAX engine for videos",
                    "Android's capture adds about 0.3 s of delay, so voices can lag behind lips. Turn off to play videos (YouTube, video players) through Android's effects with no delay; music keeps the CINEMAX engine.",
                    state.videosUseFullEngine) { Cinemax.setVideosUseFullEngine(it) }
                val f = state.full
                val st = f.stats
                InfoRow("Status", when (f.status) {
                    FullEngineState.Status.RUNNING -> "running"
                    FullEngineState.Status.STARTING -> "starting…"
                    FullEngineState.Status.NEEDS_CONSENT -> "waiting for your OK"
                    FullEngineState.Status.FAILED -> "failed: ${f.message}"
                    FullEngineState.Status.OFF -> if (state.enabled) "off" else "CINEMAX is off"
                })
                if (st != null && state.fullRunning) {
                    InfoRow("Added delay", "about ${st.addedDelayMs} ms")
                    InfoRow("Processor load", "%.1f %%".format(st.cpuPercent))
                    InfoRow("Processing", st.processedApps.joinToString().ifEmpty { "nothing playing" })
                    if (st.unprocessedApps.isNotEmpty()) InfoRow("Blocks CINEMAX", st.unprocessedApps.joinToString())
                }
                InfoRow("Mute originals", if (state.muteSetupDone) "ready" else "needs one-time USB setup")
                if (!state.muteSetupDone)
                    Note("Connect the phone to a PC with USB debugging and run:\nadb shell pm grant app.cinemax.mobile android.permission.DUMP")
                if (state.enabled && !state.fullRunning && f.status != FullEngineState.Status.STARTING)
                    OutlineButton("Start CINEMAX engine", onStartFullEngine)
            } else {
                Note("Android's own equaliser, virtualizer and reverb on the phone's output. No capture prompt and no added delay, but a simpler, flatter sound than the laptop.")
            }
        }
        Card {
            Eyebrow("LISTENING MODES")
            Text("Music mode uses", color = Text2, fontSize = 13.sp)
            Choice(listOf(PresetId.CONCERT_HALL, PresetId.MUSIC), state.musicPreset) { Cinemax.setModePresets(music = it) }
            Spacer(Modifier.height(8.dp))
            Text("Movies mode uses", color = Text2, fontSize = 13.sp)
            Choice(listOf(PresetId.CINEMA, PresetId.IMAX, PresetId.DOLBY_STYLE, PresetId.THEATRE), state.moviePreset) { Cinemax.setModePresets(movie = it) }
            Note("Auto follows what the playing app reports to Android: YouTube, Netflix and video players say \"movie\"; Spotify and YouTube Music say \"music\".")
        }
        Card {
            Eyebrow("BACKGROUND")
            ToggleRow("Start when the phone starts", "Turns CINEMAX back on after a restart.", state.startOnBoot) { Cinemax.setStartOnBoot(it) }
            ToggleRow("Close when nothing plays",
                "Closes CINEMAX by itself when nothing plays and you don't touch its screen, so it doesn't run for nothing. A paused film gets 15 minutes. Opening CINEMAX starts it again.",
                state.autoClose) { Cinemax.setAutoClose(it) }
            if (state.autoClose) {
                Text("Close after", color = Text2, fontSize = 13.sp)
                MinutesChoice(state.autoCloseMinutes) { Cinemax.setAutoCloseMinutes(it) }
            }
            OutlineButton("Allow background activity (Battery → Unrestricted)", onOpenBatterySettings)
            Note("Samsung may stop background apps to save battery. Unrestricted keeps the sound processing running.")
        }
        if (!state.fullRunning) Card {
            Eyebrow("${if (state.engineMode == EngineMode.FULL) "BASIC EFFECTS (FALLBACK)" else "ANDROID EFFECTS"} · ${state.outputName.uppercase()}")
            val e = state.engine
            StatusRow("Equaliser, bass & dynamics", e.dynamics, state.enabled)
            if (e.virtualizer && state.enabled && !state.surroundActive)
                StatusRow("Virtual surround", ok = false, enabled = true, detail = "headphones & earbuds only")
            else
                StatusRow("Virtual surround", e.virtualizer, state.enabled)
            StatusRow("Room / hall ambience", e.reverb, state.enabled)
            e.notes.forEach { Note(it) }
        }
        Card {
            Eyebrow("SAMSUNG SOUND")
            Note("Your phone also has Samsung's own Dolby Atmos and Equaliser (Settings → Sounds and vibration → Sound quality and effects). They process the same sound, and Dolby's automatic mode can even out what CINEMAX adds. For the full CINEMAX sound, turn Dolby Atmos and the Equaliser off.")
            OutlineButton("Open sound settings", onOpenSoundSettings)
        }
        OutlineButton("Reset this device's sound") { Cinemax.resetDevice() }
        Card {
            Eyebrow("ABOUT")
            Row(verticalAlignment = Alignment.CenterVertically) {
                LogoMark(Modifier.size(40.dp))
                Spacer(Modifier.width(12.dp))
                Column {
                    Text("CINEMAX Mobile 1.0", color = Text1, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
                    Row {
                        Text("Created by ", color = Text2, fontSize = 13.sp)
                        Text("Nani Chowdary", color = Accent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                    }
                }
            }
            Note("Cinematic sound engine for phone and laptop - designed and built by Nani Chowdary.")
        }
        Text("CINEMAX Mobile 1.0 · not affiliated with IMAX or Dolby", color = Text3, fontSize = 11.sp, modifier = Modifier.padding(top = 4.dp))
    }
}

// ---------------------------------------------------------------------------- 3D ears
@Composable
private fun RealEarsCard(state: AppState, onRequestCamera: () -> Unit, hasCameraPermission: () -> Boolean) {
    var testOpen by rememberSaveable { mutableStateOf(false) }
    val active = state.earProfiles.firstOrNull { it.key == state.activeEarKey }
    val measured = state.earProfiles.any { it.key != "model" }
    Card {
        Eyebrow("3D EARS · HEADPHONES")
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(active?.name ?: "Head model", color = Text1, fontSize = 15.sp, fontWeight = FontWeight.Medium, modifier = Modifier.weight(1f))
            Tag(if (state.activeEarKey == "model") "HEAD MODEL" else "REAL EARS", state.activeEarKey != "model")
        }
        active?.let { Note(it.info) }
        Note("Real ears are measured recordings of how sound reaches a real ear from every direction, so the 3D sound feels outside your head. " +
            "They work with the CINEMAX engine on headphones and earbuds.")
        if (measured) OutlineButton(if (testOpen) "Close the ear test" else "Find my ears (1 minute)") {
            testOpen = !testOpen
            if (!testOpen) Cinemax.stopEarTest()
        }
        if (testOpen) EarTestPanel(state)
        ToggleRow("Head tracking (front camera)",
            "Follows your head with the front camera so the sound stays on the phone screen when you turn - like AirPods. Runs only while sound plays on headphones; nothing is recorded or saved.",
            state.headTracking) { on -> if (on && !hasCameraPermission()) onRequestCamera() else Cinemax.setHeadTracking(on) }
        if (state.headTracking) {
            HeadStatusLine(state.head)
            ToggleRow("Turns the wrong way? Flip it", "If the sound follows your head instead of staying on the screen, switch this on.",
                state.headTrackingFlip) { Cinemax.setHeadTrackingFlip(it) }
        }
    }
}

private val EarSteps = listOf("In front", "Right", "Behind", "Left", "Above")

@Composable
private fun EarTestPanel(state: AppState) {
    val test = state.earTest
    var step by remember { mutableIntStateOf(-1) }
    LaunchedEffect(test) {
        step = -1
        if (test == null) return@LaunchedEffect
        while (true) {
            val s = ((android.os.SystemClock.elapsedRealtime() - test.startedMs) / EarTest.STEP_MS).toInt()
            if (s >= EarTest.STEPS) { step = -1; break }
            step = s
            delay(60)
        }
    }
    DisposableEffect(Unit) { onDispose { Cinemax.stopEarTest() } }

    Text("Put on your headphones, close your eyes and play each one. Choose the one where the sound is outside your head and lands " +
        "in the right place: in front, right, behind, left and above. Front and behind are the hardest - that's what to listen for.",
        color = Text2, fontSize = 12.sp, lineHeight = 16.sp)
    Box(Modifier.fillMaxWidth().padding(vertical = 4.dp), contentAlignment = Alignment.Center) {
        HeadMap(step, Modifier.size(150.dp))
    }
    Text(if (step >= 0) "Sound: ${EarSteps[step]}" else " ", color = Accent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold,
        textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth())
    state.earProfiles.forEach { prof ->
        val playing = test?.key == prof.key
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(vertical = 2.dp)) {
            SmallButton(if (playing) "Playing…" else "Play", primary = playing) { Cinemax.playEarTest(prof.key) }
            Spacer(Modifier.width(10.dp))
            Text(prof.name, color = Text1, fontSize = 13.sp, modifier = Modifier.weight(1f))
            if (prof.key == state.activeEarKey) Tag("IN USE", true)
            else SmallButton("Use these", primary = false) { Cinemax.setEarProfile(prof.key) }
        }
    }
}

/** Top view of a head; the spot where the test sound is lights up. */
@Composable
private fun HeadMap(step: Int, modifier: Modifier) {
    Canvas(modifier) {
        val s = size.minDimension
        val c = Offset(size.width / 2, size.height / 2)
        val line = Stroke(width = s * 0.012f)
        drawCircle(Color(0x33FFFFFF), radius = s * 0.17f, center = c, style = line)
        drawCircle(Color(0x14FFFFFF), radius = s * 0.17f, center = c)
        val nose = androidx.compose.ui.graphics.Path().apply {
            moveTo(c.x - s * 0.04f, c.y - s * 0.16f); lineTo(c.x, c.y - s * 0.21f); lineTo(c.x + s * 0.04f, c.y - s * 0.16f)
        }
        drawPath(nose, Color(0x66FFFFFF), style = line)
        drawOval(Color(0x55FFFFFF), topLeft = Offset(c.x - s * 0.195f, c.y - s * 0.05f), size = Size(s * 0.05f, s * 0.1f))
        drawOval(Color(0x55FFFFFF), topLeft = Offset(c.x + s * 0.145f, c.y - s * 0.05f), size = Size(s * 0.05f, s * 0.1f))
        val spots = listOf(Offset(c.x, c.y - s * 0.36f), Offset(c.x + s * 0.36f, c.y), Offset(c.x, c.y + s * 0.36f),
            Offset(c.x - s * 0.36f, c.y), c)
        spots.forEachIndexed { i, o ->
            val on = i == step
            val r = if (i == 4) s * 0.05f else s * 0.06f
            drawCircle(if (on) Accent else Color(0x33FFFFFF), radius = r, center = o)
            if (on) drawCircle(Accent.copy(alpha = 0.45f), radius = s * 0.11f, center = o, style = Stroke(width = s * 0.015f))
        }
    }
}

@Composable
private fun HeadStatusLine(h: HeadStatus) {
    val warn = Color(0xFFFFB020)
    val (text, color) = when (h.status) {
        "tracking" -> "Tracking · turned ${abs(h.yaw).roundToInt()}° ${if (h.yaw >= 0) "right" else "left"}" to Accent
        "starting" -> "Starting the camera…" to Text3
        "no-face" -> "Looking for your face…" to warn
        "off" -> h.message.ifEmpty { "Waiting" } to Text3
        else -> h.message.ifEmpty { "Camera unavailable" } to warn
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(8.dp).clip(CircleShape).background(color))
        Spacer(Modifier.width(8.dp))
        Text(text, color = Text2, fontSize = 12.sp, lineHeight = 16.sp)
    }
}

// ---------------------------------------------------------------------------- DOLBY-STYLE (Home)
@Composable
private fun DolbyCard(state: AppState) {
    val p = state.params
    val on = p.preset == PresetId.DOLBY_STYLE || (p.preset == PresetId.CUSTOM && p.base == PresetId.DOLBY_STYLE)
    val shape = RoundedCornerShape(16.dp)
    Row(
        Modifier.fillMaxWidth().clip(shape).background(if (on) Color(0xFF1C1512) else Surface)
            .border(1.dp, if (on) Accent.copy(alpha = 0.6f) else Line, shape).padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text("DOLBY-STYLE", color = Text1, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 2.sp)
            Text(if (on) "On · 7.1.4 Atmos-style: voices on a real centre, 4 overhead speakers, volume-aware bass, Smart EQ"
                 else "Atmos-style 7.1.4 sound for headphones and TVs, with Smart EQ",
                color = Text2, fontSize = 12.sp, lineHeight = 16.sp)
        }
        Spacer(Modifier.width(10.dp))
        Switch(on, { if (it) Cinemax.applyPreset(PresetId.DOLBY_STYLE) else Cinemax.setMode(ListeningMode.AUTO) },
            enabled = state.enabled,
            colors = SwitchDefaults.colors(checkedTrackColor = Accent, checkedThumbColor = Color.White))
    }
}

// ---------------------------------------------------------------------------- TV
@Composable
private fun TvScreen(state: AppState) {
    val tv = state.tv
    val ctx = LocalContext.current
    val p = state.params
    val dolbyOn = p.preset == PresetId.DOLBY_STYLE || (p.preset == PresetId.CUSTOM && p.base == PresetId.DOLBY_STYLE)
    ScreenColumn {
        Title("TV & SOUNDBAR", "TV")
        Card {
            Eyebrow("PLAYING ON")
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(state.outputName, color = Text1, fontSize = 20.sp, fontWeight = FontWeight.Light, modifier = Modifier.weight(1f))
                Tag(if (tv.isTv) "TV" else state.outputKind.label.uppercase(), tv.isTv)
            }
            Note(when {
                tv.isTv -> "CINEMAX tunes the sound for this TV: speaker tuning, surround sized to your room and lip-sync below."
                state.tvPossible -> "If this is your TV or soundbar, switch on \"This is a TV or soundbar\" below."
                else -> "Connect your TV over Bluetooth (steps below) and it shows up here with its sound settings."
            })
        }
        if (state.tvPossible || tv.isTv) TvSoundCard(state)
        else Card {
            Eyebrow("TV SOUND · WHEN YOUR TV IS CONNECTED")
            Note("• TV speakers / Soundbar / Receiver tuning: fuller bass and clearer voices from thin TV speakers, gentle for a soundbar, untouched for a receiver.\n" +
                "• Screen size and seat distance: the surround is sized to your room instead of to headphones.\n" +
                "• Lip-sync: delay the sound when the TV picture runs late.\n" +
                "• A TV is never treated as headphones, even if it says it is over Bluetooth.")
        }
        Card {
            Eyebrow("DOLBY-STYLE ON THE TV")
            Note("Bluetooth carries plain stereo, so real Dolby can't travel from a phone to a TV. DOLBY-STYLE is CINEMAX's own Atmos-like " +
                "processing: a big room with height, rich bass and clear voices, made before the sound leaves the phone.")
            if (dolbyOn) Text("DOLBY-STYLE is on", color = Accent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            else OutlineButton("Use DOLBY-STYLE") { Cinemax.applyPreset(PresetId.DOLBY_STYLE) }
        }
        Card {
            Eyebrow("CONNECT YOUR TV")
            Text("1. On the TV, turn on receiving sound from phones over Bluetooth (Samsung: Sound Mirroring or Bluetooth audio; " +
                "LG: Bluetooth sound sharing / Sound Share). Many Android and Google TVs can't receive.", color = Text2, fontSize = 13.sp, lineHeight = 18.sp)
            Text("2. Pair the TV in the phone's Bluetooth settings.", color = Text2, fontSize = 13.sp, lineHeight = 18.sp)
            OutlineButton("Open Bluetooth settings") {
                runCatching { ctx.startActivity(android.content.Intent(android.provider.Settings.ACTION_BLUETOOTH_SETTINGS)
                    .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)) }
            }
            Text("3. Play something. The TV appears at the top; if CINEMAX doesn't recognise it by name, switch on \"This is a TV or soundbar\".",
                color = Text2, fontSize = 13.sp, lineHeight = 18.sp)
            Note("Delay: Bluetooth and the CINEMAX engine together add about half a second. Watching on the TV itself (cast / Smart View)? " +
                "Use Lip-sync if voices come early. Watching on the phone screen? Settings → \"CINEMAX engine for videos\" off removes CINEMAX's part.")
        }
    }
}

@Composable
private fun TvSoundCard(state: AppState) {
    val tv = state.tv
    Card {
        Eyebrow("TV SOUND · ${state.outputName.uppercase()}")
        ToggleRow("This is a TV or soundbar",
            "Tunes the sound for TV speakers and sizes the surround to your room. Some TVs call themselves \"headphones\" over Bluetooth - this makes CINEMAX treat them as speakers.",
            tv.isTv) { v -> Cinemax.setTvSetup { it.copy(isTv = v) } }
        if (tv.isTv) {
            Segmented(listOf("TV speakers", "Soundbar", "Receiver"), tv.speakers - 1) { i -> Cinemax.setTvSetup { it.copy(speakers = i + 1) } }
            ParamSlider("Screen size", tv.sizeInch, 24f..100f, { "${it.roundToInt()}\"" }) { v ->
                Cinemax.setTvSetup { it.copy(sizeInch = v.roundToInt().toFloat()) }
            }
            ParamSlider("Seat distance", tv.distanceM, 1f..6f, { "%.1f m".format(it) }) { v ->
                Cinemax.setTvSetup { it.copy(distanceM = (v * 10).roundToInt() / 10f) }
            }
            ParamSlider("Lip-sync", tv.lipSyncMs, 0f..300f, { if (it < 1f) "Off" else "+${it.roundToInt()} ms" }) { v ->
                Cinemax.setTvSetup { it.copy(lipSyncMs = ((v / 5).roundToInt() * 5).toFloat()) }
            }
            Note("Lip-sync delays the sound: use it when the TV picture runs late (screen mirroring, TV picture processing) and voices come before the lips move.")
            if (state.outputKind == OutputKind.BLUETOOTH)
                Note("Bluetooth carries plain stereo, so Dolby can't travel over it - the DOLBY-STYLE preset is how you get the Dolby-like sound on the TV. Keep the phone within a few metres of the TV.")
        }
    }
}

// ---------------------------------------------------------------------------- building blocks
@Composable
private fun ScreenColumn(content: @Composable () -> Unit) {
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(PaddingValues(horizontal = 18.dp, vertical = 16.dp)),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) { content() }
}

@Composable
private fun Card(content: @Composable () -> Unit) {
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(20.dp)).background(Surface).border(1.dp, Line, RoundedCornerShape(20.dp)).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) { content() }
}

@Composable private fun Eyebrow(text: String) = Text(text, color = Text3, fontSize = 11.sp, letterSpacing = 2.5.sp, fontWeight = FontWeight.Medium)
@Composable private fun Note(text: String) = Text(text, color = Text3, fontSize = 12.sp, lineHeight = 16.sp)

@Composable
private fun Title(eyebrow: String, title: String) {
    Column {
        Eyebrow(eyebrow)
        Text(title, color = Text1, fontSize = 30.sp, fontWeight = FontWeight.Light)
    }
}

@Composable
private fun ParamSlider(label: String, value: Float, range: ClosedFloatingPointRange<Float>, format: (Float) -> String, onChange: (Float) -> Unit) {
    Column {
        Row {
            Text(label, color = Text1, fontSize = 14.sp, modifier = Modifier.weight(1f))
            Text(format(value), color = Text1, fontSize = 14.sp, fontWeight = FontWeight.Medium)
        }
        Slider(
            value = value.coerceIn(range.start, range.endInclusive), onValueChange = onChange, valueRange = range,
            colors = SliderDefaults.colors(thumbColor = Color.White, activeTrackColor = Color(0xFFEDEDED), inactiveTrackColor = Color(0x26FFFFFF)),
        )
    }
}

@Composable
private fun ToggleRow(title: String, sub: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(vertical = 4.dp)) {
        Column(Modifier.weight(1f)) {
            Text(title, color = Text1, fontSize = 14.sp)
            Text(sub, color = Text3, fontSize = 12.sp)
        }
        Switch(checked, onChange, colors = SwitchDefaults.colors(checkedTrackColor = Accent, checkedThumbColor = Color.White))
    }
}

@Composable
private fun Choice(options: List<PresetId>, selected: PresetId, onSelect: (PresetId) -> Unit) {
    Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Surface2).padding(3.dp), horizontalArrangement = Arrangement.spacedBy(3.dp)) {
        options.forEach { id ->
            val active = id == selected
            Box(
                Modifier.weight(1f).clip(RoundedCornerShape(9.dp)).background(if (active) AccentSoft else Color.Transparent)
                    .clickable { onSelect(id) }.padding(vertical = 9.dp),
                contentAlignment = Alignment.Center,
            ) { Text(id.title, color = if (active) Text1 else Text2, fontSize = if (options.size > 3) 10.sp else 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1) }
        }
    }
}

@Composable
private fun StatusRow(label: String, ok: Boolean, enabled: Boolean, detail: String? = null) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        val dot = when { !enabled -> Text3; ok -> Color(0xFF4CD08A); detail != null -> Color(0xFFFFB020); else -> Color(0xFFFF5A4A) }
        Box(Modifier.size(8.dp).clip(CircleShape).background(dot))
        Spacer(Modifier.width(10.dp))
        Text(label, color = Text1, fontSize = 13.sp, modifier = Modifier.weight(1f))
        Text(detail ?: if (!enabled) "off" else if (ok) "active on all apps" else "not available", color = Text2, fontSize = 12.sp)
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(verticalAlignment = Alignment.Top) {
        Text(label, color = Text2, fontSize = 13.sp, modifier = Modifier.width(120.dp))
        Text(value, color = Text1, fontSize = 13.sp, modifier = Modifier.weight(1f))
    }
}

@Composable
private fun MinutesChoice(selected: Int, onSelect: (Int) -> Unit) {
    Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Surface2).padding(3.dp), horizontalArrangement = Arrangement.spacedBy(3.dp)) {
        listOf(1, 2, 5, 10).forEach { m ->
            val active = m == selected
            Box(
                Modifier.weight(1f).clip(RoundedCornerShape(10.dp)).background(if (active) AccentSoft else Color.Transparent)
                    .then(if (active) Modifier.border(1.dp, Accent.copy(alpha = 0.5f), RoundedCornerShape(10.dp)) else Modifier)
                    .clickable { onSelect(m) }.padding(vertical = 10.dp),
                contentAlignment = Alignment.Center,
            ) { Text("$m min", color = if (active) Text1 else Text2, fontSize = 13.sp, fontWeight = FontWeight.SemiBold) }
        }
    }
}

@Composable
private fun EngineChoice(selected: EngineMode, onSelect: (EngineMode) -> Unit) {
    Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Surface2).padding(3.dp), horizontalArrangement = Arrangement.spacedBy(3.dp)) {
        listOf(EngineMode.FULL to "CINEMAX engine", EngineMode.BUILTIN to "Android effects").forEach { (m, label) ->
            val active = m == selected
            Box(
                Modifier.weight(1f).clip(RoundedCornerShape(10.dp)).background(if (active) AccentSoft else Color.Transparent)
                    .then(if (active) Modifier.border(1.dp, Accent.copy(alpha = 0.5f), RoundedCornerShape(10.dp)) else Modifier)
                    .clickable { onSelect(m) }.padding(vertical = 10.dp),
                contentAlignment = Alignment.Center,
            ) { Text(label, color = if (active) Text1 else Text2, fontSize = 13.sp, fontWeight = FontWeight.SemiBold) }
        }
    }
}

@Composable
private fun OutlineButton(text: String, onClick: () -> Unit) {
    Box(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).border(1.dp, Line, RoundedCornerShape(12.dp))
            .clickable { onClick() }.padding(vertical = 12.dp, horizontal = 14.dp),
        contentAlignment = Alignment.Center,
    ) { Text(text, color = Text1, fontSize = 13.sp, fontWeight = FontWeight.Medium, textAlign = TextAlign.Center) }
}

@Composable
private fun PowerSwitch(on: Boolean, onChange: (Boolean) -> Unit) {
    val offset by animateDpAsState(if (on) 30.dp else 0.dp, tween(200), label = "thumb")
    val track by animateColorAsState(if (on) Accent else Color(0xFF2A2A2E), tween(200), label = "track")
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(if (on) "ON" else "OFF", color = if (on) Text1 else Text3, fontSize = 13.sp, fontWeight = FontWeight.Bold, letterSpacing = 2.sp)
        Spacer(Modifier.width(10.dp))
        Box(Modifier.width(62.dp).height(32.dp).clip(RoundedCornerShape(16.dp)).background(track).clickable { onChange(!on) }.padding(3.dp)) {
            Box(Modifier.offset(x = offset).size(26.dp).clip(CircleShape).background(Color.White))
        }
    }
}

@Composable
private fun LogoMark(modifier: Modifier) {
    Canvas(modifier) {
        val r = size.minDimension * 0.36f
        val c = Offset(size.width / 2, size.height / 2)
        drawArc(Color.White, startAngle = 40f, sweepAngle = 280f, useCenter = false,
            topLeft = Offset(c.x - r, c.y - r), size = Size(2 * r, 2 * r), style = Stroke(width = size.minDimension * 0.14f, cap = StrokeCap.Round))
        drawCircle(Accent, radius = size.minDimension * 0.075f, center = Offset(c.x + r, c.y))
    }
}

@Composable
private fun NavIcon(index: Int, selected: Boolean) {
    val color = if (selected) Accent else Text3
    Canvas(Modifier.size(22.dp)) {
        val s = size.minDimension
        val st = Stroke(width = s * 0.09f, cap = StrokeCap.Round)
        when (index) {
            0 -> { // home: speaker-ish arcs
                drawCircle(color, radius = s * 0.12f, center = Offset(s * 0.5f, s * 0.5f))
                drawArc(color, -50f, 100f, false, Offset(s * 0.18f, s * 0.18f), Size(s * 0.64f, s * 0.64f), style = st)
                drawArc(color, 130f, 100f, false, Offset(s * 0.18f, s * 0.18f), Size(s * 0.64f, s * 0.64f), style = st)
            }
            1 -> for (k in 0..2) { // equalizer sliders
                val x = s * (0.25f + 0.25f * k)
                drawLine(color, Offset(x, s * 0.15f), Offset(x, s * 0.85f), st.width, StrokeCap.Round)
                drawCircle(color, s * 0.09f, Offset(x, s * (0.35f + 0.2f * ((k + 1) % 3))))
            }
            2 -> { // effects: wave
                val path = androidx.compose.ui.graphics.Path().apply {
                    moveTo(s * 0.1f, s * 0.5f)
                    cubicTo(s * 0.3f, s * 0.1f, s * 0.4f, s * 0.9f, s * 0.6f, s * 0.5f)
                    cubicTo(s * 0.7f, s * 0.3f, s * 0.8f, s * 0.3f, s * 0.9f, s * 0.5f)
                }
                drawPath(path, color, style = st)
            }
            3 -> { // TV: a screen on a stand
                drawRoundRect(color, topLeft = Offset(s * 0.1f, s * 0.18f), size = Size(s * 0.8f, s * 0.52f),
                    cornerRadius = CornerRadius(s * 0.08f, s * 0.08f), style = st)
                drawLine(color, Offset(s * 0.35f, s * 0.86f), Offset(s * 0.65f, s * 0.86f), st.width, StrokeCap.Round)
                drawLine(color, Offset(s * 0.5f, s * 0.7f), Offset(s * 0.5f, s * 0.86f), st.width, StrokeCap.Round)
            }
            else -> { // settings: gear-ish
                drawCircle(color, radius = s * 0.3f, style = st)
                drawCircle(color, radius = s * 0.1f)
            }
        }
    }
}

@Composable
private fun Segmented(options: List<String>, selected: Int, onSelect: (Int) -> Unit) {
    Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Surface2).padding(3.dp), horizontalArrangement = Arrangement.spacedBy(3.dp)) {
        options.forEachIndexed { i, label ->
            val active = i == selected
            Box(
                Modifier.weight(1f).clip(RoundedCornerShape(10.dp)).background(if (active) AccentSoft else Color.Transparent)
                    .then(if (active) Modifier.border(1.dp, Accent.copy(alpha = 0.5f), RoundedCornerShape(10.dp)) else Modifier)
                    .clickable { onSelect(i) }.padding(vertical = 10.dp),
                contentAlignment = Alignment.Center,
            ) { Text(label, color = if (active) Text1 else Text2, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1) }
        }
    }
}

@Composable
private fun SmallButton(text: String, primary: Boolean, onClick: () -> Unit) {
    Box(
        Modifier.clip(RoundedCornerShape(10.dp)).background(if (primary) Accent else Color.Transparent)
            .border(1.dp, if (primary) Accent else Line, RoundedCornerShape(10.dp))
            .clickable { onClick() }.padding(horizontal = 12.dp, vertical = 7.dp),
        contentAlignment = Alignment.Center,
    ) { Text(text, color = if (primary) Color.Black else Text1, fontSize = 12.sp, fontWeight = FontWeight.SemiBold) }
}

@Composable
private fun Tag(text: String, accent: Boolean) {
    Text(text, color = if (accent) Accent else Text3, fontSize = 10.sp, letterSpacing = 1.5.sp, fontWeight = FontWeight.SemiBold,
        modifier = Modifier.clip(RoundedCornerShape(6.dp)).background(if (accent) AccentSoft else Surface2).padding(horizontal = 7.dp, vertical = 3.dp))
}

private fun pct(v: Float) = "${(v * 100).roundToInt()}%"
private fun db(v: Float) = (if (v > 0.05f) "+" else if (v < -0.05f) "−" else "") + "%.1f dB".format(abs(v))
