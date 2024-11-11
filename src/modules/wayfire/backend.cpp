#include "modules/wayfire/backend.hpp"

#include <json/json.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <ranges>
#include <stdexcept>
#include <thread>

namespace waybar::modules::wayfire {

std::weak_ptr<IPC> IPC::instance;

// C++23: std::byteswap
inline auto byteswap(uint32_t x) -> uint32_t {
  return (x & 0xff000000) >> 24 | (x & 0x00ff0000) >> 8 | (x & 0x0000ff00) << 8 |
         (x & 0x000000ff) << 24;
}

auto pack_and_write(Sock& sock, std::string_view msg) -> void {
  uint32_t len = msg.size();
  if constexpr (std::endian::native != std::endian::little) len = byteswap(len);
  (void)write(sock.fd, &len, 4);
  (void)write(sock.fd, msg.data(), msg.size());
}

auto read_exact(Sock& sock, size_t n) -> std::string {
  auto buf = std::string(n, 0);
  for (size_t i = 0; i < n;) i += read(sock.fd, &buf[i], n - i);
  return buf;
}

// https://github.com/WayfireWM/pywayfire/blob/69b7c21/wayfire/ipc.py#L438
inline auto is_mapped_toplevel_view(const Json::Value& view) -> bool {
  return view["mapped"].asBool() and view["role"] != "desktop-environment" and
         view["pid"].asInt() != -1;
}

auto State::Wset::count_ws(const Json::Value& pos) -> Workspace& {
  auto x = pos["x"].asInt();
  auto y = pos["y"].asInt();
  return wss.at(ws_w * y + x);
}

auto State::Wset::locate_ws(const Json::Value& geo) -> Workspace& {
  auto x = geo["x"].asInt() / output.get().w;
  auto y = geo["y"].asInt() / output.get().h;
  return wss.at(ws_w * y + x);
}

// output updates are not notified by event, so manual updates are required
auto State::update_output(const Json::Value& output_data) -> void {
  auto& output = outputs.at(output_data["name"].asString());
  output.id = output_data["id"].asUInt();
  output.w = output_data["geometry"]["width"].asUInt();
  output.h = output_data["geometry"]["height"].asUInt();
  output.wset_idx = output_data["wset-index"].asUInt();
}

// wset updates are not notified by event, so manual updates are required
auto State::update_wset(const Json::Value& wset_data) -> void {
  // todo: new wset
  auto& wset = wsets.at(wset_data["index"].asUInt());
  auto ws_w = wset_data["workspace"]["grid_width"].asUInt();
  auto ws_h = wset_data["workspace"]["grid_height"].asUInt();
  // when ws size changed, then update wsets/views info
  views_expired = wsets_expired = wsets_expired or (wset.ws_w != ws_w) or (wset.ws_h != ws_h);
}

auto IPC::get_instance() -> std::shared_ptr<IPC> {
  auto p = instance.lock();
  if (!p) instance = p = std::shared_ptr<IPC>(new IPC);
  return p;
}

auto IPC::connect() -> Sock {
  auto* path = std::getenv("WAYFIRE_SOCKET");
  if (path == nullptr) {
    throw std::runtime_error{"Wayfire IPC: ipc not available"};
  }

  auto sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock == -1) {
    throw std::runtime_error{"Wayfire IPC: socket() failed"};
  }

  auto addr = sockaddr_un{.sun_family = AF_UNIX};
  std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
  addr.sun_path[sizeof(addr.sun_path) - 1] = 0;

  if (::connect(sock, (const sockaddr*)&addr, sizeof(addr)) == -1) {
    close(sock);
    throw std::runtime_error{"Wayfire IPC: connect() failed"};
  }

  return {sock};
}

auto IPC::receive(Sock& sock) -> Json::Value {
  auto len = *reinterpret_cast<uint32_t*>(read_exact(sock, 4).data());
  if constexpr (std::endian::native != std::endian::little) len = byteswap(len);
  auto msg = read_exact(sock, len);

  Json::Value json;
  std::string err;
  auto* reader = reader_builder.newCharReader();
  if (!reader->parse(&*msg.begin(), &*msg.end(), &json, &err)) {
    throw std::runtime_error{"Wayfire IPC: parse json failed: " + err};
  }
  return json;
}

auto IPC::send(const std::string& method, Json::Value&& data) -> Json::Value {
  spdlog::debug("Wayfire IPC: send method \"{}\"", method);
  auto sock = connect();

  Json::Value msg;
  msg["method"] = method;
  msg["data"] = std::move(data);

  pack_and_write(sock, Json::writeString(writer_builder, msg));
  auto res = receive(sock);
  root_event_handler(method, res);
  return res;
}

auto IPC::start() -> void {
  spdlog::info("Wayfire IPC: starting");

  // init state
  send("window-rules/list-outputs", {});
  send("window-rules/list-wsets", {});
  send("window-rules/list-views", {});
  send("window-rules/get-focused-output", {});

  std::thread([&] {
    auto sock = connect();

    {
      Json::Value msg;
      msg["method"] = "window-rules/events/watch";
      pack_and_write(sock, Json::writeString(writer_builder, msg));
      if (receive(sock)["result"] != "ok") {
        spdlog::error(
            "Wayfire IPC: method \"window-rules/events/watch\""
            " have failed");
        return;
      }
    }

    while (auto msg = receive(sock)) {
      auto ev = msg["event"].asString();
      spdlog::debug("Wayfire IPC: received event \"{}\"", ev);
      root_event_handler(ev, msg);
    }
  }).detach();
}

auto IPC::register_handler(const std::string& event, const EventHandler& handler) -> void {
  auto _ = std::lock_guard{handlers_mutex};
  handlers.emplace_back(event, handler);
}

auto IPC::unregister_handler(EventHandler& handler) -> void {
  auto _ = std::lock_guard{handlers_mutex};
  handlers.remove_if([&](auto& e) { return &e.second.get() == &handler; });
}

auto IPC::root_event_handler(const std::string& event, const Json::Value& data) -> void {
  {
    auto _ = lock_state();
    update_state_handler(event, data);
  }
  if (state.wsets_expired) {
    state.wsets_expired = false;
    send("window-rules/list-wsets", {});
  }
  if (state.views_expired) {
    state.views_expired = false;
    send("window-rules/list-views", {});
  }
  if (not(state.wsets_expired or state.views_expired)) {
    auto _ = std::lock_guard{handlers_mutex};
    for (const auto& [_event, handler] : handlers)
      if (_event == event) handler(event);
  }
}

auto IPC::update_state_handler(const std::string& event, const Json::Value& data) -> void {
  // IPC events
  // https://github.com/WayfireWM/wayfire/blob/053b222/plugins/ipc-rules/ipc-events.hpp#L108-L125
  /*
    [x] view-mapped
    [x] view-unmapped
    [ ] view-set-output  // -> view-wset-changed
    [ ] view-geometry-changed // can detect view movement when vwidth/vheight resizing
    [x] view-wset-changed
    [ ] view-focused  // -> output-gain-focus
    [ ] view-title-changed
    [ ] view-app-id-changed
    [ ] plugin-activation-state-changed
    [x] output-gain-focus

    [ ] view-tiled
    [ ] view-minimized
    [ ] view-fullscreened
    [x] view-sticky
    [x] view-workspace-changed
    [x] output-wset-changed
    [x] wset-workspace-changed
  */

  if (event == "view-mapped") {
    // data: { event, view }
    if (const auto& view_data = data["view"]; is_mapped_toplevel_view(view_data)) {
      try {
        // view_data["wset-index"] could be messed up, but that's noise
        auto& wset = state.wsets.at(view_data["wset-index"].asUInt());
        auto& ws = wset.locate_ws(view_data["geometry"]);

        ws.num_views++;
        if (view_data["sticky"].asBool()) ws.num_sticky_views++;
      } catch (std::exception&) {
      }
    }
    return;
  }

  if (event == "view-unmapped") {
    // data: { event, view }
    if (const auto& view_data = data["view"]; is_mapped_toplevel_view(view_data)) {
      auto& wset = state.wsets.at(view_data["wset-index"].asUInt());
      auto& ws = wset.locate_ws(view_data["geometry"]);

      ws.num_views--;
      if (view_data["sticky"].asBool()) ws.num_sticky_views--;
    }
    return;
  }

  if (event == "view-wset-changed") {
    // data: { event, old-wset: wset, new-wset: wset, view }
    const auto& old_wset_data = data["old-wset"];
    const auto& new_wset_data = data["new_wset"];
    auto& old_wset = state.wsets.at(old_wset_data["index"].asUInt());
    auto& new_wset = state.wsets.at(new_wset_data["index"].asUInt());
    auto& old_ws = old_wset.count_ws(old_wset_data["workspace"]);
    auto& new_ws = new_wset.count_ws(new_wset_data["workspace"]);

    old_ws.num_views--;
    new_ws.num_views++;
    if (data["view"]["sticky"].asBool()) {
      old_ws.num_sticky_views--;
      new_ws.num_sticky_views++;
    }

    state.update_wset(old_wset_data);
    state.update_wset(new_wset_data);
    return;
  }

  if (event == "output-gain-focus") {
    // data: { event, output }
    const auto& output_data = data["output"];

    state.focused_output_name = output_data["name"].asString();

    state.update_output(output_data);
    return;
  }

  if (event == "view-sticky") {
    // data: { event, view }
    const auto& view_data = data["view"];
    auto& wset = state.wsets.at(view_data["wset-index"].asUInt());
    auto& ws = wset.locate_ws(view_data["geometry"]);

    if (view_data["sticky"].asBool())
      ws.num_sticky_views++;
    else
      ws.num_sticky_views--;
    return;
  }

  if (event == "view-workspace-changed") {
    // data: { event, from: point, to: point, view }
    const auto& view_data = data["view"];
    auto& wset = state.wsets.at(view_data["wset-index"].asUInt());

    try {
      // data["from"] could be messed up, but that's noise
      auto& old_ws = wset.count_ws(data["from"]);
      auto& new_ws = wset.count_ws(data["to"]);

      old_ws.num_views--;
      new_ws.num_views++;
      if (view_data["sticky"].asBool()) {
        old_ws.num_sticky_views--;
        new_ws.num_sticky_views++;
      }
    } catch (std::exception&) {
    }
    return;
  }

  if (event == "output-wset-changed") {
    // data: { event, new-wset: id, output: id, new-wset-data: wset, output-data: output }
    const auto& wset_data = data["new-wset-data"];
    const auto& output_data = data["output-data"];
    auto& output = state.outputs.at(output_data["name"].asString());
    auto wset_idx = wset_data["index"].asUInt();

    state.wsets.at(wset_idx).output = output;
    output.wset_idx = wset_idx;

    state.update_wset(wset_data);
    state.update_output(output_data);
    return;
  }

  if (event == "wset-workspace-changed") {
    // data: { event, previous-workspace: point, new-workspace: point,
    //         output: id, wset: id, output-data: output, wset-data: wset }
    const auto& wset_data = data["wset-data"];
    auto& wset = state.wsets.at(wset_data["index"].asUInt());
    auto x = data["new-workspace"]["x"].asUInt();
    auto y = data["new-workspace"]["y"].asUInt();

    wset.ws_idx = wset.ws_w * y + x;

    state.update_output(data["output-data"]);
    state.update_wset(wset_data);
    return;
  }

  // IPC responses
  // https://github.com/WayfireWM/wayfire/blob/053b222/plugins/ipc-rules/ipc-rules.cpp#L27-L37

  if (event == "window-rules/list-views") {
    // data: [ view ]
    for (auto& [_, wset] : state.wsets) std::ranges::fill(wset.wss, State::Workspace{});
    for (const auto& view_data : data | std::views::filter(is_mapped_toplevel_view)) {
      try {
        // view_data["geometry"] could be messed up, but that's noise
        auto& wset = state.wsets.at(view_data["wset-index"].asUInt());
        auto& ws = wset.locate_ws(view_data["geometry"]);

        ws.num_views++;
        if (view_data["sticky"].asBool()) ws.num_sticky_views++;
      } catch (std::exception&) {
      }
    }
    return;
  }

  if (event == "window-rules/list-outputs") {
    // data: [ output ]
    state.outputs.clear();
    for (const auto& output_data : data) {
      state.outputs.emplace(output_data["name"].asString(),
                            State::Output{
                                .id = output_data["id"].asUInt(),
                                .w = output_data["geometry"]["width"].asUInt(),
                                .h = output_data["geometry"]["height"].asUInt(),
                                .wset_idx = output_data["wset-index"].asUInt(),
                            });
    }
    return;
  }

  if (event == "window-rules/list-wsets") {
    // data: [ wset ]
    state.wsets.clear();  // does not keep views info, so manual list-views required
    for (const auto& wset_data : data) {
      const auto& ws_data = wset_data["workspace"];
      auto ws_w = ws_data["grid_width"].asUInt();
      auto ws_h = ws_data["grid_height"].asUInt();

      state.wsets.emplace(wset_data["index"].asUInt(),
                          State::Wset{
                              .output = state.outputs.at(wset_data["output-name"].asString()),
                              .wss = std::vector<State::Workspace>(ws_w * ws_h),
                              .ws_w = ws_w,
                              .ws_h = ws_h,
                              .ws_idx = ws_w * ws_data["y"].asUInt() + ws_data["x"].asUInt(),
                          });
    }
    return;
  }

  if (event == "window-rules/get-focused-output") {
    // data: { ok, info: output }
    const auto& output_data = data["info"];

    state.focused_output_name = output_data["name"].asString();

    state.update_output(output_data);
    return;
  }
}

}  // namespace waybar::modules::wayfire
