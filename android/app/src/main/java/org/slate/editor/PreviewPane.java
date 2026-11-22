package org.slate.editor;

import android.annotation.SuppressLint;
import android.content.Context;
import android.net.Uri;
import android.webkit.*;
import org.json.JSONObject;
import java.io.ByteArrayInputStream;

@SuppressLint("SetJavaScriptEnabled")
final class PreviewPane extends WebView {
    private static final String PAGE = "file:///android_asset/preview/index.html";
    private String script;
    private boolean ready;

    PreviewPane(Context context) {
        super(context);
        getSettings().setJavaScriptEnabled(true);
        getSettings().setAllowContentAccess(false);
        getSettings().setAllowFileAccess(false);
        getSettings().setBlockNetworkLoads(true);
        setWebViewClient(new WebViewClient() {
            @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                return !request.getUrl().toString().startsWith(PAGE + "#");
            }
            @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest req) {
                Uri uri = req.getUrl();
                String path = uri.getPath();
                if ("file".equals(uri.getScheme()) && path != null &&
                    path.startsWith("/android_asset/preview/") && !path.contains("..")) return null;
                return new WebResourceResponse("text/plain", "utf-8", new ByteArrayInputStream(new byte[0]));
            }
            @Override public void onPageFinished(WebView view, String url) {
                ready = true;
                if (script != null) evaluateJavascript(script, null);
            }
        });
        loadUrl(PAGE);
    }

    void render(String source, Appearance theme) {
        setBackgroundColor(theme.background());
        script = "window.slatePreview.render(" + JSONObject.quote(source) + "," + theme.json() + ")";
        if (ready) evaluateJavascript(script, null);
    }
}
