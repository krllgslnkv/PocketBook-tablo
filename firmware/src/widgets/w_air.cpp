// Воздух в кабинете (CO₂/TVOC) — половина draw_limits_and_air
// предшественника, лимиты подавлены флагом show_limits=false
// (widgets/prims.cpp), даже если данные лимитов есть: widgets::limits —
// отдельный виджет.
#include "widget.h"

#include "prims.h"

namespace widgets {

namespace {

using canvas::Canvas;
using layout::DeviceInfo;
using layout::Rect;
using slots::Store;

// Префикс air_ — см. комментарий в widgets/w_markets.cpp.
const char* const kAirSlots[] = {"co2", "tvoc", "temp", "temperature", "Температура", "hum", "humidity", "Влажность", nullptr};

bool air_visible(const Store& store, const Instance&) {
    return layout::has_data(store.find("co2")) || layout::has_data(store.find("tvoc")) ||
           layout::has_data(store.find("temp")) || layout::has_data(store.find("temperature")) ||
           layout::has_data(store.find("Температура")) || layout::has_data(store.find("hum")) ||
           layout::has_data(store.find("humidity")) || layout::has_data(store.find("Влажность")) ||
           prims::has_failure_reason(store, "home.status");  // причина рисуется в draw_limits_and_air
}

void air_draw(Canvas& c, const Store& store, const DeviceInfo& d, Rect r, const Instance&) {
    prims::draw_limits_and_air(c, store, d, r, /*show_limits=*/false, /*show_air=*/true);
}

}  // namespace

extern const Spec kAirSpec = {
    "air", "Air", Size::kFlex, 202, kAirSlots, 300, &air_visible, &air_draw,
};

}  // namespace widgets
