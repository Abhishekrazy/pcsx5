package org.pcsx5.experimental;

import android.app.Activity;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.view.View;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    static { System.loadLibrary("pcsx5_app"); }
    private static native boolean nativeSelfTest();
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private volatile boolean destroyed;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int padding = (int)(24 * getResources().getDisplayMetrics().density);
        layout.setPadding(padding, padding, padding, padding);
        TextView status = new TextView(this);
        status.setTextSize(20);
        status.setText("PCSX5 Experimental\n\nSynthetic scalar developer build. No PS5 game compatibility.\n\nRun the self-test to compare the ARM64 translator with the interpreter inside this app.");
        Button run = new Button(this);
        run.setText("Run synthetic self-test");
        run.setOnClickListener(new View.OnClickListener() {
          @Override public void onClick(View view) {
            run.setEnabled(false);
            status.setText("Running bounded synthetic test…");
            worker.execute(new Runnable() {
              @Override public void run() {
                final boolean passed = nativeSelfTest();
                runOnUiThread(new Runnable() {
                  @Override public void run() {
                    if (destroyed) return;
                    status.setText(passed ? "PASS: ARM64 and interpreter agree.\nThis is not a game-compatibility result." : "FAIL: execution or JIT capability unavailable. No success inferred.");
                    run.setEnabled(true);
                  }
                });
              }
            });
          }
        });
        layout.addView(status);
        layout.addView(run);
        setContentView(layout);
    }
    @Override protected void onDestroy() {
        destroyed = true;
        worker.shutdownNow();
        super.onDestroy();
    }
}
