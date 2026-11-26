package dev.deskmate.client;

import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;

/**
 * Quiet Squircle (Scheme A) light/dark adaptive palette tokens and pure Android
 * framework Drawable factories.
 */
final class StudioDrawables {
    static final class Palette {
        final boolean dark;
        final int canvas;
        final int surface;
        final int subtle;
        final int hairline;
        final int softHairline;

        final int ink;
        final int inkSecondary;
        final int inkMuted;

        final int accent;
        final int accentFg;
        final int accentSoft;
        final int accentBorder;

        final int positive;
        final int positiveSoft;
        final int positiveBorder;

        final int warning;
        final int warningSoft;
        final int warningBorder;

        final int danger;
        final int dangerSoft;
        final int dangerBorder;

        Palette(boolean dark,
                int canvas, int surface, int subtle, int hairline, int softHairline,
                int ink, int inkSecondary, int inkMuted,
                int accent, int accentFg, int accentSoft, int accentBorder,
                int positive, int positiveSoft, int positiveBorder,
                int warning, int warningSoft, int warningBorder,
                int danger, int dangerSoft, int dangerBorder) {
            this.dark = dark;
            this.canvas = canvas;
            this.surface = surface;
            this.subtle = subtle;
            this.hairline = hairline;
            this.softHairline = softHairline;
            this.ink = ink;
            this.inkSecondary = inkSecondary;
            this.inkMuted = inkMuted;
            this.accent = accent;
            this.accentFg = accentFg;
            this.accentSoft = accentSoft;
            this.accentBorder = accentBorder;
            this.positive = positive;
            this.positiveSoft = positiveSoft;
            this.positiveBorder = positiveBorder;
            this.warning = warning;
            this.warningSoft = warningSoft;
            this.warningBorder = warningBorder;
            this.danger = danger;
            this.dangerSoft = dangerSoft;
            this.dangerBorder = dangerBorder;
        }
    }

    static final Palette LIGHT = new Palette(
        false,
        0xFFF5F5F3, 0xFFFFFFFF, 0xFFECECE8, 0xFFE2E1DC, 0xFFECEBE6,
        0xFF18181B, 0xFF5A5A60, 0xFF8B8B92,
        0xFF2563EB, 0xFFFFFFFF, 0xFFEFF6FF, 0xFFBFDBFE,
        0xFF15803D, 0xFFECFDF5, 0xFFA7F3D0,
        0xFFB45309, 0xFFFFFBEB, 0xFFFDE68A,
        0xFFDC2626, 0xFFFEF2F2, 0xFFFECACA
    );

    static final Palette DARK = new Palette(
        true,
        0xFF111214, 0xFF181A1E, 0xFF212429, 0xFF2A2D33, 0xFF22252B,
        0xFFF4F4F5, 0xFFA1A1AA, 0xFF71717A,
        0xFF4F8BFF, 0xFF091326, 0xFF18253D, 0xFF294A82,
        0xFF34D399, 0xFF122921, 0xFF1F5441,
        0xFFFBBF24, 0xFF2C2412, 0xFF594518,
        0xFFF87171, 0xFF2E1618, 0xFF5E2428
    );

    static final int COLOR_HUD_BG = 0xDB181A1E;
    static final int COLOR_HUD_BORDER = 0x26FFFFFF;
    static final int COLOR_HUD_DIVIDER = 0xFF3F3F46;
    static final int COLOR_HUD_INK = 0xFFF4F4F5;
    static final int COLOR_HUD_MUTED = 0xFFA1A1AA;
    static final int COLOR_HUD_ACCENT = 0xFF4F8BFF;
    static final int COLOR_HUD_DROP = 0xFFF87171;

    private StudioDrawables() {}

    static Palette palette(Context context) {
        final int nightMode = context.getResources().getConfiguration().uiMode
            & Configuration.UI_MODE_NIGHT_MASK;
        return nightMode == Configuration.UI_MODE_NIGHT_YES ? DARK : LIGHT;
    }

    static GradientDrawable card(Context context, int fillColor, int strokeColor,
                                 float radiusDp, float strokeDp) {
        final GradientDrawable gd = new GradientDrawable();
        gd.setShape(GradientDrawable.RECTANGLE);
        gd.setColor(fillColor);
        gd.setCornerRadius(DeviceLayout.dp(context, radiusDp));
        if (strokeDp > 0f && strokeColor != Color.TRANSPARENT) {
            gd.setStroke(Math.max(1, DeviceLayout.dp(context, strokeDp)), strokeColor);
        }
        return gd;
    }

    static GradientDrawable pill(Context context, int fillColor, int strokeColor,
                                 float strokeDp) {
        return card(context, fillColor, strokeColor, 999f, strokeDp);
    }

    static Drawable primaryButton(Context context, Palette p) {
        return pill(context, p.accent, Color.TRANSPARENT, 0f);
    }

    static GradientDrawable circleDot(int fillColor) {
        final GradientDrawable gd = new GradientDrawable();
        gd.setShape(GradientDrawable.OVAL);
        gd.setColor(fillColor);
        return gd;
    }
}
