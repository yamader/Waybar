#include "modules/wayfire/workspaces.hpp"

#include <spdlog/spdlog.h>

#include <string>

#include "modules/wayfire/backend.hpp"

namespace waybar::modules::wayfire {

Workspaces::Workspaces(const std::string& id, const Bar& bar, const Json::Value& config)
    : AModule{config, "workspaces", id, false, false},
      bar{bar},
      ipc{IPC::get_instance()},
      handler{[this](const auto& e) {
        spdlog::warn("emit {}", e);
        dp.emit();
      }} {
  box.set_name("workspaces");
  if (!id.empty()) box.get_style_context()->add_class(id);
  box.get_style_context()->add_class(MODULE_CLASS);
  event_box_.add(box);

  ipc->register_handler("view-mapped", handler);
  ipc->register_handler("view-unmapped", handler);
  ipc->register_handler("view-wset-changed", handler);
  ipc->register_handler("output-gain-focus", handler);
  ipc->register_handler("view-sticky", handler);
  ipc->register_handler("view-workspace-changed", handler);
  ipc->register_handler("output-wset-changed", handler);
  ipc->register_handler("wset-workspace-changed", handler);

  ipc->register_handler("window-rules/list-views", handler);
  ipc->register_handler("window-rules/list-outputs", handler);
  ipc->register_handler("window-rules/list-wsets", handler);
  ipc->register_handler("window-rules/get-focused-output", handler);

  dp.emit();
}

Workspaces::~Workspaces() { ipc->unregister_handler(handler); }

auto Workspaces::update() -> void {
  {
    auto _ = ipc->lock_state();

    const auto& output_name = bar.output->name;
    spdlog::debug("outputs_n: {}", ipc->get_outputs().size());
    const auto& output = ipc->get_outputs().at(output_name);
    spdlog::debug("wsets_n: {}", ipc->get_wsets().size());
    const auto& wset = ipc->get_wsets().at(output.wset_idx);

    auto output_focused = ipc->get_focused_output_name() == output_name;
    auto ws_w = wset.ws_w;
    auto ws_h = wset.ws_h;
    auto num_wss = ws_w * ws_h;

    // add buttons for new workspaces
    for (auto i = buttons.size(); i < num_wss; i++) {
      auto& btn = buttons.emplace_back();
      box.pack_start(btn, false, false, 0);
      btn.set_relief(Gtk::RELIEF_NONE);
      if (!config_["disable_click"].asBool()) {
        btn.signal_pressed().connect([=, this] {
          Json::Value data;
          data["x"] = i % ws_w;
          data["y"] = i / ws_h;
          data["output-id"] = output.id;
          ipc->send("vswitch/set-workspace", data);
        });
      }
    }

    // remove buttons for removed workspaces
    buttons.resize(num_wss);

    // update buttons
    for (auto i = 0; i < num_wss; i++) {
      auto& btn = buttons[i];
      auto ctx = btn.get_style_context();

      if (i == wset.ws_idx)
        ctx->add_class("focused");
      else
        ctx->remove_class("focused");
      if (output_focused)
        ctx->add_class("current_output");
      else
        ctx->remove_class("current_output");
      if (wset.wss[i].num_views == 0)
        ctx->add_class("empty");
      else
        ctx->remove_class("empty");

      // todo: format
      btn.set_label(std::to_string(i));
      btn.show();
    }
  }

  spdlog::info("wayfire workspace updated");

  AModule::update();
}

}  // namespace waybar::modules::wayfire
