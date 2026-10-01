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

    // Ask the clipboard owner to write the selection as mimeType. Returns the read end
    // of a pipe the owner writes into, or -1 if the clipboard has no such type. The
    // caller owns the fd.
    int                                                   receive(const std::string& mimeType);

    // Take the clipboard with our own source offering mimeTypes. onSend(mimeType, fd) runs
    // whenever an app pastes; it owns fd and must write the content there and close it.
    // Returns false if the clipboard is not available.
    bool                                                  setSelection(const std::vector<std::string>& mimeTypes, std::function<void(const std::string&, int)> onSend);

    // Give up our source, if we have one
    void                                                  dropOwnSelection();

    // Whether the current clipboard selection is our own source
    bool                                                  ownsSelection() const;

    // Fired on every clipboard selection change, with the new mime types (empty = cleared)
    // and whether the new selection is the source set with setSelection()
    std::function<void(const std::vector<std::string>&, bool)> onSelectionChanged;

    // Fired when another client takes the clipboard away from our source
    std::function<void()>                                 onOwnSelectionLost;

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

    SP<CCExtDataControlSourceV1>     m_source;
    std::vector<std::string>         m_sourceMimeTypes;
    bool                             m_selectionIsOwn = false;
    std::function<void(const std::string&, int)> m_onSend;

    // selection events still to come for our own set_selection requests. The protocol
    // does not say whose source a selection offer belongs to: an offer is taken as ours
    // only while such an event is pending, our source is alive and the offer carries
    // exactly the types our source offered.
    int                              m_pendingOwnSelections = 0;

    const std::vector<std::string>   m_noMimeTypes;
};
