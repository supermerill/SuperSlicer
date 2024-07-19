#pragma once

#include "../../exception.hpp"
#include "xcb_event.hpp"

#include <assert.h>
#include <memory>
#include <optional>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace clipboardxx {
namespace xcb {

constexpr uint8_t kBitsPerByte = 8;
constexpr uint8_t kFilterXcbEventType = 0x80;

class Xcb {
public:
    using Atom = xcb_atom_t;
    using Window = xcb_window_t;

    class XcbException : public exception {
    public:
        XcbException(const std::string &reason, int32_t error_code)
            : exception(reason + " (" + std::to_string(error_code) + ")"){};
    };

    Xcb() : m_conn(create_connection()), m_window(create_window(m_conn.get())) {}

    Atom create_atom(const std::string &name) {
        xcb_intern_atom_cookie_t cookie = xcb_intern_atom(m_conn.get(), false, name.size(), name.c_str());

        xcb_generic_error_t* error = nullptr;
        std::unique_ptr<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(m_conn.get(), cookie, &error));
        handle_generic_error(error, "Cannot create atom with name '" + name + "'");

        return reply->atom;
    }

    void become_selection_owner(Atom selection) {
        xcb_void_cookie_t cookie = xcb_set_selection_owner_checked(m_conn.get(), m_window, selection, XCB_CURRENT_TIME);
        handle_generic_error(xcb_request_check(m_conn.get(), cookie), "Cannot become owner of clipboard selection");
        xcb_flush(m_conn.get());
    }

    std::optional<std::unique_ptr<Event>> get_latest_event() {
        std::unique_ptr<xcb_generic_event_t> event(xcb_poll_for_event(m_conn.get()));

        // TODO: find a workaround for this
        assert(m_conn != nullptr);

        if (!event)
            return std::nullopt;
        return convert_generic_event_to_event(std::move(event));
    }

    template <typename Container, typename ValueType = typename Container::value_type>
    void write_on_window_property(Window window, Atom property, Atom target, const Container &data) {
        xcb_change_property(m_conn.get(), XCB_PROP_MODE_REPLACE, window, property, target,
                            sizeof(ValueType) * kBitsPerByte, data.size(), data.data());
        xcb_flush(m_conn.get());
    }

    void notify_window_property_change(Window window, Atom property, Atom target, Atom selection) {
        const xcb_selection_notify_event_t event{.response_type = XCB_SELECTION_NOTIFY,
                                                 .pad0 = 0,
                                                 .sequence = 0,
                                                 .time = XCB_CURRENT_TIME,
                                                 .requestor = window,
                                                 .selection = selection,
                                                 .target = target,
                                                 .property = property};

        xcb_send_event(m_conn.get(), false, window, XCB_EVENT_MASK_PROPERTY_CHANGE,
                       reinterpret_cast<const char*>(&event));
        xcb_flush(m_conn.get());
    }

    void request_selection_data(Atom selection, Atom target, Atom result) {
        xcb_convert_selection_checked(m_conn.get(), m_window, selection, target, result, XCB_CURRENT_TIME);
        xcb_flush(m_conn.get());
    }

    std::string get_our_property_value(Atom property) {
        xcb_get_property_cookie_t cookie =
            xcb_get_property(m_conn.get(), static_cast<uint8_t>(true), m_window, property, XCB_ATOM_ANY, 0, -1);

        xcb_generic_error_t* error = nullptr;
        std::unique_ptr<xcb_get_property_reply_t> reply(xcb_get_property_reply(m_conn.get(), cookie, &error));
        std::unique_ptr<xcb_generic_error_t> error_ptr(error);
        if (error != nullptr)
            return std::string("");

        const char* data = reinterpret_cast<const char*>(xcb_get_property_value(reply.get()));
        uint32_t length = xcb_get_property_value_length(reply.get());
        return std::string(data, length);
    }

private:
    class XcbConnectionDeleter {
    public:
        void operator()(xcb_connection_t* conn) { xcb_disconnect(conn); }
    };

    using XcbConnectionPtr = std::unique_ptr<xcb_connection_t, XcbConnectionDeleter>;

    XcbConnectionPtr create_connection() const {
        XcbConnectionPtr connection(xcb_connect(nullptr, nullptr));
        int32_t error = xcb_connection_has_error(connection.get());
        if (error > 0)
            throw XcbException("Cannot connect to X server", error);

        return connection;
    }

    xcb_window_t create_window(xcb_connection_t* conn) const {
        xcb_screen_t* screen = get_root_screen(conn);
        xcb_window_t window = xcb_generate_id(conn);

        uint32_t mask_value = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_void_cookie_t cookie = xcb_create_window_checked(conn, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 1,
                                                             1, 0, XCB_WINDOW_CLASS_COPY_FROM_PARENT,
                                                             screen->root_visual, XCB_CW_EVENT_MASK, &mask_value);
        handle_generic_error(xcb_request_check(conn, cookie), "Cannot create window");

        return window;
    }

    xcb_screen_t* get_root_screen(xcb_connection_t* conn) const {
        const xcb_setup_t* setup_info = xcb_get_setup(conn);
        xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup_info);
        return screens.data;
    }

    void handle_generic_error(xcb_generic_error_t* error, const std::string &error_msg) const {
        std::unique_ptr<xcb_generic_error_t> error_ptr(error);
        if (error_ptr)
            throw XcbException(error_msg, error_ptr->error_code);
    }

    std::unique_ptr<Event> convert_generic_event_to_event(std::unique_ptr<xcb_generic_event_t> event) {
        uint8_t event_type = event->response_type & ~kFilterXcbEventType;
        switch (event_type) {
        // someone requested clipboard data
        case XCB_SELECTION_REQUEST: {
            xcb_selection_request_event_t* sel_request_event =
                reinterpret_cast<xcb_selection_request_event_t*>(event.get());
            return std::make_unique<RequestSelectionEvent>(sel_request_event->requestor, sel_request_event->owner,
                                                           sel_request_event->selection, sel_request_event->target,
                                                           sel_request_event->property);
        }

        // we are no longer owner of clipboard
        case XCB_SELECTION_CLEAR: {
            xcb_selection_clear_event_t* sel_clear_event = reinterpret_cast<xcb_selection_clear_event_t*>(event.get());
            return std::make_unique<SelectionClearEvent>(sel_clear_event->selection);
        }

        // our selection has been changed
        case XCB_SELECTION_NOTIFY: {
            xcb_selection_notify_event_t* sel_notify_event =
                reinterpret_cast<xcb_selection_notify_event_t*>(event.get());
            return std::make_unique<SelectionNotifyEvent>(sel_notify_event->requestor, sel_notify_event->selection,
                                                          sel_notify_event->target, sel_notify_event->property);
        }

        default:
            return std::make_unique<Event>(Event::Type::kNone);
        }
    }

    const XcbConnectionPtr m_conn;
    const xcb_window_t m_window;
};

} // namespace xcb
} // namespace clipboardxx
