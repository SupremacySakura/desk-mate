package dev.deskmate.client;

import android.view.Surface;

final class NativeBridge {
    static {
        System.loadLibrary("deskmate_client");
    }

    static final int ACTION_DOWN = 0;
    static final int ACTION_MOVE = 1;
    static final int ACTION_UP = 2;
    static final int ACTION_CANCEL = 3;

    private NativeBridge() {}

    static native long create();

    static native boolean start(long handle, int accessoryFd, Surface surface,
                                int width, int height, int refreshHz);

    static native void requestStop(long handle);

    static native void stop(long handle);

    static native void destroy(long handle);

    static native void setSurface(long handle, Surface surface);

    static native void setViewport(long handle, int width, int height);

    static native void setPreferredCodec(long handle, String codec);

    static native void setTouchEnabled(long handle, boolean enabled);

    static native void touch(long handle, int action, int pointerId, float x,
                             float y);

    static native void touchFrame(long handle, boolean cancel, int count,
                                  int[] ids, float[] xs, float[] ys);

    static native String snapshot(long handle);
}
