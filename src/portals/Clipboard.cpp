#include "Clipboard.hpp"
#include "../core/PortalManager.hpp"
#include "../shared/FrontendGuard.hpp"
#include "../helpers/Log.hpp"

#include <algorithm>
#include <unistd.h>

constexpr int                   TRANSFER_TIMEOUT_MS = 10000;
static const sdbus::Error::Name PORTAL_ERROR_FAILED = sdbus::Error::Name{"org.freedesktop.portal.Error.Failed"};

CClipboardPortal::CClipboardPortal() {
    m_pObject = sdbus::createObject(*g_pPortalManager->getConnection(), OBJECT_PATH);

    m_pObject
        ->addVTable(
            sdbus::registerMethod("RequestClipboard").implementedAs([this](sdbus::ObjectPath o, std::unordered_map<std::string, sdbus::Variant> m) { onRequestClipboard(o, m); }),
            sdbus::registerMethod("SetSelection").implementedAs([this](sdbus::ObjectPath o, std::unordered_map<std::string, sdbus::Variant> m) { onSetSelection(o, m); }),
            sdbus::registerMethod("SelectionWrite").implementedAs([this](sdbus::ObjectPath o, uint32_t serial) { return onSelectionWrite(o, serial); }),
            sdbus::registerMethod("SelectionWriteDone").implementedAs([this](sdbus::ObjectPath o, uint32_t serial, bool success) { onSelectionWriteDone(o, serial, success); }),
            sdbus::registerMethod("SelectionRead").implementedAs([this](sdbus::ObjectPath o, std::string mime) { return onSelectionRead(o, mime); }),
            sdbus::registerSignal("SelectionOwnerChanged").withParameters<sdbus::ObjectPath, std::unordered_map<std::string, sdbus::Variant>>(),
            sdbus::registerSignal("SelectionTransfer").withParameters<sdbus::ObjectPath, std::string, uint32_t>(),
            sdbus::registerProperty("version").withGetter([] { return sc<uint32_t>(1); }))
        .forInterface(INTERFACE_NAME);

    // the clipboard changed: let every session with clipboard access know
    g_pPortalManager->m_sHelpers.dataControl->onSelectionChanged = [this](const std::vector<std::string>& mimeTypes, bool own) {
        m_ownSelectionIsCurrent = own;
        if (g_pPortalManager->m_sPortals.remoteDesktop)
            announceSelection(g_pPortalManager->m_sPortals.remoteDesktop->clipboardSessions());
    };

    g_pPortalManager->m_sHelpers.dataControl->onOwnSelectionLost = [this]() {
        m_owner.reset();
        m_ownSelectionIsCurrent = false;
    };

    Debug::log(LOG, "[clipboard] registered");
}

CClipboardPortal::~CClipboardPortal() {
    // the helper outlives us during shutdown, don't leave it calling into a dead portal
    if (g_pPortalManager->m_sHelpers.dataControl) {
        g_pPortalManager->m_sHelpers.dataControl->onSelectionChanged = nullptr;
        g_pPortalManager->m_sHelpers.dataControl->onOwnSelectionLost = nullptr;
        g_pPortalManager->m_sHelpers.dataControl->dropOwnSelection();
    }

    for (auto& t : m_transfers) {
        if (t.fd >= 0)
            close(t.fd);
    }
}

void CClipboardPortal::announceSelection(const std::vector<sdbus::ObjectPath>& sessions) {
    const auto& MIMETYPES = g_pPortalManager->m_sHelpers.dataControl->selectionMimeTypes();

    for (const auto& session : sessions) {
        // The owner must be told it owns the content, or it would read back what it
        // just set and send it to the other side again
        const bool                                      IS_OWNER = m_ownSelectionIsCurrent && m_owner == session;

        std::unordered_map<std::string, sdbus::Variant> options;
        options["mime_types"]       = sdbus::Variant{MIMETYPES};
        options["session_is_owner"] = sdbus::Variant{IS_OWNER};

        Debug::log(LOG, "[clipboard] SelectionOwnerChanged -> {} ({} mime types, owner: {})", std::string{session}, MIMETYPES.size(), IS_OWNER);
        m_pObject->emitSignal("SelectionOwnerChanged").onInterface(INTERFACE_NAME).withArguments(session, options);
    }
}

void CClipboardPortal::requireClipboard(const sdbus::ObjectPath& sessionHandle, const char* method) {
    if (g_pPortalManager->m_sPortals.remoteDesktop && g_pPortalManager->m_sPortals.remoteDesktop->clipboardEnabled(sessionHandle))
        return;

    Debug::log(ERR, "[clipboard] {}: clipboard not enabled for session {}", method, std::string{sessionHandle});
    throw sdbus::Error{PORTAL_ERROR_FAILED, "Clipboard is not enabled for this session"};
}

void CClipboardPortal::onRequestClipboard(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts) {
    requireFrontendCaller(*m_pObject, "RequestClipboard");

    Debug::log(LOG, "[clipboard] RequestClipboard for session {}", std::string{sessionHandle});

    if (!g_pPortalManager->m_sPortals.remoteDesktop || !g_pPortalManager->m_sPortals.remoteDesktop->requestClipboard(sessionHandle))
        throw sdbus::Error{PORTAL_ERROR_FAILED, "Clipboard can only be requested for a RemoteDesktop session that has not started"};
}

void CClipboardPortal::onSetSelection(sdbus::ObjectPath sessionHandle, std::unordered_map<std::string, sdbus::Variant> opts) {
    requireFrontendCaller(*m_pObject, "SetSelection");

    requireClipboard(sessionHandle, "SetSelection");

    std::vector<std::string> mimeTypes;
    if (opts.contains("mime_types"))
        mimeTypes = opts["mime_types"].get<std::vector<std::string>>();

    Debug::log(LOG, "[clipboard] SetSelection from {} with {} mime types", std::string{sessionHandle}, mimeTypes.size());

    if (mimeTypes.empty()) {
        if (m_owner == sessionHandle) {
            g_pPortalManager->m_sHelpers.dataControl->dropOwnSelection();
            m_owner.reset();
        }
        return;
    }

    if (!g_pPortalManager->m_sHelpers.dataControl->setSelection(mimeTypes, [this, sessionHandle](const std::string& mime, int fd) { startTransfer(sessionHandle, mime, fd); }))
        throw sdbus::Error{PORTAL_ERROR_FAILED, "The clipboard is not available"};

    m_owner = sessionHandle;
}

void CClipboardPortal::startTransfer(const sdbus::ObjectPath& sessionHandle, const std::string& mimeType, int fd) {
    const auto SERIAL = m_nextSerial++;
    m_transfers.emplace_back(STransfer{.serial = SERIAL, .session = sessionHandle, .fd = fd});

    Debug::log(LOG, "[clipboard] SelectionTransfer {} serial {} -> {}", mimeType, SERIAL, std::string{sessionHandle});
    m_pObject->emitSignal("SelectionTransfer").onInterface(INTERFACE_NAME).withArguments(sessionHandle, mimeType, SERIAL);

    // a pasting app blocks until its fd is closed, never leave it hanging
    g_pPortalManager->addTimer({TRANSFER_TIMEOUT_MS, [this, SERIAL]() {
                                    if (std::ranges::any_of(m_transfers, [SERIAL](const auto& t) { return t.serial == SERIAL; })) {
                                        Debug::log(WARN, "[clipboard] transfer serial {} timed out", SERIAL);
                                        endTransfer(SERIAL);
                                    }
                                }});
}

void CClipboardPortal::endTransfer(uint32_t serial) {
    const auto IT = std::ranges::find_if(m_transfers, [serial](const auto& t) { return t.serial == serial; });
    if (IT == m_transfers.end())
        return;

    if (IT->fd >= 0)
        close(IT->fd);
    m_transfers.erase(IT);
}

sdbus::UnixFd CClipboardPortal::onSelectionWrite(sdbus::ObjectPath sessionHandle, uint32_t serial) {
    requireFrontendCaller(*m_pObject, "SelectionWrite");

    requireClipboard(sessionHandle, "SelectionWrite");

    const auto IT = std::ranges::find_if(m_transfers, [&](const auto& t) { return t.serial == serial && t.session == sessionHandle; });
    if (IT == m_transfers.end() || IT->fd < 0) {
        Debug::log(ERR, "[clipboard] SelectionWrite: no pending transfer with serial {}", serial);
        throw sdbus::Error{PORTAL_ERROR_FAILED, "No pending transfer with this serial"};
    }

    // hand the fd over: the app only sees the end of the data once the writer closes it,
    // so we must not keep a copy
    const int FD = IT->fd;
    IT->fd       = -1;

    Debug::log(LOG, "[clipboard] SelectionWrite serial {}", serial);
    return sdbus::UnixFd{FD, sdbus::adopt_fd};
}

void CClipboardPortal::onSelectionWriteDone(sdbus::ObjectPath sessionHandle, uint32_t serial, bool success) {
    requireFrontendCaller(*m_pObject, "SelectionWriteDone");

    requireClipboard(sessionHandle, "SelectionWriteDone");

    Debug::log(success ? LOG : WARN, "[clipboard] SelectionWriteDone serial {} success {}", serial, success);
    endTransfer(serial);
}

void CClipboardPortal::sessionClosed(const sdbus::ObjectPath& sessionHandle) {
    std::vector<uint32_t> serials;
    for (const auto& t : m_transfers) {
        if (t.session == sessionHandle)
            serials.emplace_back(t.serial);
    }
    for (const auto SERIAL : serials)
        endTransfer(SERIAL);

    // nobody can provide the content anymore
    if (m_owner == sessionHandle) {
        g_pPortalManager->m_sHelpers.dataControl->dropOwnSelection();
        m_owner.reset();
        m_ownSelectionIsCurrent = false;
    }
}

sdbus::UnixFd CClipboardPortal::onSelectionRead(sdbus::ObjectPath sessionHandle, std::string mimeType) {
    requireFrontendCaller(*m_pObject, "SelectionRead");

    requireClipboard(sessionHandle, "SelectionRead");

    // Reading our own source would ask this very session for the data while it waits
    // on the read, stalling it until the transfer times out
    if (m_owner == sessionHandle && g_pPortalManager->m_sHelpers.dataControl->ownsSelection())
        throw sdbus::Error{PORTAL_ERROR_FAILED, "The session already owns the clipboard content"};

    const int FD = g_pPortalManager->m_sHelpers.dataControl->receive(mimeType);
    if (FD < 0)
        throw sdbus::Error{PORTAL_ERROR_FAILED, "The clipboard has no content of the requested type"};

    Debug::log(LOG, "[clipboard] SelectionRead {} for {}", mimeType, std::string{sessionHandle});
    return sdbus::UnixFd{FD, sdbus::adopt_fd};
}
