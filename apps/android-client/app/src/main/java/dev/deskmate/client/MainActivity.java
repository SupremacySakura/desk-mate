package dev.deskmate.client;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.hardware.usb.UsbAccessory;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    private enum Page {
        HOME,
        SETTINGS,
        DIAGNOSTICS,
        DISPLAY
    }

    private static final String PREFS_NAME = "deskmate_prefs";
    private static final String KEY_SHOW_FPS = "deskMateShowFps";
    private static final String KEY_CODEC = "deskMateCodec";

    private static final String[] CODEC_VALUES = { "auto", "hevc", "h264" };
    private static final String[] CODEC_TITLES = { "自动", "HEVC", "H.264" };
    private static final String[] CODEC_SUBTITLES = { "优先 HEVC", "H.265", "AVC" };

    private static final class EngineSnapshot {
        String session = "idle";
        String failure = "";
        String profile = "";
        String codec = "";
        String resolution = "";
        String preferredCodec = "auto";
        long rxBytes;
        String surface = "none";
        long framesReceived;
        long framesDropped;
        long decoderRendered;
        long decoderDropped;
        long inputSent;
        long inputRejected;
        String error = "";
    }

    private StudioDrawables.Palette palette;

    private SurfaceView surfaceView;
    private LinearLayout shellContainer;
    private LinearLayout headerContainer;
    private View headerDivider;
    private ScrollView scrollView;
    private LinearLayout contentContainer;
    private View bottomDivider;
    private LinearLayout bottomContainer;

    private LinearLayout displayHudContainer;
    private TextView hudReturnBtn;
    private TextView hudToggleBtn;
    private LinearLayout hudMetricsPill;
    private TextView hudFpsValue;
    private TextView hudBitrateValue;
    private TextView hudCodecValue;
    private View hudResDivider;
    private TextView hudResValue;
    private View hudDropDivider;
    private TextView hudDropValue;

    private AccessoryLink accessoryLink;
    private long nativeHandle;
    private Surface surface;
    private ParcelFileDescriptor pendingDescriptor;
    private boolean started;
    private boolean touchCaptureActive;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private int[] pointerIds = new int[10];
    private float[] pointerXs = new float[10];
    private float[] pointerYs = new float[10];

    private SharedPreferences prefs;
    private boolean isPhone;
    private boolean compactLayout;
    private DeviceLayout.SafeInsets safeInsets = new DeviceLayout.SafeInsets(0, 0, 0, 0);

    private Page page = Page.HOME;
    private AccessoryLink.Snapshot link =
        new AccessoryLink.Snapshot(AccessoryLink.State.SEARCHING, "starting", -1);
    private EngineSnapshot engine;
    private String rawEngineJson = "{}";

    private boolean frameReady;
    private boolean enteredThisLink;
    private boolean controlsExpanded;
    private boolean showFps;
    private String preferredCodec = "auto";
    private boolean codecChangedWhileConnected;

    private int fps;
    private double bitrateMbps;
    private String activeCodec = "";
    private String activeResolution = "";
    private long droppedFrames;
    private long lastRendered;
    private long lastRxBytes;
    private long lastSampleAt;

    private String lastStructureKey = "";
    private WiredLinkSchematicView schematicView;
    private TextView topStatusTextView;
    private View topStatusDotView;
    private LinearLayout topStatusBadgeView;
    private TextView statusTitleView;
    private TextView statusDetailView;
    private TextView liveSpecValueView;
    private LinearLayout livePerfRowView;
    private TextView livePerfValueView;
    private TextView settingsCodecStatusView;
    private TextView settingsCodecHintView;
    private TextView whisperCodecView;
    private View whisperDotView;
    private final List<TextView> diagnosticValueViews = new ArrayList<>();
    private final List<LinearLayout> codecOptionCards = new ArrayList<>();
    private final List<TextView> codecOptionTitles = new ArrayList<>();
    private final List<TextView> codecOptionSubtitles = new ArrayList<>();

    private boolean hudStateInitialized;
    private boolean lastHudExpanded;
    private boolean lastHudShowFps;
    private int lastHudFps = -1;
    private float lastPreferredRefreshHz = -1.0f;
    private String lastHudBitrateText = "";
    private String lastHudCodec = "";
    private String lastHudRes = "";
    private long lastHudDrops = -1L;
    private String lastBadgeStateKey = "";
    private String lastSelectedCodecCard;

    private final Runnable refreshTick = new Runnable() {
        @Override
        public void run() {
            pollEngineState();
            final long delayMs =
                (page == Page.DISPLAY && frameReady && !showFps) ? 1000L : 250L;
            handler.postDelayed(this, delayMs);
        }
    };

    private final Runnable retryAccessory = new Runnable() {
        @Override
        public void run() {
            if (accessoryLink != null) {
                accessoryLink.openAccessory();
            }
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        palette = StudioDrawables.palette(this);
        prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        showFps = prefs.getBoolean(KEY_SHOW_FPS, false);
        preferredCodec = normalizeCodec(prefs.getString(KEY_CODEC, "auto"));
        isPhone = DeviceLayout.isPhoneDevice(this);
        compactLayout = DeviceLayout.isCompactLayout(this);

        final View root = findViewById(R.id.root);
        surfaceView = findViewById(R.id.video_surface);
        shellContainer = findViewById(R.id.shell_container);
        displayHudContainer = findViewById(R.id.display_hud);

        shellContainer.setBackgroundColor(palette.canvas);
        shellContainer.setClickable(true);
        shellContainer.setFocusable(true);
        initShellSkeleton();
        initDisplayHud();

        root.setOnApplyWindowInsetsListener((v, insets) -> {
            safeInsets = DeviceLayout.readSafeInsets(insets);
            applyChromeInsets();
            return insets;
        });

        surfaceView.getHolder().addCallback(this);
        surfaceView.setOnTouchListener((view, event) -> handleTouch(event));

        nativeHandle = NativeBridge.create();
        NativeBridge.setPreferredCodec(nativeHandle, preferredCodec);
        NativeBridge.setTouchEnabled(nativeHandle, false);

        accessoryLink = new AccessoryLink(this, new AccessoryLink.Listener() {
            @Override
            public void onStateChanged(AccessoryLink.Snapshot snapshot) {
                if (snapshot.state != AccessoryLink.State.CONNECTED &&
                    link.state == AccessoryLink.State.CONNECTED) {
                    resetSessionUiState();
                }
                link = snapshot;
                renderAll();
            }

            @Override
            public void onAccessory(ParcelFileDescriptor descriptor, UsbAccessory accessory) {
                pendingDescriptor = descriptor;
                startSession();
                renderAll();
            }

            @Override
            public void onDetached() {
                pendingDescriptor = null;
                stopSession();
                resetSessionUiState();
                renderAll();
                handler.removeCallbacks(retryAccessory);
                handler.postDelayed(retryAccessory, 500L);
            }
        });

        DeviceLayout.applyWindowPresentation(this, isPhone, false);
        renderAll();
        accessoryLink.start();
        handler.post(refreshTick);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (accessoryLink != null) {
            accessoryLink.openAccessory();
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        palette = StudioDrawables.palette(this);
        compactLayout = DeviceLayout.isCompactLayout(this);
        lastStructureKey = "";
        shellContainer.setBackgroundColor(palette.canvas);
        if (headerDivider != null) {
            headerDivider.setBackgroundColor(palette.hairline);
        }
        if (bottomDivider != null) {
            bottomDivider.setBackgroundColor(palette.hairline);
        }
        DeviceLayout.applyWindowPresentation(this, isPhone, page == Page.DISPLAY);
        renderAll();
        applyChromeInsets();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (page == Page.DIAGNOSTICS) {
            navigate(Page.SETTINGS);
            return;
        }
        if (page != Page.HOME) {
            navigate(Page.HOME);
            return;
        }
        super.onBackPressed();
    }

    private void initShellSkeleton() {
        shellContainer.removeAllViews();
        shellContainer.setOrientation(LinearLayout.VERTICAL);

        headerContainer = new LinearLayout(this);
        headerContainer.setOrientation(LinearLayout.HORIZONTAL);
        headerContainer.setGravity(Gravity.CENTER_VERTICAL);
        headerContainer.setBackgroundColor(palette.surface);
        shellContainer.addView(headerContainer, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        headerDivider = makeHairlineDivider(palette.hairline, 0.8f);
        shellContainer.addView(headerDivider);

        scrollView = new ScrollView(this);
        scrollView.setFillViewport(true);
        scrollView.setVerticalScrollBarEnabled(false);
        contentContainer = new LinearLayout(this);
        contentContainer.setOrientation(LinearLayout.VERTICAL);
        contentContainer.setGravity(Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        scrollView.addView(contentContainer, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        shellContainer.addView(scrollView, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        bottomDivider = makeHairlineDivider(palette.hairline, 0.8f);
        shellContainer.addView(bottomDivider);

        bottomContainer = new LinearLayout(this);
        bottomContainer.setOrientation(LinearLayout.HORIZONTAL);
        bottomContainer.setGravity(Gravity.CENTER_VERTICAL);
        shellContainer.addView(bottomContainer, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    private void initDisplayHud() {
        hudStateInitialized = false;
        displayHudContainer.removeAllViews();
        displayHudContainer.setOrientation(LinearLayout.HORIZONTAL);
        displayHudContainer.setGravity(Gravity.CENTER_VERTICAL);

        hudReturnBtn = new TextView(this);
        hudReturnBtn.setText("← 返回主页");
        hudReturnBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, isPhone ? 12f : 12.5f);
        hudReturnBtn.setTypeface(Typeface.DEFAULT_BOLD);
        hudReturnBtn.setTextColor(StudioDrawables.COLOR_HUD_INK);
        hudReturnBtn.setGravity(Gravity.CENTER);
        hudReturnBtn.setBackground(StudioDrawables.pill(
            this, StudioDrawables.COLOR_HUD_BG, StudioDrawables.COLOR_HUD_BORDER, 0.8f));
        hudReturnBtn.setPadding(dp(isPhone ? 12f : 14f), 0, dp(isPhone ? 12f : 14f), 0);
        final LinearLayout.LayoutParams retLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, dp(isPhone ? 34f : 38f));
        retLp.rightMargin = dp(8f);
        hudReturnBtn.setOnClickListener(v -> navigate(Page.HOME));
        displayHudContainer.addView(hudReturnBtn, retLp);

        hudToggleBtn = new TextView(this);
        hudToggleBtn.setText("▾");
        hudToggleBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, isPhone ? 15f : 16f);
        hudToggleBtn.setTextColor(StudioDrawables.COLOR_HUD_INK);
        hudToggleBtn.setGravity(Gravity.CENTER);
        hudToggleBtn.setBackground(StudioDrawables.pill(
            this, StudioDrawables.COLOR_HUD_BG, StudioDrawables.COLOR_HUD_BORDER, 0.8f));
        hudToggleBtn.setOnClickListener(v -> {
            controlsExpanded = !controlsExpanded;
            updateDisplayHudState();
        });
        displayHudContainer.addView(hudToggleBtn, new LinearLayout.LayoutParams(
            dp(isPhone ? 34f : 38f), dp(isPhone ? 34f : 38f)));

        final View spacer = new View(this);
        displayHudContainer.addView(spacer, new LinearLayout.LayoutParams(0, 1, 1f));

        hudMetricsPill = new LinearLayout(this);
        hudMetricsPill.setOrientation(LinearLayout.HORIZONTAL);
        hudMetricsPill.setGravity(Gravity.CENTER_VERTICAL);
        hudMetricsPill.setClickable(false);
        hudMetricsPill.setFocusable(false);
        hudMetricsPill.setBackground(StudioDrawables.pill(
            this, StudioDrawables.COLOR_HUD_BG, StudioDrawables.COLOR_HUD_BORDER, 0.8f));
        hudMetricsPill.setPadding(
            dp(isPhone ? 10f : 14f), dp(isPhone ? 6f : 7f),
            dp(isPhone ? 10f : 14f), dp(isPhone ? 6f : 7f));

        final View pillDot = new View(this);
        pillDot.setBackground(StudioDrawables.circleDot(StudioDrawables.COLOR_HUD_ACCENT));
        final LinearLayout.LayoutParams pillDotLp = new LinearLayout.LayoutParams(dp(6f), dp(6f));
        pillDotLp.rightMargin = dp(isPhone ? 7f : 10f);
        hudMetricsPill.addView(pillDot, pillDotLp);

        hudFpsValue = makeHudText("0", StudioDrawables.COLOR_HUD_ACCENT, isPhone ? 11.5f : 13f, true);
        final TextView fpsUnit = makeHudText("FPS", StudioDrawables.COLOR_HUD_MUTED, isPhone ? 10f : 10.5f, false);
        hudMetricsPill.addView(hudFpsValue, rightMarginLp(4f));
        hudMetricsPill.addView(fpsUnit, rightMarginLp(isPhone ? 7f : 10f));

        hudMetricsPill.addView(makeHudDivider());

        hudBitrateValue = makeHudText("0.0", StudioDrawables.COLOR_HUD_INK, isPhone ? 11.5f : 13f, true);
        final TextView mbpsUnit = makeHudText("Mbps", StudioDrawables.COLOR_HUD_MUTED, isPhone ? 10f : 10.5f, false);
        hudMetricsPill.addView(hudBitrateValue, rightMarginLp(4f));
        hudMetricsPill.addView(mbpsUnit, rightMarginLp(isPhone ? 7f : 10f));

        hudMetricsPill.addView(makeHudDivider());

        hudCodecValue = makeHudText("--", StudioDrawables.COLOR_HUD_INK, isPhone ? 11f : 12f, true);
        hudMetricsPill.addView(hudCodecValue);

        hudResDivider = makeHudDivider();
        hudMetricsPill.addView(hudResDivider);
        hudResValue = makeHudText("", StudioDrawables.COLOR_HUD_MUTED, isPhone ? 11f : 12f, false);
        hudMetricsPill.addView(hudResValue);

        hudDropDivider = makeHudDivider();
        hudMetricsPill.addView(hudDropDivider);
        hudDropValue = makeHudText("", StudioDrawables.COLOR_HUD_DROP, isPhone ? 11f : 12f, false);
        hudMetricsPill.addView(hudDropValue);

        displayHudContainer.addView(hudMetricsPill, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    private void resetSessionUiState() {
        frameReady = false;
        enteredThisLink = false;
        fps = 0;
        bitrateMbps = 0.0;
        activeCodec = "";
        activeResolution = "";
        droppedFrames = 0L;
        codecChangedWhileConnected = false;
        lastRendered = 0L;
        lastRxBytes = 0L;
        lastSampleAt = 0L;
        engine = null;
        rawEngineJson = "{}";
        applyWindowPreferredRefreshRate(0.0f);
        syncTouchCapture();
        if (page == Page.DISPLAY) {
            navigate(Page.HOME);
        }
    }

    private void pollEngineState() {
        if (nativeHandle == 0L) {
            return;
        }
        final String json = NativeBridge.snapshot(nativeHandle);
        if (!json.equals(rawEngineJson)) {
            rawEngineJson = json;
            engine = parseEngine(json);
        }
        if (link.state == AccessoryLink.State.CONNECTED && engine != null) {
            updateTelemetry(engine);
            if ("streaming".equals(engine.session) &&
                "attached".equals(engine.surface) &&
                engine.decoderRendered > 0L) {
                frameReady = true;
                applyWindowPreferredRefreshRate(parseNegotiatedRefreshHz(activeResolution));
                if (!enteredThisLink) {
                    enteredThisLink = true;
                    navigate(Page.DISPLAY);
                    return;
                }
            } else if (!"streaming".equals(engine.session) &&
                       !"paused".equals(engine.session)) {
                frameReady = false;
                applyWindowPreferredRefreshRate(0.0f);
            }
            syncTouchCapture();
        }
        renderAll();
    }

    private EngineSnapshot parseEngine(String json) {
        if (json == null || json.isEmpty() || "{}".equals(json)) {
            return null;
        }
        try {
            final JSONObject obj = new JSONObject(json);
            final EngineSnapshot snap = new EngineSnapshot();
            snap.session = obj.optString("session", obj.optString("state", "idle"));
            snap.failure = obj.optString("failure", "");
            snap.profile = obj.optString("profile", "");
            snap.codec = obj.optString("codec", "");
            snap.resolution = obj.optString("resolution", "");
            snap.preferredCodec = obj.optString("preferredCodec", "auto");
            snap.rxBytes = obj.optLong("rxBytes", obj.optLong("rx", 0L));
            snap.surface = obj.optString("surface", "none");
            snap.framesReceived = obj.optLong("framesReceived", 0L);
            snap.framesDropped = obj.optLong("framesDropped", 0L);
            snap.decoderRendered = obj.optLong("decoderRendered", 0L);
            snap.decoderDropped = obj.optLong("decoderDropped", 0L);
            snap.inputSent = obj.optLong("inputSent", obj.optLong("input", 0L));
            snap.inputRejected = obj.optLong("inputRejected", 0L);
            snap.error = obj.optString("error", "");
            return snap;
        } catch (Exception ignored) {
            return null;
        }
    }

    private void updateTelemetry(EngineSnapshot parsed) {
        if (parsed == null) {
            fps = 0;
            bitrateMbps = 0.0;
            activeCodec = "";
            activeResolution = "";
            droppedFrames = 0L;
            return;
        }
        activeCodec = parsed.codec != null ? parsed.codec : "";
        activeResolution = parsed.resolution != null ? parsed.resolution : "";
        droppedFrames = parsed.framesDropped + parsed.decoderDropped;

        final long now = System.currentTimeMillis();
        final long rendered = parsed.decoderRendered;
        final long rxBytes = parsed.rxBytes;
        if (lastSampleAt == 0L || rendered < lastRendered || rxBytes < lastRxBytes) {
            lastRendered = rendered;
            lastRxBytes = rxBytes;
            lastSampleAt = now;
            return;
        }
        final long elapsedMs = now - lastSampleAt;
        if (elapsedMs < 450L) {
            return;
        }
        final long frameDelta = rendered - lastRendered;
        final long byteDelta = rxBytes - lastRxBytes;
        fps = Math.round((float) (frameDelta * 1000L) / (float) elapsedMs);
        bitrateMbps = ((double) byteDelta * 8.0) / ((double) elapsedMs * 1000.0);
        lastRendered = rendered;
        lastRxBytes = rxBytes;
        lastSampleAt = now;
    }

    private void navigate(Page target) {
        final boolean leavingDisplay = (page == Page.DISPLAY && target != Page.DISPLAY);
        controlsExpanded = false;
        page = target;
        syncTouchCapture();
        DeviceLayout.applyWindowPresentation(this, isPhone, page == Page.DISPLAY);
        lastStructureKey = "";
        renderAll();
        applyChromeInsets();
        if (leavingDisplay) {
            handler.removeCallbacks(refreshTick);
            handler.post(refreshTick);
        }
    }

    private void syncTouchCapture() {
        final boolean active =
            (page == Page.DISPLAY && link.state == AccessoryLink.State.CONNECTED && frameReady);
        if (active != touchCaptureActive) {
            touchCaptureActive = active;
            if (nativeHandle != 0L) {
                NativeBridge.setTouchEnabled(nativeHandle, active);
            }
        }
    }

    private void selectCodec(String codec) {
        final String normalized = normalizeCodec(codec);
        if (preferredCodec.equals(normalized)) {
            return;
        }
        preferredCodec = normalized;
        prefs.edit().putString(KEY_CODEC, normalized).apply();
        if (nativeHandle != 0L) {
            NativeBridge.setPreferredCodec(nativeHandle, normalized);
        }
        if (link.state == AccessoryLink.State.CONNECTED) {
            codecChangedWhileConnected = true;
        }
        renderAll();
    }

    private void applyChromeInsets() {
        if (headerContainer != null) {
            if (compactLayout) {
                headerContainer.setPadding(
                    dp(16f) + safeInsets.left,
                    Math.max(safeInsets.top + dp(6f), dp(14f)),
                    dp(16f) + safeInsets.right,
                    dp(10f));
            } else {
                headerContainer.setPadding(
                    dp(28f) + safeInsets.left,
                    Math.max(safeInsets.top + dp(8f), dp(18f)),
                    dp(28f) + safeInsets.right,
                    dp(10f));
            }
        }
        if (bottomContainer != null) {
            if (compactLayout) {
                bottomContainer.setPadding(
                    dp(12f) + safeInsets.left, dp(6f),
                    dp(12f) + safeInsets.right, Math.max(dp(8f), safeInsets.bottom));
            } else {
                bottomContainer.setMinimumHeight(dp(32f) + safeInsets.bottom);
                bottomContainer.setPadding(
                    dp(28f) + safeInsets.left, dp(6f),
                    dp(28f) + safeInsets.right, Math.max(dp(6f), safeInsets.bottom));
            }
        }
        if (displayHudContainer != null) {
            displayHudContainer.setPadding(
                Math.max(dp(14f), safeInsets.left + dp(8f)),
                Math.max(dp(12f), safeInsets.top + dp(6f)),
                Math.max(dp(14f), safeInsets.right + dp(8f)),
                0);
        }
    }

    private void renderAll() {
        if (page == Page.DISPLAY) {
            shellContainer.setVisibility(View.GONE);
            displayHudContainer.setVisibility(View.VISIBLE);
            updateDisplayHudState();
            return;
        }

        displayHudContainer.setVisibility(View.GONE);
        shellContainer.setVisibility(View.VISIBLE);

        final String structureKey = palette.dark + "|" + compactLayout + "|" + page.name()
            + "|" + link.state.name() + "|" + frameReady + "|" + codecChangedWhileConnected
            + "|" + isPhone;
        if (!structureKey.equals(lastStructureKey)) {
            lastStructureKey = structureKey;
            rebuildShellViews();
            applyChromeInsets();
        }
        updateShellLiveValues();
    }

    private void updateDisplayHudState() {
        final String bitrateText = String.format(Locale.US, "%.1f", bitrateMbps);
        final String codecText = !activeCodec.isEmpty() ? activeCodec : "--";
        if (hudStateInitialized &&
            controlsExpanded == lastHudExpanded &&
            showFps == lastHudShowFps &&
            fps == lastHudFps &&
            bitrateText.equals(lastHudBitrateText) &&
            codecText.equals(lastHudCodec) &&
            activeResolution.equals(lastHudRes) &&
            droppedFrames == lastHudDrops) {
            return;
        }
        if (!hudStateInitialized || controlsExpanded != lastHudExpanded) {
            hudReturnBtn.setVisibility(controlsExpanded ? View.VISIBLE : View.GONE);
            hudToggleBtn.setText(controlsExpanded ? "▴" : "▾");
        }
        if (!hudStateInitialized || showFps != lastHudShowFps) {
            hudMetricsPill.setVisibility(showFps ? View.VISIBLE : View.GONE);
        }
        if (showFps) {
            if (!hudStateInitialized || !lastHudShowFps || fps != lastHudFps) {
                hudFpsValue.setText(String.valueOf(fps));
            }
            if (!hudStateInitialized || !lastHudShowFps || !bitrateText.equals(lastHudBitrateText)) {
                hudBitrateValue.setText(bitrateText);
            }
            if (!hudStateInitialized || !lastHudShowFps || !codecText.equals(lastHudCodec)) {
                hudCodecValue.setText(codecText);
            }
            if (!hudStateInitialized || !lastHudShowFps || !activeResolution.equals(lastHudRes)) {
                if (!activeResolution.isEmpty()) {
                    hudResDivider.setVisibility(View.VISIBLE);
                    hudResValue.setVisibility(View.VISIBLE);
                    hudResValue.setText(activeResolution);
                } else {
                    hudResDivider.setVisibility(View.GONE);
                    hudResValue.setVisibility(View.GONE);
                }
            }
            if (!hudStateInitialized || !lastHudShowFps || droppedFrames != lastHudDrops) {
                if (droppedFrames > 0L) {
                    hudDropDivider.setVisibility(View.VISIBLE);
                    hudDropValue.setVisibility(View.VISIBLE);
                    hudDropValue.setText("丢帧 " + droppedFrames);
                } else {
                    hudDropDivider.setVisibility(View.GONE);
                    hudDropValue.setVisibility(View.GONE);
                }
            }
        }
        hudStateInitialized = true;
        lastHudExpanded = controlsExpanded;
        lastHudShowFps = showFps;
        lastHudFps = fps;
        lastHudBitrateText = bitrateText;
        lastHudCodec = codecText;
        lastHudRes = activeResolution;
        lastHudDrops = droppedFrames;
    }

    private void rebuildShellViews() {
        lastBadgeStateKey = "";
        lastSelectedCodecCard = null;
        schematicView = null;
        topStatusTextView = null;
        topStatusDotView = null;
        topStatusBadgeView = null;
        statusTitleView = null;
        statusDetailView = null;
        liveSpecValueView = null;
        livePerfRowView = null;
        livePerfValueView = null;
        settingsCodecStatusView = null;
        settingsCodecHintView = null;
        whisperCodecView = null;
        whisperDotView = null;
        diagnosticValueViews.clear();
        codecOptionCards.clear();
        codecOptionTitles.clear();
        codecOptionSubtitles.clear();

        buildHeader();
        buildScrollBody();
        buildBottomBar();
    }

    private void buildHeader() {
        headerContainer.removeAllViews();
        headerContainer.setBackgroundColor(palette.surface);

        if (compactLayout) {
            if (page != Page.HOME) {
                headerContainer.addView(makeBackHeaderButton(dp(32f), dp(12f)));
            } else {
                headerContainer.addView(makeBrandTitle());
            }
            final View spacer = new View(this);
            headerContainer.addView(spacer, new LinearLayout.LayoutParams(0, 1, 1f));

            headerContainer.addView(makeStatusBadge());
            return;
        }

        if (page != Page.HOME) {
            headerContainer.addView(makeBackHeaderButton(dp(34f), dp(13f)));
        } else {
            headerContainer.addView(makeBrandTitle(), rightMarginLp(10f));
            headerContainer.addView(makeStatusBadge());
        }

        final View spacer = new View(this);
        headerContainer.addView(spacer, new LinearLayout.LayoutParams(0, 1, 1f));

        final LinearLayout pillGroup = new LinearLayout(this);
        pillGroup.setOrientation(LinearLayout.HORIZONTAL);
        pillGroup.setGravity(Gravity.CENTER_VERTICAL);
        pillGroup.setPadding(dp(3f), dp(3f), dp(3f), dp(3f));
        pillGroup.setBackground(StudioDrawables.pill(
            this, palette.subtle, palette.softHairline, 0.8f));
        pillGroup.addView(makeHeaderTab(Page.HOME, "连接"));
        pillGroup.addView(makeHeaderTab(Page.SETTINGS, "设置"));
        pillGroup.addView(makeHeaderTab(Page.DIAGNOSTICS, "诊断"));
        headerContainer.addView(pillGroup);
    }

    private View makeStatusBadge() {
        topStatusBadgeView = new LinearLayout(this);
        topStatusBadgeView.setOrientation(LinearLayout.HORIZONTAL);
        topStatusBadgeView.setGravity(Gravity.CENTER_VERTICAL);
        topStatusBadgeView.setPadding(dp(10f), dp(4f), dp(10f), dp(4f));

        topStatusDotView = new View(this);
        final LinearLayout.LayoutParams dotLp = new LinearLayout.LayoutParams(dp(6f), dp(6f));
        dotLp.rightMargin = dp(6f);
        topStatusBadgeView.addView(topStatusDotView, dotLp);

        topStatusTextView = new TextView(this);
        topStatusTextView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        topStatusTextView.setTypeface(Typeface.DEFAULT_BOLD);
        topStatusBadgeView.addView(topStatusTextView);
        return topStatusBadgeView;
    }

    private View makeBrandTitle() {
        final TextView brand = new TextView(this);
        brand.setText("DeskMate");
        brand.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
        brand.setTypeface(Typeface.DEFAULT_BOLD);
        brand.setTextColor(palette.ink);
        return brand;
    }

    private View makeBackHeaderButton(int heightPx, int padHPx) {
        final LinearLayout btn = new LinearLayout(this);
        btn.setOrientation(LinearLayout.HORIZONTAL);
        btn.setGravity(Gravity.CENTER_VERTICAL);
        btn.setPadding(padHPx, 0, padHPx, 0);
        btn.setBackground(StudioDrawables.pill(
            this, palette.subtle, palette.hairline, 0.8f));
        btn.setLayoutParams(new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, heightPx));

        final TextView arrow = new TextView(this);
        arrow.setText("←");
        arrow.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        arrow.setTextColor(palette.accent);
        btn.addView(arrow, rightMarginLp(5f));

        final TextView label = new TextView(this);
        label.setText("返回");
        label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        label.setTextColor(palette.ink);
        btn.addView(label);

        btn.setOnClickListener(v -> onBackPressed());
        return btn;
    }

    private View makeHeaderTab(Page target, String labelText) {
        final boolean active = (page == target);
        final TextView tv = new TextView(this);
        tv.setText(labelText);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        tv.setTypeface(active ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        tv.setTextColor(active ? palette.ink : palette.inkSecondary);
        tv.setGravity(Gravity.CENTER);
        tv.setPadding(dp(14f), dp(6f), dp(14f), dp(6f));
        if (active) {
            tv.setBackground(StudioDrawables.pill(this, palette.surface, Color.TRANSPARENT, 0f));
        }
        tv.setOnClickListener(v -> navigate(target));
        return tv;
    }

    private void buildScrollBody() {
        contentContainer.removeAllViews();
        contentContainer.setGravity((!compactLayout && page == Page.HOME)
            ? Gravity.CENTER
            : (Gravity.TOP | Gravity.CENTER_HORIZONTAL));

        if (page == Page.HOME) {
            buildHomeContent();
        } else if (page == Page.SETTINGS) {
            buildSettingsContent();
        } else {
            buildDiagnosticsContent();
        }
    }

    private void buildHomeContent() {
        schematicView = new WiredLinkSchematicView(this);
        if (compactLayout) {
            final LinearLayout col = new LinearLayout(this);
            col.setOrientation(LinearLayout.VERTICAL);
            col.setPadding(dp(16f), dp(14f), dp(16f), dp(20f));
            col.addView(schematicView, bottomMarginLp(14f));
            col.addView(buildStatusHeaderBlock(true), bottomMarginLp(14f));

            if (link.state == AccessoryLink.State.ERROR) {
                col.addView(makePrimaryButton("重试 USB 连接", true,
                    () -> accessoryLink.retry()), bottomMarginLp(14f));
            } else if (frameReady) {
                col.addView(makePrimaryButton(
                    isPhone ? "进入横屏扩展桌面" : "进入扩展桌面",
                    true, () -> navigate(Page.DISPLAY)), bottomMarginLp(14f));
            }

            if (link.state == AccessoryLink.State.SEARCHING ||
                link.state == AccessoryLink.State.AUTHORIZING) {
                col.addView(buildConnectionGuideCard());
            } else if (link.state == AccessoryLink.State.CONNECTED) {
                col.addView(buildLiveSpecCard());
            }

            contentContainer.addView(col, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            return;
        }

        final LinearLayout wrapper = new LinearLayout(this);
        wrapper.setOrientation(LinearLayout.HORIZONTAL);
        wrapper.setGravity(Gravity.CENTER_VERTICAL);
        wrapper.setPadding(dp(28f), dp(20f), dp(28f), dp(20f));

        final LinearLayout leftCol = new LinearLayout(this);
        leftCol.setOrientation(LinearLayout.VERTICAL);
        leftCol.addView(schematicView);
        final LinearLayout.LayoutParams leftLp = new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.44f);
        leftLp.rightMargin = dp(24f);
        wrapper.addView(leftCol, leftLp);

        final LinearLayout rightCol = new LinearLayout(this);
        rightCol.setOrientation(LinearLayout.VERTICAL);
        rightCol.addView(buildStatusHeaderBlock(false), bottomMarginLp(14f));

        if (link.state == AccessoryLink.State.SEARCHING ||
            link.state == AccessoryLink.State.AUTHORIZING) {
            rightCol.addView(buildConnectionGuideCard(), bottomMarginLp(14f));
        } else if (link.state == AccessoryLink.State.CONNECTED) {
            rightCol.addView(buildLiveSpecCard(), bottomMarginLp(14f));
        }

        if (link.state == AccessoryLink.State.ERROR) {
            rightCol.addView(makePrimaryButton("重试 USB 连接", false,
                () -> accessoryLink.retry()), bottomMarginLp(14f));
        } else if (frameReady) {
            rightCol.addView(makePrimaryButton("进入扩展桌面", false,
                () -> navigate(Page.DISPLAY)), bottomMarginLp(14f));
        }

        final LinearLayout quickRow = new LinearLayout(this);
        quickRow.setOrientation(LinearLayout.HORIZONTAL);
        quickRow.addView(makeSmallActionButton("编码与浮窗设置", false,
            () -> navigate(Page.SETTINGS)), rightMarginLp(10f));
        quickRow.addView(makeSmallActionButton("查看连接诊断", true,
            () -> navigate(Page.DIAGNOSTICS)));
        rightCol.addView(quickRow);

        wrapper.addView(rightCol, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.56f));

        final LinearLayout.LayoutParams wrapLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        contentContainer.addView(wrapper, wrapLp);
        clampMaxWidth(wrapper, 1000);
    }

    private View buildStatusHeaderBlock(boolean compact) {
        final LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        statusTitleView = new TextView(this);
        statusTitleView.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 20f : 24f);
        statusTitleView.setTypeface(Typeface.DEFAULT_BOLD);
        statusTitleView.setTextColor(palette.ink);
        col.addView(statusTitleView, bottomMarginLp(6f));

        statusDetailView = new TextView(this);
        statusDetailView.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 13f : 13.5f);
        statusDetailView.setLineSpacing(dp(3f), 1.0f);
        statusDetailView.setTextColor(palette.inkSecondary);
        col.addView(statusDetailView);

        return col;
    }

    private View buildConnectionGuideCard() {
        final LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(StudioDrawables.card(
            this, palette.surface, palette.hairline, 16f, 0.8f));

        card.addView(makeStepRow("1", "确保电脑端的 DeskMate 已启动运行"));
        card.addView(makeHairlineDivider(palette.softHairline, 0.8f));
        card.addView(makeStepRow("2", "使用高速 USB-C 数据线连接电脑与" + deviceLabel()));
        card.addView(makeHairlineDivider(palette.softHairline, 0.8f));
        card.addView(makeStepRow("3", "系统弹出访问确认时点击允许 USB 配件连接"));
        return card;
    }

    private View makeStepRow(String num, String text) {
        final LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(14f), dp(10f), dp(14f), dp(10f));

        final TextView numView = new TextView(this);
        numView.setText(num);
        numView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        numView.setTypeface(Typeface.DEFAULT_BOLD);
        numView.setTextColor(palette.accent);
        row.addView(numView, new LinearLayout.LayoutParams(dp(22f), ViewGroup.LayoutParams.WRAP_CONTENT));

        final TextView textView = new TextView(this);
        textView.setText(text);
        textView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        textView.setTextColor(palette.inkSecondary);
        row.addView(textView, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return row;
    }

    private View buildLiveSpecCard() {
        final LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(StudioDrawables.card(
            this, palette.surface, palette.hairline, 16f, 0.8f));

        final LinearLayout specRow = new LinearLayout(this);
        specRow.setOrientation(LinearLayout.HORIZONTAL);
        specRow.setGravity(Gravity.CENTER_VERTICAL);
        specRow.setPadding(dp(14f), dp(11f), dp(14f), dp(11f));

        final TextView specLabel = new TextView(this);
        specLabel.setText("显示规格");
        specLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        specLabel.setTextColor(palette.inkMuted);
        specRow.addView(specLabel, new LinearLayout.LayoutParams(
            dp(68f), ViewGroup.LayoutParams.WRAP_CONTENT));

        liveSpecValueView = new TextView(this);
        liveSpecValueView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        liveSpecValueView.setTypeface(Typeface.DEFAULT_BOLD);
        liveSpecValueView.setTextColor(palette.ink);
        specRow.addView(liveSpecValueView, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        card.addView(specRow);

        if (frameReady) {
            card.addView(makeHairlineDivider(palette.softHairline, 0.8f));

            livePerfRowView = new LinearLayout(this);
            livePerfRowView.setOrientation(LinearLayout.HORIZONTAL);
            livePerfRowView.setGravity(Gravity.CENTER_VERTICAL);
            livePerfRowView.setPadding(dp(14f), dp(11f), dp(14f), dp(11f));

            final TextView perfLabel = new TextView(this);
            perfLabel.setText("实时性能");
            perfLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            perfLabel.setTextColor(palette.accent);
            livePerfRowView.addView(perfLabel, new LinearLayout.LayoutParams(
                dp(68f), ViewGroup.LayoutParams.WRAP_CONTENT));

            livePerfValueView = new TextView(this);
            livePerfValueView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
            livePerfValueView.setTypeface(Typeface.DEFAULT_BOLD);
            livePerfValueView.setTextColor(palette.ink);
            livePerfRowView.addView(livePerfValueView, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            card.addView(livePerfRowView);
        }
        return card;
    }

    private void buildSettingsContent() {
        if (compactLayout) {
            final LinearLayout col = new LinearLayout(this);
            col.setOrientation(LinearLayout.VERTICAL);
            col.setPadding(dp(16f), dp(14f), dp(16f), dp(20f));
            appendSettingsFields(col, true);
            contentContainer.addView(col, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            return;
        }

        final LinearLayout wrapper = new LinearLayout(this);
        wrapper.setOrientation(LinearLayout.HORIZONTAL);
        wrapper.setGravity(Gravity.TOP);
        wrapper.setPadding(dp(28f), dp(18f), dp(28f), dp(24f));

        final LinearLayout leftCol = new LinearLayout(this);
        leftCol.setOrientation(LinearLayout.VERTICAL);
        schematicView = new WiredLinkSchematicView(this);
        leftCol.addView(schematicView, bottomMarginLp(10f));

        final LinearLayout prefCard = new LinearLayout(this);
        prefCard.setOrientation(LinearLayout.VERTICAL);
        prefCard.setPadding(dp(14f), dp(14f), dp(14f), dp(14f));
        prefCard.setBackground(StudioDrawables.card(
            this, palette.surface, palette.hairline, 16f, 0.8f));

        final TextView prefLabel = new TextView(this);
        prefLabel.setText("当前编码偏好");
        prefLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        prefLabel.setTextColor(palette.inkMuted);
        prefCard.addView(prefLabel, bottomMarginLp(4f));

        settingsCodecStatusView = new TextView(this);
        settingsCodecStatusView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        settingsCodecStatusView.setTypeface(Typeface.DEFAULT_BOLD);
        settingsCodecStatusView.setTextColor(palette.ink);
        prefCard.addView(settingsCodecStatusView);

        leftCol.addView(prefCard);
        final LinearLayout.LayoutParams leftLp = new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.35f);
        leftLp.rightMargin = dp(24f);
        wrapper.addView(leftCol, leftLp);

        final LinearLayout rightCol = new LinearLayout(this);
        rightCol.setOrientation(LinearLayout.VERTICAL);
        appendSettingsFields(rightCol, false);
        wrapper.addView(rightCol, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.65f));

        contentContainer.addView(wrapper, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        clampMaxWidth(wrapper, 1000);
    }

    private void appendSettingsFields(LinearLayout parent, boolean compact) {
        parent.addView(makePageHeader("系统设置", compact ? 19f : 20f), bottomMarginLp(14f));

        if (compact) {
            final LinearLayout compactSpecCard = new LinearLayout(this);
            compactSpecCard.setOrientation(LinearLayout.HORIZONTAL);
            compactSpecCard.setGravity(Gravity.CENTER_VERTICAL);
            compactSpecCard.setPadding(dp(14f), dp(12f), dp(14f), dp(12f));
            compactSpecCard.setBackground(StudioDrawables.card(
                this, palette.surface, palette.hairline, 16f, 0.8f));

            final TextView lbl = new TextView(this);
            lbl.setText("当前编码与规格");
            lbl.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            lbl.setTextColor(palette.inkMuted);
            compactSpecCard.addView(lbl, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

            settingsCodecStatusView = new TextView(this);
            settingsCodecStatusView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
            settingsCodecStatusView.setTypeface(Typeface.DEFAULT_BOLD);
            settingsCodecStatusView.setTextColor(palette.ink);
            compactSpecCard.addView(settingsCodecStatusView);

            parent.addView(compactSpecCard, bottomMarginLp(14f));
        }

        parent.addView(makeSectionHeader("画面与性能浮窗"), bottomMarginLp(8f));

        final LinearLayout fpsCard = new LinearLayout(this);
        fpsCard.setOrientation(LinearLayout.HORIZONTAL);
        fpsCard.setGravity(Gravity.CENTER_VERTICAL);
        fpsCard.setPadding(dp(16f), dp(12f), dp(16f), dp(12f));
        fpsCard.setBackground(StudioDrawables.card(
            this, palette.surface, palette.hairline, 16f, 0.8f));

        final LinearLayout fpsLabels = new LinearLayout(this);
        fpsLabels.setOrientation(LinearLayout.VERTICAL);
        final TextView fpsTitle = new TextView(this);
        fpsTitle.setText("性能参数浮窗");
        fpsTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        fpsTitle.setTypeface(Typeface.DEFAULT_BOLD);
        fpsTitle.setTextColor(palette.ink);
        fpsLabels.addView(fpsTitle, bottomMarginLp(3f));

        final TextView fpsSub = new TextView(this);
        fpsSub.setText("全屏投屏时在右上角显示帧率、码率、编码格式与丢帧统计");
        fpsSub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        fpsSub.setTextColor(palette.inkSecondary);
        fpsLabels.addView(fpsSub);

        final LinearLayout.LayoutParams fpsLblLp = new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        fpsLblLp.rightMargin = dp(12f);
        fpsCard.addView(fpsLabels, fpsLblLp);

        final Switch fpsSwitch = new Switch(this);
        fpsSwitch.setShowText(false);
        fpsSwitch.setChecked(showFps);
        styleSwitch(fpsSwitch, showFps);
        fpsSwitch.setOnCheckedChangeListener((btn, checked) -> {
            showFps = checked;
            prefs.edit().putBoolean(KEY_SHOW_FPS, checked).apply();
            styleSwitch(fpsSwitch, checked);
        });
        fpsCard.addView(fpsSwitch);
        parent.addView(fpsCard, bottomMarginLp(14f));

        if (isPhone) {
            final LinearLayout oriCard = new LinearLayout(this);
            oriCard.setOrientation(LinearLayout.VERTICAL);
            oriCard.setPadding(dp(16f), dp(12f), dp(16f), dp(12f));
            oriCard.setBackground(StudioDrawables.card(
                this, palette.surface, palette.hairline, 16f, 0.8f));

            final TextView oriTitle = new TextView(this);
            oriTitle.setText("横竖屏自动切换");
            oriTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
            oriTitle.setTypeface(Typeface.DEFAULT_BOLD);
            oriTitle.setTextColor(palette.ink);
            oriCard.addView(oriTitle, bottomMarginLp(3f));

            final TextView oriSub = new TextView(this);
            oriSub.setText("连接前保持竖屏便于配置，进入扩展画面后自动切换横屏并支持左右翻转");
            oriSub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            oriSub.setLineSpacing(dp(2f), 1.0f);
            oriSub.setTextColor(palette.inkSecondary);
            oriCard.addView(oriSub);

            parent.addView(oriCard, bottomMarginLp(14f));
        }

        parent.addView(makeSectionHeader("视频编码格式"), bottomMarginLp(6f));

        settingsCodecHintView = new TextView(this);
        settingsCodecHintView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        settingsCodecHintView.setTextColor(palette.inkSecondary);
        parent.addView(settingsCodecHintView, bottomMarginLp(10f));

        final LinearLayout codecRow = new LinearLayout(this);
        codecRow.setOrientation(LinearLayout.HORIZONTAL);
        for (int index = 0; index < CODEC_VALUES.length; index++) {
            final String val = CODEC_VALUES[index];
            final LinearLayout card = new LinearLayout(this);
            card.setOrientation(LinearLayout.VERTICAL);
            card.setGravity(Gravity.CENTER_HORIZONTAL);
            card.setPadding(
                dp(compact ? 6f : 10f), dp(11f),
                dp(compact ? 6f : 10f), dp(11f));

            final TextView titleTv = new TextView(this);
            titleTv.setText(CODEC_TITLES[index]);
            titleTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 13f : 14f);
            titleTv.setGravity(Gravity.CENTER_HORIZONTAL);
            card.addView(titleTv, bottomMarginLp(4f));

            final TextView subTv = new TextView(this);
            subTv.setText(CODEC_SUBTITLES[index]);
            subTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 10.5f : 11f);
            subTv.setGravity(Gravity.CENTER_HORIZONTAL);
            card.addView(subTv, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

            card.setOnClickListener(v -> selectCodec(val));

            codecOptionCards.add(card);
            codecOptionTitles.add(titleTv);
            codecOptionSubtitles.add(subTv);

            final LinearLayout.LayoutParams cardLp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            if (index + 1 < CODEC_VALUES.length) {
                cardLp.rightMargin = dp(compact ? 8f : 10f);
            }
            codecRow.addView(card, cardLp);
        }
        parent.addView(codecRow, bottomMarginLp(12f));

        if (codecChangedWhileConnected && link.state == AccessoryLink.State.CONNECTED) {
            final LinearLayout banner = new LinearLayout(this);
            banner.setOrientation(LinearLayout.HORIZONTAL);
            banner.setGravity(Gravity.CENTER_VERTICAL);
            banner.setPadding(dp(14f), dp(10f), dp(14f), dp(10f));
            banner.setBackground(StudioDrawables.card(
                this, palette.accentSoft, palette.accentBorder, 14f, 0.8f));

            final TextView msg = new TextView(this);
            msg.setText("编码偏好已更新，重新连接 USB 会话后生效");
            msg.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            msg.setTextColor(palette.accent);
            final LinearLayout.LayoutParams msgLp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            msgLp.rightMargin = dp(12f);
            banner.addView(msg, msgLp);

            final TextView reconnBtn = new TextView(this);
            reconnBtn.setText("立即重连");
            reconnBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            reconnBtn.setTypeface(Typeface.DEFAULT_BOLD);
            reconnBtn.setTextColor(palette.accentFg);
            reconnBtn.setGravity(Gravity.CENTER);
            reconnBtn.setPadding(dp(14f), 0, dp(14f), 0);
            reconnBtn.setBackground(StudioDrawables.pill(
                this, palette.accent, Color.TRANSPARENT, 0f));
            reconnBtn.setOnClickListener(v -> {
                codecChangedWhileConnected = false;
                accessoryLink.reconnect();
            });
            banner.addView(reconnBtn, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, dp(32f)));

            parent.addView(banner, bottomMarginLp(14f));
        }

        parent.addView(makeSectionHeader("连接诊断"), bottomMarginLp(8f));

        final LinearLayout diagEntry = new LinearLayout(this);
        diagEntry.setOrientation(LinearLayout.HORIZONTAL);
        diagEntry.setGravity(Gravity.CENTER_VERTICAL);
        diagEntry.setPadding(dp(16f), dp(12f), dp(16f), dp(12f));
        diagEntry.setBackground(StudioDrawables.card(
            this, palette.surface, palette.hairline, 16f, 0.8f));

        final LinearLayout diagLabels = new LinearLayout(this);
        diagLabels.setOrientation(LinearLayout.VERTICAL);
        final TextView diagTitle = new TextView(this);
        diagTitle.setText("查看连接与解码统计");
        diagTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        diagTitle.setTypeface(Typeface.DEFAULT_BOLD);
        diagTitle.setTextColor(palette.ink);
        diagLabels.addView(diagTitle, bottomMarginLp(2f));

        final TextView diagSub = new TextView(this);
        diagSub.setText("检视 USB 状态、协商参数、码率与帧率统计");
        diagSub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        diagSub.setTextColor(palette.inkSecondary);
        diagLabels.addView(diagSub);
        diagEntry.addView(diagLabels, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        final TextView viewLink = new TextView(this);
        viewLink.setText("查看 →");
        viewLink.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        viewLink.setTypeface(Typeface.DEFAULT_BOLD);
        viewLink.setTextColor(palette.accent);
        diagEntry.addView(viewLink);

        diagEntry.setOnClickListener(v -> navigate(Page.DIAGNOSTICS));
        parent.addView(diagEntry);
    }

    private void buildDiagnosticsContent() {
        if (compactLayout) {
            final LinearLayout col = new LinearLayout(this);
            col.setOrientation(LinearLayout.VERTICAL);
            col.setPadding(dp(16f), dp(14f), dp(16f), dp(20f));
            col.addView(makePageHeader("连接与解码诊断", 19f), bottomMarginLp(12f));
            col.addView(buildDiagnosticsCard(true), bottomMarginLp(12f));
            col.addView(makeReconnectUsbButton(dp(42f)));
            contentContainer.addView(col, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            return;
        }

        final LinearLayout wrapper = new LinearLayout(this);
        wrapper.setOrientation(LinearLayout.HORIZONTAL);
        wrapper.setGravity(Gravity.TOP);
        wrapper.setPadding(dp(28f), dp(18f), dp(28f), dp(24f));

        final LinearLayout leftCol = new LinearLayout(this);
        leftCol.setOrientation(LinearLayout.VERTICAL);
        schematicView = new WiredLinkSchematicView(this);
        leftCol.addView(schematicView, bottomMarginLp(10f));
        leftCol.addView(makeReconnectUsbButton(dp(38f)));
        final LinearLayout.LayoutParams leftLp = new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.35f);
        leftLp.rightMargin = dp(24f);
        wrapper.addView(leftCol, leftLp);

        final LinearLayout rightCol = new LinearLayout(this);
        rightCol.setOrientation(LinearLayout.VERTICAL);
        rightCol.addView(makePageHeader("连接与解码诊断", 20f), bottomMarginLp(12f));
        rightCol.addView(buildDiagnosticsCard(false), bottomMarginLp(12f));

        final LinearLayout btnRow = new LinearLayout(this);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);
        btnRow.addView(makeSmallActionButton("← 返回系统设置", false,
            () -> navigate(Page.SETTINGS)), rightMarginLp(10f));
        btnRow.addView(makeSmallActionButton("返回主页", true,
            () -> navigate(Page.HOME)));
        rightCol.addView(btnRow);

        wrapper.addView(rightCol, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.65f));

        contentContainer.addView(wrapper, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        clampMaxWidth(wrapper, 1000);
    }

    private View buildDiagnosticsCard(boolean compact) {
        final String[] labels = {
            "连接状态", "USB 链路", "会话阶段", "协商编码",
            "输出分辨率", "实时性能", "已渲染帧", "丢帧统计",
            "触控转发", "失败原因", "错误详情"
        };
        final LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(StudioDrawables.card(
            this, palette.surface, palette.hairline, 16f, 0.8f));

        for (int index = 0; index < labels.length; index++) {
            if (index > 0) {
                card.addView(makeHairlineDivider(palette.softHairline, 0.8f));
            }
            final LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setPadding(dp(14f), dp(10f), dp(14f), dp(10f));

            final TextView lbl = new TextView(this);
            lbl.setText(labels[index]);
            lbl.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 12f : 12.5f);
            lbl.setTextColor(palette.inkMuted);
            row.addView(lbl, new LinearLayout.LayoutParams(
                dp(compact ? 78f : 96f), ViewGroup.LayoutParams.WRAP_CONTENT));

            final TextView val = new TextView(this);
            val.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 12f : 12.5f);
            val.setTypeface(Typeface.DEFAULT_BOLD);
            val.setTextColor(palette.ink);
            row.addView(val, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

            diagnosticValueViews.add(val);
            card.addView(row);
        }
        return card;
    }

    private View makeReconnectUsbButton(int heightPx) {
        final TextView btn = new TextView(this);
        btn.setText("重新握手 USB 连接");
        btn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        btn.setTypeface(Typeface.DEFAULT_BOLD);
        btn.setTextColor(palette.accent);
        btn.setGravity(Gravity.CENTER);
        btn.setBackground(StudioDrawables.card(
            this, palette.accentSoft, palette.accentBorder, 11f, 0.8f));
        btn.setLayoutParams(new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, heightPx));
        btn.setOnClickListener(v -> {
            if (link.state == AccessoryLink.State.CONNECTED) {
                accessoryLink.reconnect();
            } else {
                accessoryLink.retry();
            }
        });
        return btn;
    }

    private void buildBottomBar() {
        bottomContainer.removeAllViews();
        bottomContainer.setBackgroundColor(palette.surface);

        if (compactLayout) {
            bottomContainer.addView(makeBottomTab(Page.HOME, "连接"));
            bottomContainer.addView(makeBottomTab(Page.SETTINGS, "设置"));
            bottomContainer.addView(makeBottomTab(Page.DIAGNOSTICS, "诊断"));
            return;
        }

        final TextView leftNote = new TextView(this);
        leftNote.setText("USB-C AOA 硬件直连 · 自动 HiDPI 虚拟屏 · 多点与双指触控");
        leftNote.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        leftNote.setTextColor(palette.inkMuted);
        bottomContainer.addView(leftNote, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        final LinearLayout rightGroup = new LinearLayout(this);
        rightGroup.setOrientation(LinearLayout.HORIZONTAL);
        rightGroup.setGravity(Gravity.CENTER_VERTICAL);

        whisperDotView = new View(this);
        final LinearLayout.LayoutParams dotLp = new LinearLayout.LayoutParams(dp(6f), dp(6f));
        dotLp.rightMargin = dp(6f);
        rightGroup.addView(whisperDotView, dotLp);

        whisperCodecView = new TextView(this);
        whisperCodecView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        whisperCodecView.setTextColor(palette.inkSecondary);
        rightGroup.addView(whisperCodecView);

        bottomContainer.addView(rightGroup);
    }

    private View makeBottomTab(Page target, String labelText) {
        final boolean active = (page == target);
        final TextView tv = new TextView(this);
        tv.setText(labelText);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        tv.setGravity(Gravity.CENTER);
        tv.setTypeface(active ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        tv.setTextColor(active ? palette.accent : palette.inkSecondary);
        tv.setPadding(dp(8f), dp(8f), dp(8f), dp(8f));
        if (active) {
            tv.setBackground(StudioDrawables.pill(
                this, palette.accentSoft, Color.TRANSPARENT, 0f));
        }
        tv.setOnClickListener(v -> navigate(target));
        final LinearLayout.LayoutParams tabLp = new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        tv.setLayoutParams(tabLp);
        return tv;
    }

    private static void setTextIfChanged(TextView view, String text) {
        if (view != null && !TextUtils.equals(view.getText(), text)) {
            view.setText(text);
        }
    }

    private void updateShellLiveValues() {
        final int dotColor = statusAccentColor();
        if (topStatusDotView != null) {
            topStatusDotView.setBackground(StudioDrawables.circleDot(dotColor));
        }
        if (topStatusBadgeView != null && topStatusTextView != null) {
            final String badgeKey = palette.dark + "|" + shortStatusLabel() + "|" + link.state.name() + "|" + frameReady;
            setTextIfChanged(topStatusTextView, shortStatusLabel());
            if (!badgeKey.equals(lastBadgeStateKey)) {
                lastBadgeStateKey = badgeKey;
                final int fg;
                final int bg;
                final int br;
                if (frameReady) {
                    fg = palette.accent;
                    bg = palette.accentSoft;
                    br = palette.accentBorder;
                } else if (link.state == AccessoryLink.State.CONNECTED) {
                    fg = palette.positive;
                    bg = palette.positiveSoft;
                    br = palette.positiveBorder;
                } else if (link.state == AccessoryLink.State.AUTHORIZING) {
                    fg = palette.warning;
                    bg = palette.warningSoft;
                    br = palette.warningBorder;
                } else if (link.state == AccessoryLink.State.ERROR) {
                    fg = palette.danger;
                    bg = palette.dangerSoft;
                    br = palette.dangerBorder;
                } else {
                    fg = palette.inkSecondary;
                    bg = palette.subtle;
                    br = palette.hairline;
                }
                topStatusTextView.setTextColor(fg);
                topStatusBadgeView.setBackground(StudioDrawables.pill(this, bg, br, 0.8f));
            }
        }

        if (schematicView != null) {
            schematicView.updateState(
                link.state == AccessoryLink.State.CONNECTED,
                frameReady,
                fps,
                activeCodec,
                activeResolution,
                page != Page.HOME || compactLayout,
                isPhone);
        }

        setTextIfChanged(statusTitleView, statusTitle());
        setTextIfChanged(statusDetailView, statusDetail());

        if (liveSpecValueView != null) {
            final String codecText = !activeCodec.isEmpty() ? activeCodec : codecDisplayName(preferredCodec);
            final String resText = !activeResolution.isEmpty() ? activeResolution : "协商中";
            setTextIfChanged(liveSpecValueView, codecText + " · " + resText);
        }
        if (livePerfValueView != null) {
            setTextIfChanged(livePerfValueView,
                fps + " FPS · " + String.format(Locale.US, "%.1f", bitrateMbps) + " Mbps · 触控就绪");
        }

        if (settingsCodecStatusView != null) {
            if (!activeCodec.isEmpty()) {
                setTextIfChanged(settingsCodecStatusView, activeCodec +
                    (!activeResolution.isEmpty() ? " · " + activeResolution : ""));
            } else {
                setTextIfChanged(settingsCodecStatusView, codecDisplayName(preferredCodec));
            }
        }
        if (settingsCodecHintView != null) {
            if (!activeCodec.isEmpty()) {
                setTextIfChanged(settingsCodecHintView, "当前协商：" + activeCodec +
                    (!activeResolution.isEmpty() ? " · " + activeResolution : ""));
            } else {
                setTextIfChanged(settingsCodecHintView, "选择传输编码偏好，默认自动优先使用 HEVC 硬件解码");
            }
        }
        final String codecRenderKey = palette.dark + "|" + preferredCodec;
        if (!codecRenderKey.equals(lastSelectedCodecCard)) {
            lastSelectedCodecCard = codecRenderKey;
            for (int index = 0; index < codecOptionCards.size() && index < CODEC_VALUES.length; index++) {
                final boolean selected = preferredCodec.equals(CODEC_VALUES[index]);
                codecOptionCards.get(index).setBackground(StudioDrawables.card(
                    this,
                    selected ? palette.accentSoft : palette.surface,
                    selected ? palette.accentBorder : palette.hairline,
                    11f, 0.8f));
                codecOptionTitles.get(index).setTypeface(selected ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
                codecOptionTitles.get(index).setTextColor(selected ? palette.accent : palette.ink);
                codecOptionSubtitles.get(index).setTextColor(selected ? palette.accent : palette.inkMuted);
            }
        }

        if (diagnosticValueViews.size() == 11) {
            final String[] vals = diagnosticValues();
            for (int i = 0; i < 11; i++) {
                setTextIfChanged(diagnosticValueViews.get(i), vals[i]);
            }
        }

        if (whisperDotView != null) {
            whisperDotView.setBackground(StudioDrawables.circleDot(dotColor));
        }
        if (whisperCodecView != null) {
            final String codecPart = !activeCodec.isEmpty() ? activeCodec : preferredCodec.toUpperCase(Locale.US);
            final String resPart = !activeResolution.isEmpty() ? activeResolution : "待机";
            setTextIfChanged(whisperCodecView, codecPart + " · " + resPart);
        }
    }

    private String[] diagnosticValues() {
        if (engine == null) {
            return new String[] {
                statusTitle(),
                link.message,
                "尚未建立",
                "未协商（偏好：" + codecDisplayName(preferredCodec) + "）",
                "未协商",
                "0 FPS · 0.0 Mbps",
                "0 帧",
                "0 帧",
                "已发送 0 · 丢弃 0",
                "无",
                "暂无会话数据"
            };
        }
        final long totalDrop = engine.framesDropped + engine.decoderDropped;
        return new String[] {
            statusTitle(),
            link.message,
            engine.session,
            (!engine.codec.isEmpty() ? engine.codec : "未协商")
                + "（偏好：" + codecDisplayName(engine.preferredCodec) + "）",
            !engine.resolution.isEmpty()
                ? engine.resolution
                : (!engine.profile.isEmpty() ? engine.profile : "未协商"),
            fps + " FPS · " + String.format("%.1f", bitrateMbps) + " Mbps",
            engine.decoderRendered + " 帧",
            totalDrop + " 帧",
            "已发送 " + engine.inputSent + " · 丢弃 " + engine.inputRejected,
            !engine.failure.isEmpty() ? engine.failure : "无",
            !engine.error.isEmpty() ? engine.error : "无"
        };
    }

    private String deviceLabel() {
        return isPhone ? "手机" : "平板";
    }

    private String shortStatusLabel() {
        if (frameReady) {
            return "推流中";
        }
        if (link.state == AccessoryLink.State.CONNECTED) {
            return "USB 已连";
        }
        if (link.state == AccessoryLink.State.AUTHORIZING) {
            return "待授权";
        }
        if (link.state == AccessoryLink.State.ERROR) {
            return "异常";
        }
        return "待机";
    }

    private String statusTitle() {
        if (link.state == AccessoryLink.State.ERROR) {
            return link.message.contains("permission") ? "需要 USB 配件授权" : "暂时无法连接";
        }
        if (link.state == AccessoryLink.State.AUTHORIZING) {
            return "等待 USB 配件授权";
        }
        if (link.state == AccessoryLink.State.CONNECTED) {
            return frameReady ? "扩展桌面已就绪" : "正在准备画面";
        }
        return "连接数据线即可扩展";
    }

    private String statusDetail() {
        if (link.state == AccessoryLink.State.ERROR) {
            return link.message.contains("permission")
                ? "点按下方重试，并在系统提示弹窗中允许 USB 配件连接。"
                : "请检查电脑端 DeskMate 状态与 USB 数据线连接后重试。";
        }
        if (link.state == AccessoryLink.State.AUTHORIZING) {
            return "请在系统提示弹窗中点击允许，建立高速 USB 配件通信通道。";
        }
        if (link.state == AccessoryLink.State.CONNECTED) {
            return frameReady
                ? "已建立低延迟有线扩展桌面，可随时进入全屏画面与反向触控。"
                : "USB 已连接，正在协商编码格式与虚拟显示器首帧画面。";
        }
        if (isPhone) {
            return "连接电脑后将自动匹配手机比例创建虚拟扩展屏，出画后自动切换横屏，支持低延迟画面与反向触控。";
        }
        return "连接电脑后将自动匹配平板比例创建虚拟扩展屏，支持低延迟画面与反向触控。";
    }

    private int statusAccentColor() {
        if (frameReady) {
            return palette.accent;
        }
        if (link.state == AccessoryLink.State.CONNECTED) {
            return palette.positive;
        }
        if (link.state == AccessoryLink.State.AUTHORIZING) {
            return palette.warning;
        }
        if (link.state == AccessoryLink.State.ERROR) {
            return palette.danger;
        }
        return palette.inkMuted;
    }

    private String codecDisplayName(String value) {
        if ("hevc".equals(value)) {
            return "HEVC (H.265)";
        }
        if ("h264".equals(value)) {
            return "H.264 (AVC)";
        }
        return "自动（优先 HEVC）";
    }

    private static String normalizeCodec(String value) {
        if ("hevc".equals(value) || "h264".equals(value)) {
            return value;
        }
        return "auto";
    }

    private View makePageHeader(String titleText, float titleSp) {
        final TextView title = new TextView(this);
        title.setText(titleText);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, titleSp);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(palette.ink);
        return title;
    }

    private View makeSectionHeader(String titleText) {
        final TextView tv = new TextView(this);
        tv.setText(titleText);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setTextColor(palette.ink);
        tv.setPadding(0, dp(4f), 0, dp(2f));
        return tv;
    }

    private View makePrimaryButton(String label, boolean fullWidth, Runnable onTap) {
        final LinearLayout btn = new LinearLayout(this);
        btn.setOrientation(LinearLayout.HORIZONTAL);
        btn.setGravity(Gravity.CENTER);
        btn.setBackground(StudioDrawables.primaryButton(this, palette));
        btn.setPadding(
            dp(fullWidth ? 18f : 22f), dp(fullWidth ? 12f : 10f),
            dp(fullWidth ? 18f : 22f), dp(fullWidth ? 12f : 10f));
        btn.setLayoutParams(new LinearLayout.LayoutParams(
            fullWidth ? ViewGroup.LayoutParams.MATCH_PARENT : ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT));

        final TextView tv = new TextView(this);
        tv.setText(label);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setTextColor(palette.accentFg);
        btn.addView(tv, rightMarginLp(8f));

        final TextView arrow = new TextView(this);
        arrow.setText("→");
        arrow.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        arrow.setTextColor(palette.accentFg);
        btn.addView(arrow);

        btn.setOnClickListener(v -> onTap.run());
        return btn;
    }

    private View makeSmallActionButton(String label, boolean accentTone, Runnable onTap) {
        final TextView btn = new TextView(this);
        btn.setText(label);
        btn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        btn.setTypeface(accentTone ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        btn.setTextColor(accentTone ? palette.accent : palette.ink);
        btn.setGravity(Gravity.CENTER);
        btn.setPadding(dp(14f), 0, dp(14f), 0);
        btn.setBackground(StudioDrawables.card(
            this,
            accentTone ? palette.accentSoft : palette.surface,
            accentTone ? palette.accentBorder : palette.hairline,
            11f, 0.8f));
        btn.setLayoutParams(new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, dp(36f)));
        btn.setOnClickListener(v -> onTap.run());
        return btn;
    }

    private void styleSwitch(Switch sw, boolean checked) {
        sw.setThumbTintList(ColorStateList.valueOf(Color.WHITE));
        sw.setTrackTintList(ColorStateList.valueOf(
            checked ? palette.accent : palette.hairline));
    }

    private TextView makeHudText(String text, int color, float sp, boolean bold) {
        final TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextColor(color);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        tv.setTypeface(bold ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        tv.setMaxLines(1);
        return tv;
    }

    private View makeHudDivider() {
        final View v = new View(this);
        v.setBackgroundColor(StudioDrawables.COLOR_HUD_DIVIDER);
        final LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            Math.max(1, dp(1f)), dp(12f));
        lp.leftMargin = dp(isPhone ? 7f : 10f);
        lp.rightMargin = dp(isPhone ? 7f : 10f);
        v.setLayoutParams(lp);
        return v;
    }

    private View makeHairlineDivider(int color, float strokeDp) {
        return makeHairlineDivider(color, strokeDp, 0f);
    }

    private View makeHairlineDivider(int color, float strokeDp, float bottomMarginDp) {
        final View divider = new View(this);
        divider.setBackgroundColor(color);
        final LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(strokeDp)));
        if (bottomMarginDp > 0f) {
            lp.bottomMargin = dp(bottomMarginDp);
        }
        divider.setLayoutParams(lp);
        return divider;
    }

    private void clampMaxWidth(LinearLayout view, int maxWidthDp) {
        final float widthDp = DeviceLayout.windowWidthDp(this);
        if (widthDp > maxWidthDp) {
            final ViewGroup.LayoutParams lp = view.getLayoutParams();
            lp.width = dp(maxWidthDp);
            view.setLayoutParams(lp);
        }
    }

    private LinearLayout.LayoutParams bottomMarginLp(float dpVal) {
        final LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(dpVal);
        return lp;
    }

    private LinearLayout.LayoutParams rightMarginLp(float dpVal) {
        final LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.rightMargin = dp(dpVal);
        return lp;
    }

    private int dp(float value) {
        return DeviceLayout.dp(this, value);
    }

    private void startSession() {
        if (started || pendingDescriptor == null || surface == null || !surface.isValid()) {
            return;
        }
        final int width = surfaceView.getWidth();
        final int height = surfaceView.getHeight();
        if (width <= 0 || height <= 0) {
            return;
        }
        final int refreshHz = Math.round(currentRefreshHz());
        NativeBridge.setPreferredCodec(nativeHandle, preferredCodec);
        NativeBridge.setViewport(nativeHandle, width, height);
        started = NativeBridge.start(nativeHandle, pendingDescriptor.getFd(),
                                     surface, width, height, refreshHz);
        if (!started) {
            link = new AccessoryLink.Snapshot(
                AccessoryLink.State.ERROR, "failed to start native client engine", -1);
            renderAll();
        } else {
            pendingDescriptor = null;
        }
    }

    @SuppressWarnings("deprecation")
    private float currentRefreshHz() {
        final Display display;
        if (Build.VERSION.SDK_INT >= 30) {
            display = getDisplay();
        } else {
            display = ((WindowManager) getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay();
        }
        if (display == null) {
            return 60.0f;
        }
        float bestHz = display.getRefreshRate();
        final Display.Mode current = display.getMode();
        final Display.Mode[] modes = display.getSupportedModes();
        if (current != null && modes != null) {
            final int curW = current.getPhysicalWidth();
            final int curH = current.getPhysicalHeight();
            for (final Display.Mode mode : modes) {
                if (mode.getPhysicalWidth() == curW &&
                    mode.getPhysicalHeight() == curH &&
                    mode.getRefreshRate() > bestHz) {
                    bestHz = mode.getRefreshRate();
                }
            }
        }
        return bestHz > 0.0f ? bestHz : 60.0f;
    }

    private static float parseNegotiatedRefreshHz(String resolution) {
        if (resolution == null) {
            return 0.0f;
        }
        final int at = resolution.lastIndexOf('@');
        if (at < 0 || at + 1 >= resolution.length()) {
            return 0.0f;
        }
        try {
            final float hz = Float.parseFloat(resolution.substring(at + 1).trim());
            return hz > 0.0f ? hz : 0.0f;
        } catch (NumberFormatException ignored) {
            return 0.0f;
        }
    }

    private void applyWindowPreferredRefreshRate(float hz) {
        final float target = hz > 0.0f ? hz : 0.0f;
        if (Math.abs(target - lastPreferredRefreshHz) < 0.1f) {
            return;
        }
        lastPreferredRefreshHz = target;
        final WindowManager.LayoutParams params = getWindow().getAttributes();
        params.preferredRefreshRate = target;
        getWindow().setAttributes(params);
    }

    private void stopSession() {
        if (nativeHandle == 0L) {
            return;
        }
        pendingDescriptor = null;
        NativeBridge.requestStop(nativeHandle);
        if (accessoryLink != null) {
            accessoryLink.close();
        }
        NativeBridge.stop(nativeHandle);
        started = false;
    }

    private boolean handleTouch(MotionEvent event) {
        if (!started || !touchCaptureActive) {
            return false;
        }
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_MOVE:
                sendPointerFrame(event);
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                sendPointerFrameExcluding(event, event.getActionIndex());
                break;
            case MotionEvent.ACTION_CANCEL:
                NativeBridge.touchFrame(nativeHandle, true, 0, null, null, null);
                break;
            default:
                break;
        }
        return true;
    }

    private void sendPointerFrame(MotionEvent event) {
        final int count = event.getPointerCount();
        ensureCapacity(count);
        for (int index = 0; index < count; ++index) {
            pointerIds[index] = event.getPointerId(index);
            pointerXs[index] = event.getX(index);
            pointerYs[index] = event.getY(index);
        }
        NativeBridge.touchFrame(nativeHandle, false, count, pointerIds, pointerXs, pointerYs);
    }

    private void sendPointerFrameExcluding(MotionEvent event, int skipIndex) {
        final int count = event.getPointerCount();
        ensureCapacity(count);
        int written = 0;
        for (int index = 0; index < count; ++index) {
            if (index == skipIndex) {
                continue;
            }
            pointerIds[written] = event.getPointerId(index);
            pointerXs[written] = event.getX(index);
            pointerYs[written] = event.getY(index);
            ++written;
        }
        NativeBridge.touchFrame(nativeHandle, false, written, pointerIds, pointerXs, pointerYs);
    }

    private void ensureCapacity(int count) {
        if (pointerIds.length >= count) {
            return;
        }
        pointerIds = new int[count];
        pointerXs = new float[count];
        pointerYs = new float[count];
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        surface = holder.getSurface();
        NativeBridge.setSurface(nativeHandle, surface);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        final Surface holderSurface = holder.getSurface();
        if (holderSurface != surface) {
            surface = holderSurface;
            NativeBridge.setSurface(nativeHandle, surface);
        }
        NativeBridge.setViewport(nativeHandle, width, height);
        startSession();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        NativeBridge.setSurface(nativeHandle, null);
        surface = null;
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacks(refreshTick);
        handler.removeCallbacks(retryAccessory);
        if (nativeHandle != 0L) {
            NativeBridge.setTouchEnabled(nativeHandle, false);
        }
        stopSession();
        if (accessoryLink != null) {
            accessoryLink.stop();
        }
        if (nativeHandle != 0L) {
            NativeBridge.destroy(nativeHandle);
            nativeHandle = 0L;
        }
        super.onDestroy();
    }
}
