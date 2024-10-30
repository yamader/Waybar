#pragma once

#include <gtkmm/button.h>
#include <json/json.h>

#include <memory>
#include <vector>

#include "AModule.hpp"
#include "bar.hpp"
#include "modules/wayfire/backend.hpp"

namespace waybar::modules::wayfire {

class Workspaces : public AModule {
  const Bar& bar;
  std::shared_ptr<IPC> ipc;
  EventHandler handler;

  Gtk::Box box;
  std::vector<Gtk::Button> buttons;

  auto update() -> void override;

 public:
  Workspaces(const std::string& id, const Bar& bar, const Json::Value& config);
  ~Workspaces() override;
};

}  // namespace waybar::modules::wayfire
