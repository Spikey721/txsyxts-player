/*
 * mpris.cpp — MPRIS 2.2 MediaPlayer2 + MediaPlayer2.Player
 *
 * Registers as  org.mpris.MediaPlayer2.txsyxts
 * Object path:  /org/mpris/MediaPlayer2
 *
 * Uses the raw libdbus-1 C API (always present on a normal Linux desktop,
 * no additional C++ wrapper required).
 */

#include "mpris.hpp"

#include <dbus/dbus.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace txs {

// ── MPRIS introspection XML ────────────────────────────────────────────────
static const char* INTROSPECT_XML = R"xml(
<!DOCTYPE node PUBLIC
  "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"
  "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node name="/org/mpris/MediaPlayer2">

  <interface name="org.freedesktop.DBus.Introspectable">
    <method name="Introspect">
      <arg direction="out" name="xml_data" type="s"/>
    </method>
  </interface>

  <interface name="org.mpris.MediaPlayer2">
    <property name="Identity"       type="s" access="read"/>
    <property name="DesktopEntry"   type="s" access="read"/>
    <property name="CanQuit"        type="b" access="read"/>
    <property name="CanRaise"       type="b" access="read"/>
    <property name="HasTrackList"   type="b" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
    <method name="Quit"/>
    <method name="Raise"/>
  </interface>

  <interface name="org.mpris.MediaPlayer2.Player">
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="LoopStatus"     type="s" access="readwrite"/>
    <property name="Rate"           type="d" access="readwrite"/>
    <property name="Shuffle"        type="b" access="readwrite"/>
    <property name="Metadata"       type="a{sv}" access="read"/>
    <property name="Volume"         type="d" access="readwrite"/>
    <property name="Position"       type="x" access="read"/>
    <property name="MinimumRate"    type="d" access="read"/>
    <property name="MaximumRate"    type="d" access="read"/>
    <property name="CanGoNext"      type="b" access="read"/>
    <property name="CanGoPrevious"  type="b" access="read"/>
    <property name="CanPlay"        type="b" access="read"/>
    <property name="CanPause"       type="b" access="read"/>
    <property name="CanSeek"        type="b" access="read"/>
    <property name="CanControl"     type="b" access="read"/>
    <method name="Next"/>
    <method name="Previous"/>
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Stop"/>
    <method name="Play"/>
    <method name="Seek">
      <arg direction="in" name="Offset" type="x"/>
    </method>
    <method name="SetPosition">
      <arg direction="in" name="TrackId" type="o"/>
      <arg direction="in" name="Position" type="x"/>
    </method>
    <method name="OpenUri">
      <arg direction="in" name="Uri" type="s"/>
    </method>
  </interface>

</node>
)xml";

// ── shared player state (thread-safe) ────────────────────────────────────
struct PlayerState {
    std::string title;
    std::string artist;
    std::string art_url;
    double position_sec = 0.0;
    double duration_sec = 0.0;
    bool playing = false;
    bool paused  = false;
};

// ── Impl ──────────────────────────────────────────────────────────────────
struct MprisImpl {
    DBusConnection* conn = nullptr;
    std::thread     thread;
    std::atomic<bool> running{false};

    std::mutex   state_mtx;
    PlayerState  state;
    PlayerState  prev_state; // to detect what changed

    // weak back-ref to parent for callbacks
    MprisServer* owner = nullptr;
};

// ── helpers ───────────────────────────────────────────────────────────────

static void append_string(DBusMessageIter* iter, const char* val) {
    dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &val);
}

static void append_bool(DBusMessageIter* iter, dbus_bool_t val) {
    dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &val);
}

static void append_double(DBusMessageIter* iter, double val) {
    dbus_message_iter_append_basic(iter, DBUS_TYPE_DOUBLE, &val);
}

static void append_int64(DBusMessageIter* iter, dbus_int64_t val) {
    dbus_message_iter_append_basic(iter, DBUS_TYPE_INT64, &val);
}

// Append an a{sv} dict entry with a string value
static void dict_append_string(DBusMessageIter* dict,
                                const char* key, const char* val) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    append_string(&entry, key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    append_string(&variant, val);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

// Append an a{sv} dict entry with an int64 value
static void dict_append_int64(DBusMessageIter* dict,
                               const char* key, dbus_int64_t val) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    append_string(&entry, key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "x", &variant);
    append_int64(&variant, val);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

// Append an a{sv} dict entry with an object-path value
static void dict_append_objpath(DBusMessageIter* dict,
                                const char* key, const char* path) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    append_string(&entry, key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "o", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

// Append an a{sv} dict entry with an as (array-of-strings) value
static void dict_append_as(DBusMessageIter* dict,
                            const char* key, const char** strings, int n) {
    DBusMessageIter entry, variant, arr;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    append_string(&entry, key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant);
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &arr);
    for (int i = 0; i < n; ++i) {
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &strings[i]);
    }
    dbus_message_iter_close_container(&variant, &arr);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

// ── reply builders ────────────────────────────────────────────────────────

static void reply_introspect(DBusConnection* conn, DBusMessage* msg) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    append_string(&args, INTROSPECT_XML);
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
}

static void reply_void(DBusConnection* conn, DBusMessage* msg) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
}

// Build and append Metadata dict (a{sv}) into an existing iterator
static void append_metadata(DBusMessageIter* outer, const PlayerState& ps) {
    // The Metadata property is of type a{sv}
    DBusMessageIter dict;
    dbus_message_iter_open_container(outer, DBUS_TYPE_ARRAY, "{sv}", &dict);

    // mpris:trackid (required by MPRIS spec)
    const char* trackid = "/org/txsyxts/track/1";
    dict_append_objpath(&dict, "mpris:trackid", trackid);

    if (!ps.title.empty()) {
        dict_append_string(&dict, "xesam:title", ps.title.c_str());
    }

    if (!ps.artist.empty()) {
        // xesam:artist is as (array of strings)
        const char* artists[1] = { ps.artist.c_str() };
        dict_append_as(&dict, "xesam:artist", artists, 1);
    }

    if (!ps.art_url.empty()) {
        dict_append_string(&dict, "mpris:artUrl", ps.art_url.c_str());
    }

    if (ps.duration_sec > 0.0) {
        dbus_int64_t dur_us = static_cast<dbus_int64_t>(ps.duration_sec * 1e6);
        dict_append_int64(&dict, "mpris:length", dur_us);
    }

    dbus_message_iter_close_container(outer, &dict);
}

// Handle org.freedesktop.DBus.Properties.Get
static void handle_get(DBusConnection* conn, DBusMessage* msg,
                        const PlayerState& ps) {
    const char* iface = nullptr;
    const char* prop  = nullptr;
    DBusMessageIter args;
    if (!dbus_message_iter_init(msg, &args)) return;
    dbus_message_iter_get_basic(&args, &iface);
    dbus_message_iter_next(&args);
    dbus_message_iter_get_basic(&args, &prop);

    DBusMessage* reply = dbus_message_new_method_return(msg);
    DBusMessageIter out, var;
    dbus_message_iter_init_append(reply, &out);

    auto send_str = [&](const char* val) {
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT, "s", &var);
        append_string(&var, val);
        dbus_message_iter_close_container(&out, &var);
    };
    auto send_bool = [&](dbus_bool_t val) {
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT, "b", &var);
        append_bool(&var, val);
        dbus_message_iter_close_container(&out, &var);
    };
    auto send_double = [&](double val) {
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT, "d", &var);
        append_double(&var, val);
        dbus_message_iter_close_container(&out, &var);
    };
    auto send_int64 = [&](dbus_int64_t val) {
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT, "x", &var);
        append_int64(&var, val);
        dbus_message_iter_close_container(&out, &var);
    };

    bool handled = true;

    if (strcmp(iface, "org.mpris.MediaPlayer2") == 0) {
        if (strcmp(prop, "Identity")     == 0) send_str("txsyxts");
        else if (strcmp(prop, "DesktopEntry") == 0) send_str("txsyxts");
        else if (strcmp(prop, "CanQuit")  == 0) send_bool(FALSE);
        else if (strcmp(prop, "CanRaise") == 0) send_bool(FALSE);
        else if (strcmp(prop, "HasTrackList") == 0) send_bool(FALSE);
        else if (strcmp(prop, "SupportedUriSchemes") == 0) {
            dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT, "as", &var);
            DBusMessageIter arr;
            dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &arr);
            dbus_message_iter_close_container(&var, &arr);
            dbus_message_iter_close_container(&out, &var);
        }
        else if (strcmp(prop, "SupportedMimeTypes") == 0) {
            dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT, "as", &var);
            DBusMessageIter arr;
            dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &arr);
            dbus_message_iter_close_container(&var, &arr);
            dbus_message_iter_close_container(&out, &var);
        }
        else handled = false;
    } else if (strcmp(iface, "org.mpris.MediaPlayer2.Player") == 0) {
        if (strcmp(prop, "PlaybackStatus") == 0) {
            const char* status = ps.playing ? (ps.paused ? "Paused" : "Playing") : "Stopped";
            send_str(status);
        }
        else if (strcmp(prop, "LoopStatus")     == 0) send_str("None");
        else if (strcmp(prop, "Rate")           == 0) send_double(1.0);
        else if (strcmp(prop, "Shuffle")        == 0) send_bool(FALSE);
        else if (strcmp(prop, "Volume")         == 0) send_double(1.0);
        else if (strcmp(prop, "Position")       == 0) {
            send_int64(static_cast<dbus_int64_t>(ps.position_sec * 1e6));
        }
        else if (strcmp(prop, "MinimumRate")    == 0) send_double(1.0);
        else if (strcmp(prop, "MaximumRate")    == 0) send_double(1.0);
        else if (strcmp(prop, "CanGoNext")      == 0) send_bool(TRUE);
        else if (strcmp(prop, "CanGoPrevious")  == 0) send_bool(TRUE);
        else if (strcmp(prop, "CanPlay")        == 0) send_bool(TRUE);
        else if (strcmp(prop, "CanPause")       == 0) send_bool(TRUE);
        else if (strcmp(prop, "CanSeek")        == 0) send_bool(TRUE);
        else if (strcmp(prop, "CanControl")     == 0) send_bool(TRUE);
        else if (strcmp(prop, "Metadata") == 0) {
            dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT, "a{sv}", &var);
            append_metadata(&var, ps);
            dbus_message_iter_close_container(&out, &var);
        }
        else handled = false;
    } else {
        handled = false;
    }

    if (!handled) {
        dbus_message_unref(reply);
        DBusMessage* err = dbus_message_new_error(msg,
            DBUS_ERROR_UNKNOWN_PROPERTY, prop);
        dbus_connection_send(conn, err, nullptr);
        dbus_message_unref(err);
        return;
    }

    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
}

// Handle org.freedesktop.DBus.Properties.GetAll
static void handle_getall(DBusConnection* conn, DBusMessage* msg,
                            const PlayerState& ps) {
    const char* iface = nullptr;
    DBusMessageIter args;
    if (!dbus_message_iter_init(msg, &args)) return;
    dbus_message_iter_get_basic(&args, &iface);

    DBusMessage* reply = dbus_message_new_method_return(msg);
    DBusMessageIter out, dict;
    dbus_message_iter_init_append(reply, &out);
    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{sv}", &dict);

    auto add_str = [&](const char* key, const char* val) {
        DBusMessageIter e, v;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
        append_string(&e, key);
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &v);
        append_string(&v, val);
        dbus_message_iter_close_container(&e, &v);
        dbus_message_iter_close_container(&dict, &e);
    };
    auto add_bool = [&](const char* key, dbus_bool_t val) {
        DBusMessageIter e, v;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
        append_string(&e, key);
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "b", &v);
        append_bool(&v, val);
        dbus_message_iter_close_container(&e, &v);
        dbus_message_iter_close_container(&dict, &e);
    };
    auto add_double = [&](const char* key, double val) {
        DBusMessageIter e, v;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
        append_string(&e, key);
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "d", &v);
        append_double(&v, val);
        dbus_message_iter_close_container(&e, &v);
        dbus_message_iter_close_container(&dict, &e);
    };
    auto add_int64 = [&](const char* key, dbus_int64_t val) {
        DBusMessageIter e, v;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
        append_string(&e, key);
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "x", &v);
        append_int64(&v, val);
        dbus_message_iter_close_container(&e, &v);
        dbus_message_iter_close_container(&dict, &e);
    };

    if (strcmp(iface, "org.mpris.MediaPlayer2") == 0) {
        add_str("Identity", "txsyxts");
        add_str("DesktopEntry", "txsyxts");
        add_bool("CanQuit",      FALSE);
        add_bool("CanRaise",     FALSE);
        add_bool("HasTrackList", FALSE);
        // SupportedUriSchemes / SupportedMimeTypes — empty arrays
        for (const char* key : {"SupportedUriSchemes", "SupportedMimeTypes"}) {
            DBusMessageIter e, v, arr;
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
            append_string(&e, key);
            dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "as", &v);
            dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "s", &arr);
            dbus_message_iter_close_container(&v, &arr);
            dbus_message_iter_close_container(&e, &v);
            dbus_message_iter_close_container(&dict, &e);
        }
    } else if (strcmp(iface, "org.mpris.MediaPlayer2.Player") == 0) {
        const char* status = ps.playing ? (ps.paused ? "Paused" : "Playing") : "Stopped";
        add_str("PlaybackStatus", status);
        add_str("LoopStatus",     "None");
        add_double("Rate",         1.0);
        add_bool("Shuffle",        FALSE);
        add_double("Volume",       1.0);
        add_int64("Position",  static_cast<dbus_int64_t>(ps.position_sec * 1e6));
        add_double("MinimumRate",  1.0);
        add_double("MaximumRate",  1.0);
        add_bool("CanGoNext",     TRUE);
        add_bool("CanGoPrevious", TRUE);
        add_bool("CanPlay",       TRUE);
        add_bool("CanPause",      TRUE);
        add_bool("CanSeek",       TRUE);
        add_bool("CanControl",    TRUE);

        // Metadata
        DBusMessageIter e, v;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
        append_string(&e, "Metadata");
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "a{sv}", &v);
        append_metadata(&v, ps);
        dbus_message_iter_close_container(&e, &v);
        dbus_message_iter_close_container(&dict, &e);
    }

    dbus_message_iter_close_container(&out, &dict);
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
}

// ── message filter ────────────────────────────────────────────────────────

static DBusHandlerResult message_handler(DBusConnection* conn,
                                          DBusMessage*    msg,
                                          void*           data) {
    auto* impl = static_cast<MprisImpl*>(data);

    const char* path   = dbus_message_get_path(msg);
    const char* iface  = dbus_message_get_interface(msg);
    const char* member = dbus_message_get_member(msg);

    if (!path || strcmp(path, "/org/mpris/MediaPlayer2") != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // ── Introspect ─────────────────────────────────────────────────────
    if (iface && strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0
             && strcmp(member, "Introspect") == 0) {
        reply_introspect(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    // ── Properties ─────────────────────────────────────────────────────
    if (iface && strcmp(iface, "org.freedesktop.DBus.Properties") == 0) {
        PlayerState ps;
        { std::lock_guard<std::mutex> lk(impl->state_mtx); ps = impl->state; }

        if (strcmp(member, "Get") == 0) {
            handle_get(conn, msg, ps);
        } else if (strcmp(member, "GetAll") == 0) {
            handle_getall(conn, msg, ps);
        } else if (strcmp(member, "Set") == 0) {
            // Ignore write attempts (read-only properties for now)
            reply_void(conn, msg);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    // ── org.mpris.MediaPlayer2 ──────────────────────────────────────────
    if (iface && strcmp(iface, "org.mpris.MediaPlayer2") == 0) {
        reply_void(conn, msg); // Quit / Raise — no-op
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    // ── org.mpris.MediaPlayer2.Player ──────────────────────────────────
    if (iface && strcmp(iface, "org.mpris.MediaPlayer2.Player") == 0) {
        auto* owner = impl->owner;

        if (strcmp(member, "PlayPause") == 0) {
            if (owner && owner->on_play_pause) owner->on_play_pause();
        } else if (strcmp(member, "Play") == 0) {
            if (owner && owner->on_play_pause) owner->on_play_pause();
        } else if (strcmp(member, "Pause") == 0) {
            if (owner && owner->on_play_pause) owner->on_play_pause();
        } else if (strcmp(member, "Stop") == 0) {
            if (owner && owner->on_stop) owner->on_stop();
        } else if (strcmp(member, "Next") == 0) {
            if (owner && owner->on_next) owner->on_next();
        } else if (strcmp(member, "Previous") == 0) {
            if (owner && owner->on_prev) owner->on_prev();
        } else if (strcmp(member, "Seek") == 0) {
            DBusMessageIter it;
            if (dbus_message_iter_init(msg, &it)) {
                dbus_int64_t offset_us = 0;
                dbus_message_iter_get_basic(&it, &offset_us);
                if (owner && owner->on_seek) {
                    PlayerState ps;
                    { std::lock_guard<std::mutex> lk(impl->state_mtx); ps = impl->state; }
                    double new_pos = ps.position_sec + offset_us / 1e6;
                    owner->on_seek(new_pos < 0 ? 0 : new_pos);
                }
            }
        } else if (strcmp(member, "SetPosition") == 0) {
            DBusMessageIter it;
            if (dbus_message_iter_init(msg, &it)) {
                dbus_message_iter_next(&it); // skip TrackId object path
                dbus_int64_t pos_us = 0;
                dbus_message_iter_get_basic(&it, &pos_us);
                if (owner && owner->on_seek) {
                    owner->on_seek(pos_us / 1e6);
                }
            }
        } else if (strcmp(member, "OpenUri") == 0) {
            // Not supported
        }

        reply_void(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

// ── MprisServer ────────────────────────────────────────────────────────────

MprisServer::MprisServer() : impl_(new MprisImpl) {
    static_cast<MprisImpl*>(impl_)->owner = this;
}

MprisServer::~MprisServer() {
    stop();
    delete static_cast<MprisImpl*>(impl_);
}

void MprisServer::start() {
    auto* im = static_cast<MprisImpl*>(impl_);
    DBusError err;
    dbus_error_init(&err);

    im->conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return;
    }
    if (!im->conn) return;

    int ret = dbus_bus_request_name(im->conn,
                                     "org.mpris.MediaPlayer2.txsyxts",
                                     DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return;
    }
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) return;

    // Register object path handler
    static const DBusObjectPathVTable vtable = {
        nullptr,
        message_handler,
        nullptr, nullptr, nullptr, nullptr
    };
    dbus_connection_register_object_path(im->conn,
                                          "/org/mpris/MediaPlayer2",
                                          &vtable, im);

    im->running = true;
    im->thread = std::thread([im]() {
        while (im->running) {
            dbus_connection_read_write_dispatch(im->conn, 50 /*ms*/);
        }
    });
}

// Emit org.freedesktop.DBus.Properties.PropertiesChanged for the Player interface.
// changed_props is an a{sv} dict of changed properties.
static void emit_properties_changed(DBusConnection* conn,
                                     const PlayerState& ps,
                                     bool meta_changed,
                                     bool status_changed) {
    if (!conn) return;
    if (!meta_changed && !status_changed) return;

    DBusMessage* sig = dbus_message_new_signal(
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged");
    if (!sig) return;

    DBusMessageIter args;
    dbus_message_iter_init_append(sig, &args);

    // arg 0: interface name
    const char* iface = "org.mpris.MediaPlayer2.Player";
    append_string(&args, iface);

    // arg 1: changed properties a{sv}
    DBusMessageIter dict;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);

    if (status_changed) {
        const char* status = ps.playing ? (ps.paused ? "Paused" : "Playing") : "Stopped";
        DBusMessageIter e, v;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
        append_string(&e, "PlaybackStatus");
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &v);
        append_string(&v, status);
        dbus_message_iter_close_container(&e, &v);
        dbus_message_iter_close_container(&dict, &e);
    }

    if (meta_changed) {
        DBusMessageIter e, v;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
        append_string(&e, "Metadata");
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "a{sv}", &v);
        append_metadata(&v, ps);
        dbus_message_iter_close_container(&e, &v);
        dbus_message_iter_close_container(&dict, &e);
    }

    dbus_message_iter_close_container(&args, &dict);

    // arg 2: invalidated properties as (empty)
    DBusMessageIter inv;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &inv);
    dbus_message_iter_close_container(&args, &inv);

    dbus_connection_send(conn, sig, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(sig);
}

void MprisServer::update(const std::string& title,
                          const std::string& artist,
                          const std::string& art_url,
                          double position_sec,
                          double duration_sec,
                          bool playing,
                          bool paused) {
    auto* im = static_cast<MprisImpl*>(impl_);

    PlayerState next{ title, artist, art_url, position_sec, duration_sec, playing, paused };

    bool meta_changed, status_changed;
    DBusConnection* conn;
    {
        std::lock_guard<std::mutex> lk(im->state_mtx);
        meta_changed   = (next.title   != im->prev_state.title   ||
                          next.artist  != im->prev_state.artist  ||
                          next.art_url != im->prev_state.art_url ||
                          next.duration_sec != im->prev_state.duration_sec);
        status_changed = (next.playing != im->prev_state.playing ||
                          next.paused  != im->prev_state.paused);
        im->state      = next;
        if (meta_changed || status_changed) im->prev_state = next;
        conn = im->conn;
    }

    emit_properties_changed(conn, next, meta_changed, status_changed);
}

void MprisServer::stop() {
    auto* im = static_cast<MprisImpl*>(impl_);
    if (!im->running) return;
    im->running = false;
    if (im->thread.joinable()) im->thread.join();
    if (im->conn) {
        dbus_connection_unref(im->conn);
        im->conn = nullptr;
    }
}

} // namespace txs
