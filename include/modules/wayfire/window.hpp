#pragma once

#include "AAppIconLabel.hpp"
#include "bar.hpp"
#include "modules/wayfire/backend.hpp"

namespace waybar::modules::wayfire {

class Window : public AAppIconLabel {
  const Bar& bar;
  std::shared_ptr<IPC> ipc;
  EventHandler handler;

 public:
  Window(const std::string& id, const Bar& bar, const Json::Value& config);
  ~Window() override;

  auto update() -> void override;
};

}  // namespace waybar::modules::wayfire
