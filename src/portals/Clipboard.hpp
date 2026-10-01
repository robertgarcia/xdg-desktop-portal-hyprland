#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <sdbus-c++/sdbus-c++.h>

#include "../dbusDefines.hpp"

// org.freedesktop.impl.portal.Clipboard backend. It owns no sessions: it lets
// RemoteDesktop sessions that asked for it before Start share the clipboard.
class CClipboardPortal {
  public:
    CClipboardPortal();
    ~CClipboardPortal();

    void          onRequestClipboard(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts);
    void          onSetSelection(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts);
    sdbus::UnixFd onSelectionWrite(sdbus::ObjectPath sessionHandle, uint32_t serial);
    void          onSelectionWriteDone(sdbus::ObjectPath sessionHandle, uint32_t serial, bool success);
    sdbus::UnixFd onSelectionRead(sdbus::ObjectPath sessionHandle, std::string mimeType);

    // Send SelectionOwnerChanged with the current clipboard content to the given sessions
    void announceSelection(const std::vector<sdbus::ObjectPath>& sessions);

    // A RemoteDesktop session went away: drop what it owned
    void sessionClosed(const sdbus::ObjectPath& sessionHandle);

  private:
    // throws a portal error unless the session exists, was started and has the clipboard
    void requireClipboard(const sdbus::ObjectPath& sessionHandle, const char* method);

    // An app is pasting content a session owns: the app reads from fd until the
    // session writes it through SelectionWrite and closes it.
    struct STransfer {
        uint32_t          serial = 0;
        sdbus::ObjectPath session;
        int               fd = -1; // handed over on SelectionWrite
    };

    void                            startTransfer(const sdbus::ObjectPath& sessionHandle, const std::string& mimeType, int fd);
    void                            endTransfer(uint32_t serial);

    std::unique_ptr<sdbus::IObject> m_pObject;

    // session whose SetSelection put the current content on the clipboard
    std::optional<sdbus::ObjectPath> m_owner;
    bool                             m_ownSelectionIsCurrent = false;

    std::vector<STransfer>           m_transfers;
    uint32_t                         m_nextSerial = 1;

    const sdbus::InterfaceName       INTERFACE_NAME = sdbus::InterfaceName{"org.freedesktop.impl.portal.Clipboard"};
    const sdbus::ObjectPath          OBJECT_PATH    = sdbus::ObjectPath{"/org/freedesktop/portal/desktop"};
};
