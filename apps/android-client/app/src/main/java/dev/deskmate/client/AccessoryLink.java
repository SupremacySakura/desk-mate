package dev.deskmate.client;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.IOException;

/**
 * Owns the UsbManager accessory handle. Native code only borrows accessoryFd.
 * Mirrors HarmonyOS AccessoryLink states: searching, authorizing, connected, error.
 */
final class AccessoryLink {
    private static final String TAG = "DeskMateLink";
    private static final String ACTION_USB_PERMISSION =
        "dev.deskmate.client.USB_PERMISSION";

    enum State {
        SEARCHING,
        AUTHORIZING,
        CONNECTED,
        ERROR
    }

    static final class Snapshot {
        final State state;
        final String message;
        final int accessoryFd;

        Snapshot(State state, String message, int accessoryFd) {
            this.state = state;
            this.message = message;
            this.accessoryFd = accessoryFd;
        }
    }

    interface Listener {
        void onStateChanged(Snapshot snapshot);
        void onAccessory(ParcelFileDescriptor descriptor, UsbAccessory accessory);
        void onDetached();
    }

    private final Context context;
    private final UsbManager manager;
    private final Listener listener;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final BroadcastReceiver receiver;

    private ParcelFileDescriptor descriptor;
    private boolean started;
    private boolean receiverRegistered;
    private boolean permissionPending;
    private Snapshot currentSnapshot = new Snapshot(State.SEARCHING, "waiting for USB host", -1);

    private final Runnable pollScan = new Runnable() {
        @Override
        public void run() {
            if (!started) {
                return;
            }
            if (descriptor == null && !permissionPending &&
                currentSnapshot.state != State.ERROR) {
                openAccessory();
            }
            handler.postDelayed(this, 1000L);
        }
    };

    AccessoryLink(Context context, Listener listener) {
        this.context = context.getApplicationContext();
        this.manager = (UsbManager) this.context.getSystemService(Context.USB_SERVICE);
        this.listener = listener;
        this.receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context ctx, Intent intent) {
                final String action = intent.getAction();
                if (ACTION_USB_PERMISSION.equals(action)) {
                    permissionPending = false;
                    final boolean granted = intent.getBooleanExtra(
                        UsbManager.EXTRA_PERMISSION_GRANTED, false);
                    if (granted) {
                        emit(State.SEARCHING, "permission granted, opening accessory", -1);
                        openAccessory();
                    } else {
                        emit(State.ERROR, "USB accessory permission denied", -1);
                    }
                } else if (UsbManager.ACTION_USB_ACCESSORY_DETACHED.equals(action)) {
                    close();
                    permissionPending = false;
                    emit(State.SEARCHING, "accessory detached, waiting for USB", -1);
                    listener.onDetached();
                } else if (UsbManager.ACTION_USB_ACCESSORY_ATTACHED.equals(action)) {
                    openAccessory();
                }
            }
        };
    }

    Snapshot snapshot() {
        return currentSnapshot;
    }

    void start() {
        if (started) {
            openAccessory();
            return;
        }
        started = true;
        if (!receiverRegistered) {
            final IntentFilter filter = new IntentFilter();
            filter.addAction(ACTION_USB_PERMISSION);
            filter.addAction(UsbManager.ACTION_USB_ACCESSORY_ATTACHED);
            filter.addAction(UsbManager.ACTION_USB_ACCESSORY_DETACHED);
            if (Build.VERSION.SDK_INT >= 33) {
                context.registerReceiver(receiver, filter,
                    Context.RECEIVER_NOT_EXPORTED);
            } else {
                context.registerReceiver(receiver, filter);
            }
            receiverRegistered = true;
        }
        emit(State.SEARCHING, "scanning USB accessories", -1);
        openAccessory();
        handler.removeCallbacks(pollScan);
        handler.postDelayed(pollScan, 1000L);
    }

    void retry() {
        permissionPending = false;
        close();
        emit(State.SEARCHING, "retrying USB accessory connection", -1);
        openAccessory();
    }

    void reconnect() {
        permissionPending = false;
        close();
        emit(State.SEARCHING, "reconnecting USB accessory", -1);
        listener.onDetached();
        handler.postDelayed(this::openAccessory, 250L);
    }

    void openAccessory() {
        if (!started || descriptor != null || manager == null) {
            return;
        }
        final UsbAccessory[] accessories = manager.getAccessoryList();
        if (accessories == null || accessories.length == 0) {
            if (currentSnapshot.state != State.SEARCHING &&
                currentSnapshot.state != State.ERROR) {
                emit(State.SEARCHING, "waiting for USB accessory", -1);
            }
            return;
        }
        final UsbAccessory accessory = accessories[0];
        if (!manager.hasPermission(accessory)) {
            if (permissionPending) {
                return;
            }
            permissionPending = true;
            emit(State.AUTHORIZING, "awaiting USB accessory permission", -1);
            final Intent permIntent = new Intent(ACTION_USB_PERMISSION);
            permIntent.setPackage(context.getPackageName());
            final PendingIntent permission = PendingIntent.getBroadcast(
                context, 0, permIntent,
                PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            manager.requestPermission(accessory, permission);
            return;
        }
        try {
            descriptor = manager.openAccessory(accessory);
        } catch (SecurityException error) {
            emit(State.ERROR, "openAccessory permission failed: " + error.getMessage(), -1);
            return;
        }
        if (descriptor == null || descriptor.getFd() <= 0) {
            close();
            emit(State.ERROR, "accessory descriptor is invalid", -1);
            return;
        }
        final int fd = descriptor.getFd();
        Log.i(TAG, "accessory opened fd=" + fd);
        emit(State.CONNECTED, "USB accessory opened (fd=" + fd + ")", fd);
        listener.onAccessory(descriptor, accessory);
    }

    void close() {
        if (descriptor != null) {
            try {
                descriptor.close();
            } catch (IOException error) {
                Log.w(TAG, "closeAccessory failed", error);
            }
            descriptor = null;
        }
    }

    void stop() {
        started = false;
        handler.removeCallbacks(pollScan);
        handler.removeCallbacksAndMessages(null);
        close();
        if (receiverRegistered) {
            try {
                context.unregisterReceiver(receiver);
            } catch (IllegalArgumentException ignored) {
            }
            receiverRegistered = false;
        }
    }

    private void emit(State state, String message, int fd) {
        currentSnapshot = new Snapshot(state, message, fd);
        listener.onStateChanged(currentSnapshot);
    }
}
