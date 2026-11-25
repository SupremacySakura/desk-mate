package dev.deskmate.client;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
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
 * Architectural wired dual-display schematic (Mac host + USB-C AOA data bus + Android tablet/phone)
 * matching HarmonyOS WiredLinkSchematic.
 */
final class WiredLinkSchematicView extends LinearLayout {
    private boolean linked;
    private boolean streaming;
    private int fps;
    private String codecText = "";
    private String resolutionText = "";
    private boolean compact;
    private boolean isPhone;

    private final View topDot;
    private final TextView badgeView;
    private final SchematicCanvas canvasView;
    private final TextView footerValueView;

    WiredLinkSchematicView(Context context) {
        super(context);
        setOrientation(VERTICAL);
        setBackground(StudioDrawables.card(
            context, StudioDrawables.COLOR_WHITE, StudioDrawables.COLOR_HAIRLINE, 4f, 0.8f));
        setClipToPadding(true);

        // Top spec bar
        final LinearLayout header = new LinearLayout(context);
        header.setOrientation(HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(14), dp(10), dp(14), dp(8));

        final LinearLayout titleRow = new LinearLayout(context);
        titleRow.setOrientation(HORIZONTAL);
        titleRow.setGravity(Gravity.CENTER_VERTICAL);

        topDot = new View(context);
        final LayoutParams dotLp = new LayoutParams(dp(5), dp(5));
        dotLp.rightMargin = dp(6);
        titleRow.addView(topDot, dotLp);

        final TextView title = new TextView(context);
        title.setText("TOPOLOGY // 副屏链路拓扑");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9.5f);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setLetterSpacing(0.08f);
        title.setTextColor(StudioDrawables.COLOR_INK_SUBTLE);
        title.setMaxLines(1);
        title.setEllipsize(TextUtils.TruncateAt.END);
        titleRow.addView(title);

        final LayoutParams titleRowLp = new LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        titleRowLp.rightMargin = dp(8);
        header.addView(titleRow, titleRowLp);

        badgeView = new TextView(context);
        badgeView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f);
        badgeView.setTypeface(Typeface.DEFAULT_BOLD);
        badgeView.setPadding(dp(7), dp(2), dp(7), dp(2));
        badgeView.setMaxLines(1);
        header.addView(badgeView);

        addView(header, new LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        addView(makeDivider());

        canvasView = new SchematicCanvas(context);
        canvasView.setBackgroundColor(StudioDrawables.COLOR_CANVAS_MID);
        addView(canvasView, new LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(216)));

        addView(makeDivider());

        // Bottom spec footer
        final LinearLayout footer = new LinearLayout(context);
        footer.setOrientation(HORIZONTAL);
        footer.setGravity(Gravity.CENTER_VERTICAL);
        footer.setBackgroundColor(StudioDrawables.COLOR_WHITE);
        footer.setPadding(dp(14), dp(9), dp(14), dp(10));

        final TextView footerLabel = new TextView(context);
        footerLabel.setText("USB · AOA 硬件直连");
        footerLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        footerLabel.setTextColor(StudioDrawables.COLOR_INK_SUBTLE);
        footer.addView(footerLabel);

        footerValueView = new TextView(context);
        footerValueView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        footerValueView.setTypeface(Typeface.DEFAULT_BOLD);
        footerValueView.setTextColor(StudioDrawables.COLOR_INK_SECONDARY);
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
        final String safeCodec = codecText != null ? codecText : "";
        final String safeRes = resolutionText != null ? resolutionText : "";
        final boolean sizeChanged = (this.compact != compact || this.isPhone != isPhone);
        if (this.linked == linked && this.streaming == streaming && this.fps == fps &&
            this.codecText.equals(safeCodec) && this.resolutionText.equals(safeRes) &&
            !sizeChanged) {
            return;
        }
        this.linked = linked;
        this.streaming = streaming;
        this.fps = fps;
        this.codecText = safeCodec;
        this.resolutionText = safeRes;
        this.compact = compact;
        this.isPhone = isPhone;
        if (sizeChanged) {
            final int heightDp = compact ? (isPhone ? 148 : 170) : 216;
            final ViewGroup.LayoutParams lp = canvasView.getLayoutParams();
            lp.height = dp(heightDp);
            canvasView.setLayoutParams(lp);
        }
        applyVisuals();
    }

    private void applyVisuals() {
        final int dotColor = streaming
            ? StudioDrawables.COLOR_CRIMSON
            : (linked ? StudioDrawables.COLOR_CHAMPAGNE : StudioDrawables.COLOR_INK_SUBTLE);
        topDot.setBackgroundColor(dotColor);

        if (streaming) {
            badgeView.setText("推流中");
            badgeView.setTextColor(StudioDrawables.COLOR_CRIMSON);
            badgeView.setBackground(StudioDrawables.card(
                getContext(), StudioDrawables.COLOR_ROSE_WASH, StudioDrawables.COLOR_ROSE_BORDER, 2f, 0.7f));
        } else if (linked) {
            badgeView.setText("USB 已连接");
            badgeView.setTextColor(StudioDrawables.COLOR_CHAMPAGNE_DARK);
            badgeView.setBackground(StudioDrawables.card(
                getContext(), StudioDrawables.COLOR_SUBTLE, StudioDrawables.COLOR_HAIRLINE, 2f, 0.7f));
        } else {
            badgeView.setText("待连接");
            badgeView.setTextColor(StudioDrawables.COLOR_INK_MUTED);
            badgeView.setBackground(StudioDrawables.card(
                getContext(), StudioDrawables.COLOR_SUBTLE, StudioDrawables.COLOR_HAIRLINE, 2f, 0.7f));
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
        divider.setBackgroundColor(StudioDrawables.COLOR_SOFT_HAIRLINE);
        divider.setLayoutParams(new LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(0.75f))));
        return divider;
    }

    private int dp(float value) {
        return DeviceLayout.dp(getContext(), value);
    }

    private final class SchematicCanvas extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();
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
                ? StudioDrawables.COLOR_CRIMSON
                : (linked ? StudioDrawables.COLOR_CHAMPAGNE : StudioDrawables.COLOR_INK_SUBTLE);

            // Corner calibration brackets
            final float inset = px(compact ? 9f : 12f);
            final float tick = px(compact ? 6f : 7f);
            paint.reset();
            paint.setAntiAlias(true);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(StudioDrawables.COLOR_HAIRLINE);
            paint.setStrokeWidth(px(1f));
            path.reset();
            path.moveTo(inset, inset + tick);
            path.lineTo(inset, inset);
            path.lineTo(inset + tick, inset);
            path.moveTo(w - inset - tick, inset);
            path.lineTo(w - inset, inset);
            path.lineTo(w - inset, inset + tick);
            path.moveTo(inset, h - inset - tick);
            path.lineTo(inset, h - inset);
            path.lineTo(inset + tick, h - inset);
            path.moveTo(w - inset - tick, h - inset);
            path.lineTo(w - inset, h - inset);
            path.lineTo(w - inset, h - inset - tick);
            canvas.drawPath(path, paint);

            // Dashed horizontal drafting guide
            final float cy = h * 0.5f;
            paint.setColor(StudioDrawables.COLOR_SOFT_HAIRLINE);
            paint.setPathEffect(new DashPathEffect(new float[] { px(3f), px(4f) }, 0f));
            path.reset();
            path.moveTo(px(compact ? 16f : 22f), cy);
            path.lineTo(w - px(compact ? 16f : 22f), cy);
            canvas.drawPath(path, paint);
            paint.setPathEffect(null);

            final float macW = Math.min(w * (compact ? 0.36f : 0.34f), px(compact ? 96f : 118f));
            final float macH = macW * 0.64f;
            final float macX = w * (compact ? 0.08f : 0.14f);
            final float macY = cy - macH * 0.56f;

            final float padW = Math.min(w * (compact ? 0.38f : 0.36f), px(compact ? 104f : 128f));
            final float padH = isPhone ? padW * 0.56f : padW * 0.68f;
            final float padX = w * (compact ? 0.92f : 0.86f) - padW;
            final float padY = cy - padH * 0.44f;

            // Stepped USB-C AOA data bus
            final float busStartX = macX + macW;
            final float busStartY = macY + macH * 0.52f;
            final float busEndX = padX;
            final float busEndY = padY + padH * 0.52f;
            final float busMidX = (busStartX + busEndX) * 0.5f;

            paint.setColor(linked ? StudioDrawables.COLOR_CRIMSON : 0xFFCFC8BC);
            paint.setStrokeWidth(px(linked ? 1.8f : 1.1f));
            path.reset();
            path.moveTo(busStartX, busStartY);
            path.lineTo(busMidX, busStartY);
            path.lineTo(busMidX, busEndY);
            path.lineTo(busEndX, busEndY);
            canvas.drawPath(path, paint);

            // Terminal square pads & signal node
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(activeColor);
            canvas.drawRect(busStartX - px(2), busStartY - px(2), busStartX + px(2), busStartY + px(2), paint);
            canvas.drawRect(busEndX - px(2), busEndY - px(2), busEndX + px(2), busEndY + px(2), paint);
            final float midY = (busStartY + busEndY) * 0.5f;
            canvas.drawRect(busMidX - px(3), midY - px(3), busMidX + px(3), midY + px(3), paint);

            // 1. Primary Mac display
            rect.set(macX, macY, macX + macW, macY + macH);
            paint.setColor(StudioDrawables.COLOR_WHITE);
            canvas.drawRoundRect(rect, px(3f), px(3f), paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(StudioDrawables.COLOR_INK);
            paint.setStrokeWidth(px(1.25f));
            canvas.drawRoundRect(rect, px(3f), px(3f), paint);

            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0xFFF2EDE4);
            canvas.drawRect(macX + px(1), macY + px(1), macX + macW - px(1), macY + px(7), paint);

            final float macCenterX = macX + macW * 0.5f;
            final float standBottom = macY + macH + px(9);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(StudioDrawables.COLOR_INK_MUTED);
            paint.setStrokeWidth(px(1.2f));
            canvas.drawLine(macCenterX, macY + macH, macCenterX, standBottom, paint);
            canvas.drawLine(macCenterX - macW * 0.16f, standBottom,
                macCenterX + macW * 0.16f, standBottom, paint);

            paint.setStyle(Paint.Style.FILL);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setColor(StudioDrawables.COLOR_INK);
            paint.setTypeface(Typeface.DEFAULT_BOLD);
            paint.setTextSize(px(compact ? 10f : 11f));
            canvas.drawText("MAC 主屏", macCenterX, macY + macH * 0.54f, paint);

            paint.setColor(StudioDrawables.COLOR_INK_SUBTLE);
            paint.setTypeface(Typeface.MONOSPACE);
            paint.setTextSize(px(9f));
            canvas.drawText("HOST", macCenterX, macY + macH * 0.74f, paint);

            // 2. Android Tablet / Phone extended display frame
            final float cornerRadius = px(isPhone ? 5f : 3.5f);
            rect.set(padX, padY, padX + padW, padY + padH);
            paint.setColor(streaming
                ? StudioDrawables.COLOR_ROSE_WASH
                : (linked ? StudioDrawables.COLOR_SUBTLE : StudioDrawables.COLOR_WHITE));
            canvas.drawRoundRect(rect, cornerRadius, cornerRadius, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(linked ? StudioDrawables.COLOR_CRIMSON : StudioDrawables.COLOR_INK_SECONDARY);
            paint.setStrokeWidth(px(linked ? 1.65f : 1.15f));
            canvas.drawRoundRect(rect, cornerRadius, cornerRadius, paint);

            rect.set(padX + px(4), padY + px(4), padX + padW - px(4), padY + padH - px(4));
            paint.setColor(streaming ? StudioDrawables.COLOR_ROSE_BORDER : StudioDrawables.COLOR_SOFT_HAIRLINE);
            paint.setStrokeWidth(px(0.8f));
            canvas.drawRoundRect(rect, px(isPhone ? 3f : 2f), px(isPhone ? 3f : 2f), paint);

            paint.setStyle(Paint.Style.FILL);
            if (isPhone) {
                paint.setColor(streaming ? StudioDrawables.COLOR_ROSE_BORDER : StudioDrawables.COLOR_HAIRLINE);
                canvas.drawRect(padX + px(2.5f), padY + padH * 0.36f,
                    padX + px(4.3f), padY + padH * 0.64f, paint);
            }

            paint.setColor(activeColor);
            canvas.drawRect(padX + padW - px(11), padY + px(7),
                padX + padW - px(7), padY + px(11), paint);

            final float padCenterX = padX + padW * 0.5f;
            paint.setColor(streaming ? StudioDrawables.COLOR_CRIMSON : StudioDrawables.COLOR_INK);
            paint.setTypeface(Typeface.DEFAULT_BOLD);
            paint.setTextSize(px(compact ? 10f : 11f));
            final String titleText = (streaming && fps > 0)
                ? (fps + " FPS")
                : (isPhone ? "手机副屏" : "扩展副屏");
            canvas.drawText(titleText, padCenterX, padY + padH * 0.52f, paint);

            paint.setColor(linked ? StudioDrawables.COLOR_INK_MUTED : 0xFFA49EA6);
            paint.setTypeface(Typeface.MONOSPACE);
            paint.setTextSize(px(9f));
            final String subText = streaming
                ? (!codecText.isEmpty() ? codecText : "LIVE")
                : (linked ? "USB READY" : "STANDBY");
            canvas.drawText(subText, padCenterX, padY + padH * 0.74f, paint);
        }
    }
}
