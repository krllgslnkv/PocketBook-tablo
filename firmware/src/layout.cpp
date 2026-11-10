// Раскладка — шапка кадра плюс сборка дашборда по данным. Числа каркаса ниже
// — не оценка на глаз, а обмер эталона тем же способом, каким его потом
// проверяет tools/compare_frame.py (самый длинный сплошной пробег тёмных
// пикселей в строке/столбце — линия, текст такого пробега не даёт). Подробный
// разбор — Status Log в .claude/plans/tablo.md.
//
// Отрисовка конкретных блоков (Рынки, Лимиты, Воздух, Почта, Сегодня, …)
// переехала в widgets/w_*.cpp — единый протокол виджета (widgets/widget.h,
// docs/widgets.md). Здесь остаётся то, что знает только раскладка: шапка,
// арифметика календаря, правило ряда (S/M/flex) и сборка активного
// дашборда — draw_frame() ниже просто раскладывает виджеты по их Spec и не
// знает имён конкретных слотов.
#include "layout.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "../assets/terminus_14.h"
#include "../assets/terminus_16.h"
#include "../assets/terminus_20.h"
#include "../assets/terminus_24.h"
#include "config.h"
#include "font.h"
#include "i18n.h"
#include "widgets/prims.h"
#include "widgets/widget.h"
#include "wifi_qr.h"

namespace layout {

using canvas::Canvas;
using canvas::Color;
using fonts::draw_text;
using fonts::text_width;
using slots::Slot;
using slots::Store;

namespace {

// ── каркас (docs/architecture.md, обмер reference/cockpit-reference.png) ──
constexpr int16_t MARGIN = 15;
constexpr int16_t HEADER_RULE_Y = 49;   // горизонтальная линия под шапкой
constexpr int16_t BODY_TOP = 63;        // y верхнего ряда — эйброу-линия ляжет на 71
constexpr int16_t ROW_DIVIDER_Y = 298;  // линия между верхним и нижним рядом
constexpr int16_t ROW_GAP = 12;         // 310 - 298
// Зазор между виджетами В РЯДУ — раздельно по рядам, TOP_GAP != BOTTOM_GAP в
// эталоне (19 против 17), см. docs/widgets.md, «Правило ряда».
constexpr int16_t TOP_GAP = 19;     // между Рынками и правой колонкой
constexpr int16_t BOTTOM_GAP = 17;  // между Почтой и Сегодня

// ── арифметика календаря (алгоритм civil_from_days, H. Hinnant, public
// domain) — не тянем <ctime>/localtime ради пяти чисел и чтобы поведение не
// зависело от libc хоста и таргета одновременно. ──

int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) --q;
    return q;
}

int64_t floor_mod(int64_t a, int64_t b) { return a - floor_div(a, b) * b; }

void civil_from_days(int64_t z, int* year, unsigned* month, unsigned* day) {
    z += 719468;
    int64_t era = floor_div(z, 146097);
    unsigned doe = static_cast<unsigned>(z - era * 146097);              // [0, 146096]
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    int64_t y = static_cast<int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
    unsigned mp = (5 * doy + 2) / 153;                       // [0, 11]
    *day = doy - (153 * mp + 2) / 5 + 1;                     // [1, 31]
    *month = mp + (mp < 10 ? 3 : static_cast<unsigned>(-9));  // [1, 12]
    *year = static_cast<int>(y + (*month <= 2 ? 1 : 0));
}

void draw_wifi_bars(Canvas& c, int16_t x, int16_t y, int8_t bars) {
    constexpr int16_t kIconH = 16;
    for (int8_t i = 0; i < 4; ++i) {
        int16_t bar_h = static_cast<int16_t>(4 + i * 3);
        int16_t bx = static_cast<int16_t>(x + i * 7);
        int16_t by = static_cast<int16_t>(y + kIconH - bar_h);
        c.rect(bx, by, 5, bar_h, Color::Black);
        if (i < bars && bar_h > 2) {
            c.fill_rect(static_cast<int16_t>(bx + 1), static_cast<int16_t>(by + 1), 3,
                        static_cast<int16_t>(bar_h - 2), Color::Black);
        }
    }
    if (bars == 0) {
        // Перечёркивание иконки при отсутствии соединения с Wi-Fi
        c.line(static_cast<int16_t>(x - 1), static_cast<int16_t>(y + kIconH),
               static_cast<int16_t>(x + 27), y, Color::Black);
        c.line(static_cast<int16_t>(x - 1), static_cast<int16_t>(y + kIconH - 1),
               static_cast<int16_t>(x + 27), static_cast<int16_t>(y - 1), Color::Black);
    }
}

void draw_battery_icon(Canvas& c, int16_t x, int16_t y, int8_t pct) {
    c.rect(x, y, 22, 13, Color::Black);
    c.fill_rect(static_cast<int16_t>(x + 22), static_cast<int16_t>(y + 3), 2, 7, Color::Black);
    int8_t clamped = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    int16_t fill_w = static_cast<int16_t>((22 - 4) * clamped / 100);
    if (fill_w > 0) {
        c.fill_rect(static_cast<int16_t>(x + 2), static_cast<int16_t>(y + 2), fill_w, 9,
                    Color::Black);
    }
}

// ── шапка ──

void draw_header(Canvas& c, const DeviceInfo& d, bool show_setup_url = false) {
    Civil now_civil = to_civil(d.now, d.timezone_minutes);

    const int16_t margin = (c.width() == 800) ? 15 : static_cast<int16_t>((c.width() * 15 + 400) / 800);
    const int16_t header_rule_y = (c.height() == 600) ? 49 : static_cast<int16_t>((c.height() * 49 + 300) / 600);
    const int16_t header_baseline = (c.height() == 600) ? 32 : static_cast<int16_t>((c.height() * 32 + 300) / 600);
    const int16_t icon_y = static_cast<int16_t>(header_baseline - 13);
    const int16_t wifi_y = static_cast<int16_t>(header_baseline - 16);

    char date_buf[48];
    std::snprintf(date_buf, sizeof(date_buf), "%s · %d %s", i18n::weekday(now_civil.weekday),
                  now_civil.day, i18n::month(now_civil.month - 1));
    draw_text(c, fonts::Terminus24, margin, header_baseline, date_buf, Color::Black, 1, /*bold=*/true);

    if (show_setup_url && d.setup_url.length() > 0) {
        int16_t url_w = text_width(fonts::Terminus14, d.setup_url.c_str());
        int16_t url_x = static_cast<int16_t>((c.width() - url_w) / 2);
        draw_text(c, fonts::Terminus14, url_x, static_cast<int16_t>(header_baseline + 6), d.setup_url.c_str(), Color::Black, 1, false);
    }

    char clock_buf[24];
    if (d.next_update_at > 0) {
        Civil next_civil = to_civil(d.next_update_at, d.timezone_minutes);
        std::snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d → %02d:%02d", now_civil.hour,
                      now_civil.minute, next_civil.hour, next_civil.minute);
    } else {
        std::snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", now_civil.hour, now_civil.minute);
    }

    int16_t x = static_cast<int16_t>(c.width() - margin);
    int16_t clock_w = text_width(fonts::Terminus24, clock_buf);
    x = static_cast<int16_t>(x - clock_w);
    draw_text(c, fonts::Terminus24, x, header_baseline, clock_buf, Color::Black, 1, /*bold=*/true);
    x = static_cast<int16_t>(x - 16);

    if (d.battery_pct >= 0) {
        char batt_buf[8];
        std::snprintf(batt_buf, sizeof(batt_buf), "%d%%", d.battery_pct);
        int16_t w = text_width(fonts::Terminus20, batt_buf);
        x = static_cast<int16_t>(x - w);
        draw_text(c, fonts::Terminus20, x, header_baseline, batt_buf, Color::Black, 1, /*bold=*/true);
        x = static_cast<int16_t>(x - 6 - 24);
        draw_battery_icon(c, x, icon_y, d.battery_pct);
    } else {
        const char* usb = "USB";
        int16_t w = text_width(fonts::Terminus20, usb);
        x = static_cast<int16_t>(x - w);
        draw_text(c, fonts::Terminus20, x, header_baseline, usb, Color::Black, 1, /*bold=*/true);
    }
    x = static_cast<int16_t>(x - 16 - 22);
    draw_wifi_bars(c, x, wifi_y, wifi_bars(d.wifi_rssi));

    c.fill_rect(margin, header_rule_y, static_cast<int16_t>(c.width() - 2 * margin), 2,
                Color::Black);
}

}  // namespace

String format_decimal(float value, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(value));
    // Десятичный разделитель — по языку экрана: «96,40» для ru, «96.40» для en.
    const char point = i18n::lang() == i18n::Lang::kRu ? ',' : '.';
    for (char* p = buf; *p; ++p) {
        if (*p == '.') *p = point;
    }

    // Разряды целой части разделяем пробелом: «80 689» вместо «80689». На
    // пятизначном курсе без разделителя читатель считает нули глазами, а
    // эталон эту группировку делает. Работаем с буфером, а не со String:
    // хостовый шим не умеет индексацию, а вести две ветки ради форматирования
    // числа — лишнее.
    int int_end = 0;
    while (buf[int_end] != '\0' && buf[int_end] != point) ++int_end;
    const int digits_start = (buf[0] == '-' || buf[0] == '+') ? 1 : 0;
    if (int_end - digits_start <= 4) return String(buf);  // до четырёх цифр группировка мешает

    char out[40];
    int w = 0;
    for (int i = 0; i < int_end && w < static_cast<int>(sizeof(out)) - 2; ++i) {
        if (i > digits_start && (int_end - i) % 3 == 0) out[w++] = ' ';
        out[w++] = buf[i];
    }
    for (int i = int_end; buf[i] != '\0' && w < static_cast<int>(sizeof(out)) - 1; ++i) {
        out[w++] = buf[i];
    }
    out[w] = '\0';
    return String(out);
}


int8_t wifi_bars(int32_t rssi_dbm) {
    if (rssi_dbm == 0) return 0;  // сентинел DeviceInfo::wifi_rssi — «не в сети»
    if (rssi_dbm >= -55) return 4;
    if (rssi_dbm >= -65) return 3;
    if (rssi_dbm >= -75) return 2;
    return 1;
}

uint8_t segments_for_width(int16_t w) {
    constexpr int16_t kMinSegWidth = 10;  // с зазором в 3px, см. draw_segbar
    int16_t fit = static_cast<int16_t>(w / kMinSegWidth);
    if (fit < 4) return 4;
    if (fit > 20) return 20;
    return static_cast<uint8_t>(fit);
}

Civil to_civil(uint32_t unix_time, int16_t timezone_minutes) {
    int64_t local = static_cast<int64_t>(unix_time) + static_cast<int64_t>(timezone_minutes) * 60;
    int64_t days = floor_div(local, 86400);
    int64_t secs_of_day = floor_mod(local, 86400);

    int year;
    unsigned month, day;
    civil_from_days(days, &year, &month, &day);

    Civil c;
    c.day = static_cast<int>(day);
    c.month = static_cast<int>(month);
    // 1970-01-01 (days == 0) — четверг, индекс 3 в WEEKDAYS (Пн=0).
    c.weekday = static_cast<int>(floor_mod(days + 3, 7));
    c.hour = static_cast<int>(secs_of_day / 3600);
    c.minute = static_cast<int>((secs_of_day % 3600) / 60);
    return c;
}

bool has_data(const Slot* s) { return s != nullptr && !s->empty(); }

String format_percent(const Slot* s, uint32_t now) {
    if (!has_data(s)) return String("—");
    int pct = static_cast<int>(s->number + (s->number >= 0 ? 0.5f : -0.5f));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char buf[16];
    if (s->stale(now)) {
        std::snprintf(buf, sizeof(buf), "≈%d%%", pct);
    } else {
        std::snprintf(buf, sizeof(buf), "%d%%", pct);
    }
    return String(buf);
}

String format_value(const Slot* s, uint32_t now, int decimals, const char* suffix) {
    if (!has_data(s)) return String("—");
    String num = format_decimal(s->number, decimals);
    char buf[48];
    if (s->stale(now)) {
        std::snprintf(buf, sizeof(buf), "≈%s%s", num.c_str(), suffix);
    } else {
        std::snprintf(buf, sizeof(buf), "%s%s", num.c_str(), suffix);
    }
    return String(buf);
}

void compute_columns(Rect row, int16_t gap, const bool* visible, uint8_t total, Rect* out) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < total; ++i) {
        if (visible[i]) ++count;
    }
    if (count == 0) {
        for (uint8_t i = 0; i < total; ++i) out[i] = Rect{};
        return;
    }
    int16_t total_gap = static_cast<int16_t>(gap * (count - 1));
    int16_t col_w = static_cast<int16_t>((row.w - total_gap) / count);
    int16_t cursor = row.x;
    for (uint8_t i = 0; i < total; ++i) {
        if (!visible[i]) {
            out[i] = Rect{};
            continue;
        }
        out[i] = Rect{cursor, row.y, col_w, row.h};
        cursor = static_cast<int16_t>(cursor + col_w + gap);
    }
}

void compute_columns_fixed_first(Rect row, int16_t gap, bool first_visible, bool second_visible,
                                  int16_t first_width, Rect* first, Rect* second) {
    if (first_visible && second_visible) {
        *first = Rect{row.x, row.y, first_width, row.h};
        int16_t second_x = static_cast<int16_t>(row.x + first_width + gap);
        *second = Rect{second_x, row.y, static_cast<int16_t>(row.x + row.w - second_x), row.h};
    } else if (first_visible) {
        *first = row;
        *second = Rect{};
    } else if (second_visible) {
        *second = row;
        *first = Rect{};
    } else {
        *first = Rect{};
        *second = Rect{};
    }
}

void compute_columns_fixed_second(Rect row, int16_t gap, bool first_visible, bool second_visible,
                                   int16_t second_width, Rect* first, Rect* second) {
    if (first_visible && second_visible) {
        int16_t second_x = static_cast<int16_t>(row.x + row.w - second_width);
        *second = Rect{second_x, row.y, second_width, row.h};
        *first = Rect{row.x, row.y, static_cast<int16_t>(second_x - row.x - gap), row.h};
    } else if (first_visible) {
        *first = row;
        *second = Rect{};
    } else if (second_visible) {
        *second = row;
        *first = Rect{};
    } else {
        *first = Rect{};
        *second = Rect{};
    }
}

void compute_two_rows(Rect body, int16_t gap, bool row1_has, bool row2_has, Rect* row1,
                      Rect* row2, bool expand_top = false) {
    if (row1_has && row2_has) {
        int16_t total = static_cast<int16_t>(body.h - gap);
        int16_t h1;
        if (body.h <= 420) {
            // Оригинальное соотношение 800x480 (TRMNL 7.5" от Pavel Lunev) — 235px верхний ряд
            h1 = static_cast<int16_t>(total * 0.603f);
        } else if (expand_top) {
            // PocketBook 800x600 при наличии 3 агентов И климата — расширяем верхний ряд до 328px,
            // давая плашкам СВЕЖО/НОРМА аккуратный отступ от разделителя рядов
            h1 = static_cast<int16_t>((total * 328 + 255) / 510);
        } else {
            // PocketBook 800x600 — освобождаем место под 5 писем в нижнем ряду (~51%)
            h1 = static_cast<int16_t>((total * 250 + 255) / 510);
        }
        int16_t h2 = static_cast<int16_t>(total - h1);
        *row1 = Rect{body.x, body.y, body.w, h1};
        *row2 = Rect{body.x, static_cast<int16_t>(body.y + h1 + gap), body.w, h2};
    } else if (row1_has) {
        *row1 = body;
        *row2 = Rect{};
    } else if (row2_has) {
        *row1 = Rect{};
        *row2 = body;
    } else {
        *row1 = Rect{};
        *row2 = Rect{};
    }
}

void layout_row(Rect row, int16_t gap, const std::vector<widgets::Instance>& items,
                const std::vector<bool>& visible, Rect* out) {
    const size_t n = items.size();
    for (size_t i = 0; i < n; ++i) out[i] = Rect{};

    size_t visible_count = 0;
    size_t flex_count = 0;
    int16_t fixed_sum = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!visible[i]) continue;
        ++visible_count;
        if (items[i].size == widgets::Size::kFlex) {
            ++flex_count;
        } else {
            fixed_sum = static_cast<int16_t>(fixed_sum + widgets::size_px(items[i].size, row.w));
        }
    }
    if (visible_count == 0) return;

    const int16_t gap_total = static_cast<int16_t>(gap * static_cast<int16_t>(visible_count - 1));

    // Нет ни одного flex среди видимых — видимые делят весь ряд поровну
    // (docs/widgets.md, «Правило ряда»). При visible_count==1 это отдаёт всю
    // ширину единственному видимому — так Рынки в эталоне занимают весь
    // верхний ряд, когда правая колонка пропала.
    // Переполнение: фиксированные ширины (плюс минимум на каждый flex) не
    // влезают в ряд — например три M по 296 в 770 px. Без этой ветки flex
    // получал бы отрицательную ширину, а хвост ряда уезжал за край панели
    // молча. Делим поровну между видимыми — кадр остаётся в границах, а
    // владелец видит на предпросмотре и на панели одно и то же.
    const int16_t min_flex = static_cast<int16_t>((row.w * 120 + 385) / 770);
    const bool overflow =
        fixed_sum + gap_total + static_cast<int16_t>(flex_count) * min_flex > row.w;
    const bool equal_split = flex_count == 0 || overflow;

    int16_t equal_w = 0;
    int16_t flex_w = 0;
    if (equal_split) {
        equal_w = static_cast<int16_t>((row.w - gap_total) / static_cast<int16_t>(visible_count));
    } else {
        const int16_t remaining = static_cast<int16_t>(row.w - fixed_sum - gap_total);
        flex_w = static_cast<int16_t>(remaining / static_cast<int16_t>(flex_count));
    }

    int16_t cursor = row.x;
    for (size_t i = 0; i < n; ++i) {
        if (!visible[i]) continue;
        const int16_t w = equal_split ? equal_w
                          : items[i].size == widgets::Size::kFlex
                              ? flex_w
                              : widgets::size_px(items[i].size, row.w);
        out[i] = Rect{cursor, row.y, w, row.h};
        cursor = static_cast<int16_t>(cursor + w + gap);
    }
}

void draw_frame(Canvas& canvas, const Store& store, const DeviceInfo& device,
                const config::Dashboard& dashboard) {
    canvas.fill(Color::White);
    draw_header(canvas, device);

    const int16_t margin = (canvas.width() == 800) ? 15 : static_cast<int16_t>((canvas.width() * 15 + 400) / 800);
    const int16_t body_top = (canvas.height() == 600) ? 63 : static_cast<int16_t>((canvas.height() * 63 + 300) / 600);
    const int16_t row_gap = (canvas.height() == 600) ? 12 : static_cast<int16_t>((canvas.height() * 12 + 300) / 600);
    const int16_t top_gap = (canvas.width() == 800) ? 19 : static_cast<int16_t>((canvas.width() * 19 + 400) / 800);
    const int16_t bottom_gap = (canvas.width() == 800) ? 17 : static_cast<int16_t>((canvas.width() * 17 + 400) / 800);

    Rect body{margin, body_top, static_cast<int16_t>(canvas.width() - 2 * margin),
              static_cast<int16_t>(canvas.height() - body_top - margin)};

    // Видимость каждого инстанса — по его widgets::Spec::visible: раскладка
    // не знает имён слотов, только у кого спросить.
    std::vector<bool> visible[2];
    bool row_has[2] = {false, false};
    for (int r = 0; r < 2; ++r) {
        const std::vector<widgets::Instance>& items = dashboard.rows[r];
        visible[r].assign(items.size(), false);
        for (size_t i = 0; i < items.size(); ++i) {
            const widgets::Spec* spec = widgets::find(items[i].type.c_str());
            const bool v = spec != nullptr && spec->visible(store, items[i]);
            visible[r][i] = v;
            if (v) row_has[r] = true;
        }
    }

    bool has_any_data = false;
    for (const auto& kv : store.all()) {
        const char* key = kv.first.c_str();
        const size_t len = kv.first.length();
        const bool is_status = (len >= 7 && std::strcmp(key + len - 7, ".status") == 0);
        if (!is_status && !kv.second.empty()) {
            has_any_data = true;
            break;
        }
    }

    if (!has_any_data || (!row_has[0] && !row_has[1])) {
        if (device.setup_url.length() > 0) {
            draw_setup(canvas, device, device.setup_url);
            return;
        }
        // Устройство настроено, но данных пока нет
        const bool is_ru = (i18n::lang() == i18n::Lang::kRu);
        const char* msg = (device.wifi_rssi == 0)
                              ? (is_ru ? "Подключите PocketBook к сети Wi-Fi" : "Connect PocketBook to Wi-Fi network")
                              : (is_ru ? "Подключение к сети · Ожидание данных..." : "Connecting to network · Waiting for data...");
        int16_t mw = text_width(fonts::Terminus20, msg);
        int16_t mx = static_cast<int16_t>((canvas.width() - mw) / 2);
        int16_t my = static_cast<int16_t>(body.y + body.h / 2);
        draw_text(canvas, fonts::Terminus20, mx, my, msg, Color::Black, 1, /*bold=*/true);
        if (device.wifi_rssi == 0) {
            const char* sub = is_ru ? "Включите Wi-Fi в настройках PocketBook и выберите сеть"
                                    : "Enable Wi-Fi in PocketBook Settings and join a network";
            int16_t sw = text_width(fonts::Terminus16, sub);
            draw_text(canvas, fonts::Terminus16, static_cast<int16_t>((canvas.width() - sw) / 2),
                      static_cast<int16_t>(my + 28), sub, Color::Black);
        }
        return;
    }

    bool has_claude = layout::has_data(store.find("limit.claude.5h")) || layout::has_data(store.find("limit.claude.week"));
    bool has_codex = layout::has_data(store.find("limit.codex"));
    bool has_antigravity = layout::has_data(store.find("limit.antigravity.5h")) || layout::has_data(store.find("limit.antigravity.week"));
    bool has_air = layout::has_data(store.find("co2")) || layout::has_data(store.find("tvoc")) ||
                   layout::has_data(store.find("temp")) || layout::has_data(store.find("hum")) ||
                   layout::has_data(store.find("temperature")) || layout::has_data(store.find("humidity"));
    bool expand_top = (has_claude && has_codex && has_antigravity && has_air);

    Rect row1, row2;
    compute_two_rows(body, row_gap, row_has[0], row_has[1], &row1, &row2, expand_top);

    if (row_has[0] && row_has[1]) {
        // Разделитель садится на границу рядов, а не в середину зазора
        const int16_t rule_w = static_cast<int16_t>(canvas.width() - 2 * margin);
        canvas.hline(margin, static_cast<int16_t>(row1.y + row1.h), rule_w, Color::Black);
        canvas.hline(margin, static_cast<int16_t>(row1.y + row1.h + 1), rule_w, Color::Black);
    }

    const int16_t row_gap_pair[2] = {top_gap, bottom_gap};
    const Rect rows[2] = {row1, row2};
    for (int r = 0; r < 2; ++r) {
        if (!row_has[r]) continue;
        const std::vector<widgets::Instance>& items = dashboard.rows[r];
        std::vector<Rect> rects(items.size());
        layout_row(rows[r], row_gap_pair[r], items, visible[r], rects.data());

        bool drawn_any = false;
        for (size_t i = 0; i < items.size(); ++i) {
            if (!visible[r][i]) continue;
            const widgets::Instance& item = items[i];
            if (item.divider && drawn_any) {
                // Вертикальная линия 2px в зазоре слева от виджета — тот же
                // приём, что раньше был жёстко зашит для Сегодня (два
                // соседних vline вместо толщины линии в canvas).
                const int16_t x2 = static_cast<int16_t>(rects[i].x - row_gap_pair[r] + 2);
                const int16_t x1 = static_cast<int16_t>(rects[i].x - row_gap_pair[r] + 1);
                canvas.vline(x2, rows[r].y, rows[r].h, Color::Black);
                canvas.vline(x1, rows[r].y, rows[r].h, Color::Black);
            }
            const widgets::Spec* spec = widgets::find(item.type.c_str());
            if (spec != nullptr) spec->draw(canvas, store, device, rects[i], item);
            drawn_any = true;
        }
    }
}

namespace {

// ── экран учётных данных точки доступа (docs/decisions.md, п.8) ──
//
// Раскладка: слева текстом сеть и пароль (камера может не сработать — тогда
// вводят руками), справа — QR той же информации в формате WIFI:. Разнесены
// зазором AP_QR_GAP, а не поставлены впритык, — иначе QR читается как
// приклеенный сбоку довесок, а не равноправная часть кадра.
constexpr int16_t AP_TITLE_Y = 34;          // базовая линия заголовка, как у даты в шапке (draw_header)
constexpr int16_t AP_BODY_TOP = 82;         // под линией под заголовком
constexpr uint8_t AP_QR_MODULE_PX = 6;      // ≥4-5px на модуль — читается с руки на 800x480
constexpr uint8_t AP_QR_QUIET_MODULES = 4;  // обязательная светлая рамка по спецификации QR
constexpr int16_t AP_QR_GAP = 30;

}  // namespace

void draw_boot(Canvas& canvas, const char* status) {
    canvas.fill(Color::White);
    const int16_t cx = static_cast<int16_t>(canvas.width() / 2);

    // Угловые метки — тот же 2px штрих, что у линий кадра: экран включения
    // должен выглядеть частью того же прибора, а не заставкой другой программы.
    constexpr int16_t kTick = 24;
    const int16_t right = static_cast<int16_t>(canvas.width() - MARGIN);
    const int16_t bottom = static_cast<int16_t>(canvas.height() - MARGIN);
    canvas.fill_rect(MARGIN, MARGIN, kTick, 2, Color::Black);
    canvas.fill_rect(MARGIN, MARGIN, 2, kTick, Color::Black);
    canvas.fill_rect(static_cast<int16_t>(right - kTick), MARGIN, kTick, 2, Color::Black);
    canvas.fill_rect(static_cast<int16_t>(right - 2), MARGIN, 2, kTick, Color::Black);
    canvas.fill_rect(MARGIN, static_cast<int16_t>(bottom - 2), kTick, 2, Color::Black);
    canvas.fill_rect(MARGIN, static_cast<int16_t>(bottom - kTick), 2, kTick, Color::Black);
    canvas.fill_rect(static_cast<int16_t>(right - kTick), static_cast<int16_t>(bottom - 2), kTick, 2,
                     Color::Black);
    canvas.fill_rect(static_cast<int16_t>(right - 2), static_cast<int16_t>(bottom - kTick), 2, kTick,
                     Color::Black);

    // Имя — Terminus24 при scale=2, как пароль точки доступа: у PlexMono букв
    // нет (только цифры и знаки), а второго крупного шрифта с латиницей мы
    // намеренно не заводили (font.h).
    const char* name = "pb-tablo";
    const int16_t name_w = static_cast<int16_t>(text_width(fonts::Terminus24, name) * 2);
    draw_text(canvas, fonts::Terminus24, static_cast<int16_t>(cx - name_w / 2), 228, name,
              Color::Black, 2, /*bold=*/true);

    // Короткая линия под именем — как правило под шапкой кадра, но по ширине
    // слова, не экрана: это подпись к имени, а не разделитель рядов.
    canvas.fill_rect(static_cast<int16_t>(cx - name_w / 2), 246, name_w, 2, Color::Black);

    const char* tagline = i18n::tr(i18n::Str::kTagline);
    const int16_t tag_w = text_width(fonts::Terminus14, tagline);
    draw_text(canvas, fonts::Terminus14, static_cast<int16_t>(cx - tag_w / 2), 276, tagline,
              Color::Black, 1, /*bold=*/true);

    // Состояние — внизу слева, где в рабочем кадре стоит почта: глаз уже
    // приучен искать «что сейчас происходит» там.
    draw_text(canvas, fonts::Terminus14, static_cast<int16_t>(MARGIN + 12),
              static_cast<int16_t>(bottom - 12), status, Color::Black, 1, false);
    const char* hw = "TRMNL 7.5\" · XIAO ESP32-S3";
    const int16_t hw_w = text_width(fonts::Terminus14, hw);
    draw_text(canvas, fonts::Terminus14, static_cast<int16_t>(right - 12 - hw_w),
              static_cast<int16_t>(bottom - 12), hw, Color::Black, 1, false);
}

void draw_ap_credentials(Canvas& canvas, const String& ssid, const String& password) {
    canvas.fill(Color::White);

    draw_text(canvas, fonts::Terminus24, MARGIN, AP_TITLE_Y, i18n::tr(i18n::Str::kApSetup), Color::Black, 1,
              /*bold=*/true);
    canvas.fill_rect(MARGIN, HEADER_RULE_Y, static_cast<int16_t>(canvas.width() - 2 * MARGIN), 2,
                     Color::Black);

    int16_t body_bottom = static_cast<int16_t>(canvas.height() - MARGIN);
    int16_t body_h = static_cast<int16_t>(body_bottom - AP_BODY_TOP);

    int16_t qr_side = wifi_qr::side_for(AP_QR_MODULE_PX, AP_QR_QUIET_MODULES);
    int16_t qr_x = static_cast<int16_t>(canvas.width() - MARGIN - qr_side);
    int16_t qr_y = static_cast<int16_t>(AP_BODY_TOP + (body_h - qr_side) / 2);

    String payload = wifi_qr::payload(ssid, password);
    // draw() возвращает 0, если payload не поместился в фиксированную версию
    // QR (wifi_qr.h) — молча не рисуем код, а не роняем весь кадр: имя сети и
    // пароль текстом ниже остаются рабочим способом подключиться руками.
    wifi_qr::draw(canvas, qr_x, qr_y, payload, AP_QR_MODULE_PX, AP_QR_QUIET_MODULES);

    // Текстовая колонка слева от QR — та же ширина, что осталась после него
    // и зазора, а не фиксированное число: так верстка не разъедется, если
    // геометрию QR однажды придётся поменять.
    int16_t text_w = static_cast<int16_t>(qr_x - AP_QR_GAP - MARGIN);

    int16_t y = static_cast<int16_t>(AP_BODY_TOP + (body_h - 260) / 2);  // 260 — высота блока текста ниже

    // Terminus, не PlexMono: у PlexMono в наборе только цифры и пунктуация
    // (используется для курсов и процентов) — букв SSID/пароля в нём просто
    // нет, глиф молча не рисуется (font.h: find_glyph возвращает nullptr, а
    // draw_text пропускает символ). Terminus покрывает весь ASCII.
    widgets::prims::draw_eyebrow(canvas, Rect{MARGIN, y, text_w, 0}, i18n::tr(i18n::Str::kNetwork));
    y = static_cast<int16_t>(y + widgets::prims::kEyebrowTextOffset + 34);
    draw_text(canvas, fonts::Terminus24, MARGIN, y,
              widgets::prims::truncate_to_width(fonts::Terminus24, ssid.c_str(), text_w).c_str(),
              Color::Black, 1, /*bold=*/true);

    y = static_cast<int16_t>(y + 50);
    widgets::prims::draw_eyebrow(canvas, Rect{MARGIN, y, text_w, 0}, i18n::tr(i18n::Str::kPassword));
    y = static_cast<int16_t>(y + widgets::prims::kEyebrowTextOffset + 56);
    // Terminus24 при scale=2 — тот же приём, что курс BTC в блоке Рынков
    // (см. font.h про kern 48 без пятого файла шрифта): пароль должен быть
    // зрительно доминирующим, чтобы его можно было перепечатать со стола без
    // наклона к панели. Алфавит пароля (config.cpp) фиксирован по длине (10
    // символов) — можно рисовать без truncate_to_width, переполнения не
    // бывает даже с запасом по ширине колонки.
    draw_text(canvas, fonts::Terminus24, MARGIN, y, password.c_str(), Color::Black, 2,
              /*bold=*/true);

    y = static_cast<int16_t>(y + 60);
    draw_text(canvas, fonts::Terminus14, MARGIN, y,
              i18n::tr(i18n::Str::kApHint1), Color::Black, 1, false);
    y = static_cast<int16_t>(y + 22);
    draw_text(canvas, fonts::Terminus14, MARGIN, y,
              i18n::tr(i18n::Str::kApHint2), Color::Black, 1, false);
}

void draw_setup(Canvas& canvas, const DeviceInfo& device, const String& setup_url) {
    canvas.fill(Color::White);
    draw_header(canvas, device, /*show_setup_url=*/false);

    const bool is_ru = (i18n::lang() == i18n::Lang::kRu);

    if (device.wifi_rssi == 0) {
        // Устройство не подключено к Wi-Fi
        const char* title = is_ru ? "Подключите PocketBook к сети Wi-Fi"
                                  : "Connect PocketBook to Wi-Fi network";
        const int16_t tw = text_width(fonts::Terminus24, title);
        const int16_t tx = static_cast<int16_t>((canvas.width() - tw) / 2);
        draw_text(canvas, fonts::Terminus24, tx, 250, title, Color::Black, 1, /*bold=*/true);
        draw_text(canvas, fonts::Terminus24, tx, 251, title, Color::Black, 1, /*bold=*/true);

        const char* sub1 = is_ru ? "Включите Wi-Fi в настройках PocketBook и выберите сеть."
                                 : "Enable Wi-Fi in PocketBook Settings and join a network.";
        const int16_t s1w = text_width(fonts::Terminus16, sub1);
        draw_text(canvas, fonts::Terminus16, static_cast<int16_t>((canvas.width() - s1w) / 2), 300,
                  sub1, Color::Black, 1, false);

        const char* sub2 = is_ru ? "После подключения здесь появится адрес настройки."
                                 : "Once connected, the setup address will appear here.";
        const int16_t s2w = text_width(fonts::Terminus16, sub2);
        draw_text(canvas, fonts::Terminus16, static_cast<int16_t>((canvas.width() - s2w) / 2), 330,
                  sub2, Color::Black, 1, false);
        return;
    }

    String url = setup_url.length() > 0 ? setup_url : String("http://pb-tablo.local:8080");

    // Для визуального отображения убираем http://
    std::string display_url(url.c_str());
    if (display_url.rfind("http://", 0) == 0) {
        display_url = display_url.substr(7);
    } else if (display_url.rfind("https://", 0) == 0) {
        display_url = display_url.substr(8);
    }

    // 1. Сверху по центру адрес без http://, двойной жирный штрих (Terminus 24)
    const int16_t url_w = text_width(fonts::Terminus24, display_url.c_str());
    const int16_t url_x = static_cast<int16_t>((canvas.width() - url_w) / 2);
    const int16_t url_y = 105;
    draw_text(canvas, fonts::Terminus24, url_x, url_y, display_url.c_str(), Color::Black, 1, /*bold=*/true);
    draw_text(canvas, fonts::Terminus24, url_x, static_cast<int16_t>(url_y + 1), display_url.c_str(), Color::Black, 1, /*bold=*/true);

    // 2. Ниже по центру большой QR-код (QR кодирует полный url с http:// для телефонов)
    constexpr uint8_t kModulePx = 7;
    constexpr uint8_t kQuietModules = 4;
    const int16_t qr_side = wifi_qr::side_for(kModulePx, kQuietModules);  // 343px
    const int16_t qr_x = static_cast<int16_t>((canvas.width() - qr_side) / 2);
    const int16_t qr_y = 150;

    wifi_qr::draw(canvas, qr_x, qr_y, url, kModulePx, kQuietModules);

    // 3. Внизу экрана пояснение
    const char* hint = is_ru ? "Откройте адрес в браузере или отсканируйте QR-код"
                             : "Open the address in your browser or scan the QR code";
    const int16_t hint_w = text_width(fonts::Terminus16, hint);
    const int16_t hint_x = static_cast<int16_t>((canvas.width() - hint_w) / 2);
    const int16_t hint_y = static_cast<int16_t>(canvas.height() - 40);
    draw_text(canvas, fonts::Terminus16, hint_x, hint_y, hint, Color::Black, 1, /*bold=*/true);
}

}  // namespace layout
