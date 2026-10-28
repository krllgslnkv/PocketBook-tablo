// tablo_render — PocketBook standalone renderer.
// Renders Tablo dashboards for PocketBook 625 (800x600 E-Ink).
// Reuses the exact upstream C++ layout, canvas, fonts, widgets, and slots logic.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../firmware/src/board.h"
#include "../firmware/src/canvas_mem.h"
#include "../firmware/src/config.cpp"
#include "../firmware/src/i18n.h"
#include "../firmware/src/layout.h"
#include "../firmware/src/slots.h"
#include "../firmware/src/widgets/widget.h"

#include <ArduinoJson.h>

namespace {

std::string read_file_to_string(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

void rotate_90_right(const canvas::CanvasMemory& src, canvas::CanvasMemory& dst) {
    // Rotates 800x600 landscape to 600x800 portrait (for PB625 screen matrix)
    const int16_t sw = src.width();
    const int16_t sh = src.height();
    for (int16_t y = 0; y < sh; ++y) {
        for (int16_t x = 0; x < sw; ++x) {
            if (src.at(x, y)) {
                dst.pixel(static_cast<int16_t>(sh - 1 - y), x, canvas::Color::Black);
            }
        }
    }
}

void rotate_90_left(const canvas::CanvasMemory& src, canvas::CanvasMemory& dst) {
    const int16_t sw = src.width();
    const int16_t sh = src.height();
    for (int16_t y = 0; y < sh; ++y) {
        for (int16_t x = 0; x < sw; ++x) {
            if (src.at(x, y)) {
                dst.pixel(y, static_cast<int16_t>(sw - 1 - x), canvas::Color::Black);
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "";
    std::string slots_path = "";
    std::string out_path = "tablo_live.png";
    std::string rot_mode = "right";  // "right", "left", "none"
    std::string boot_status = "";
    std::string ap_ssid = "";
    std::string ap_password = "";
    std::string setup_url = "";
    bool force_setup = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "-slots" && i + 1 < argc) {
            slots_path = argv[++i];
        } else if (arg == "-out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "-rot" && i + 1 < argc) {
            rot_mode = argv[++i];
        } else if (arg == "-boot" && i + 1 < argc) {
            boot_status = argv[++i];
        } else if (arg == "-setup-url" && i + 1 < argc) {
            setup_url = argv[++i];
        } else if (arg == "-setup") {
            force_setup = true;
        } else if (arg == "-ap" && i + 2 < argc) {
            ap_ssid = argv[++i];
            ap_password = argv[++i];
        } else if (arg == "-print-widgets") {
            JsonDocument doc;
            JsonArray arr = doc.to<JsonArray>();
            size_t count = 0;
            const widgets::Spec* const* specs = widgets::all(&count);
            for (size_t j = 0; j < count; ++j) {
                const widgets::Spec* spec = specs[j];
                JsonObject o = arr.add<JsonObject>();
                o["type"] = spec->type;
                o["title"] = spec->title;
                o["default_size"] = widgets::size_to_string(spec->default_size);
                o["min_width"] = spec->min_width;
                JsonArray slots = o["slots"].to<JsonArray>();
                for (const char* const* p = spec->slots; p != nullptr && *p != nullptr; ++p) {
                    slots.add(*p);
                }
                o["refresh"] = spec->refresh_seconds;
            }
            std::string out;
            serializeJson(doc, out);
            std::cout << out << std::endl;
            return 0;
        } else if (arg == "-print-config") {
            bool secrets = (i + 1 < argc && std::string(argv[i + 1]) == "1");
            config::Settings s = config::defaults();
            if (!config_path.empty()) {
                std::string cfg_json = read_file_to_string(config_path);
                if (!cfg_json.empty()) config::from_json(cfg_json, s);
            }
            std::cout << config::to_json(s, secrets).c_str() << std::endl;
            return 0;
        }
    }

    config::Settings settings = config::defaults();
    if (!config_path.empty()) {
        std::string cfg_json = read_file_to_string(config_path);
        if (!cfg_json.empty()) {
            config::from_json(cfg_json, settings);
        }
    }

    i18n::set_lang(i18n::from_code(settings.lang.c_str()));

    canvas::CanvasMemory src_canvas(board::SCREEN_WIDTH, board::SCREEN_HEIGHT);

    if (!boot_status.empty()) {
        layout::draw_boot(src_canvas, boot_status.c_str());
    } else if (!ap_ssid.empty()) {
        layout::draw_ap_credentials(src_canvas, ap_ssid, ap_password);
    } else {
        slots::Store store;
        layout::DeviceInfo device;
        device.now = static_cast<uint32_t>(time(nullptr));
        device.next_update_at = device.now + 300;
        device.timezone_minutes = settings.timezone_minutes;
        device.battery_pct = 100;
        device.wifi_rssi = -60;

        if (!slots_path.empty()) {
            std::string slots_json = read_file_to_string(slots_path);
            if (!slots_json.empty()) {
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, slots_json);
                if (!err) {
                    if (doc["device"].is<JsonObject>()) {
                        JsonObject dev = doc["device"];
                        if (dev["battery_pct"].is<int>()) device.battery_pct = dev["battery_pct"];
                        if (dev["wifi_rssi"].is<int>()) device.wifi_rssi = dev["wifi_rssi"];
                        if (dev["now"].is<uint32_t>()) device.now = dev["now"];
                        if (dev["next_update_at"].is<uint32_t>()) device.next_update_at = dev["next_update_at"];
                        if (dev["timezone_minutes"].is<int>()) device.timezone_minutes = dev["timezone_minutes"];
                        if (dev["setup_url"].is<const char*>()) {
                            device.setup_url = dev["setup_url"].as<const char*>();
                        }
                        if (dev["lang"].is<const char*>()) {
                            i18n::set_lang(i18n::from_code(dev["lang"]));
                        }
                    }

                    if (doc["slots"].is<JsonObject>()) {
                        JsonObject s_obj = doc["slots"];
                        for (JsonPair kv : s_obj) {
                            const char* slot_id = kv.key().c_str();
                            JsonObject s_val = kv.value().as<JsonObject>();
                            slots::Slot sl;
                            sl.number = s_val["number"] | 0.0f;
                            sl.delta = s_val["delta"] | 0.0f;
                            sl.ok = s_val["ok"] | true;
                            sl.at = s_val["at"] | device.now;
                            sl.ttl = s_val["ttl"] | 300;
                            if (s_val["error"].is<const char*>()) {
                                sl.error = String(s_val["error"].as<const char*>());
                            }
                            if (s_val["text"].is<const char*>()) {
                                sl.text = String(s_val["text"].as<const char*>());
                            } else {
                                char buf[32];
                                std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(sl.number));
                                sl.text = String(buf);
                            }

                            const char* conn = s_val["connector"] | "daemon";
                            store.put(slot_id, sl, conn);

                            if (s_val["history"].is<JsonArray>()) {
                                std::vector<float> hist;
                                for (float v : s_val["history"].as<JsonArray>()) {
                                    hist.push_back(v);
                                }
                                if (!hist.empty()) {
                                    store.put_history(slot_id, hist.data(), static_cast<uint8_t>(hist.size()), conn);
                                }
                            }
                        }
                    }

                    if (doc["failed"].is<JsonObject>()) {
                        for (JsonPair kv : doc["failed"].as<JsonObject>()) {
                            store.mark_failed(kv.key().c_str(), kv.value().as<const char*>());
                        }
                    }
                }
            }
        }

        if (!setup_url.empty()) {
            device.setup_url = setup_url.c_str();
        }

        if (force_setup) {
            layout::draw_setup(src_canvas, device, device.setup_url);
        } else {
            uint8_t db_idx = settings.active_dashboard;
            if (db_idx >= settings.dashboards.size()) db_idx = 0;
            layout::draw_frame(src_canvas, store, device, settings.dashboards[db_idx]);
        }
    }

    bool ok = false;
    if (rot_mode == "right" || rot_mode == "90") {
        canvas::CanvasMemory dst_canvas(board::SCREEN_HEIGHT, board::SCREEN_WIDTH);
        rotate_90_right(src_canvas, dst_canvas);
        ok = dst_canvas.save_png(out_path);
    } else if (rot_mode == "left" || rot_mode == "270") {
        canvas::CanvasMemory dst_canvas(board::SCREEN_HEIGHT, board::SCREEN_WIDTH);
        rotate_90_left(src_canvas, dst_canvas);
        ok = dst_canvas.save_png(out_path);
    } else {
        ok = src_canvas.save_png(out_path);
    }

    if (ok) {
        std::printf("OK -> %s\n", out_path.c_str());
        return 0;
    } else {
        std::fprintf(stderr, "FAIL -> %s\n", out_path.c_str());
        return 1;
    }
}
