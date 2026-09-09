package org.pcsx5.experimental;

import android.app.Activity;
import android.app.Application;
import android.app.Instrumentation;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.TextView;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

// Compiled/registered only by build-android-app.ps1 -DeviceTests. No AndroidX
// or privileged device-wide interaction; exercises this app's real UI/JNI path.
public final class DeviceAcceptance extends Instrumentation {
    private final LinkedBlockingQueue<Activity> resumed = new LinkedBlockingQueue<>();
    private final LinkedBlockingQueue<Activity> stopped = new LinkedBlockingQueue<>();
    private final LinkedBlockingQueue<Activity> destroyed = new LinkedBlockingQueue<>();
    private final Application.ActivityLifecycleCallbacks lifecycle = new Application.ActivityLifecycleCallbacks() {
        public void onActivityCreated(Activity a, Bundle state) {}
        public void onActivityStarted(Activity a) {}
        public void onActivityResumed(Activity a) { if (a instanceof MainActivity) resumed.add(a); }
        public void onActivityPaused(Activity a) {}
        public void onActivityStopped(Activity a) { if (a instanceof MainActivity) stopped.add(a); }
        public void onActivitySaveInstanceState(Activity a, Bundle state) {}
        public void onActivityDestroyed(Activity a) { if (a instanceof MainActivity) destroyed.add(a); }
    };
    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    private static <T extends View> T find(View view, Class<T> type) {
        if (type.isInstance(view) && !(type == TextView.class && view instanceof Button)) return type.cast(view);
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup)view;
            for (int i = 0; i < group.getChildCount(); ++i) {
                T result = find(group.getChildAt(i), type);
                if (result != null) return result;
            }
        }
        return null;
    }
    private void runTest(final Activity activity) throws Exception {
        final CountDownLatch complete = new CountDownLatch(1);
        final TextView[] status = new TextView[1];
        final Button[] button = new Button[1];
        final TextWatcher watcher = new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            public void onTextChanged(CharSequence s, int start, int before, int count) {
                if (s.toString().startsWith("PASS:") || s.toString().startsWith("FAIL:")) complete.countDown();
            }
            public void afterTextChanged(Editable s) {}
        };
        runOnMainSync(new Runnable() { public void run() {
            View content = activity.findViewById(android.R.id.content);
            status[0] = find(content, TextView.class);
            button[0] = find(content, Button.class);
            require(status[0] != null && button[0] != null && button[0].isEnabled(), "Missing/enabled UI controls");
            status[0].addTextChangedListener(watcher);
            require(button[0].performClick(), "Button did not handle click");
        }});
        require(complete.await(15, TimeUnit.SECONDS), "Native/UI self-test completion timed out");
        runOnMainSync(new Runnable() { public void run() {
            status[0].removeTextChangedListener(watcher);
            require(status[0].getText().toString().startsWith("PASS:"), "Native self-test reported failure");
            require(button[0].isEnabled(), "Button not restored after completion");
        }});
    }
    private Activity next(LinkedBlockingQueue<Activity> queue, String event) throws Exception {
        Activity result = queue.poll(15, TimeUnit.SECONDS);
        require(result != null, "Timed out waiting for " + event);
        return result;
    }
    @Override public void onStart() {
        Bundle result = new Bundle();
        Application app = (Application)getTargetContext().getApplicationContext();
        app.registerActivityLifecycleCallbacks(lifecycle);
        Activity current = null;
        try {
            Intent launch = new Intent(getTargetContext(), MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            current = startActivitySync(launch);
            require(next(resumed, "launch") == current, "Wrong launched activity");
            runTest(current); runTest(current);
            resumed.clear(); destroyed.clear();
            final Activity original = current;
            runOnMainSync(new Runnable() { public void run() { original.recreate(); }});
            current = next(resumed, "recreation");
            require(current != original && next(destroyed, "old instance destruction") == original, "Recreation did not replace activity");
            runTest(current);
            resumed.clear(); destroyed.clear();
            final Activity beforeRotation = current;
            runOnMainSync(new Runnable() { public void run() {
                int orientation = beforeRotation.getResources().getConfiguration().orientation;
                beforeRotation.setRequestedOrientation(orientation == Configuration.ORIENTATION_LANDSCAPE ?
                    ActivityInfo.SCREEN_ORIENTATION_PORTRAIT : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
            }});
            current = next(resumed, "orientation change");
            require(current != beforeRotation && next(destroyed, "rotation destruction") == beforeRotation, "Rotation did not recreate activity");
            runTest(current);
            stopped.clear(); resumed.clear();
            final Activity background = current;
            runOnMainSync(new Runnable() { public void run() { require(background.moveTaskToBack(true), "Could not background own task"); }});
            require(next(stopped, "background stop") == background, "Wrong stopped activity");
            getTargetContext().startActivity(launch.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
            current = next(resumed, "foreground resume");
            runTest(current);
            destroyed.clear();
            final Activity finished = current;
            runOnMainSync(new Runnable() { public void run() { finished.finish(); }});
            require(next(destroyed, "finish destruction") == finished, "Wrong finished activity");
            current = null;
            result.putString("pcsx5", "PASS: 5 real UI/JNI tests; repeat, recreate, rotate, background/resume, finish");
            finish(Activity.RESULT_OK, result);
        } catch (Throwable failure) {
            result.putString("pcsx5", "FAIL: " + failure.toString());
            if (current != null) {
                final Activity failed = current;
                runOnMainSync(new Runnable() { public void run() { failed.finish(); }});
            }
            finish(Activity.RESULT_CANCELED, result);
        } finally { app.unregisterActivityLifecycleCallbacks(lifecycle); }
    }
}
