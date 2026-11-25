package dev.deskmate.client;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;

/**
 * Alabaster Studio color tokens and pure Android framework Drawable factories
 * matching the HarmonyOS and macOS DeskMate interfaces.
 */
final class StudioDrawables {
    static final int COLOR_WHITE = 0xFFFFFFFF;
    static final int COLOR_CANVAS_MID = 0xFFFCFBF8;
    static final int COLOR_CANVAS_EDGE = 0xFFF4F0E8;
    static final int COLOR_SUBTLE = 0xFFFAF7F2;
    static final int COLOR_SHELL_CHROME = 0xF2FFFFFF;
    static final int COLOR_HEADER_CHROME = 0xCCFFFFFF;

    static final int COLOR_HAIRLINE = 0xFFDCD6CC;
    static final int COLOR_SOFT_HAIRLINE = 0xFFEAE4DD;

    static final int COLOR_INK = 0xFF232026;
    static final int COLOR_INK_BODY = 0xFF4D4752;
    static final int COLOR_INK_SECONDARY = 0xFF5A545E;
    static final int COLOR_INK_MUTED = 0xFF767079;
    static final int COLOR_INK_SUBTLE = 0xFF8C858E;

    static final int COLOR_CRIMSON = 0xFF9D193D;
    static final int COLOR_CRIMSON_PRESSED = 0xFF7E1230;
    static final int COLOR_ROSE_WASH = 0xFFF7EAEF;
    static final int COLOR_ROSE_BORDER = 0xFFD9B6C2;

    static final int COLOR_CHAMPAGNE = 0xFFB9975B;
    static final int COLOR_CHAMPAGNE_SOFT = 0x75B9975B;
    static final int COLOR_CHAMPAGNE_DARK = 0xFF7D602B;

    static final int COLOR_JADE = 0xFF2D7D5B;
    static final int COLOR_AMBER = 0xFFA8641E;

    static final int COLOR_HUD_BG = 0xD918161B;
    static final int COLOR_HUD_BORDER = 0xFF5A5059;
    static final int COLOR_HUD_DIVIDER = 0xFF4B444E;
    static final int COLOR_HUD_WHITE = 0xFFFCFBF8;
    static final int COLOR_HUD_FPS = 0xFF8BF2C4;
    static final int COLOR_HUD_LABEL = 0xFFD6D1CC;
    static final int COLOR_HUD_CODEC = 0xFFE6C994;
    static final int COLOR_HUD_RES = 0xFFE2DDD6;
    static final int COLOR_HUD_DROP = 0xFFF8B4B4;
    static final int COLOR_RETURN_BTN_BG = 0xF5FCFBF8;

    private StudioDrawables() {}

    static Drawable alabasterBackground() {
        final GradientDrawable gd = new GradientDrawable(
            GradientDrawable.Orientation.TL_BR,
            new int[] { COLOR_WHITE, COLOR_CANVAS_MID, COLOR_CANVAS_EDGE });
        gd.setGradientType(GradientDrawable.LINEAR_GRADIENT);
        return gd;
    }

    static GradientDrawable card(Context context, int fillColor, int strokeColor,
                                 float radiusDp, float strokeDp) {
        final GradientDrawable gd = new GradientDrawable();
        gd.setShape(GradientDrawable.RECTANGLE);
        gd.setColor(fillColor);
        gd.setCornerRadius(DeviceLayout.dp(context, radiusDp));
        gd.setStroke(Math.max(1, DeviceLayout.dp(context, strokeDp)), strokeColor);
        return gd;
    }

    static Drawable primaryButton(Context context) {
        final GradientDrawable base = new GradientDrawable();
        base.setShape(GradientDrawable.RECTANGLE);
        base.setColor(COLOR_CRIMSON);
        base.setCornerRadius(DeviceLayout.dp(context, 3f));
        base.setStroke(Math.max(1, DeviceLayout.dp(context, 0.8f)), COLOR_CRIMSON_PRESSED);

        final GradientDrawable topHighlight = new GradientDrawable();
        topHighlight.setShape(GradientDrawable.RECTANGLE);
        topHighlight.setColor(COLOR_CHAMPAGNE_SOFT);

        final LayerDrawable layers = new LayerDrawable(new Drawable[] { base, topHighlight });
        final int padH = Math.max(1, DeviceLayout.dp(context, 1f));
        final int height = Math.max(2, DeviceLayout.dp(context, 1.3f));
        layers.setLayerInset(1, padH, padH, padH, DeviceLayout.dp(context, 42f));
        layers.setLayerSize(1, -1, height);
        return layers;
    }

    static GradientDrawable squareIndicator(Context context, int fillColor,
                                            int strokeColor, float strokeDp) {
        final GradientDrawable gd = new GradientDrawable();
        gd.setShape(GradientDrawable.RECTANGLE);
        gd.setColor(fillColor);
        if (strokeDp > 0f && strokeColor != Color.TRANSPARENT) {
            gd.setStroke(Math.max(1, DeviceLayout.dp(context, strokeDp)), strokeColor);
        }
        return gd;
    }
}
