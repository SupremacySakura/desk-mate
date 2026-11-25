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

    private SurfaceView surfaceView;
    private LinearLayout shellContainer;
    private LinearLayout headerContainer;
    private ScrollView scrollView;
    private LinearLayout contentContainer;
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
    private TextView eyebrowView;
    private View eyebrowDotView;
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
    private final List<View> codecOptionDots = new ArrayList<>();
    private final List<TextView> codecOptionTitles = new ArrayList<>();
    private final List<TextView> codecOptionSubtitles = new ArrayList<>();

    private boolean hudStateInitialized;
    private boolean lastHudExpanded;
    private boolean lastHudShowFps;
    private int lastHudFps = -1;
    private String lastHudBitrateText = "";
    private String lastHudCodec = "";
    private String lastHudRes = "";
    private long lastHudDrops = -1L;
    private Boolean lastBadgeHighlight;
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

        prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        showFps = prefs.getBoolean(KEY_SHOW_FPS, false);
        preferredCodec = normalizeCodec(prefs.getString(KEY_CODEC, "auto"));
        isPhone = DeviceLayout.isPhoneDevice(this);
        compactLayout = DeviceLayout.isCompactLayout(this);

        final View root = findViewById(R.id.root);
        surfaceView = findViewById(R.id.video_surface);
        shellContainer = findViewById(R.id.shell_container);
        displayHudContainer = findViewById(R.id.display_hud);

        shellContainer.setBackground(StudioDrawables.alabasterBackground());
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
        compactLayout = DeviceLayout.isCompactLayout(this);
        lastStructureKey = "";
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
        headerContainer.setBackgroundColor(StudioDrawables.COLOR_HEADER_CHROME);
        shellContainer.addView(headerContainer, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        shellContainer.addView(makeHairlineDivider(StudioDrawables.COLOR_HAIRLINE, 0.8f));

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

        shellContainer.addView(makeHairlineDivider(StudioDrawables.COLOR_HAIRLINE, 0.8f));

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
        hudReturnBtn.setTextColor(StudioDrawables.COLOR_INK);
        hudReturnBtn.setGravity(Gravity.CENTER);
        hudReturnBtn.setBackground(StudioDrawables.card(
            this, StudioDrawables.COLOR_RETURN_BTN_BG, StudioDrawables.COLOR_HAIRLINE, 4f, 0.8f));
        hudReturnBtn.setPadding(dp(isPhone ? 11f : 14f), 0, dp(isPhone ? 11f : 14f), 0);
        final LinearLayout.LayoutParams retLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, dp(isPhone ? 34f : 40f));
        retLp.rightMargin = dp(8f);
        hudReturnBtn.setOnClickListener(v -> navigate(Page.HOME));
        displayHudContainer.addView(hudReturnBtn, retLp);

        hudToggleBtn = new TextView(this);
        hudToggleBtn.setText("▾");
        hudToggleBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, isPhone ? 15f : 17f);
        hudToggleBtn.setTextColor(StudioDrawables.COLOR_HUD_WHITE);
        hudToggleBtn.setGravity(Gravity.CENTER);
        hudToggleBtn.setBackground(StudioDrawables.card(
            this, StudioDrawables.COLOR_HUD_BG, StudioDrawables.COLOR_HUD_BORDER, 4f, 0.8f));
        hudToggleBtn.setOnClickListener(v -> {
            controlsExpanded = !controlsExpanded;
            updateDisplayHudState();
        });
        displayHudContainer.addView(hudToggleBtn, new LinearLayout.LayoutParams(
            dp(isPhone ? 34f : 40f), dp(isPhone ? 34f : 40f)));

        final View spacer = new View(this);
        displayHudContainer.addView(spacer, new LinearLayout.LayoutParams(
            0, 1, 1f));

        hudMetricsPill = new LinearLayout(this);
        hudMetricsPill.setOrientation(LinearLayout.HORIZONTAL);
        hudMetricsPill.setGravity(Gravity.CENTER_VERTICAL);
        hudMetricsPill.setClickable(false);
        hudMetricsPill.setFocusable(false);
        hudMetricsPill.setBackground(StudioDrawables.card(
            this, StudioDrawables.COLOR_HUD_BG, StudioDrawables.COLOR_HUD_BORDER, 4f, 0.8f));
        hudMetricsPill.setPadding(
            dp(isPhone ? 9f : 12f), dp(isPhone ? 5f : 7f),
            dp(isPhone ? 9f : 12f), dp(isPhone ? 5f : 7f));

        final View pillDot = new View(this);
        pillDot.setBackgroundColor(StudioDrawables.COLOR_CRIMSON);
        final LinearLayout.LayoutParams pillDotLp = new LinearLayout.LayoutParams(dp(5f), dp(5f));
        pillDotLp.rightMargin = dp(isPhone ? 7f : 10f);
        hudMetricsPill.addView(pillDot, pillDotLp);

        hudFpsValue = makeHudText("0", StudioDrawables.COLOR_HUD_FPS, isPhone ? 11.5f : 13f, true);
        final TextView fpsUnit = makeHudText("FPS", StudioDrawables.COLOR_HUD_LABEL, isPhone ? 10f : 10.5f, false);
        hudMetricsPill.addView(hudFpsValue, rightMarginLp(4f));
        hudMetricsPill.addView(fpsUnit, rightMarginLp(isPhone ? 7f : 10f));

        hudMetricsPill.addView(makeHudDivider());

        hudBitrateValue = makeHudText("0.0", StudioDrawables.COLOR_HUD_WHITE, isPhone ? 11.5f : 13f, true);
        final TextView mbpsUnit = makeHudText("Mbps", StudioDrawables.COLOR_HUD_LABEL, isPhone ? 10f : 10.5f, false);
        hudMetricsPill.addView(hudBitrateValue, rightMarginLp(4f));
        hudMetricsPill.addView(mbpsUnit, rightMarginLp(isPhone ? 7f : 10f));

        hudMetricsPill.addView(makeHudDivider());

        hudCodecValue = makeHudText("--", StudioDrawables.COLOR_HUD_CODEC, isPhone ? 11f : 12f, true);
        hudMetricsPill.addView(hudCodecValue);

        hudResDivider = makeHudDivider();
        hudMetricsPill.addView(hudResDivider);
        hudResValue = makeHudText("", StudioDrawables.COLOR_HUD_RES, isPhone ? 11f : 12f, false);
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
                if (!enteredThisLink) {
                    enteredThisLink = true;
                    navigate(Page.DISPLAY);
                    return;
                }
            } else if (!"streaming".equals(engine.session) &&
                       !"paused".equals(engine.session)) {
                frameReady = false;
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
                    Math.max(safeInsets.top + dp(8f), dp(22f)),
                    dp(28f) + safeInsets.right,
                    dp(8f));
            }
        }
        if (bottomContainer != null) {
            if (compactLayout) {
                bottomContainer.setPadding(
                    dp(8f) + safeInsets.left, 0,
                    dp(8f) + safeInsets.right, safeInsets.bottom);
            } else {
                bottomContainer.setMinimumHeight(dp(30f) + safeInsets.bottom);
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

        final String structureKey = compactLayout + "|" + page.name() + "|" + link.state.name()
            + "|" + frameReady + "|" + codecChangedWhileConnected + "|" + isPhone;
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
        lastBadgeHighlight = null;
        lastSelectedCodecCard = null;
        schematicView = null;
        topStatusTextView = null;
        topStatusDotView = null;
        topStatusBadgeView = null;
        eyebrowView = null;
        eyebrowDotView = null;
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
        codecOptionDots.clear();
        codecOptionTitles.clear();
        codecOptionSubtitles.clear();

        buildHeader();
        buildScrollBody();
        buildBottomBar();
    }

    private void buildHeader() {
        headerContainer.removeAllViews();
        if (compactLayout) {
            if (page != Page.HOME) {
                headerContainer.addView(makeBackHeaderButton(dp(32f), dp(11f)));
            } else {
                headerContainer.addView(makeBrandTitle(false));
            }
            final View spacer = new View(this);
            headerContainer.addView(spacer, new LinearLayout.LayoutParams(0, 1, 1f));

            topStatusBadgeView = new LinearLayout(this);
            topStatusBadgeView.setOrientation(LinearLayout.HORIZONTAL);
            topStatusBadgeView.setGravity(Gravity.CENTER_VERTICAL);
            topStatusBadgeView.setPadding(dp(9f), dp(4f), dp(9f), dp(4f));

            topStatusDotView = new View(this);
            final LinearLayout.LayoutParams dotLp = new LinearLayout.LayoutParams(dp(4f), dp(4f));
            dotLp.rightMargin = dp(6f);
            topStatusBadgeView.addView(topStatusDotView, dotLp);

            topStatusTextView = new TextView(this);
            topStatusTextView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
            topStatusTextView.setTypeface(Typeface.DEFAULT_BOLD);
            topStatusBadgeView.addView(topStatusTextView);

            headerContainer.addView(topStatusBadgeView);
            return;
        }

        if (page != Page.HOME) {
            headerContainer.addView(makeBackHeaderButton(dp(34f), dp(12f)));
        } else {
            headerContainer.addView(makeBrandTitle(true));
        }
        final View spacer = new View(this);
        headerContainer.addView(spacer, new LinearLayout.LayoutParams(0, 1, 1f));

        final LinearLayout tabsRow = new LinearLayout(this);
        tabsRow.setOrientation(LinearLayout.HORIZONTAL);
        tabsRow.setGravity(Gravity.CENTER_VERTICAL);
        tabsRow.addView(makeHeaderTab(Page.HOME, "DISPLAY", "扩展显示"), rightMarginLp(8f));
        tabsRow.addView(makeHeaderTab(Page.SETTINGS, "SETTINGS", "系统设置"), rightMarginLp(8f));
        tabsRow.addView(makeHeaderTab(Page.DIAGNOSTICS, "DIAGNOSTICS", "连接诊断"));
        headerContainer.addView(tabsRow);
    }

    private View makeBrandTitle(boolean showSubtitle) {
        final LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);

        topStatusDotView = new View(this);
        final LinearLayout.LayoutParams dotLp = new LinearLayout.LayoutParams(dp(5f), dp(5f));
        dotLp.rightMargin = dp(8f);
        row.addView(topStatusDotView, dotLp);

        final TextView brand = new TextView(this);
        brand.setText("DESKMATE");
        brand.setTextSize(TypedValue.COMPLEX_UNIT_SP, showSubtitle ? 14f : 13.5f);
        brand.setTypeface(Typeface.DEFAULT_BOLD);
        brand.setLetterSpacing(showSubtitle ? 0.14f : 0.12f);
        brand.setTextColor(StudioDrawables.COLOR_INK);
        row.addView(brand);

        if (showSubtitle) {
            final TextView sub = new TextView(this);
            sub.setText("  // 扩展屏终端");
            sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
            sub.setTextColor(StudioDrawables.COLOR_INK_SUBTLE);
            row.addView(sub);
        }
        return row;
    }

    private View makeBackHeaderButton(int heightPx, int padHPx) {
        final LinearLayout btn = new LinearLayout(this);
        btn.setOrientation(LinearLayout.HORIZONTAL);
        btn.setGravity(Gravity.CENTER_VERTICAL);
        btn.setPadding(padHPx, 0, padHPx, 0);
        btn.setBackground(StudioDrawables.card(
            this, StudioDrawables.COLOR_WHITE, StudioDrawables.COLOR_HAIRLINE, 3f, 0.8f));
        btn.setLayoutParams(new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, heightPx));

        final TextView arrow = new TextView(this);
        arrow.setText("←");
        arrow.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        arrow.setTextColor(StudioDrawables.COLOR_CRIMSON);
        btn.addView(arrow, rightMarginLp(6f));

        final TextView label = new TextView(this);
        label.setText("返回");
        label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        label.setTextColor(StudioDrawables.COLOR_INK);
        btn.addView(label);

        btn.setOnClickListener(v -> onBackPressed());
        return btn;
    }

    private View makeHeaderTab(Page target, String en, String zh) {
        final boolean active = (page == target);
        final LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        col.setPadding(dp(6f), dp(6f), dp(6f), dp(2f));

        final LinearLayout topRow = new LinearLayout(this);
        topRow.setOrientation(LinearLayout.HORIZONTAL);
        topRow.setGravity(Gravity.CENTER);

        final View dot = new View(this);
        dot.setBackground(StudioDrawables.squareIndicator(
            this,
            active ? StudioDrawables.COLOR_CRIMSON : Color.TRANSPARENT,
            active ? StudioDrawables.COLOR_CRIMSON : StudioDrawables.COLOR_INK_SUBTLE,
            0.8f));
        final LinearLayout.LayoutParams dotLp = new LinearLayout.LayoutParams(dp(4f), dp(4f));
        dotLp.rightMargin = dp(6f);
        topRow.addView(dot, dotLp);

        final TextView label = new TextView(this);
        label.setText(en + " / " + zh);
        label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        label.setTypeface(active ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        label.setTextColor(active ? StudioDrawables.COLOR_CRIMSON : StudioDrawables.COLOR_INK_MUTED);
        topRow.addView(label);

        col.addView(topRow, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        final View underline = new View(this);
        underline.setBackgroundColor(active ? StudioDrawables.COLOR_CRIMSON : Color.TRANSPARENT);
        final LinearLayout.LayoutParams lineLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, Math.max(2, dp(1.5f)));
        lineLp.topMargin = dp(5f);
        col.addView(underline, lineLp);

        col.setOnClickListener(v -> navigate(target));
        return col;
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
        wrapper.setPadding(dp(28f), dp(18f), dp(28f), dp(18f));

        final LinearLayout leftCol = new LinearLayout(this);
        leftCol.setOrientation(LinearLayout.VERTICAL);
        leftCol.addView(schematicView);
        final LinearLayout.LayoutParams leftLp = new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.43f);
        leftLp.rightMargin = dp(28f);
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
            () -> navigate(Page.SETTINGS)), rightMarginLp(12f));
        quickRow.addView(makeSmallActionButton("查看连接诊断", true,
            () -> navigate(Page.DIAGNOSTICS)));
        rightCol.addView(quickRow);

        wrapper.addView(rightCol, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.57f));

        final LinearLayout.LayoutParams wrapLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        contentContainer.addView(wrapper, wrapLp);
        clampMaxWidth(wrapper, 1040);
    }

    private View buildStatusHeaderBlock(boolean compact) {
        final LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        final LinearLayout eyebrowRow = new LinearLayout(this);
        eyebrowRow.setOrientation(LinearLayout.HORIZONTAL);
        eyebrowRow.setGravity(Gravity.CENTER_VERTICAL);

        eyebrowDotView = new View(this);
        final LinearLayout.LayoutParams dotLp = new LinearLayout.LayoutParams(dp(5f), dp(5f));
        dotLp.rightMargin = dp(7f);
        eyebrowRow.addView(eyebrowDotView, dotLp);

        eyebrowView = new TextView(this);
        eyebrowView.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 10f : 10.5f);
        eyebrowView.setTypeface(Typeface.DEFAULT_BOLD);
        eyebrowView.setLetterSpacing(0.11f);
        eyebrowRow.addView(eyebrowView);
        col.addView(eyebrowRow, bottomMarginLp(8f));

        statusTitleView = new TextView(this);
        statusTitleView.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 22f : 26f);
        statusTitleView.setTextColor(StudioDrawables.COLOR_INK);
        col.addView(statusTitleView, bottomMarginLp(8f));

        statusDetailView = new TextView(this);
        statusDetailView.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 13f : 13.5f);
        statusDetailView.setLineSpacing(dp(3f), 1.0f);
        statusDetailView.setTextColor(StudioDrawables.COLOR_INK_SECONDARY);
        col.addView(statusDetailView);

        return col;
    }

    private View buildConnectionGuideCard() {
        final LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(StudioDrawables.card(
            this, StudioDrawables.COLOR_WHITE, StudioDrawables.COLOR_HAIRLINE, 4f, 0.8f));

        card.addView(makeStepRow("01", "确保 Mac 上的 DeskMate 已启动运行"));
        card.addView(makeHairlineDivider(StudioDrawables.COLOR_SOFT_HAIRLINE, 0.75f));
        card.addView(makeStepRow("02", "使用高速 USB 数据线连接 Mac 与" + deviceLabel()));
        card.addView(makeHairlineDivider(StudioDrawables.COLOR_SOFT_HAIRLINE, 0.75f));
        card.addView(makeStepRow("03", "弹出询问时点击允许访问 USB 配件"));
        return card;
    }

    private View makeStepRow(String num, String text) {
        final LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12f), dp(9f), dp(12f), dp(9f));

        final TextView numView = new TextView(this);
        numView.setText(num);
        numView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        numView.setTypeface(Typeface.DEFAULT_BOLD);
        numView.setTextColor(StudioDrawables.COLOR_CRIMSON);
        row.addView(numView, new LinearLayout.LayoutParams(dp(34f), ViewGroup.LayoutParams.WRAP_CONTENT));

        final TextView textView = new TextView(this);
        textView.setText(text);
        textView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        textView.setTextColor(StudioDrawables.COLOR_INK_BODY);
        row.addView(textView, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return row;
    }

    private View buildLiveSpecCard() {
        final LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(12f), dp(12f), dp(12f), dp(12f));
        card.setBackground(StudioDrawables.card(
            this, StudioDrawables.COLOR_WHITE, StudioDrawables.COLOR_HAIRLINE, 4f, 0.8f));

        final LinearLayout specRow = new LinearLayout(this);
        specRow.setOrientation(LinearLayout.HORIZONTAL);
        specRow.setGravity(Gravity.CENTER_VERTICAL);

        final TextView specLabel = new TextView(this);
        specLabel.setText("显示规格");
        specLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        specLabel.setTextColor(StudioDrawables.COLOR_INK_SUBTLE);
        specRow.addView(specLabel, new LinearLayout.LayoutParams(
            dp(68f), ViewGroup.LayoutParams.WRAP_CONTENT));

        liveSpecValueView = new TextView(this);
        liveSpecValueView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        liveSpecValueView.setTypeface(Typeface.DEFAULT_BOLD);
        liveSpecValueView.setTextColor(StudioDrawables.COLOR_INK);
        specRow.addView(liveSpecValueView, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        card.addView(specRow);

        if (frameReady) {
            final View div = makeHairlineDivider(StudioDrawables.COLOR_SOFT_HAIRLINE, 0.75f);
            final LinearLayout.LayoutParams divLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(0.75f)));
            divLp.topMargin = dp(8f);
            divLp.bottomMargin = dp(8f);
            card.addView(div, divLp);

            livePerfRowView = new LinearLayout(this);
            livePerfRowView.setOrientation(LinearLayout.HORIZONTAL);
            livePerfRowView.setGravity(Gravity.CENTER_VERTICAL);

            final TextView perfLabel = new TextView(this);
            perfLabel.setText("实时性能");
            perfLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
            perfLabel.setTextColor(StudioDrawables.COLOR_CRIMSON);
            livePerfRowView.addView(perfLabel, new LinearLayout.LayoutParams(
                dp(68f), ViewGroup.LayoutParams.WRAP_CONTENT));

            livePerfValueView = new TextView(this);
            livePerfValueView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
            livePerfValueView.setTypeface(Typeface.DEFAULT_BOLD);
            livePerfValueView.setTextColor(StudioDrawables.COLOR_INK);
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
        wrapper.setPadding(dp(28f), dp(16f), dp(28f), dp(24f));

        final LinearLayout leftCol = new LinearLayout(this);
        leftCol.setOrientation(LinearLayout.VERTICAL);
        schematicView = new WiredLinkSchematicView(this);
        leftCol.addView(schematicView, bottomMarginLp(10f));

        final LinearLayout prefCard = new LinearLayout(this);
        prefCard.setOrientation(LinearLayout.VERTICAL);
        prefCard.setPadding(dp(12f), dp(12f), dp(12f), dp(12f));
        prefCard.setBackground(StudioDrawables.card(
            this, StudioDrawables.COLOR_WHITE, StudioDrawables.COLOR_HAIRLINE, 4f, 0.8f));

        final TextView prefLabel = new TextView(this);
        prefLabel.setText("当前编码偏好");
        prefLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        prefLabel.setTextColor(StudioDrawables.COLOR_INK_SUBTLE);
        prefCard.addView(prefLabel, bottomMarginLp(4f));

        settingsCodecStatusView = new TextView(this);
        settingsCodecStatusView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        settingsCodecStatusView.setTypeface(Typeface.DEFAULT_BOLD);
        settingsCodecStatusView.setTextColor(StudioDrawables.COLOR_INK);
        prefCard.addView(settingsCodecStatusView);

        leftCol.addView(prefCard);
        final LinearLayout.LayoutParams leftLp = new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.34f);
        leftLp.rightMargin = dp(26f);
        wrapper.addView(leftCol, leftLp);

        final LinearLayout rightCol = new LinearLayout(this);
        rightCol.setOrientation(LinearLayout.VERTICAL);
        appendSettingsFields(rightCol, false);
        wrapper.addView(rightCol, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.66f));

        contentContainer.addView(wrapper, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        clampMaxWidth(wrapper, 1040);
    }

    private void appendSettingsFields(LinearLayout parent, boolean compact) {
        parent.addView(makePageHeader(
            "SETTINGS // 显示与解码配置", "系统设置", compact ? 20f : 21f), bottomMarginLp(14f));

        if (compact) {
            final LinearLayout compactSpecCard = new LinearLayout(this);
            compactSpecCard.setOrientation(LinearLayout.HORIZONTAL);
            compactSpecCard.setGravity(Gravity.CENTER_VERTICAL);
            compactSpecCard.setPadding(dp(12f), dp(10f), dp(12f), dp(10f));
            compactSpecCard.setBackground(StudioDrawables.card(
                this, StudioDrawables.COLOR_WHITE, StudioDrawables.COLOR_HAIRLINE, 4f, 0.8f));

            final TextView lbl = new TextView(this);
            lbl.setText("当前编码与规格");
            lbl.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
            lbl.setTextColor(StudioDrawables.COLOR_INK_SUBTLE);
            compactSpecCard.addView(lbl, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

            settingsCodecStatusView = new TextView(this);
            settingsCodecStatusView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
            settingsCodecStatusView.setTypeface(Typeface.DEFAULT_BOLD);
            settingsCodecStatusView.setTextColor(StudioDrawables.COLOR_INK);
            compactSpecCard.addView(settingsCodecStatusView);

            parent.addView(compactSpecCard, bottomMarginLp(14f));
        }

        parent.addView(makeSectionHeader("DISPLAY", "画面与性能浮窗"), bottomMarginLp(10f));

        final LinearLayout fpsCard = new LinearLayout(this);
        fpsCard.setOrientation(LinearLayout.HORIZONTAL);
        fpsCard.setGravity(Gravity.CENTER_VERTICAL);
        fpsCard.setPadding(dp(14f), dp(10f), dp(14f), dp(10f));
        fpsCard.setBackground(StudioDrawables.card(
            this, StudioDrawables.COLOR_WHITE, StudioDrawables.COLOR_HAIRLINE, 4f, 0.8f));

        final LinearLayout fpsLabels = new LinearLayout(this);
        fpsLabels.setOrientation(LinearLayout.VERTICAL);
        final TextView fpsTitle = new TextView(this);
        fpsTitle.setText("性能参数浮窗");
        fpsTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14.5f);
        fpsTitle.setTypeface(Typeface.DEFAULT_BOLD);
        fpsTitle.setTextColor(StudioDrawables.COLOR_INK);
        fpsLabels.addView(fpsTitle, bottomMarginLp(3f));

        final TextView fpsSub = new TextView(this);
        fpsSub.setText("全屏投屏时在右上角显示帧率、码率、编码格式与丢帧统计");
        fpsSub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        fpsSub.setTextColor(StudioDrawables.COLOR_INK_MUTED);
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
            oriCard.setPadding(dp(14f), dp(10f), dp(14f), dp(10f));
            oriCard.setBackground(StudioDrawables.card(
                this, StudioDrawables.COLOR_WHITE, StudioDrawables.COLOR_HAIRLINE, 4f, 0.8f));

            final TextView oriTitle = new TextView(this);
            oriTitle.setText("横竖屏自动切换");
            oriTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
            oriTitle.setTypeface(Typeface.DEFAULT_BOLD);
            oriTitle.setTextColor(StudioDrawables.COLOR_INK);
            oriCard.addView(oriTitle, bottomMarginLp(3f));

            final TextView oriSub = new TextView(this);
            oriSub.setText("连接前保持竖屏便于配置，进入扩展画面后自动切换横屏并支持左右翻转");
            oriSub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            oriSub.setLineSpacing(dp(2f), 1.0f);
            oriSub.setTextColor(StudioDrawables.COLOR_INK_MUTED);
            oriCard.addView(oriSub);

            parent.addView(oriCard, bottomMarginLp(14f));
        }

        parent.addView(makeSectionHeader("CODEC", "视频编码格式"), bottomMarginLp(8f));

        settingsCodecHintView = new TextView(this);
        settingsCodecHintView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        settingsCodecHintView.setTextColor(StudioDrawables.COLOR_INK_MUTED);
        parent.addView(settingsCodecHintView, bottomMarginLp(10f));

        final LinearLayout codecRow = new LinearLayout(this);
        codecRow.setOrientation(LinearLayout.HORIZONTAL);
        for (int index = 0; index < CODEC_VALUES.length; index++) {
            final String val = CODEC_VALUES[index];
            final LinearLayout card = new LinearLayout(this);
            card.setOrientation(LinearLayout.VERTICAL);
            card.setGravity(Gravity.CENTER_HORIZONTAL);
            card.setPadding(
                dp(compact ? 6f : 10f), dp(10f),
                dp(compact ? 6f : 10f), dp(10f));

            final LinearLayout titleRow = new LinearLayout(this);
            titleRow.setOrientation(LinearLayout.HORIZONTAL);
            titleRow.setGravity(Gravity.CENTER_VERTICAL);

            final View dot = new View(this);
            final LinearLayout.LayoutParams dotLp = new LinearLayout.LayoutParams(dp(4f), dp(4f));
            dotLp.rightMargin = dp(compact ? 4f : 6f);
            titleRow.addView(dot, dotLp);

            final TextView titleTv = new TextView(this);
            titleTv.setText(CODEC_TITLES[index]);
            titleTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 13f : 14f);
            titleRow.addView(titleTv);

            final LinearLayout.LayoutParams titleRowLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            titleRowLp.bottomMargin = dp(4f);
            card.addView(titleRow, titleRowLp);

            final TextView subTv = new TextView(this);
            subTv.setText(CODEC_SUBTITLES[index]);
            subTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 10.5f : 11f);
            subTv.setGravity(Gravity.CENTER_HORIZONTAL);
            card.addView(subTv, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

            card.setOnClickListener(v -> selectCodec(val));

            codecOptionCards.add(card);
            codecOptionDots.add(dot);
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
            banner.setPadding(dp(12f), dp(8f), dp(12f), dp(8f));
            banner.setBackground(StudioDrawables.card(
                this, StudioDrawables.COLOR_ROSE_WASH, StudioDrawables.COLOR_ROSE_BORDER, 3f, 0.8f));

            final TextView msg = new TextView(this);
            msg.setText("编码偏好已更新，重新连接 USB 会话后生效");
            msg.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            msg.setTextColor(StudioDrawables.COLOR_CRIMSON);
            final LinearLayout.LayoutParams msgLp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            msgLp.rightMargin = dp(12f);
            banner.addView(msg, msgLp);

            final TextView reconnBtn = new TextView(this);
            reconnBtn.setText("立即重连");
            reconnBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
            reconnBtn.setTypeface(Typeface.DEFAULT_BOLD);
            reconnBtn.setTextColor(StudioDrawables.COLOR_WHITE);
            reconnBtn.setGravity(Gravity.CENTER);
            reconnBtn.setPadding(dp(12f), 0, dp(12f), 0);
            reconnBtn.setBackground(StudioDrawables.card(
                this, StudioDrawables.COLOR_CRIMSON, StudioDrawables.COLOR_CRIMSON_PRESSED, 3f, 0.8f));
            reconnBtn.setOnClickListener(v -> {
                codecChangedWhileConnected = false;
                accessoryLink.reconnect();
            });
            banner.addView(reconnBtn, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, dp(30f)));

            parent.addView(banner, bottomMarginLp(14f));
        }

        parent.addView(makeSectionHeader("DIAGNOSTICS", "实时诊断"), bottomMarginLp(10f));

        final LinearLayout diagEntry = new LinearLayout(this);
        diagEntry.setOrientation(LinearLayout.HORIZONTAL);
        diagEntry.setGravity(Gravity.CENTER_VERTICAL);
        diagEntry.setPadding(dp(14f), dp(10f), dp(14f), dp(10f));
        diagEntry.setBackground(StudioDrawables.card(
            this, StudioDrawables.COLOR_WHITE, StudioDrawables.COLOR_HAIRLINE, 4f, 0.8f));

        final LinearLayout diagLabels = new LinearLayout(this);
        diagLabels.setOrientation(LinearLayout.VERTICAL);
        final TextView diagTitle = new TextView(this);
        diagTitle.setText("查看连接与解码统计");
        diagTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        diagTitle.setTypeface(Typeface.DEFAULT_BOLD);
        diagTitle.setTextColor(StudioDrawables.COLOR_INK);
        diagLabels.addView(diagTitle, bottomMarginLp(2f));

        final TextView diagSub = new TextView(this);
        diagSub.setText("检视 USB 状态、协商参数、码率与帧率统计");
        diagSub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        diagSub.setTextColor(StudioDrawables.COLOR_INK_MUTED);
        diagLabels.addView(diagSub);
        diagEntry.addView(diagLabels, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        final TextView viewLink = new TextView(this);
        viewLink.setText("查看 →");
        viewLink.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        viewLink.setTypeface(Typeface.DEFAULT_BOLD);
        viewLink.setTextColor(StudioDrawables.COLOR_CRIMSON);
        diagEntry.addView(viewLink);

        diagEntry.setOnClickListener(v -> navigate(Page.DIAGNOSTICS));
        parent.addView(diagEntry);
    }

    private void buildDiagnosticsContent() {
        if (compactLayout) {
            final LinearLayout col = new LinearLayout(this);
            col.setOrientation(LinearLayout.VERTICAL);
            col.setPadding(dp(16f), dp(14f), dp(16f), dp(20f));
            col.addView(makePageHeader("DIAGNOSTICS // 会话与传输摘要", "连接与解码诊断", 20f), bottomMarginLp(12f));
            col.addView(makeHairlineDivider(StudioDrawables.COLOR_HAIRLINE, 0.8f, 12f));
            col.addView(buildDiagnosticsCard(true), bottomMarginLp(12f));
            col.addView(makeReconnectUsbButton(dp(40f)));
            contentContainer.addView(col, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            return;
        }

        final LinearLayout wrapper = new LinearLayout(this);
        wrapper.setOrientation(LinearLayout.HORIZONTAL);
        wrapper.setGravity(Gravity.TOP);
        wrapper.setPadding(dp(28f), dp(16f), dp(28f), dp(24f));

        final LinearLayout leftCol = new LinearLayout(this);
        leftCol.setOrientation(LinearLayout.VERTICAL);
        schematicView = new WiredLinkSchematicView(this);
        leftCol.addView(schematicView, bottomMarginLp(10f));
        leftCol.addView(makeReconnectUsbButton(dp(34f)));
        final LinearLayout.LayoutParams leftLp = new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.34f);
        leftLp.rightMargin = dp(26f);
        wrapper.addView(leftCol, leftLp);

        final LinearLayout rightCol = new LinearLayout(this);
        rightCol.setOrientation(LinearLayout.VERTICAL);
        rightCol.addView(makePageHeader("DIAGNOSTICS // 会话与传输摘要", "连接与解码诊断", 21f), bottomMarginLp(12f));
        rightCol.addView(makeHairlineDivider(StudioDrawables.COLOR_HAIRLINE, 0.8f, 12f));
        rightCol.addView(buildDiagnosticsCard(false), bottomMarginLp(12f));

        final LinearLayout btnRow = new LinearLayout(this);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);
        btnRow.addView(makeSmallActionButton("← 返回系统设置", false,
            () -> navigate(Page.SETTINGS)), rightMarginLp(12f));
        btnRow.addView(makeSmallActionButton("返回主页", true,
            () -> navigate(Page.HOME)));
        rightCol.addView(btnRow);

        wrapper.addView(rightCol, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.66f));

        contentContainer.addView(wrapper, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        clampMaxWidth(wrapper, 1040);
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
            this, StudioDrawables.COLOR_WHITE, StudioDrawables.COLOR_HAIRLINE, 4f, 0.8f));

        for (int index = 0; index < labels.length; index++) {
            if (index > 0) {
                card.addView(makeHairlineDivider(StudioDrawables.COLOR_SOFT_HAIRLINE, 0.75f));
            }
            final LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setPadding(dp(12f), dp(8f), dp(12f), dp(8f));

            final TextView lbl = new TextView(this);
            lbl.setText(labels[index]);
            lbl.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 12f : 12.5f);
            lbl.setTextColor(StudioDrawables.COLOR_INK_MUTED);
            row.addView(lbl, new LinearLayout.LayoutParams(
                dp(compact ? 78f : 96f), ViewGroup.LayoutParams.WRAP_CONTENT));

            final TextView val = new TextView(this);
            val.setTextSize(TypedValue.COMPLEX_UNIT_SP, compact ? 12f : 12.5f);
            val.setTypeface(Typeface.DEFAULT_BOLD);
            val.setTextColor(StudioDrawables.COLOR_INK);
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
        btn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        btn.setTypeface(Typeface.DEFAULT_BOLD);
        btn.setTextColor(StudioDrawables.COLOR_CRIMSON);
        btn.setGravity(Gravity.CENTER);
        btn.setBackground(StudioDrawables.card(
            this, StudioDrawables.COLOR_ROSE_WASH, StudioDrawables.COLOR_ROSE_BORDER, 3f, 0.8f));
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
        if (compactLayout) {
            bottomContainer.setBackgroundColor(StudioDrawables.COLOR_SHELL_CHROME);
            bottomContainer.addView(makeBottomTab(Page.HOME, "DISPLAY", "扩展显示"));
            bottomContainer.addView(makeBottomTab(Page.SETTINGS, "SETTINGS", "系统设置"));
            bottomContainer.addView(makeBottomTab(Page.DIAGNOSTICS, "DIAG", "连接诊断"));
            return;
        }

        bottomContainer.setBackgroundColor(StudioDrawables.COLOR_HEADER_CHROME);
        final TextView leftNote = new TextView(this);
        leftNote.setText("USB · AOA 硬件直连  //  自动 HiDPI 虚拟屏  //  多点与双指触控");
        leftNote.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        leftNote.setTextColor(StudioDrawables.COLOR_INK_SUBTLE);
        bottomContainer.addView(leftNote, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        final LinearLayout rightGroup = new LinearLayout(this);
        rightGroup.setOrientation(LinearLayout.HORIZONTAL);
        rightGroup.setGravity(Gravity.CENTER_VERTICAL);

        whisperDotView = new View(this);
        final LinearLayout.LayoutParams dotLp = new LinearLayout.LayoutParams(dp(4f), dp(4f));
        dotLp.rightMargin = dp(6f);
        rightGroup.addView(whisperDotView, dotLp);

        whisperCodecView = new TextView(this);
        whisperCodecView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        whisperCodecView.setTextColor(StudioDrawables.COLOR_INK_SECONDARY);
        rightGroup.addView(whisperCodecView);

        bottomContainer.addView(rightGroup);
    }

    private View makeBottomTab(Page target, String en, String zh) {
        final boolean active = (page == target);
        final LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        col.setPadding(dp(4f), dp(6f), dp(4f), dp(8f));

        final View bar = new View(this);
        bar.setBackgroundColor(active ? StudioDrawables.COLOR_CRIMSON : Color.TRANSPARENT);
        col.addView(bar, new LinearLayout.LayoutParams(dp(26f), Math.max(2, dp(2f))));

        final LinearLayout enRow = new LinearLayout(this);
        enRow.setOrientation(LinearLayout.HORIZONTAL);
        enRow.setGravity(Gravity.CENTER);
        final LinearLayout.LayoutParams enRowLp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        enRowLp.topMargin = dp(3f);
        enRowLp.bottomMargin = dp(2f);

        final View dot = new View(this);
        dot.setBackground(StudioDrawables.squareIndicator(
            this,
            active ? StudioDrawables.COLOR_CRIMSON : Color.TRANSPARENT,
            active ? StudioDrawables.COLOR_CRIMSON : StudioDrawables.COLOR_INK_SUBTLE,
            0.8f));
        final LinearLayout.LayoutParams dotLp = new LinearLayout.LayoutParams(dp(4f), dp(4f));
        dotLp.rightMargin = dp(4f);
        enRow.addView(dot, dotLp);

        final TextView enTv = new TextView(this);
        enTv.setText(en);
        enTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9f);
        enTv.setTypeface(Typeface.DEFAULT_BOLD);
        enTv.setLetterSpacing(0.08f);
        enTv.setTextColor(active ? StudioDrawables.COLOR_CRIMSON : StudioDrawables.COLOR_INK_SUBTLE);
        enRow.addView(enTv);
        col.addView(enRow, enRowLp);

        final TextView zhTv = new TextView(this);
        zhTv.setText(zh);
        zhTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        zhTv.setGravity(Gravity.CENTER_HORIZONTAL);
        zhTv.setTypeface(active ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        zhTv.setTextColor(active ? StudioDrawables.COLOR_INK : StudioDrawables.COLOR_INK_MUTED);
        col.addView(zhTv, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        col.setOnClickListener(v -> navigate(target));
        final LinearLayout.LayoutParams tabLp = new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        col.setLayoutParams(tabLp);
        return col;
    }

    private static void setTextIfChanged(TextView view, String text) {
        if (view != null && !TextUtils.equals(view.getText(), text)) {
            view.setText(text);
        }
    }

    private void updateShellLiveValues() {
        final int accent = statusAccent();
        if (topStatusDotView != null) {
            topStatusDotView.setBackgroundColor(accent);
        }
        if (topStatusBadgeView != null && topStatusTextView != null) {
            final boolean highlight = (frameReady || link.state == AccessoryLink.State.ERROR);
            setTextIfChanged(topStatusTextView, shortStatusLabel());
            if (lastBadgeHighlight == null || lastBadgeHighlight != highlight) {
                lastBadgeHighlight = highlight;
                topStatusTextView.setTextColor(highlight ? StudioDrawables.COLOR_CRIMSON : StudioDrawables.COLOR_INK_SECONDARY);
                topStatusBadgeView.setBackground(StudioDrawables.card(
                    this,
                    highlight ? StudioDrawables.COLOR_ROSE_WASH : StudioDrawables.COLOR_SUBTLE,
                    highlight ? StudioDrawables.COLOR_ROSE_BORDER : StudioDrawables.COLOR_HAIRLINE,
                    3f, 0.8f));
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

        if (eyebrowDotView != null) {
            eyebrowDotView.setBackgroundColor(accent);
        }
        if (eyebrowView != null) {
            setTextIfChanged(eyebrowView, statusEyebrow());
            eyebrowView.setTextColor(accent);
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
        if (!preferredCodec.equals(lastSelectedCodecCard)) {
            lastSelectedCodecCard = preferredCodec;
            for (int index = 0; index < codecOptionCards.size() && index < CODEC_VALUES.length; index++) {
                final boolean selected = preferredCodec.equals(CODEC_VALUES[index]);
                codecOptionCards.get(index).setBackground(StudioDrawables.card(
                    this,
                    selected ? StudioDrawables.COLOR_ROSE_WASH : StudioDrawables.COLOR_WHITE,
                    selected ? StudioDrawables.COLOR_ROSE_BORDER : StudioDrawables.COLOR_HAIRLINE,
                    3f, 0.8f));
                codecOptionDots.get(index).setBackground(StudioDrawables.squareIndicator(
                    this,
                    selected ? StudioDrawables.COLOR_CRIMSON : Color.TRANSPARENT,
                    selected ? StudioDrawables.COLOR_CRIMSON : StudioDrawables.COLOR_INK_SUBTLE,
                    0.8f));
                codecOptionTitles.get(index).setTypeface(selected ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
                codecOptionTitles.get(index).setTextColor(selected ? StudioDrawables.COLOR_CRIMSON : StudioDrawables.COLOR_INK);
                codecOptionSubtitles.get(index).setTextColor(selected ? StudioDrawables.COLOR_CRIMSON : StudioDrawables.COLOR_INK_MUTED);
            }
        }

        if (diagnosticValueViews.size() == 11) {
            final String[] vals = diagnosticValues();
            for (int i = 0; i < 11; i++) {
                setTextIfChanged(diagnosticValueViews.get(i), vals[i]);
            }
        }

        if (whisperDotView != null) {
            whisperDotView.setBackgroundColor(accent);
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
            return frameReady ? "扩展显示运行中" : "正在准备画面";
        }
        return "等待 USB 有线接入";
    }

    private String statusDetail() {
        if (link.state == AccessoryLink.State.ERROR) {
            return link.message.contains("permission")
                ? "点按下方重试，并在系统提示弹窗中允许 USB 配件连接。"
                : "请检查 Mac 端 DeskMate 状态与 USB 数据线连接后重试。";
        }
        if (link.state == AccessoryLink.State.AUTHORIZING) {
            return "请在系统提示弹窗中点击允许，建立高速 USB 配件通信通道。";
        }
        if (link.state == AccessoryLink.State.CONNECTED) {
            return frameReady
                ? "扩展桌面已建立并保持实时推流，可随时进入全屏扩展画面。"
                : "USB 已连接，正在协商编码格式与虚拟显示器首帧画面。";
        }
        if (isPhone) {
            return "连接 Mac 后将自动匹配手机比例创建虚拟扩展屏，进入连接后自动横屏，支持低延迟画面与反向触控。";
        }
        return "连接 Mac 后将自动匹配平板比例创建虚拟扩展屏，支持低延迟画面与反向触控。";
    }

    private String statusEyebrow() {
        if (link.state == AccessoryLink.State.CONNECTED) {
            return frameReady ? "STREAMING // 画面就绪" : "USB LINKED // 已连接";
        }
        if (link.state == AccessoryLink.State.ERROR) {
            return "FAULT // 连接异常";
        }
        if (link.state == AccessoryLink.State.AUTHORIZING) {
            return "AUTH // 等待配件授权";
        }
        return "STANDBY // 等待 USB 接入";
    }

    private int statusAccent() {
        if (frameReady) {
            return StudioDrawables.COLOR_JADE;
        }
        if (link.state == AccessoryLink.State.ERROR || link.state == AccessoryLink.State.CONNECTED) {
            return StudioDrawables.COLOR_CRIMSON;
        }
        if (link.state == AccessoryLink.State.AUTHORIZING) {
            return StudioDrawables.COLOR_AMBER;
        }
        return StudioDrawables.COLOR_INK_SUBTLE;
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

    private View makePageHeader(String eyebrow, String titleText, float titleSp) {
        final LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        final TextView eye = new TextView(this);
        eye.setText(eyebrow);
        eye.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f);
        eye.setTypeface(Typeface.DEFAULT_BOLD);
        eye.setLetterSpacing(0.11f);
        eye.setTextColor(StudioDrawables.COLOR_CRIMSON);
        col.addView(eye, bottomMarginLp(4f));

        final TextView title = new TextView(this);
        title.setText(titleText);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, titleSp);
        title.setTextColor(StudioDrawables.COLOR_INK);
        col.addView(title);
        return col;
    }

    private View makeSectionHeader(String en, String zh) {
        final LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(4f), 0, dp(2f));

        final TextView enTv = new TextView(this);
        enTv.setText(en);
        enTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        enTv.setTypeface(Typeface.DEFAULT_BOLD);
        enTv.setLetterSpacing(0.11f);
        enTv.setTextColor(StudioDrawables.COLOR_CRIMSON);
        row.addView(enTv, rightMarginLp(6f));

        final TextView slash = new TextView(this);
        slash.setText("/");
        slash.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        slash.setTextColor(StudioDrawables.COLOR_HAIRLINE);
        row.addView(slash, rightMarginLp(6f));

        final TextView zhTv = new TextView(this);
        zhTv.setText(zh);
        zhTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        zhTv.setTypeface(Typeface.DEFAULT_BOLD);
        zhTv.setTextColor(StudioDrawables.COLOR_INK);
        row.addView(zhTv, rightMarginLp(10f));

        final View line = makeHairlineDivider(StudioDrawables.COLOR_HAIRLINE, 0.8f);
        row.addView(line, new LinearLayout.LayoutParams(0, Math.max(1, dp(0.8f)), 1f));
        return row;
    }

    private View makePrimaryButton(String label, boolean fullWidth, Runnable onTap) {
        final LinearLayout btn = new LinearLayout(this);
        btn.setOrientation(LinearLayout.HORIZONTAL);
        btn.setGravity(Gravity.CENTER);
        btn.setBackground(StudioDrawables.primaryButton(this));
        btn.setPadding(
            dp(fullWidth ? 18f : 20f), dp(fullWidth ? 12f : 10f),
            dp(fullWidth ? 18f : 20f), dp(fullWidth ? 13f : 11f));
        btn.setLayoutParams(new LinearLayout.LayoutParams(
            fullWidth ? ViewGroup.LayoutParams.MATCH_PARENT : ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT));

        final TextView tv = new TextView(this);
        tv.setText(label);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setLetterSpacing(0.04f);
        tv.setTextColor(StudioDrawables.COLOR_WHITE);
        btn.addView(tv, rightMarginLp(10f));

        final TextView arrow = new TextView(this);
        arrow.setText("→");
        arrow.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        arrow.setTextColor(StudioDrawables.COLOR_WHITE);
        btn.addView(arrow);

        btn.setOnClickListener(v -> onTap.run());
        return btn;
    }

    private View makeSmallActionButton(String label, boolean roseAccent, Runnable onTap) {
        final TextView btn = new TextView(this);
        btn.setText(label);
        btn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        btn.setTypeface(roseAccent ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        btn.setTextColor(roseAccent ? StudioDrawables.COLOR_CRIMSON : StudioDrawables.COLOR_INK);
        btn.setGravity(Gravity.CENTER);
        btn.setPadding(dp(12f), 0, dp(12f), 0);
        btn.setBackground(StudioDrawables.card(
            this,
            roseAccent ? StudioDrawables.COLOR_ROSE_WASH : StudioDrawables.COLOR_WHITE,
            roseAccent ? StudioDrawables.COLOR_ROSE_BORDER : StudioDrawables.COLOR_HAIRLINE,
            3f, 0.8f));
        btn.setLayoutParams(new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, dp(34f)));
        btn.setOnClickListener(v -> onTap.run());
        return btn;
    }

    private void styleSwitch(Switch sw, boolean checked) {
        sw.setThumbTintList(ColorStateList.valueOf(
            checked ? StudioDrawables.COLOR_WHITE : StudioDrawables.COLOR_SUBTLE));
        sw.setTrackTintList(ColorStateList.valueOf(
            checked ? StudioDrawables.COLOR_CRIMSON : StudioDrawables.COLOR_HAIRLINE));
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
        if (Build.VERSION.SDK_INT >= 30) {
            final Display display = getDisplay();
            if (display != null) {
                return display.getRefreshRate();
            }
        }
        final Display display =
            ((WindowManager) getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay();
        return display != null ? display.getRefreshRate() : 60.0f;
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
