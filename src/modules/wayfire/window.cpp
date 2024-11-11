#include "modules/wayfire/window.hpp"

#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <spdlog/spdlog.h>

#include "util/rewrite_string.hpp"
#include "util/sanitize_str.hpp"

namespace waybar::modules::wayfire {

Window::Window(const std::string& id, const Bar& bar, const Json::Value& config)
    : AAppIconLabel(config, "window", id, "{title}", 0, true),
      bar{bar},
      ipc{IPC::get_instance()},
      handler{[this](const auto&) { dp.emit(); }} {
  ipc->register_handler("WindowsChanged", handler);
  ipc->register_handler("WindowOpenedOrChanged", handler);
  ipc->register_handler("WindowClosed", handler);
  ipc->register_handler("WindowFocusChanged", handler);

  dp.emit();
}

Window::~Window() { ipc->unregister_handler(handler); }

}  // namespace waybar::modules::wayfire
