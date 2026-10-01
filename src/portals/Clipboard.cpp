#include "Clipboard.hpp"
#include "../core/PortalManager.hpp"
#include "../helpers/Log.hpp"

static const sdbus::Error::Name PORTAL_ERROR_FAILED = sdbus::Error::Name{"org.freedesktop.portal.Error.Failed"};

CClipboardPortal::CClipboardPortal() {
    m_pObject = sdbus::createObject(*g_pPortalManager->getConnection(), OBJECT_PATH);

    m_pObject
        ->addVTable(sdbus::registerMethod("RequestClipboard")
                        .implementedAs([this](sdbus::ObjectPath o, std::unordered_map<std::string, sdbus::Variant> m) { onRequestClipboard(o, m); }),
                    sdbus::registerMethod("SetSelection")
                        .implementedAs([this](sdbus::ObjectPath o, std::unordered_map<std::string, sdbus::Variant> m) { onSetSelection(o, m); }),
                    sdbus::registerMethod("SelectionWrite").implementedAs([this](sdbus::ObjectPath o, uint32_t serial) { return onSelectionWrite(o, serial); }),
                    sdbus::registerMethod("SelectionWriteDone")
                        .implementedAs([this](sdbus::ObjectPath o, uint32_t serial, bool success) { onSelectionWriteDone(o, serial, success); }),
                    sdbus::registerMethod("SelectionRead").implementedAs([this](sdbus::ObjectPath o, std::string mime) { return onSelectionRead(o, mime); }),
                    sdbus::registerSignal("SelectionOwnerChanged").withParameters<sdbus::ObjectPath, std::unordered_map<std::string, sdbus::Variant>>(),
                    sdbus::registerSignal("SelectionTransfer").withParameters<sdbus::ObjectPath, std::string, uint32_t>(),
                    sdbus::registerProperty("version").withGetter([] { return sc<uint32_t>(1); }))
        .forInterface(INTERFACE_NAME);

    Debug::log(LOG, "[clipboard] registered");
}

void CClipboardPortal::requireClipboard(const sdbus::ObjectPath& sessionHandle, const char* method) {
    if (g_pPortalManager->m_sPortals.remoteDesktop && g_pPortalManager->m_sPortals.remoteDesktop->clipboardEnabled(sessionHandle))
        return;

    Debug::log(ERR, "[clipboard] {}: clipboard not enabled for session {}", method, std::string{sessionHandle});
    throw sdbus::Error{PORTAL_ERROR_FAILED, "Clipboard is not enabled for this session"};
}

void CClipboardPortal::onRequestClipboard(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts) {
    Debug::log(LOG, "[clipboard] RequestClipboard for session {}", std::string{sessionHandle});

    if (!g_pPortalManager->m_sPortals.remoteDesktop || !g_pPortalManager->m_sPortals.remoteDesktop->requestClipboard(sessionHandle))
        throw sdbus::Error{PORTAL_ERROR_FAILED, "Clipboard can only be requested for a RemoteDesktop session that has not started"};
}

void CClipboardPortal::onSetSelection(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts) {
    requireClipboard(sessionHandle, "SetSelection");

    std::vector<std::string> mimeTypes;
    if (opts.contains("mime_types"))
        mimeTypes = opts["mime_types"].get<std::vector<std::string>>();

    std::string joined;
    for (const auto& m : mimeTypes) {
        joined += joined.empty() ? "" : ", ";
        joined += m;
    }

    // TODO phase 4: take the clipboard with a data-control source offering these types
    Debug::log(LOG, "[clipboard] SetSelection from {}: [{}] (not implemented yet, ignored)", std::string{sessionHandle}, joined);
}

sdbus::UnixFd CClipboardPortal::onSelectionWrite(sdbus::ObjectPath sessionHandle, uint32_t serial) {
    requireClipboard(sessionHandle, "SelectionWrite");
    // TODO phase 4
    Debug::log(LOG, "[clipboard] SelectionWrite serial {} (not implemented yet)", serial);
    throw sdbus::Error{PORTAL_ERROR_FAILED, "SelectionWrite is not implemented yet"};
}

void CClipboardPortal::onSelectionWriteDone(sdbus::ObjectPath sessionHandle, uint32_t serial, bool success) {
    requireClipboard(sessionHandle, "SelectionWriteDone");
    // TODO phase 4
    Debug::log(LOG, "[clipboard] SelectionWriteDone serial {} success {} (not implemented yet)", serial, success);
}

sdbus::UnixFd CClipboardPortal::onSelectionRead(sdbus::ObjectPath sessionHandle, std::string mimeType) {
    requireClipboard(sessionHandle, "SelectionRead");
    // TODO phase 3: hand out a pipe fed by the current data-control offer
    Debug::log(LOG, "[clipboard] SelectionRead {} (not implemented yet)", mimeType);
    throw sdbus::Error{PORTAL_ERROR_FAILED, "SelectionRead is not implemented yet"};
}
