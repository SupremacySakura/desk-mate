package dev.deskmate.client;

import android.app.Activity;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Build;
import android.util.DisplayMetrics;
import android.view.DisplayCutout;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowMetrics;

/**
 * Responsive device classification, orientation policy, and safe-area helpers
 * mirroring HarmonyOS DeviceLayout.ets.
 */
final class DeviceLayout {
    static final class SafeInsets {
        final int top;
        final int bottom;
        final int left;
        final int right;

        SafeInsets(int top, int bottom, int left, int right) {
            this.top = top;
            this.bottom = bottom;
            this.left = left;
            this.right = right;
        }
    }

    private DeviceLayout() {}

    static boolean isPhoneDevice(Context context) {
        final Configuration config = context.getResources().getConfiguration();
        return config.smallestScreenWidthDp < 600;
    }

    static boolean isCompactLayout(Activity activity) {
        return windowWidthDp(activity) < 600f;
    }

    static float windowWidthDp(Activity activity) {
        final float density = activity.getResources().getDisplayMetrics().density;
        if (density <= 0f) {
            return activity.getResources().getConfiguration().screenWidthDp;
        }
        if (Build.VERSION.SDK_INT >= 30) {
            final WindowMetrics metrics = activity.getWindowManager().getCurrentWindowMetrics();
            final Rect bounds = metrics.getBounds();
            if (bounds.width() > 0) {
                return bounds.width() / density;
            }
        }
        final DisplayMetrics dm = new DisplayMetrics();
        @SuppressWarnings("deprecation")
        final android.view.Display display = activity.getWindowManager().getDefaultDisplay();
        if (display != null) {
            @SuppressWarnings("deprecation")
            final Runnable ignore = () -> display.getMetrics(dm);
            ignore.run();
            if (dm.widthPixels > 0) {
                return dm.widthPixels / density;
            }
        }
        return activity.getResources().getConfiguration().screenWidthDp;
    }

    static int preferredOrientationForPage(boolean isPhone, boolean inDisplay) {
        if (!isPhone) {
            return ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
        }
        return inDisplay
            ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            : ActivityInfo.SCREEN_ORIENTATION_PORTRAIT;
    }

    static SafeInsets readSafeInsets(WindowInsets insets) {
        if (insets == null) {
            return new SafeInsets(0, 0, 0, 0);
        }
        if (Build.VERSION.SDK_INT >= 30) {
            final android.graphics.Insets sys = insets.getInsetsIgnoringVisibility(
                WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            return new SafeInsets(
                Math.max(0, sys.top),
                Math.max(0, sys.bottom),
                Math.max(0, sys.left),
                Math.max(0, sys.right));
        }
        @SuppressWarnings("deprecation")
        int top = Math.max(0, insets.getSystemWindowInsetTop());
        @SuppressWarnings("deprecation")
        int bottom = Math.max(0, insets.getSystemWindowInsetBottom());
        @SuppressWarnings("deprecation")
        int left = Math.max(0, insets.getSystemWindowInsetLeft());
        @SuppressWarnings("deprecation")
        int right = Math.max(0, insets.getSystemWindowInsetRight());
        if (Build.VERSION.SDK_INT >= 28) {
            final DisplayCutout cutout = insets.getDisplayCutout();
            if (cutout != null) {
                top = Math.max(top, cutout.getSafeInsetTop());
                bottom = Math.max(bottom, cutout.getSafeInsetBottom());
                left = Math.max(left, cutout.getSafeInsetLeft());
                right = Math.max(right, cutout.getSafeInsetRight());
            }
        }
        return new SafeInsets(top, bottom, left, right);
    }

    @SuppressWarnings("deprecation")
    static void applyWindowPresentation(Activity activity, boolean isPhone, boolean inDisplay) {
        final Window window = activity.getWindow();
        if (window == null) {
            return;
        }
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false);
            final WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                if (inDisplay) {
                    controller.hide(WindowInsets.Type.systemBars());
                    controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                    controller.setSystemBarsAppearance(0,
                        WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                            | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS);
                    window.setStatusBarColor(Color.BLACK);
                    window.setNavigationBarColor(Color.BLACK);
                } else {
                    controller.show(WindowInsets.Type.systemBars());
                    controller.setSystemBarsAppearance(
                        WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                            | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS,
                        WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                            | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS);
                    window.setStatusBarColor(StudioDrawables.COLOR_CANVAS_MID);
                    window.setNavigationBarColor(StudioDrawables.COLOR_SHELL_CHROME);
                }
            }
        } else {
            final View decor = window.getDecorView();
            if (inDisplay) {
                decor.setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
                window.setStatusBarColor(Color.BLACK);
                window.setNavigationBarColor(Color.BLACK);
            } else {
                int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
                if (Build.VERSION.SDK_INT >= 26) {
                    flags |= View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
                }
                decor.setSystemUiVisibility(flags);
                window.setStatusBarColor(StudioDrawables.COLOR_CANVAS_MID);
                window.setNavigationBarColor(StudioDrawables.COLOR_SHELL_CHROME);
            }
        }

        if (isPhone) {
            final int targetOrientation = preferredOrientationForPage(true, inDisplay);
            if (activity.getRequestedOrientation() != targetOrientation) {
                activity.setRequestedOrientation(targetOrientation);
            }
        }
    }

    static int dp(Context context, float value) {
        final float density = context.getResources().getDisplayMetrics().density;
        return Math.round(value * (density > 0f ? density : 1f));
    }
}
