#pragma once

#include "detail/interface.hpp"
#if defined(_WIN32) || defined(WIN32)
    #define WINDOWS
    #include "detail/windows.hpp"
#elif defined(__linux__) || defined(__FreeBSD__)
    #define LINUX
    #include "detail/linux.hpp"
#else
    #error "platform not supported"
#endif

#include <memory>
#include <string>

namespace clipboardxx {

#ifdef WINDOWS
using ClipboardType = ClipboardWindows;
#elif defined(LINUX)
using ClipboardType = ClipboardLinux;
#endif

class clipboard {
public:
    clipboard() : m_clipboard(std::make_unique<ClipboardType>()) {}

    void operator<<(const std::string &text) const { copy(text); }

    void copy(const std::string &text) const { m_clipboard->copy(text); }

    void operator>>(std::string &result) const { result = paste(); }

    std::string paste() const { return m_clipboard->paste(); }

private:
    std::unique_ptr<ClipboardInterface> m_clipboard;
};

} // namespace clipboardxx
