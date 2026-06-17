/*
 * txsyxts login helper — opens a WebKit window to Spotify OAuth login,
 * captures the authorization code from the redirect URL, and passes it back.
 *
 * Output format (stdout):
 *   line 1: code=YOUR_AUTH_CODE
 */

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <string>
#include <cstring>
#include <iostream>

#include "config.hpp"



static bool code_found = false;
static std::string captured_code;
static WebKitWebView* g_web_view = nullptr;
static GtkWidget* g_window = nullptr;

static void close_window() {
    g_timeout_add(400, [](gpointer) -> gboolean {
        gtk_main_quit();
        return FALSE;
    }, nullptr);
}

static void check_and_exit() {
    if (code_found) {
        std::cout << "code=" << captured_code << std::endl;
        std::cout.flush();
        close_window();
    }
}

static void on_load_changed(WebKitWebView* web_view, WebKitLoadEvent event, gpointer) {
    if (code_found) return;

    const gchar* uri = webkit_web_view_get_uri(web_view);
    if (!uri) return;

    if (event == WEBKIT_LOAD_STARTED) {
        std::cerr << "[txsyxts-login] loading: " << uri << std::endl;
    }

    std::string url(uri);
    std::string prefix = "http://localhost:4304/auth/spotify/callback?code=";
    
    if (url.find(prefix) == 0) {
        captured_code = url.substr(prefix.length());
        
        // Remove state param if present (e.g., &state=...)
        size_t ampersand = captured_code.find('&');
        if (ampersand != std::string::npos) {
            captured_code = captured_code.substr(0, ampersand);
        }
        
        code_found = true;
        check_and_exit();
    }
}

static gboolean on_load_failed(WebKitWebView*, WebKitLoadEvent,
                                gchar* failing_uri, GError* error, gpointer) {
    // If it fails to load localhost, that's completely fine because we just need the URL
    std::string uri(failing_uri ? failing_uri : "");
    if (uri.find("http://localhost:4304/auth/spotify/callback?code=") == 0) {
        return FALSE; // handled by load_changed
    }
    
    std::cerr << "[txsyxts-login] load failed: " << uri
              << " - " << error->message << std::endl;
    return FALSE;
}

static gboolean on_close(GtkWidget*, GdkEvent*, gpointer) {
    gtk_main_quit();
    return FALSE;
}

// ── main ───────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);

    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), "txsyxts — Spotify Login");
    gtk_window_set_default_size(GTK_WINDOW(g_window), 480, 720);
    gtk_window_set_position(GTK_WINDOW(g_window), GTK_WIN_POS_CENTER);
    g_signal_connect(g_window, "delete-event", G_CALLBACK(on_close), nullptr);

    // cookies for persistent session
    WebKitWebContext* context = webkit_web_context_get_default();
    WebKitCookieManager* cookie_mgr = webkit_web_context_get_cookie_manager(context);

    std::string cookie_path = txs::Config::config_dir() + "/webkit_cookies";
    webkit_cookie_manager_set_persistent_storage(
        cookie_mgr,
        cookie_path.c_str(),
        WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);

    webkit_cookie_manager_set_accept_policy(
        cookie_mgr, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);

    g_web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    g_signal_connect(g_web_view, "load-changed", G_CALLBACK(on_load_changed), nullptr);
    g_signal_connect(g_web_view, "load-failed", G_CALLBACK(on_load_failed), nullptr);

    WebKitSettings* settings = webkit_web_view_get_settings(g_web_view);
    webkit_settings_set_user_agent(settings,
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36");

    gtk_container_add(GTK_CONTAINER(g_window), GTK_WIDGET(g_web_view));
    gtk_widget_show_all(g_window);

    auto cfg = txs::Config::load();
    std::string login_url = "https://accounts.spotify.com/authorize?client_id=" + cfg.client_id + "&response_type=code&redirect_uri=http://localhost:4304/auth/spotify/callback&code_challenge_method=S256&code_challenge=JBIFC0c6YIrp95gP0fPDARNmhGVeRrQrbuqWYnHVqIU&scope=user-library-read%20playlist-read-private";

    std::cerr << "[txsyxts-login] opening OAuth..." << std::endl;
    webkit_web_view_load_uri(g_web_view, login_url.c_str());

    gtk_main();

    if (!code_found) {
        std::cerr << "[txsyxts-login] login cancelled" << std::endl;
        return 1;
    }
    return 0;
}
