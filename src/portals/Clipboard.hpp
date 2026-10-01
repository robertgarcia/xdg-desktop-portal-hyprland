#pragma once

#include <memory>
#include <sdbus-c++/sdbus-c++.h>

#include "../dbusDefines.hpp"

// org.freedesktop.impl.portal.Clipboard backend. It owns no sessions: it lets
// RemoteDesktop sessions that asked for it before Start share the clipboard.
class CClipboardPortal {
  public:
    CClipboardPortal();

    void          onRequestClipboard(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts);
    void          onSetSelection(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts);
    sdbus::UnixFd onSelectionWrite(sdbus::ObjectPath sessionHandle, uint32_t serial);
    void          onSelectionWriteDone(sdbus::ObjectPath sessionHandle, uint32_t serial, bool success);
    sdbus::UnixFd onSelectionRead(sdbus::ObjectPath sessionHandle, std::string mimeType);

  private:
    // throws a portal error unless the session exists, was started and has the clipboard
    void                           requireClipboard(const sdbus::ObjectPath& sessionHandle, const char* method);

    std::unique_ptr<sdbus::IObject> m_pObject;

    const sdbus::InterfaceName      INTERFACE_NAME = sdbus::InterfaceName{"org.freedesktop.impl.portal.Clipboard"};
    const sdbus::ObjectPath         OBJECT_PATH    = sdbus::ObjectPath{"/org/freedesktop/portal/desktop"};
};
