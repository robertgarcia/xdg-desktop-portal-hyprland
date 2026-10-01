#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <sdbus-c++/sdbus-c++.h>
#include <xkbcommon/xkbcommon.h>

#include "../includes.hpp"
#include "../dbusDefines.hpp"
#include "../shared/Session.hpp"
#include "../helpers/Log.hpp"

#include "wlr-virtual-pointer-unstable-v1.hpp"
#include "virtual-keyboard-unstable-v1.hpp"

struct eis;
struct eis_client;
struct eis_seat;
struct eis_device;

class CRemoteDesktopPortal {
  public:
    CRemoteDesktopPortal(SP<CCZwlrVirtualPointerManagerV1> pointerMgr, SP<CCZwpVirtualKeyboardManagerV1> keyboardMgr);
    ~CRemoteDesktopPortal() = default;

    // D-Bus session management
    dbUasv onCreateSession(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID,
                           std::unordered_map<std::string, sdbus::Variant> opts);
    dbUasv onSelectDevices(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID,
                           std::unordered_map<std::string, sdbus::Variant> opts);
    using StartResult = sdbus::Result<uint32_t, std::unordered_map<std::string, sdbus::Variant>>;
    // asynchronous: the reply is sent once the user answered the consent dialog
    void onStart(StartResult&& result, sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::string parentWindow,
                 std::unordered_map<std::string, sdbus::Variant> opts);

    enum eConsent : uint8_t {
        CONSENT_DENY,
        CONSENT_ONCE,
        CONSENT_ALWAYS,
    };

    // ConnectToEIS (libei-based input path - used by KDE Connect, etc.)
    sdbus::UnixFd onConnectToEIS(sdbus::ObjectPath sessionHandle, std::string appID,
                                 std::unordered_map<std::string, sdbus::Variant> opts);

    // Input notification handlers (fire-and-forget, void returns)
    void onNotifyPointerMotion(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts, double dx, double dy);
    void onNotifyPointerMotionAbsolute(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts, uint32_t stream, double x, double y);
    void onNotifyPointerButton(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts, int32_t button, uint32_t state);
    void onNotifyPointerAxis(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts, double dx, double dy);
    void onNotifyPointerAxisDiscrete(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts, uint32_t axis, int32_t steps);
    void onNotifyKeyboardKeycode(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts, int32_t keycode, uint32_t state);
    void onNotifyKeyboardKeysym(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts, int32_t keysym, uint32_t state);

    // Clipboard integration, see CClipboardPortal. Clipboard access has to be
    // requested before the session starts and is granted by Start.
    bool                           requestClipboard(const sdbus::ObjectPath& sessionHandle);
    bool                           clipboardEnabled(const sdbus::ObjectPath& sessionHandle);
    std::vector<sdbus::ObjectPath> clipboardSessions();

    // D-Bus properties
    uint32_t availableDeviceTypes();
    uint32_t version();

    // EIS event processing (called from the main event loop)
    void processEISEvents();

  private:
    struct SSession {
        SSession(const std::string& app, const sdbus::ObjectPath& req, const sdbus::ObjectPath& sess)
            : appid(app)
            , requestHandle(req)
            , sessionHandle(sess) {}
        ~SSession();

        std::string             appid;
        sdbus::ObjectPath       requestHandle;
        sdbus::ObjectPath       sessionHandle;

        std::unique_ptr<SDBusSession> session;
        std::unique_ptr<SDBusRequest> request;

        CWeakPointer<SSession>  self;

        uint32_t                deviceTypes    = 0; // bitmask: 1=keyboard, 2=pointer, 4=touchscreen
        bool                    started        = false;
        // Start is waiting for the consent dialog: what the session asks for is
        // frozen, so the user grants exactly what the dialog showed
        bool                    starting       = false;
        bool                          clipboardRequested = false;
        bool                          clipboardEnabled   = false;

        // persistence (RemoteDesktop v2): what the app asked for, and what a previous
        // "always allow" granted, from the restore_data the frontend passed back
        uint32_t                persistMode = 0;
        std::optional<uint32_t> restoredGrant;

        // Wayland objects (created on Start)
        SP<CCZwlrVirtualPointerV1>      virtualPointer;
        SP<CCZwpVirtualKeyboardV1>      virtualKeyboard;

        // Modifier tracking, built from the same keymap the virtual keyboard uses
        struct xkb_state* xkbState = nullptr;

        // EIS/libei state (created by ConnectToEIS)
        struct eis*                     eis          = nullptr;
        int                             eisFd        = -1; // fd to poll for EIS events
        bool                            eisReady     = false;

        // Size of the layout bounding box the EIS regions were announced in. EIS
        // offsets are unsigned, so regions are shifted to start at 0,0 and absolute
        // events arrive already relative to the box.
        uint32_t eisExtentW = 0;
        uint32_t eisExtentH = 0;
    };

    SSession* getSession(const sdbus::ObjectPath& path);

    // Create the session's devices and answer Start. persist: hand out restore_data
    void finishStart(StartResult& result, SSession* session, bool persist);

    // Send a key and keep the compositor's modifier state in sync with it
    void sendKey(SSession* session, uint32_t evdevKey, bool pressed, uint32_t time);

    // Announce one EIS region per output, shifted so the layout box starts at 0,0
    void addLayoutRegions(eis_device* dev, SSession* session);

    // Keysym → keycode conversion (via xkbcommon)
    uint32_t keycodeFromKeysym(uint32_t sym, bool level0Only = false);

    std::unique_ptr<sdbus::IObject>           m_pObject;
    std::vector<std::unique_ptr<SSession>>     m_vSessions;

    struct {
        SP<CCZwlrVirtualPointerManagerV1> pointer;
        SP<CCZwpVirtualKeyboardManagerV1> keyboard;
    } m_sState;

    // XKB state for keysym → keycode conversion
    struct xkb_context* m_xkbCtx   = nullptr;
    struct xkb_keymap*  m_xkbKeymap = nullptr;

    const sdbus::InterfaceName INTERFACE_NAME = sdbus::InterfaceName{"org.freedesktop.impl.portal.RemoteDesktop"};
    const sdbus::ObjectPath    OBJECT_PATH    = sdbus::ObjectPath{"/org/freedesktop/portal/desktop"};
};
