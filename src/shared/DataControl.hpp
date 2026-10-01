#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../includes.hpp"
#include "ext-data-control-v1.hpp"
#include "wayland.hpp"

// Watches and owns the compositor clipboard through ext-data-control-v1.
// Only the regular clipboard is handled; primary selection offers are dropped.
class CDataControl {
  public:
    CDataControl(SP<CCExtDataControlManagerV1> manager, SP<CCWlSeat> seat);

    // Mime types of the current clipboard selection, empty when the clipboard is empty
    const std::vector<std::string>&                       selectionMimeTypes() const;

    // Fired on every clipboard selection change, with the new mime types (empty = cleared)
    std::function<void(const std::vector<std::string>&)> onSelectionChanged;

  private:
    struct SOffer {
        SP<CCExtDataControlOfferV1> offer;
        std::vector<std::string>    mimeTypes;
    };

    SP<SOffer>                       offerFor(wl_proxy* proxy);
    void                             onDeviceFinished();

    SP<CCExtDataControlManagerV1>    m_manager;
    SP<CCExtDataControlDeviceV1>     m_device;

    // offers announced by data_offer that are not the selection (yet)
    std::vector<SP<SOffer>>          m_pendingOffers;
    SP<SOffer>                       m_selection;

    const std::vector<std::string>   m_noMimeTypes;
};
