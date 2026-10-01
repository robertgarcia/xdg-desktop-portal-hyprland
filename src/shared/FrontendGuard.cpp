#include "FrontendGuard.hpp"
#include "../core/PortalManager.hpp"
#include "../helpers/Log.hpp"

static std::string frontendUniqueName() {
    // not static: it must not outlive the connection at exit
    const auto  DBUS = sdbus::createProxy(*g_pPortalManager->getConnection(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});

    std::string       owner;
    try {
        DBUS->callMethod("GetNameOwner").onInterface("org.freedesktop.DBus").withArguments(std::string{"org.freedesktop.portal.Desktop"}).storeResultsTo(owner);
    } catch (const sdbus::Error& e) {
        Debug::log(ERR, "[security] could not resolve the portal frontend: {}", e.getMessage());
    }
    return owner;
}

void requireFrontendCaller(const sdbus::IObject& object, const char* method) {
    // the frontend gets a new unique name whenever it restarts, so refresh on mismatch
    static std::string frontend;

    const char*        SENDER = object.getCurrentlyProcessedMessage().getSender();
    if (SENDER && !frontend.empty() && frontend == SENDER)
        return;

    frontend = frontendUniqueName();
    if (SENDER && !frontend.empty() && frontend == SENDER)
        return;

    Debug::log(ERR, "[security] {} called directly by {}, not by the portal frontend; refused", method, SENDER ? SENDER : "(unknown)");
    throw sdbus::Error{sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}, "Only xdg-desktop-portal may call this backend"};
}
