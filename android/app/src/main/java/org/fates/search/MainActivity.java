package org.fates.search;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import android.widget.TextView;

import com.chaquo.python.Python;
import com.chaquo.python.android.AndroidPlatform;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public final class MainActivity extends Activity {
    private volatile boolean destroyed;
    private FrameLayout container;
    private TextView status;
    private WebView webView;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.rgb(17, 23, 15));
        getWindow().setNavigationBarColor(Color.rgb(17, 23, 15));

        container = new FrameLayout(this);
        container.setFitsSystemWindows(true);
        container.setBackgroundColor(Color.rgb(17, 23, 15));
        status = new TextView(this);
        status.setText("正在启动 Fates...");
        status.setTextColor(Color.WHITE);
        status.setTextSize(16);
        status.setGravity(Gravity.CENTER);
        container.addView(status, new FrameLayout.LayoutParams(-1, -1));
        setContentView(container);

        new Thread(() -> {
            try {
                File staticRoot = new File(getFilesDir(), "web");
                prepareAssets(staticRoot);
                File engine = new File(getApplicationInfo().nativeLibraryDir, "libfates.so");
                if (!engine.isFile() || !engine.canExecute()) {
                    throw new IOException("未找到可执行的 ARM64 Fates 引擎");
                }
                if (!Python.isStarted()) Python.start(new AndroidPlatform(this));
                String url = Python.getInstance().getModule("android_bridge")
                    .callAttr("start", engine.getAbsolutePath(), staticRoot.getAbsolutePath())
                    .toString();
                if (destroyed) {
                    Python.getInstance().getModule("android_bridge").callAttr("stop");
                } else {
                    runOnUiThread(() -> showWebView(url));
                }
            } catch (Exception error) {
                runOnUiThread(() -> {
                    if (!destroyed) status.setText("启动失败：" + error.getMessage());
                });
            }
        }, "fates-startup").start();
    }

    private void prepareAssets(File root) throws Exception {
        int version = getPackageManager().getPackageInfo(getPackageName(), 0).versionCode;
        if (getPreferences(MODE_PRIVATE).getInt("webVersion", -1) == version &&
            new File(root, "index.html").isFile()) return;
        copyAssetTree(getAssets(), "web", root);
        getPreferences(MODE_PRIVATE).edit().putInt("webVersion", version).apply();
    }

    private static void copyAssetTree(AssetManager assets, String source, File destination)
            throws IOException {
        String[] children = assets.list(source);
        if (children == null) throw new IOException("无法读取内置 WebUI：" + source);
        if (children.length == 0) {
            try (InputStream input = assets.open(source);
                 OutputStream output = new FileOutputStream(destination)) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            }
            return;
        }
        if (!destination.isDirectory() && !destination.mkdirs()) {
            throw new IOException("无法写入内置 WebUI：" + destination);
        }
        for (String child : children) {
            copyAssetTree(assets, source + "/" + child, new File(destination, child));
        }
    }

    private void showWebView(String url) {
        if (destroyed) return;
        Uri origin = Uri.parse(url);
        webView = new WebView(this);
        webView.setBackgroundColor(Color.rgb(17, 23, 15));
        webView.getSettings().setJavaScriptEnabled(true);
        webView.getSettings().setDomStorageEnabled(true);
        webView.getSettings().setAllowFileAccess(false);
        webView.getSettings().setAllowContentAccess(false);
        webView.getSettings().setMixedContentMode(android.webkit.WebSettings.MIXED_CONTENT_NEVER_ALLOW);
        webView.addJavascriptInterface(new ClipboardBridge(), "FatesAndroid");
        webView.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                Uri target = request.getUrl();
                return !"http".equals(target.getScheme()) ||
                    !"127.0.0.1".equals(target.getHost()) || target.getPort() != origin.getPort();
            }

            @Override
            public void onPageFinished(WebView view, String page) {
                if (url.equals(page)) status.setVisibility(View.GONE);
            }
        });
        container.addView(webView, 0, new FrameLayout.LayoutParams(-1, -1));
        webView.loadUrl(url);
    }

    private final class ClipboardBridge {
        @JavascriptInterface
        public void copyText(String value) {
            ClipboardManager clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
            clipboard.setPrimaryClip(ClipData.newPlainText("Fates", value));
        }
    }

    @Override
    public void onBackPressed() {
        if (webView != null && webView.canGoBack()) webView.goBack();
        else super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        if (webView != null) {
            container.removeView(webView);
            webView.destroy();
        }
        if (Python.isStarted()) {
            new Thread(() -> Python.getInstance().getModule("android_bridge").callAttr("stop"),
                       "fates-shutdown").start();
        }
        super.onDestroy();
    }
}
