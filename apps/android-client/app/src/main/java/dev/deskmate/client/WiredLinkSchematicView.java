package dev.deskmate.client;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Quiet Squircle (Scheme A) wired dual-display schematic (Host + USB-C AOA + Android tablet/phone).
 */
final class WiredLinkSchematicView extends LinearLayout {
    private boolean linked;
    private boolean streaming;
    private int fps;
    private String codecText = "";
    private String resolutionText = "";
    private boolean compact;
    private boolean isPhone;

    private StudioDrawables.Palette palette;
    private final LinearLayout badgeContainer;
    private final View badgeDot;
    private final TextView badgeView;
    private final SchematicCanvas canvasView;
    private final View topDivider;
    private final View bottomDivider;
    private final LinearLayout footer;
    private final TextView footerLabel;
    private final TextView footerValueView;

    WiredLinkSchematicView(Context context) {
        super(context);
        this.palette = StudioDrawables.palette(context);
        setOrientation(VERTICAL);
        setBackground(StudioDrawables.card(context, palette.surface, palette.hairline, 16f, 0.8f));
        setClipToOutline(true);

        // Top spec bar
        final LinearLayout header = new LinearLayout(context);
        header.setOrientation(HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(16), dp(11), dp(16), dp(10));

        final TextView title = new TextView(context);
        title.setText("有线链路拓扑");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(palette.ink);
        title.setMaxLines(1);
        title.setEllipsize(TextUtils.TruncateAt.END);
        final LayoutParams titleLp = new LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        titleLp.rightMargin = dp(8);
        header.addView(title, titleLp);

        badgeContainer = new LinearLayout(context);
        badgeContainer.setOrientation(HORIZONTAL);
        badgeContainer.setGravity(Gravity.CENTER_VERTICAL);
        badgeContainer.setPadding(dp(8), dp(3), dp(8), dp(3));

        badgeDot = new View(context);
        final LayoutParams dotLp = new LayoutParams(dp(6), dp(6));
        dotLp.rightMargin = dp(5);
        badgeContainer.addView(badgeDot, dotLp);

        badgeView = new TextView(context);
        badgeView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        badgeView.setTypeface(Typeface.DEFAULT_BOLD);
        badgeView.setMaxLines(1);
        badgeContainer.addView(badgeView);

        header.addView(badgeContainer);
        addView(header, new LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        topDivider = makeDivider();
        addView(topDivider);

        canvasView = new SchematicCanvas(context);
        canvasView.setBackgroundColor(palette.canvas);
        addView(canvasView, new LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(210)));

        bottomDivider = makeDivider();
        addView(bottomDivider);

        footer = new LinearLayout(context);
        footer.setOrientation(HORIZONTAL);
        footer.setGravity(Gravity.CENTER_VERTICAL);
        footer.setBackgroundColor(palette.surface);
        footer.setPadding(dp(16), dp(10), dp(16), dp(10));

        footerLabel = new TextView(context);
        footerLabel.setText("USB-C AOA 直连");
        footerLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        footerLabel.setTextColor(palette.inkMuted);
        footer.addView(footerLabel);

        footerValueView = new TextView(context);
        footerValueView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        footerValueView.setTypeface(Typeface.DEFAULT_BOLD);
        footerValueView.setTextColor(palette.inkSecondary);
        footerValueView.setMaxLines(1);
        footerValueView.setEllipsize(TextUtils.TruncateAt.END);
        footerValueView.setGravity(Gravity.END);
        final LayoutParams valLp = new LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        valLp.leftMargin = dp(8);
        footer.addView(footerValueView, valLp);

        addView(footer, new LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        applyVisuals();
    }

    void updateState(boolean linked, boolean streaming, int fps,
                     String codecText, String resolutionText,
                     boolean compact, boolean isPhone) {
        final StudioDrawables.Palette nextPalette = StudioDrawables.palette(getContext());
        final String safeCodec = codecText != null ? codecText : "";
        final String safeRes = resolutionText != null ? resolutionText : "";
        final boolean sizeChanged = (this.compact != compact || this.isPhone != isPhone);
        final boolean paletteChanged = (this.palette.dark != nextPalette.dark);
        if (!paletteChanged && this.linked == linked && this.streaming == streaming && this.fps == fps &&
            this.codecText.equals(safeCodec) && this.resolutionText.equals(safeRes) &&
            !sizeChanged) {
            return;
        }
        this.palette = nextPalette;
        this.linked = linked;
        this.streaming = streaming;
        this.fps = fps;
        this.codecText = safeCodec;
        this.resolutionText = safeRes;
        this.compact = compact;
        this.isPhone = isPhone;
        if (sizeChanged) {
            final int heightDp = compact ? (isPhone ? 152 : 172) : 210;
            final ViewGroup.LayoutParams lp = canvasView.getLayoutParams();
            lp.height = dp(heightDp);
            canvasView.setLayoutParams(lp);
        }
        applyVisuals();
    }

    private void applyVisuals() {
        setBackground(StudioDrawables.card(getContext(), palette.surface, palette.hairline, 16f, 0.8f));
        topDivider.setBackgroundColor(palette.softHairline);
        bottomDivider.setBackgroundColor(palette.softHairline);
        canvasView.setBackgroundColor(palette.canvas);
        footer.setBackgroundColor(palette.surface);
        footerLabel.setTextColor(palette.inkMuted);
        footerValueView.setTextColor(palette.inkSecondary);

        final int dotColor = streaming
            ? palette.accent
            : (linked ? palette.positive : palette.inkMuted);
        badgeDot.setBackground(StudioDrawables.circleDot(dotColor));

        if (streaming) {
            badgeView.setText("推流中");
            badgeView.setTextColor(palette.accent);
            badgeContainer.setBackground(StudioDrawables.pill(
                getContext(), palette.accentSoft, palette.accentBorder, 0.8f));
        } else if (linked) {
            badgeView.setText("USB 已连接");
            badgeView.setTextColor(palette.positive);
            badgeContainer.setBackground(StudioDrawables.pill(
                getContext(), palette.positiveSoft, palette.positiveBorder, 0.8f));
        } else {
            badgeView.setText("待连接");
            badgeView.setTextColor(palette.inkSecondary);
            badgeContainer.setBackground(StudioDrawables.pill(
                getContext(), palette.subtle, palette.hairline, 0.8f));
        }

        if (!resolutionText.isEmpty()) {
            final String codec = !codecText.isEmpty() ? codecText : "HEVC";
            footerValueView.setText(codec + " · " + resolutionText);
        } else {
            footerValueView.setText(linked ? "正在同步分辨率" : "等待数据线握手");
        }
        canvasView.invalidate();
    }

    private View makeDivider() {
        final View divider = new View(getContext());
        divider.setBackgroundColor(palette.softHairline);
        divider.setLayoutParams(new LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(0.8f))));
        return divider;
    }

    private int dp(float value) {
        return DeviceLayout.dp(getContext(), value);
    }

    private final class SchematicCanvas extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();
        private final float density;

        SchematicCanvas(Context context) {
            super(context);
            final float d = context.getResources().getDisplayMetrics().density;
            this.density = d > 0f ? d : 1f;
        }

        private float px(float dpValue) {
            return dpValue * density;
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            final float w = getWidth();
            final float h = getHeight();
            if (w <= px(10) || h <= px(10)) {
                return;
            }

            final int activeColor = streaming
                ? palette.accent
                : (linked ? palette.positive : palette.inkMuted);

            // Subtle dot grid
            paint.reset();
            paint.setAntiAlias(true);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(palette.hairline);
            final float step = px(16f);
            for (float gx = px(14f); gx < w - px(10f); gx += step) {
                for (float gy = px(14f); gy < h - px(10f); gy += step) {
                    canvas.drawCircle(gx, gy, px(0.9f), paint);
                }
            }

            final float cy = h * 0.5f;
            final float macW = Math.min(w * (compact ? 0.34f : 0.33f), px(compact ? 98f : 122f));
            final float macH = macW * 0.64f;
            final float macX = w * (compact ? 0.08f : 0.12f);
            final float macY = cy - macH * 0.54f;

            final float padW = Math.min(w * (compact ? 0.36f : 0.35f), px(compact ? 106f : 132f));
            final float padH = isPhone ? padW * 0.58f : padW * 0.68f;
            final float padX = w * (compact ? 0.92f : 0.88f) - padW;
            final float padY = cy - padH * 0.46f;

            // Smooth USB-C link connector
            final float busStartX = macX + macW;
            final float busEndX = padX;
            final float busMidX = (busStartX + busEndX) * 0.5f;

            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(linked ? activeColor : palette.hairline);
            paint.setStrokeWidth(px(linked ? 1.8f : 1.2f));
            canvas.drawLine(busStartX, cy, busEndX, cy, paint);

            // Central USB-C pill badge
            final float pillW = px(42f);
            final float pillH = px(18f);
            rect.set(busMidX - pillW * 0.5f, cy - pillH * 0.5f,
                busMidX + pillW * 0.5f, cy + pillH * 0.5f);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(linked ? palette.accentSoft : palette.subtle);
            canvas.drawRoundRect(rect, pillH * 0.5f, pillH * 0.5f, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(linked ? activeColor : palette.hairline);
            paint.setStrokeWidth(px(1f));
            canvas.drawRoundRect(rect, pillH * 0.5f, pillH * 0.5f, paint);

            paint.setStyle(Paint.Style.FILL);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTypeface(Typeface.MONOSPACE);
            paint.setTextSize(px(9f));
            paint.setColor(linked ? activeColor : palette.inkMuted);
            canvas.drawText("USB-C", busMidX, cy + px(3.2f), paint);

            // 1. Primary Host display tile
            rect.set(macX, macY, macX + macW, macY + macH);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(palette.surface);
            canvas.drawRoundRect(rect, px(10f), px(10f), paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(palette.hairline);
            paint.setStrokeWidth(px(1.2f));
            canvas.drawRoundRect(rect, px(10f), px(10f), paint);

            final float macCenterX = macX + macW * 0.5f;
            final float standBottom = macY + macH + px(9f);
            paint.setColor(palette.inkMuted);
            paint.setStrokeWidth(px(1.2f));
            canvas.drawLine(macCenterX, macY + macH, macCenterX, standBottom, paint);
            canvas.drawLine(macCenterX - macW * 0.16f, standBottom,
                macCenterX + macW * 0.16f, standBottom, paint);

            paint.setStyle(Paint.Style.FILL);
            paint.setColor(palette.ink);
            paint.setTypeface(Typeface.DEFAULT_BOLD);
            paint.setTextSize(px(compact ? 10.5f : 11.5f));
            canvas.drawText("电脑主屏", macCenterX, macY + macH * 0.48f, paint);

            paint.setColor(palette.inkMuted);
            paint.setTypeface(Typeface.MONOSPACE);
            paint.setTextSize(px(9.5f));
            canvas.drawText("HOST", macCenterX, macY + macH * 0.72f, paint);

            // 2. Android Tablet / Phone extended display tile
            final float cornerRadius = px(isPhone ? 12f : 10f);
            rect.set(padX, padY, padX + padW, padY + padH);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(streaming ? palette.accentSoft : palette.surface);
            canvas.drawRoundRect(rect, cornerRadius, cornerRadius, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(linked ? activeColor : palette.hairline);
            paint.setStrokeWidth(px(linked ? 1.5f : 1.2f));
            canvas.drawRoundRect(rect, cornerRadius, cornerRadius, paint);

            paint.setStyle(Paint.Style.FILL);
            paint.setColor(activeColor);
            canvas.drawCircle(padX + padW - px(10f), padY + px(10f), px(2.8f), paint);

            final float padCenterX = padX + padW * 0.5f;
            paint.setColor(streaming ? palette.accent : palette.ink);
            paint.setTypeface(Typeface.DEFAULT_BOLD);
            paint.setTextSize(px(compact ? 10.5f : 11.5f));
            final String titleText = (streaming && fps > 0)
                ? (fps + " FPS")
                : (isPhone ? "手机副屏" : "扩展副屏");
            canvas.drawText(titleText, padCenterX, padY + padH * 0.48f, paint);

            paint.setColor(linked ? palette.inkSecondary : palette.inkMuted);
            paint.setTypeface(Typeface.MONOSPACE);
            paint.setTextSize(px(9.5f));
            final String subText = streaming
                ? (!codecText.isEmpty() ? codecText : "LIVE")
                : (linked ? "USB READY" : "STANDBY");
            canvas.drawText(subText, padCenterX, padY + padH * 0.72f, paint);
        }
    }
}
