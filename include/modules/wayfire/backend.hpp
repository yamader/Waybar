#pragma once

#include <json/json.h>
#include <unistd.h>

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

  std::unordered_map<std::string, Json::Value> outputs;
  std::unordered_map<size_t, Json::Value> wsets;
  std::unordered_map<size_t, Json::Value> views;
  std::string focused_output_name;

  // ???: to support workspace resize
  // todo: check
  std::atomic_bool wsets_expired, views_expired;

  auto update_output(const Json::Value& output_data) -> void;
  auto update_wset(const Json::Value& wset_data) -> void;
};

// fd wrapper
struct Sock {
  int fd;
  Sock(int fd) : fd{fd} {}
  ~Sock() { close(fd); }
  Sock(const Sock&) = delete;
  auto operator=(const Sock&) = delete;
  Sock(Sock&& rhs) noexcept {
    fd = rhs.fd;
    rhs.fd = -1;
  }
  auto& operator=(Sock&& rhs) noexcept {
    fd = rhs.fd;
    rhs.fd = -1;
    return *this;
  }
};

class IPC {
  static std::weak_ptr<IPC> instance;
  Json::CharReaderBuilder reader_builder;
  Json::StreamWriterBuilder writer_builder;
  std::list<std::pair<std::string, std::reference_wrapper<const EventHandler>>> handlers;
  std::mutex handlers_mutex;
  State state;
  std::mutex state_mutex;

  IPC() { start(); }

  static auto connect() -> Sock;
  auto receive(Sock& sock) -> Json::Value;
  auto start() -> void;
  auto root_event_handler(const std::string& event, const Json::Value& data) -> void;
  auto update_state_handler(const std::string& event, const Json::Value& data) -> void;

 public:
  static auto get_instance() -> std::shared_ptr<IPC>;
  auto send(const std::string& method, Json::Value&& data) -> Json::Value;
  auto register_handler(const std::string& event, const EventHandler& handler) -> void;
  auto unregister_handler(EventHandler& handler) -> void;

  auto lock_state() -> std::lock_guard<std::mutex> { return std::lock_guard{state_mutex}; }
  auto& get_outputs() const { return state.outputs; }
  auto& get_wsets() const { return state.wsets; }
  auto& get_views() const { return state.views; }
  auto& get_focused_output_name() const { return state.focused_output_name; }
};

}  // namespace waybar::modules::wayfire
