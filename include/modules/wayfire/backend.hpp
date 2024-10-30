#pragma once

#include <json/json.h>

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace waybar::modules::wayfire {

using EventHandler = std::function<void(const std::string& event)>;

struct State {
  /*
    ┌───────────┐ ┌───────────┐
    │ output #1 │ │ output #2 │
    └─────┬─────┘ └─────┬─────┘
          └─┐           └─────┐─ ─ ─ ─ ─ ─ ─ ─ ┐
    ┌───────┴───────┐ ┌───────┴──────┐ ┌───────┴───────┐
    │ wset #1       │ │ wset #2      │ │ wset #3       │
    │┌────────────┐ │ │┌────────────┐│ │┌────────────┐ │
    ││ workspaces │ │ ││ workspaces ││ ││ workspaces │ │
    │└─┬──────────┘ │ │└────────────┘│ │└─┬──────────┘ │
    │  │ ┌─────────┐│ └──────────────┘ │  │ ┌─────────┐│
    │  ├─┤ view #1 ││                  │  └─┤ view #3 ││
    │  │ └─────────┘│                  │    └─────────┘│
    │  │ ┌─────────┐│                  └───────────────┘
    │  └─┤ view #2 ││
    │    └─────────┘│
    └───────────────┘
  */

  struct Output {
    size_t id;
    size_t w, h;
    size_t wset_idx;
  };

  struct Workspace {
    size_t num_views;
    size_t num_sticky_views;
  };

  struct Wset {
    Output* output;
    std::vector<Workspace> wss;
    size_t ws_w, ws_h, ws_idx;

    auto count_ws(const Json::Value& pos) -> Workspace&;
    auto locate_ws(const Json::Value& geo) -> Workspace&;
  };

  // Waybar uses names to identify outputs
  std::unordered_map<std::string, Output> outputs;
  std::unordered_map<size_t, Wset> wsets;
  std::string focused_output_name;

  // to support workspace resize
  std::atomic_bool wsets_expired, views_expired;

  auto update_output(const Json::Value& output_data) -> void;
  auto update_wset(const Json::Value& wset_data) -> void;
};

class IPC {
  static std::weak_ptr<IPC> instance;
  Json::CharReaderBuilder reader_builder;
  Json::StreamWriterBuilder writer_builder;
  std::list<std::pair<std::string, EventHandler*>> handlers;
  std::mutex handlers_mutex;
  State state;
  std::mutex state_mutex;

  IPC() { start(); }

  static auto connect() -> int;
  auto receive(int fd) -> Json::Value;
  auto start() -> void;
  auto root_event_handler(const std::string& event, const Json::Value& data) -> void;
  auto update_state_handler(const std::string& event, const Json::Value& data) -> void;

 public:
  static auto get_instance() -> std::shared_ptr<IPC>;
  auto send(const std::string& method, const Json::Value& data) -> Json::Value;
  auto register_handler(const std::string& event, EventHandler& handler) -> void;
  auto unregister_handler(EventHandler& handler) -> void;

  auto lock_state() -> std::lock_guard<std::mutex> { return std::lock_guard{state_mutex}; }
  auto& get_outputs() const { return state.outputs; }
  auto& get_wsets() const { return state.wsets; }
  auto& get_focused_output_name() const { return state.focused_output_name; }
};

}  // namespace waybar::modules::wayfire
