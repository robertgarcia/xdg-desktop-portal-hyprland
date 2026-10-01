#include "DataControl.hpp"
#include "../helpers/Log.hpp"
#include "../core/PortalManager.hpp"

#include <algorithm>
#include <fcntl.h>
#include <unistd.h>

static std::string joinMimeTypes(const std::vector<std::string>& mimeTypes) {
    std::string joined;
    for (const auto& m : mimeTypes) {
        if (!joined.empty())
            joined += ", ";
        joined += m;
    }
    return joined;
}

CDataControl::CDataControl(SP<CCExtDataControlManagerV1> manager, SP<CCWlSeat> seat) : m_manager(manager) {
    m_device = makeShared<CCExtDataControlDeviceV1>(m_manager->sendGetDataDevice(seat->proxy()));

    // data_offer introduces a new offer and is followed by its offer (mime type) events,
    // then by selection or primary_selection naming which offer it was.
    m_device->setDataOffer([this](CCExtDataControlDeviceV1* r, wl_proxy* proxy) {
        auto offer   = makeShared<SOffer>();
        offer->offer = makeShared<CCExtDataControlOfferV1>(proxy);
        offer->offer->setOffer([weak = WP<SOffer>{offer}](CCExtDataControlOfferV1* r, const char* mime) {
            const auto OFFER = weak.lock();
            // sources may offer the same type more than once (wl-copy does)
            if (!OFFER || std::ranges::contains(OFFER->mimeTypes, std::string_view{mime}))
                return;
            OFFER->mimeTypes.emplace_back(mime);
        });
        m_pendingOffers.emplace_back(offer);
    });

    m_device->setSelection([this](CCExtDataControlDeviceV1* r, wl_proxy* proxy) {
        // the previous selection offer is no longer valid, dropping it destroys it
        m_selection = proxy ? offerFor(proxy) : nullptr;

        Debug::log(LOG, "[datacontrol] clipboard selection changed, mime types: [{}]", joinMimeTypes(selectionMimeTypes()));

        if (onSelectionChanged)
            onSelectionChanged(selectionMimeTypes());
    });

    m_device->setPrimarySelection([this](CCExtDataControlDeviceV1* r, wl_proxy* proxy) {
        // unused, but the offer still has to be taken out of the pending list and destroyed
        if (proxy)
            offerFor(proxy);
    });

    m_device->setFinished([this](CCExtDataControlDeviceV1* r) { onDeviceFinished(); });

    Debug::log(LOG, "[datacontrol] watching the clipboard");
}

const std::vector<std::string>& CDataControl::selectionMimeTypes() const {
    return m_selection ? m_selection->mimeTypes : m_noMimeTypes;
}

int CDataControl::receive(const std::string& mimeType) {
    if (!m_selection || !std::ranges::contains(m_selection->mimeTypes, mimeType)) {
        Debug::log(WARN, "[datacontrol] receive: clipboard has no {} content", mimeType);
        return -1;
    }

    int fds[2] = {-1, -1};
    if (pipe2(fds, O_CLOEXEC) != 0) {
        Debug::log(ERR, "[datacontrol] receive: pipe2 failed: {}", strerror(errno));
        return -1;
    }

    // libwayland dups the fd while marshalling, so our write end can be closed right away.
    // Flushing matters: the owner only starts writing once it gets the request.
    m_selection->offer->sendReceive(mimeType.c_str(), fds[1]);
    wl_display_flush(g_pPortalManager->m_sWaylandConnection.display);
    close(fds[1]);

    return fds[0];
}

SP<CDataControl::SOffer> CDataControl::offerFor(wl_proxy* proxy) {
    const auto IT = std::ranges::find_if(m_pendingOffers, [proxy](const auto& o) { return o->offer->proxy() == proxy; });
    if (IT == m_pendingOffers.end()) {
        Debug::log(ERR, "[datacontrol] compositor named an offer it never announced");
        return nullptr;
    }

    auto offer = *IT;
    m_pendingOffers.erase(IT);
    return offer;
}

void CDataControl::onDeviceFinished() {
    // the device is inert from now on, e.g. the seat went away
    Debug::log(WARN, "[datacontrol] data device finished, clipboard no longer available");
    m_selection.reset();
    m_pendingOffers.clear();
    m_device.reset();

    if (onSelectionChanged)
        onSelectionChanged(m_noMimeTypes);
}
