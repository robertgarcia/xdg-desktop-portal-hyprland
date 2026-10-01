#include "RemoteDesktop.hpp"
#include "../core/PortalManager.hpp"
#include "../shared/FrontendGuard.hpp"

#include <algorithm>
#include <hyprutils/os/Process.hpp>
#include <thread>
#include <chrono>
#include <climits>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/socket.h>
#include <libeis.h>
#include <linux/input.h>
#include <xkbcommon/xkbcommon.h>

// Helper: get current time in ms for Wayland events
static uint32_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Serialize a keymap into a memfd. The size includes the terminating NUL,
// which keymap consumers expect. Returns -1 on failure, the caller owns the fd.
static int keymapToFd(xkb_keymap* keymap, size_t& size) {
    if (!keymap)
        return -1;

    char* str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!str)
        return -1;

    size         = strlen(str) + 1;
    const int FD = memfd_create("xdph-keymap", MFD_CLOEXEC);
    bool      ok = FD >= 0;
    for (size_t written = 0; ok && written < size;) {
        const auto N = write(FD, str + written, size - written);
        ok           = N > 0;
        written += N > 0 ? N : 0;
    }
    free(str);

    if (!ok) {
        if (FD >= 0)
            close(FD);
        return -1;
    }

    return FD;
}

using namespace Hyprutils::OS;

static const sdbus::Error::Name PORTAL_ERROR_FAILED = sdbus::Error::Name{"org.freedesktop.portal.Error.Failed"};

// restore_data we hand out for "always allow": (vendor, version, granted bitmask).
// The bitmask is the device types plus RESTORE_CLIPBOARD_BIT.
static const std::string       RESTORE_VENDOR        = "hyprland";
constexpr uint32_t             RESTORE_VERSION       = 1;
constexpr uint32_t             RESTORE_CLIPBOARD_BIT = 1 << 16;

static std::optional<uint32_t> parseRestoreData(const sdbus::Variant& v) {
    try {
        const auto DATA = v.get<sdbus::Struct<std::string, uint32_t, sdbus::Variant>>();
        if (std::get<0>(DATA) != RESTORE_VENDOR || std::get<1>(DATA) != RESTORE_VERSION)
            return std::nullopt;
        return std::get<2>(DATA).get<uint32_t>();
    } catch (const std::exception& e) {
        Debug::log(WARN, "[remotedesktop] ignoring unreadable restore_data: {}", e.what());
        return std::nullopt;
    }
}

static const std::string DIALOG_DENY   = "Deny";
static const std::string DIALOG_ONCE   = "Allow once";
static const std::string DIALOG_ALWAYS = "Always allow";

// Ask the user with hyprland-dialog whether an app may control the input. Blocks
// until answered; anything but an explicit allow, including a closed dialog, denies.
static CRemoteDesktopPortal::eConsent askConsent(const std::string& appID, uint32_t devices, bool clipboard) {
    std::vector<std::string> what;
    if (devices & 1)
        what.emplace_back("keyboard");
    if (devices & 2)
        what.emplace_back("pointer");
    if (clipboard)
        what.emplace_back("clipboard");

    std::string list;
    for (size_t i = 0; i < what.size(); ++i)
        list += (i == 0 ? "" : i + 1 == what.size() ? " and " : ", ") + what[i];

    // the app id comes from the frontend, still keep markup characters out of the dialog
    std::string app = appID.empty() ? "An application" : appID;
    std::erase_if(app, [](char c) { return c == '<' || c == '>' || c == '&'; });

    CProcess    proc("hyprland-dialog",
                     {"--title", "Remote desktop", "--apptitle", "Allow remote control?", "--text", app + " wants to control your " + (list.empty() ? "input" : list) + ".",
                      "--buttons", DIALOG_DENY + ";" + DIALOG_ONCE + ";" + DIALOG_ALWAYS});

    const char* WAYLAND_DISPLAY             = getenv("WAYLAND_DISPLAY");
    const char* HYPRLAND_INSTANCE_SIGNATURE = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    proc.addEnv("WAYLAND_DISPLAY", WAYLAND_DISPLAY ? WAYLAND_DISPLAY : "");
    proc.addEnv("HYPRLAND_INSTANCE_SIGNATURE", HYPRLAND_INSTANCE_SIGNATURE ? HYPRLAND_INSTANCE_SIGNATURE : "0");

    if (!proc.runSync()) {
        Debug::log(ERR, "[remotedesktop] could not run hyprland-dialog, denying");
        return CRemoteDesktopPortal::CONSENT_DENY;
    }

    std::string answer = proc.stdOut();
    while (!answer.empty() && std::isspace(sc<unsigned char>(answer.back())))
        answer.pop_back();

    Debug::log(LOG, "[remotedesktop] consent dialog answered: \"{}\"", answer);
    if (answer == DIALOG_ALWAYS)
        return CRemoteDesktopPortal::CONSENT_ALWAYS;
    if (answer == DIALOG_ONCE)
        return CRemoteDesktopPortal::CONSENT_ONCE;
    return CRemoteDesktopPortal::CONSENT_DENY;
}

// ─── CRemoteDesktopPortal implementation ─────────────────────────

CRemoteDesktopPortal::CRemoteDesktopPortal(SP<CCZwlrVirtualPointerManagerV1> pointerMgr, SP<CCZwpVirtualKeyboardManagerV1> keyboardMgr) {
    m_sState.pointer  = pointerMgr;
    m_sState.keyboard = keyboardMgr;

    // Initialize xkbcommon for keysym → keycode conversion
    m_xkbCtx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (m_xkbCtx)
        m_xkbKeymap = xkb_keymap_new_from_names(m_xkbCtx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);

    m_pObject = sdbus::createObject(*g_pPortalManager->getConnection(), OBJECT_PATH);

    m_pObject
        ->addVTable(
            sdbus::registerMethod("CreateSession")
                .implementedAs(
                    [this](sdbus::ObjectPath o1, sdbus::ObjectPath o2, std::string s, std::unordered_map<std::string, sdbus::Variant> m) { return onCreateSession(o1, o2, s, m); }),
            sdbus::registerMethod("SelectDevices")
                .implementedAs(
                    [this](sdbus::ObjectPath o1, sdbus::ObjectPath o2, std::string s, std::unordered_map<std::string, sdbus::Variant> m) { return onSelectDevices(o1, o2, s, m); }),
            sdbus::registerMethod("Start").implementedAs([this](StartResult&& result, sdbus::ObjectPath o1, sdbus::ObjectPath o2, std::string s1, std::string s2,
                                                                std::unordered_map<std::string, sdbus::Variant> m) { onStart(std::move(result), o1, o2, s1, s2, m); }),
            sdbus::registerMethod("ConnectToEIS").implementedAs([this](sdbus::ObjectPath o, std::string s, std::unordered_map<std::string, sdbus::Variant> m) {
                return onConnectToEIS(o, s, m);
            }),
            sdbus::registerMethod("NotifyPointerMotion").implementedAs([this](sdbus::ObjectPath o, std::unordered_map<std::string, sdbus::Variant> m, double d1, double d2) {
                onNotifyPointerMotion(o, m, d1, d2);
            }),
            sdbus::registerMethod("NotifyPointerMotionAbsolute")
                .implementedAs([this](sdbus::ObjectPath o, std::unordered_map<std::string, sdbus::Variant> m, uint32_t u1, double d1, double d2) {
                    onNotifyPointerMotionAbsolute(o, m, u1, d1, d2);
                }),
            sdbus::registerMethod("NotifyPointerButton").implementedAs([this](sdbus::ObjectPath o, std::unordered_map<std::string, sdbus::Variant> m, int32_t i1, uint32_t u1) {
                onNotifyPointerButton(o, m, i1, u1);
            }),
            sdbus::registerMethod("NotifyPointerAxis").implementedAs([this](sdbus::ObjectPath o, std::unordered_map<std::string, sdbus::Variant> m, double d1, double d2) {
                onNotifyPointerAxis(o, m, d1, d2);
            }),
            sdbus::registerMethod("NotifyPointerAxisDiscrete")
                .implementedAs(
                    [this](sdbus::ObjectPath o, std::unordered_map<std::string, sdbus::Variant> m, uint32_t u1, int32_t i1) { onNotifyPointerAxisDiscrete(o, m, u1, i1); }),
            sdbus::registerMethod("NotifyKeyboardKeycode").implementedAs([this](sdbus::ObjectPath o, std::unordered_map<std::string, sdbus::Variant> m, int32_t i1, uint32_t u1) {
                onNotifyKeyboardKeycode(o, m, i1, u1);
            }),
            sdbus::registerMethod("NotifyKeyboardKeysym").implementedAs([this](sdbus::ObjectPath o, std::unordered_map<std::string, sdbus::Variant> m, int32_t i1, uint32_t u1) {
                onNotifyKeyboardKeysym(o, m, i1, u1);
            }),
            sdbus::registerProperty("AvailableDeviceTypes").withGetter([this]() { return availableDeviceTypes(); }),
            sdbus::registerProperty("version").withGetter([this]() { return version(); }))
        .forInterface(INTERFACE_NAME);

    Debug::log(LOG, "[remotedesktop] registered");
}

// ─── Session management ──────────────────────────────────────────

CRemoteDesktopPortal::SSession::~SSession() {
    if (eisFd >= 0)
        g_pPortalManager->removeExtraPollFd(eisFd);
    if (eisPointer)
        eis_device_unref(eisPointer);
    // eisFd is owned by the eis context (eis_get_fd), eis_unref closes it
    if (eis)
        eis_unref(eis);

    if (xkbState)
        xkb_state_unref(xkbState);

    // the generated destructors send destroy for the virtual devices
    virtualPointer.reset();
    virtualKeyboard.reset();
    if (g_pPortalManager->m_sWaylandConnection.display)
        wl_display_flush(g_pPortalManager->m_sWaylandConnection.display);
}

dbUasv CRemoteDesktopPortal::onCreateSession(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID,
                                             std::unordered_map<std::string, sdbus::Variant> opts) {
    requireFrontendCaller(*m_pObject, "CreateSession");

    Debug::log(LOG, "[remotedesktop] New session: appid={} req={} sess={}", appID, std::string{requestHandle}, std::string{sessionHandle});

    const auto PSESSION = m_vSessions.emplace_back(std::make_unique<SSession>(appID, requestHandle, sessionHandle)).get();

    PSESSION->session            = createDBusSession(sessionHandle);
    PSESSION->session->onDestroy = [PSESSION, this]() {
        // the SDBusSession is leaked on purpose like in the other portals, its object is already gone
        PSESSION->session.release();
        Debug::log(LOG, "[remotedesktop] Session {} closed", std::string{PSESSION->sessionHandle});
        if (PSESSION->clipboardEnabled && g_pPortalManager->m_sPortals.clipboard)
            g_pPortalManager->m_sPortals.clipboard->sessionClosed(PSESSION->sessionHandle);
        // runs from a core timer on the main loop, never from inside processEISEvents
        std::erase_if(m_vSessions, [PSESSION](const auto& s) { return s.get() == PSESSION; });
    };
    PSESSION->request            = createDBusRequest(requestHandle);
    PSESSION->request->onDestroy = [PSESSION]() { PSESSION->request.release(); };

    std::unordered_map<std::string, sdbus::Variant> results;
    std::unordered_map<std::string, sdbus::Variant> res;
    // session_handle must be serialized as a string, NOT an ObjectPath,
    // because the GLib-based frontend portal uses g_variant_dict_lookup(,"&s")
    // to extract it from the response variant dict.
    res["session_handle"] = sdbus::Variant{std::string{sessionHandle}};


    Debug::log(LOG, "[remotedesktop] CreateSession returning for appid={}", appID);
    return {0, res};
}

dbUasv CRemoteDesktopPortal::onSelectDevices(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID,
                                             std::unordered_map<std::string, sdbus::Variant> opts) {
    requireFrontendCaller(*m_pObject, "SelectDevices");

    const auto PSESSION = getSession(sessionHandle);

    if (!PSESSION) {
        Debug::log(ERR, "[remotedesktop] SelectDevices: no session");
        return {1, {}};
    }

    // The frontend still lets an app select devices while Start waits for the user
    if (PSESSION->started || PSESSION->starting) {
        Debug::log(ERR, "[remotedesktop] SelectDevices: session {} already started", std::string{sessionHandle});
        return {1, {}};
    }

    for (auto& [k, v] : opts) {
        if (k == "types") {
            PSESSION->deviceTypes = v.get<uint32_t>();
            Debug::log(LOG, "[remotedesktop] devices selected: {}", PSESSION->deviceTypes);
        } else if (k == "persist_mode")
            PSESSION->persistMode = v.get<uint32_t>();
        else if (k == "restore_data")
            PSESSION->restoredGrant = parseRestoreData(v);
    }

    if (PSESSION->deviceTypes == 0) {
        Debug::log(ERR, "[remotedesktop] no device types selected, defaulting to pointer+keyboard");
        PSESSION->deviceTypes = 3;
    }

    return {0, {}};
}

void CRemoteDesktopPortal::onStart(StartResult&& result, sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::string parentWindow,
                                   std::unordered_map<std::string, sdbus::Variant> opts) {
    requireFrontendCaller(*m_pObject, "Start");

    const auto PSESSION = getSession(sessionHandle);

    if (!PSESSION) {
        Debug::log(ERR, "[remotedesktop] Start: no session");
        result.returnResults(1, {});
        return;
    }

    if (PSESSION->started) {
        Debug::log(WARN, "[remotedesktop] session already started");
        result.returnResults(0, {});
        return;
    }

    // The frontend allows a second Start while the first one waits for the user
    if (PSESSION->starting) {
        Debug::log(ERR, "[remotedesktop] Start: session {} is already starting", std::string{sessionHandle});
        result.returnResults(2, {});
        return;
    }

    if (opts.contains("persist_mode"))
        PSESSION->persistMode = opts["persist_mode"].get<uint32_t>();

    // A previous "always allow" covers this session only if it asks for nothing more
    const uint32_t WANTED = PSESSION->deviceTypes | (PSESSION->clipboardRequested ? RESTORE_CLIPBOARD_BIT : 0);
    if (PSESSION->restoredGrant && (WANTED & ~*PSESSION->restoredGrant) == 0) {
        Debug::log(LOG, "[remotedesktop] Start: restored a previous grant, not asking");
        finishStart(result, PSESSION, true);
        return;
    }

    // The dialog can stay open for long: wait for it on a thread so the other
    // sessions keep working, then finish on the main loop
    PSESSION->starting = true;
    auto pending       = std::make_shared<StartResult>(std::move(result));
    std::thread([this, pending, sessionHandle, appID, devices = PSESSION->deviceTypes, clipboard = PSESSION->clipboardRequested]() {
        const auto CONSENT = askConsent(appID, devices, clipboard);
        g_pPortalManager->addTimerFromThread({0, [this, pending, sessionHandle, CONSENT]() {
                                                  const auto PSESSION = getSession(sessionHandle);
                                                  if (!PSESSION) {
                                                      Debug::log(LOG, "[remotedesktop] session {} closed while asking for consent", std::string{sessionHandle});
                                                      pending->returnResults(2, {});
                                                      return;
                                                  }

                                                  PSESSION->starting = false;

                                                  if (CONSENT == CONSENT_DENY) {
                                                      Debug::log(LOG, "[remotedesktop] Start: denied by the user");
                                                      pending->returnResults(1, {});
                                                      return;
                                                  }

                                                  finishStart(*pending, PSESSION, CONSENT == CONSENT_ALWAYS);
                                              }});
    }).detach();
}

void CRemoteDesktopPortal::finishStart(StartResult& result, SSession* PSESSION, bool persist) {
    wl_display* display = g_pPortalManager->m_sWaylandConnection.display;
    if (!display) {
        Debug::log(ERR, "[remotedesktop] no Wayland display");
        result.returnResults(2, {});
        return;
    }

    // Create virtual pointer
    if (PSESSION->deviceTypes & 2) {
        if (!m_sState.pointer) {
            Debug::log(ERR, "[remotedesktop] no virtual pointer manager");
        } else if (!g_pPortalManager->m_sWaylandConnection.seat) {
            Debug::log(ERR, "[remotedesktop] no Wayland seat");
        } else {
            wl_proxy* seatProxy = g_pPortalManager->m_sWaylandConnection.seat->proxy();
            wl_proxy* vpProxy   = m_sState.pointer->sendCreateVirtualPointer(seatProxy);
            if (vpProxy) {
                PSESSION->virtualPointer = makeShared<CCZwlrVirtualPointerV1>(vpProxy);
                wl_display_flush(display);
                Debug::log(LOG, "[remotedesktop] virtual pointer created");
            }
        }
    }

    // Create virtual keyboard
    if (PSESSION->deviceTypes & 1) {
        if (!m_sState.keyboard) {
            Debug::log(ERR, "[remotedesktop] no virtual keyboard manager");
        } else if (!g_pPortalManager->m_sWaylandConnection.seat) {
            Debug::log(ERR, "[remotedesktop] no Wayland seat");
        } else {
            wl_proxy* seatProxy = g_pPortalManager->m_sWaylandConnection.seat->proxy();
            wl_proxy* vkProxy   = m_sState.keyboard->sendCreateVirtualKeyboard(seatProxy);
            if (vkProxy) {
                PSESSION->virtualKeyboard = makeShared<CCZwpVirtualKeyboardV1>(vkProxy);

                // Send a keymap to the compositor. Required before any key events,
                // otherwise the compositor sends a protocol error:
                //   "Key event received before a keymap was set"
                size_t    kmSize = 0;
                const int KMFD   = keymapToFd(m_xkbKeymap, kmSize);
                if (KMFD >= 0) {
                    PSESSION->virtualKeyboard->sendKeymap(1 /* WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 */, KMFD, kmSize);
                    close(KMFD);
                } else
                    Debug::log(ERR, "[remotedesktop] could not build a keymap for the virtual keyboard");

                if (m_xkbKeymap)
                    PSESSION->xkbState = xkb_state_new(m_xkbKeymap);

                wl_display_flush(display);
                Debug::log(LOG, "[remotedesktop] virtual keyboard created with keymap");
            }
        }
    }

    PSESSION->started          = true;
    PSESSION->clipboardEnabled = PSESSION->clipboardRequested && g_pPortalManager->m_sHelpers.dataControl;
    Debug::log(LOG, "[remotedesktop] Start: clipboard {}", PSESSION->clipboardEnabled ? "enabled" : PSESSION->clipboardRequested ? "requested but unavailable" : "not requested");

    const auto&                                     sessionHandle = PSESSION->sessionHandle;
    std::unordered_map<std::string, sdbus::Variant> results;
    // Must be a string, not ObjectPath — frontend expects GVariant string type
    results["session_handle"]    = sdbus::Variant{std::string{sessionHandle}};
    results["devices"]           = sdbus::Variant{PSESSION->deviceTypes};
    results["clipboard_enabled"] = sdbus::Variant{PSESSION->clipboardEnabled};

    // The frontend keeps restore_data for the app and passes it back in SelectDevices
    if (persist && PSESSION->persistMode > 0) {
        const uint32_t GRANT    = PSESSION->deviceTypes | (PSESSION->clipboardEnabled ? RESTORE_CLIPBOARD_BIT : 0);
        results["persist_mode"] = sdbus::Variant{std::min(PSESSION->persistMode, sc<uint32_t>(2))};
        results["restore_data"] = sdbus::Variant{sdbus::Struct<std::string, uint32_t, sdbus::Variant>{RESTORE_VENDOR, RESTORE_VERSION, sdbus::Variant{GRANT}}};
    }

    // Tell the new session what is on the clipboard already. Deferred so the
    // signal goes out after this reply, once the frontend knows the session started.
    if (PSESSION->clipboardEnabled)
        g_pPortalManager->addTimer({0, [sessionHandle]() {
                                        if (g_pPortalManager->m_sPortals.clipboard)
                                            g_pPortalManager->m_sPortals.clipboard->announceSelection({sessionHandle});
                                    }});

    result.returnResults(0, results);
}

// ─── ConnectToEIS (libei path) ───────────────────────────────────

sdbus::UnixFd CRemoteDesktopPortal::onConnectToEIS(sdbus::ObjectPath sessionHandle, std::string appID,
                                                   std::unordered_map<std::string, sdbus::Variant> opts) {
    requireFrontendCaller(*m_pObject, "ConnectToEIS");

    const auto PSESSION = getSession(sessionHandle);

    if (!PSESSION) {
        Debug::log(ERR, "[remotedesktop] ConnectToEIS: no session for {}", std::string(sessionHandle));
        throw sdbus::Error{PORTAL_ERROR_FAILED, "No such session"};
    }

    if (!PSESSION->started) {
        Debug::log(ERR, "[remotedesktop] ConnectToEIS: session {} not started", std::string(sessionHandle));
        throw sdbus::Error{PORTAL_ERROR_FAILED, "ConnectToEIS is only allowed after Start"};
    }

    // One EIS connection per session: replacing it would orphan the previous context,
    // whose fd would stay in the poll set undrained and keep waking the loop
    if (PSESSION->eis) {
        Debug::log(ERR, "[remotedesktop] ConnectToEIS: session {} already connected", std::string(sessionHandle));
        throw sdbus::Error{PORTAL_ERROR_FAILED, "This session is already connected to EIS"};
    }

    // Create EIS context
    PSESSION->eis = eis_new(this);
    if (!PSESSION->eis) {
        Debug::log(ERR, "[remotedesktop] failed to create EIS context");
        throw sdbus::Error{PORTAL_ERROR_FAILED, "Could not set up the EIS connection"};
    }

    eis_set_user_data(PSESSION->eis, this);

    // Set up fd backend (events are dispatched via processEISEvents)
    if (eis_setup_backend_fd(PSESSION->eis) != 0) {
        Debug::log(ERR, "[remotedesktop] eis_setup_backend_fd failed");
        eis_unref(PSESSION->eis);
        PSESSION->eis = nullptr;
        throw sdbus::Error{PORTAL_ERROR_FAILED, "Could not set up the EIS connection"};
    }

    // Get client fd to pass to caller
    int clientFd = eis_backend_fd_add_client(PSESSION->eis);
    if (clientFd < 0) {
        Debug::log(ERR, "[remotedesktop] eis_backend_fd_add_client failed");
        eis_unref(PSESSION->eis);
        PSESSION->eis = nullptr;
        throw sdbus::Error{PORTAL_ERROR_FAILED, "Could not set up the EIS connection"};
    }

    // Get the EIS fd to poll for events
    PSESSION->eisFd = eis_get_fd(PSESSION->eis);

    Debug::log(LOG, "[remotedesktop] ConnectToEIS CALLED: client_fd={}, eis_fd={}", clientFd, PSESSION->eisFd);

    // Register the EIS fd with PortalManager's poll loop
    g_pPortalManager->addExtraPollFd(PSESSION->eisFd);

    // adopt: the plain constructor dups the fd and would keep the client end open here,
    // so libeis could never see the client go away
    return sdbus::UnixFd{clientFd, sdbus::adopt_fd};
}

// ─── Input notification handlers ─────────────────────────────────

void CRemoteDesktopPortal::onNotifyPointerMotion(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts, double dx,
                                                 double dy) {
    requireFrontendCaller(*m_pObject, "NotifyPointerMotion");

    const auto PSESSION = getSession(sessionHandle);
    if (!PSESSION || !PSESSION->virtualPointer)
        return;

    Debug::log(TRACE, "[remotedesktop] NotifyPointerMotion: dx={}, dy={}", dx, dy);
    PSESSION->virtualPointer->sendMotion(currentTimeMs(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
    PSESSION->virtualPointer->sendFrame();
    wl_display_flush(g_pPortalManager->m_sWaylandConnection.display);
}

void CRemoteDesktopPortal::onNotifyPointerMotionAbsolute(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts,
                                                         uint32_t stream, double x, double y) {
    requireFrontendCaller(*m_pObject, "NotifyPointerMotionAbsolute");

    // x/y are relative to a ScreenCast stream of the session. These sessions have no
    // streams, so there is nothing to map them onto (the frontend already refuses the
    // call for that reason). Absolute motion goes through EIS, see addLayoutRegions().
    Debug::log(WARN, "[remotedesktop] NotifyPointerMotionAbsolute: session {} has no stream {}, ignored", std::string{sessionHandle}, stream);
}

void CRemoteDesktopPortal::onNotifyPointerButton(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts, int32_t button,
                                                 uint32_t state) {
    requireFrontendCaller(*m_pObject, "NotifyPointerButton");

    const auto PSESSION = getSession(sessionHandle);
    if (!PSESSION || !PSESSION->virtualPointer)
        return;

    PSESSION->virtualPointer->sendButton(currentTimeMs(), button, state);
    PSESSION->virtualPointer->sendFrame();
    wl_display_flush(g_pPortalManager->m_sWaylandConnection.display);
}

void CRemoteDesktopPortal::onNotifyPointerAxis(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts, double dx,
                                               double dy) {
    requireFrontendCaller(*m_pObject, "NotifyPointerAxis");

    const auto PSESSION = getSession(sessionHandle);
    if (!PSESSION || !PSESSION->virtualPointer)
        return;

    uint32_t time = currentTimeMs();
    if (dy != 0.0) {
        PSESSION->virtualPointer->sendAxisSource(2);
        PSESSION->virtualPointer->sendAxis(time, 0, wl_fixed_from_double(-dy));
    }
    if (dx != 0.0) {
        PSESSION->virtualPointer->sendAxisSource(2);
        PSESSION->virtualPointer->sendAxis(time, 1, wl_fixed_from_double(dx));
    }
    PSESSION->virtualPointer->sendFrame();
    wl_display_flush(g_pPortalManager->m_sWaylandConnection.display);
}

void CRemoteDesktopPortal::onNotifyPointerAxisDiscrete(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts,
                                                       uint32_t axis, int32_t steps) {
    requireFrontendCaller(*m_pObject, "NotifyPointerAxisDiscrete");

    const auto PSESSION = getSession(sessionHandle);
    if (!PSESSION || !PSESSION->virtualPointer)
        return;

    uint32_t time = currentTimeMs();
    PSESSION->virtualPointer->sendAxisSource(2);
    PSESSION->virtualPointer->sendAxisDiscrete(time, axis, wl_fixed_from_int(steps * 15), steps);
    PSESSION->virtualPointer->sendFrame();
    wl_display_flush(g_pPortalManager->m_sWaylandConnection.display);
}

void CRemoteDesktopPortal::onNotifyKeyboardKeycode(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts,
                                                   int32_t keycode, uint32_t state) {
    requireFrontendCaller(*m_pObject, "NotifyKeyboardKeycode");

    const auto PSESSION = getSession(sessionHandle);
    if (!PSESSION || !PSESSION->virtualKeyboard)
        return;

    sendKey(PSESSION, keycode, state == 1, currentTimeMs());
    wl_display_flush(g_pPortalManager->m_sWaylandConnection.display);
}

void CRemoteDesktopPortal::onNotifyKeyboardKeysym(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts,
                                                   int32_t keysym, uint32_t state) {
    requireFrontendCaller(*m_pObject, "NotifyKeyboardKeysym");

    const auto PSESSION = getSession(sessionHandle);
    if (!PSESSION || !PSESSION->virtualKeyboard)
        return;

    // Find the base (unshifted) keycode for this keysym.
    // Shift state is managed entirely by the KDE Connect handlePacket code
    // (which sends separate keyboardKeycode(KEY_LEFTSHIFT, ...) calls).
    // The backend must NOT also send shift, or the double-shift causes
    // the modifier to be released prematurely.
    uint32_t keycode = keycodeFromKeysym(keysym, false /* searchLevel0Only */);
    if (!keycode) {
        Debug::log(WARN, "[remotedesktop] keysym 0x{:x} not found in keymap", keysym);
        return;
    }

    sendKey(PSESSION, keycode, state == 1, currentTimeMs());
    wl_display_flush(g_pPortalManager->m_sWaylandConnection.display);
}

// ─── EIS event processing ───────────────────────────────────────

void CRemoteDesktopPortal::processEISEvents() {
    
    for (auto& s : m_vSessions) {
        if (!s->eis)
            continue;

        // Dispatch to process incoming data
        eis_dispatch(s->eis);

        // Process events (pull model)
        struct eis_event* event;
        int evCount = 0;
        while ((event = eis_get_event(s->eis))) {
            auto eventType = eis_event_get_type(event);
            
            evCount++;
            auto*    client    = eis_event_get_client(event);
            auto*    seat      = eis_event_get_seat(event);
            uint32_t time      = currentTimeMs();

            switch (eventType) {
            case EIS_EVENT_CLIENT_CONNECT: {
                Debug::log(LOG, "[remotedesktop] EIS client connect");
                eis_client_connect(client);

                // Create a seat with all capabilities we support
                auto* newSeat = eis_client_new_seat(client, "kdeconnect-virtual-input");
                if (!newSeat) {
                    Debug::log(ERR, "[remotedesktop] failed to create EIS seat");
                    break;
                }
                eis_seat_configure_capability(newSeat, EIS_DEVICE_CAP_POINTER);
                eis_seat_configure_capability(newSeat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
                eis_seat_configure_capability(newSeat, EIS_DEVICE_CAP_KEYBOARD);
                eis_seat_configure_capability(newSeat, EIS_DEVICE_CAP_SCROLL);
                eis_seat_configure_capability(newSeat, EIS_DEVICE_CAP_BUTTON);
                eis_seat_add(newSeat);
                // the client keeps its own reference to the seat
                eis_seat_unref(newSeat);
                Debug::log(LOG, "[remotedesktop] EIS seat added with all capabilities");
                break;
            }
            case EIS_EVENT_CLIENT_DISCONNECT: {
                Debug::log(LOG, "[remotedesktop] EIS client disconnect");
                break;
            }
            case EIS_EVENT_SEAT_BIND: {
                Debug::log(LOG, "[remotedesktop] EIS seat bind");
                // Create and announce a pointer device if requested
                if (eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER) || eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER_ABSOLUTE))
                    addPointerDevice(seat, s.get());
                if (eis_event_seat_has_capability(event, EIS_DEVICE_CAP_KEYBOARD)) {
                    auto* dev = eis_seat_new_device(seat);
                    if (dev) {
                        eis_device_configure_type(dev, EIS_DEVICE_TYPE_VIRTUAL);
                        eis_device_configure_name(dev, "Hyprland virtual keyboard");
                        eis_device_configure_capability(dev, EIS_DEVICE_CAP_KEYBOARD);
                        // Provide an XKB keymap so the EIS client can process keyboard events.
                        // Without this, ei_device_keyboard_get_keymap() returns NULL on the client,
                        // causing a crash.
                        // Same keymap as the virtual keyboard, so the keycodes the client
                        // picks are the ones the compositor interprets.
                        size_t    kmSize = 0;
                        const int KMFD   = keymapToFd(m_xkbKeymap, kmSize);
                        if (KMFD >= 0) {
                            if (auto* eisKm = eis_device_new_keymap(dev, EIS_KEYMAP_TYPE_XKB, KMFD, kmSize)) {
                                eis_keymap_add(eisKm);
                                eis_keymap_unref(eisKm);
                            }
                            close(KMFD);
                        } else
                            Debug::log(ERR, "[remotedesktop] could not build a keymap for the EIS keyboard");
                        eis_device_add(dev);
                        eis_device_resume(dev);
                        // the seat keeps its own reference; ours would keep the device (and its keymap fd) alive forever
                        eis_device_unref(dev);
                        Debug::log(LOG, "[remotedesktop] EIS keyboard device added & resumed");
                    }
                }
                break;
            }
            case EIS_EVENT_DEVICE_CLOSED: {
                // the client is done with it, don't recreate it on the next layout change
                if (eis_event_get_device(event) == s->eisPointer) {
                    eis_device_unref(s->eisPointer);
                    s->eisPointer = nullptr;
                }
                break;
            }
            case EIS_EVENT_POINTER_MOTION: {
                if (s->virtualPointer) {
                    double dx = eis_event_pointer_get_dx(event);
                    double dy = eis_event_pointer_get_dy(event);
                    
                    s->virtualPointer->sendMotion(time,
                                                  wl_fixed_from_double(dx),
                                                  wl_fixed_from_double(dy));
                }
                break;
            }
            case EIS_EVENT_POINTER_MOTION_ABSOLUTE: {
                if (s->virtualPointer) {
                    if (s->eisExtentW == 0 || s->eisExtentH == 0)
                        break;

                    // already relative to the layout box, see addLayoutRegions().
                    // The compositor maps virtual pointer absolute motion onto that same box.
                    const double x = std::clamp(eis_event_pointer_get_absolute_x(event), 0.0, sc<double>(s->eisExtentW - 1));
                    const double y = std::clamp(eis_event_pointer_get_absolute_y(event), 0.0, sc<double>(s->eisExtentH - 1));
                    s->virtualPointer->sendMotionAbsolute(time, sc<uint32_t>(x), sc<uint32_t>(y), s->eisExtentW, s->eisExtentH);
                }
                break;
            }
            case EIS_EVENT_BUTTON_BUTTON: {
                if (s->virtualPointer) {
                    uint32_t button = eis_event_button_get_button(event);
                    uint32_t state  = eis_event_button_get_is_press(event) ? 1 : 0;
                    s->virtualPointer->sendButton(time, button, state);
                }
                break;
            }
            case EIS_EVENT_SCROLL_DELTA: {
                if (s->virtualPointer) {
                    double dx = eis_event_scroll_get_dx(event);
                    double dy = eis_event_scroll_get_dy(event);
                    if (dy != 0.0) {
                        s->virtualPointer->sendAxisSource(2);
                        s->virtualPointer->sendAxis(time, 0, wl_fixed_from_double(-dy));
                    }
                    if (dx != 0.0) {
                        s->virtualPointer->sendAxisSource(2);
                        s->virtualPointer->sendAxis(time, 1, wl_fixed_from_double(dx));
                    }
                }
                break;
            }
            case EIS_EVENT_SCROLL_DISCRETE: {
                if (s->virtualPointer) {
                    // libei discrete scroll is in 1/120 of a wheel click, not in clicks.
                    // Forwarding it as clicks scrolled 120 times too far.
                    const int32_t V120[2] = {eis_event_scroll_get_discrete_dy(event), eis_event_scroll_get_discrete_dx(event)};
                    for (uint32_t axis = 0; axis < 2; ++axis) {
                        if (V120[axis] == 0)
                            continue;
                        s->virtualPointer->sendAxisSource(0 /* wheel */);
                        s->virtualPointer->sendAxisDiscrete(time, axis, wl_fixed_from_double(15.0 * V120[axis] / 120.0), V120[axis] / 120);
                    }
                }
                break;
            }
            case EIS_EVENT_KEYBOARD_KEY: {
                if (s->virtualKeyboard) {
                    sendKey(s.get(), eis_event_keyboard_get_key(event), eis_event_keyboard_get_key_is_press(event), time);
                }
                break;
            }
            case EIS_EVENT_FRAME: {
                
                // Commit all pending events with a frame
                if (s->virtualPointer)
                    s->virtualPointer->sendFrame();
                if (s->virtualPointer || s->virtualKeyboard)
                    wl_display_flush(g_pPortalManager->m_sWaylandConnection.display);
                break;
            }
            default:
                break;
            }

            eis_event_unref(event);
        }
    }
}

void CRemoteDesktopPortal::addPointerDevice(eis_seat* seat, SSession* session) {
    auto* dev = eis_seat_new_device(seat);
    if (!dev)
        return;

    eis_device_configure_type(dev, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_name(dev, "Hyprland virtual pointer");
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_POINTER);
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_BUTTON);
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_SCROLL);

    // A virtual device advertising POINTER_ABSOLUTE must have at least
    // one region, otherwise libei discards every absolute motion event.
    // Announce one region per output so clients see the real layout.
    addLayoutRegions(dev, session);

    eis_device_add(dev);
    // Sender clients (ei_new_sender, e.g. KDE Connect remote input) own
    // the start-emulating handshake: eis_device_add() leaves the device
    // paused, and the client only begins emulating after receiving
    // EI_EVENT_DEVICE_RESUMED (emitted by eis_device_resume()).
    // eis_device_start_emulating() is receiver-side API and must not be
    // called for a sender client.
    eis_device_resume(dev);

    // keep our reference: the device is replaced when the output layout changes
    if (session->eisPointer)
        eis_device_unref(session->eisPointer);
    session->eisPointer = dev;
    Debug::log(LOG, "[remotedesktop] EIS pointer device added & resumed");
}

static bool sameLayout(const std::vector<SLogicalOutputBox>& a, const std::vector<SLogicalOutputBox>& b) {
    return std::ranges::equal(a, b, [](const auto& l, const auto& r) { return l.x == r.x && l.y == r.y && l.w == r.w && l.h == r.h && l.scale == r.scale; });
}

void CRemoteDesktopPortal::outputLayoutChanged() {
    const auto BOXES = g_pPortalManager->getLogicalOutputBoxes();
    // mid-hotplug the new output may not have its geometry yet, a later change follows
    if (BOXES.empty())
        return;

    for (auto& s : m_vSessions) {
        if (!s->eisPointer || sameLayout(s->eisLayout, BOXES))
            continue;

        // EIS regions are fixed once a device is added: replace the device. Clients
        // see it removed and a new one added with the current outputs.
        Debug::log(LOG, "[remotedesktop] output layout changed, replacing the EIS pointer of {}", std::string{s->sessionHandle});
        auto* old  = s->eisPointer;
        auto* seat = eis_device_get_seat(old);
        eis_device_remove(old);
        addPointerDevice(seat, s.get());
        if (s->eisPointer == old) {
            // the new device could not be created
            eis_device_unref(old);
            s->eisPointer = nullptr;
        }
    }
}

void CRemoteDesktopPortal::addLayoutRegions(eis_device* dev, SSession* session) {
    auto boxes = g_pPortalManager->getLogicalOutputBoxes();
    session->eisLayout = boxes;
    if (boxes.empty()) {
        Debug::log(WARN, "[remotedesktop] no output geometry known yet, announcing a 1920x1080 region");
        boxes.emplace_back(SLogicalOutputBox{.w = 1920, .h = 1080});
    }

    int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
    for (const auto& b : boxes) {
        minX = std::min(minX, b.x);
        minY = std::min(minY, b.y);
        maxX = std::max(maxX, b.x + sc<int32_t>(b.w));
        maxY = std::max(maxY, b.y + sc<int32_t>(b.h));
    }

    session->eisExtentW = sc<uint32_t>(maxX - minX);
    session->eisExtentH = sc<uint32_t>(maxY - minY);

    for (const auto& b : boxes) {
        auto* region = eis_device_new_region(dev);
        if (!region)
            continue;
        eis_region_set_offset(region, sc<uint32_t>(b.x - minX), sc<uint32_t>(b.y - minY));
        eis_region_set_size(region, b.w, b.h);
        eis_region_set_physical_scale(region, b.scale);
        eis_region_add(region);
        eis_region_unref(region);
        Debug::log(LOG, "[remotedesktop] EIS region {}x{} at {},{} (layout {},{}), scale {}", b.w, b.h, b.x - minX, b.y - minY, b.x, b.y, b.scale);
    }

    Debug::log(LOG, "[remotedesktop] EIS layout box {}x{}, origin {},{}", session->eisExtentW, session->eisExtentH, minX, minY);
}

void CRemoteDesktopPortal::sendKey(SSession* session, uint32_t evdevKey, bool pressed, uint32_t time) {
    if (!session->virtualKeyboard)
        return;

    session->virtualKeyboard->sendKey(time, evdevKey, pressed ? 1 : 0);

    // The virtual keyboard protocol does not derive modifiers from key events,
    // the client has to send them. Track them with a real xkb state so Shift,
    // AltGr, CapsLock and friends work no matter which path the key came from.
    if (!session->xkbState)
        return;

    const auto CHANGED = xkb_state_update_key(session->xkbState, evdevKey + 8 /* evdev -> xkb */, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    if (!(CHANGED & (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED | XKB_STATE_MODS_LOCKED | XKB_STATE_LAYOUT_EFFECTIVE)))
        return;

    session->virtualKeyboard->sendModifiers(xkb_state_serialize_mods(session->xkbState, XKB_STATE_MODS_DEPRESSED),
                                            xkb_state_serialize_mods(session->xkbState, XKB_STATE_MODS_LATCHED), xkb_state_serialize_mods(session->xkbState, XKB_STATE_MODS_LOCKED),
                                            xkb_state_serialize_layout(session->xkbState, XKB_STATE_LAYOUT_EFFECTIVE));
}

// ─── Keysym → keycode conversion ─────────────────────────────────

uint32_t CRemoteDesktopPortal::keycodeFromKeysym(uint32_t sym, bool level0Only) {
    if (!m_xkbKeymap)
        return 0;

    xkb_keycode_t min = xkb_keymap_min_keycode(m_xkbKeymap);
    xkb_keycode_t max = xkb_keymap_max_keycode(m_xkbKeymap);

    

    int maxLevel = level0Only ? 0 : 3;
    for (xkb_keycode_t code = min; code <= max; code++) {
        for (int level = 0; level <= maxLevel; level++) {
            const xkb_keysym_t* syms;
            int nsyms = xkb_keymap_key_get_syms_by_level(m_xkbKeymap, code, 0, level, &syms);
            for (int i = 0; i < nsyms; i++) {
                if (syms[i] == static_cast<xkb_keysym_t>(sym)) {
                    // XKB keycodes are evdev scancodes + 8 in the standard evdev ruleset.
                    // code - min + 1 is wrong when min != 9; use the fixed 8 offset.
                    uint32_t evdevCode = code - 8;
                    return evdevCode;
                }
            }
        }
    }
    return 0;
}

// ─── Clipboard integration ───────────────────────────────────────

bool CRemoteDesktopPortal::requestClipboard(const sdbus::ObjectPath& sessionHandle) {
    const auto PSESSION = getSession(sessionHandle);
    if (!PSESSION) {
        Debug::log(ERR, "[remotedesktop] RequestClipboard: no session {}", std::string{sessionHandle});
        return false;
    }

    // the frontend still allows it while Start waits for the user, who was not asked about it
    if (PSESSION->started || PSESSION->starting) {
        Debug::log(ERR, "[remotedesktop] RequestClipboard: session {} already started", std::string{sessionHandle});
        return false;
    }

    PSESSION->clipboardRequested = true;
    return true;
}

bool CRemoteDesktopPortal::clipboardEnabled(const sdbus::ObjectPath& sessionHandle) {
    const auto PSESSION = getSession(sessionHandle);
    return PSESSION && PSESSION->clipboardEnabled;
}

std::vector<sdbus::ObjectPath> CRemoteDesktopPortal::clipboardSessions() {
    std::vector<sdbus::ObjectPath> sessions;
    for (const auto& s : m_vSessions) {
        if (s->clipboardEnabled)
            sessions.emplace_back(s->sessionHandle);
    }
    return sessions;
}

// ─── Properties ──────────────────────────────────────────────────

uint32_t CRemoteDesktopPortal::availableDeviceTypes() {
    uint32_t types = 0;
    if (m_sState.keyboard)
        types |= 1;
    if (m_sState.pointer)
        types |= 2;
    return types;
}

uint32_t CRemoteDesktopPortal::version() {
    return 5;
}

// ─── Session lookup ──────────────────────────────────────────────

CRemoteDesktopPortal::SSession* CRemoteDesktopPortal::getSession(const sdbus::ObjectPath& path) {
    for (auto& s : m_vSessions) {
        if (s->sessionHandle == path)
            return s.get();
    }
    return nullptr;
}

