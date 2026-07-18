#include "display/views/settings_view.h"
#include "display/common/airplane_icon.h"
#include "display/common/lvgl_utils.h"
#include "display/common/signal_bars.h"
#include "display/core/app_icons.h"
#include "display/theme/terminal_theme.h"
#include "display/views/terminal_view.h"
#include "fonts.h"
#include "display/theme/ui_theme.h"
#include "lvgl_runtime.h"
#include "net/airplane_mode.h"
#include "net/vpn_manager.h"
#include "platform/fan_control.h"
#include "platform/shell_command.h"
#include "settings.h"
#include "esp_log.h"

#include <lvgl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

#define TAG "SettingsView"

namespace home {

using jetson::ui::Color;
using LvLockGuard = jetson::ui::LvglLockGuard;

namespace {
// Run `cmd` via /bin/sh, capture combined stdout+stderr (trimmed).
std::string RunCapture(const std::string &cmd) {
    auto result = jetson::platform::RunShellCommand(cmd);
    jetson::platform::TrimTrailingWhitespace(result.output);
    return std::move(result.output);
}

// Sidebar glyphs (FONT_AWESOME / LV_SYMBOL).
struct SleepOpt { int seconds; const char *label; };
const SleepOpt kSleepOpts[] = {
    {30, "30 giây"}, {60, "1 phút"}, {120, "2 phút"}, {180, "3 phút"},
    {240, "4 phút"}, {300, "5 phút"}, {0, "Không"},
};
constexpr int kFontSizes[] = {22, 24, 26, 28, 30, 32, 34};
constexpr int kDefaultFontSize = 26;

int FontStepForSize(int size) {
    int best = 0;
    for (int i = 1; i < (int)(sizeof(kFontSizes) / sizeof(kFontSizes[0])); ++i) {
        if (std::abs(kFontSizes[i] - size) < std::abs(kFontSizes[best] - size)) best = i;
    }
    return best;
}

const char *SleepLabel(int seconds) {
    for (const auto &option : kSleepOpts)
        if (option.seconds == seconds) return option.label;
    return "Không";
}
struct TimezoneOpt { const char *id; const char *label; };
const TimezoneOpt kTimezones[] = {
    {"Asia/Ho_Chi_Minh", "Hà Nội / TP. Hồ Chí Minh"},
    {"Asia/Bangkok", "Bangkok"}, {"Asia/Tokyo", "Tokyo"},
    {"Asia/Shanghai", "Thượng Hải"}, {"Asia/Singapore", "Singapore"},
    {"Asia/Hong_Kong", "Hồng Kông"}, {"Asia/Kolkata", "Kolkata"},
    {"Asia/Dubai", "Dubai"}, {"Europe/London", "London"},
    {"Europe/Paris", "Paris"}, {"Europe/Berlin", "Berlin"},
    {"Europe/Moscow", "Moscow"}, {"America/New_York", "New York"},
    {"America/Chicago", "Chicago"}, {"America/Los_Angeles", "Los Angeles"},
    {"America/Sao_Paulo", "São Paulo"}, {"Australia/Sydney", "Sydney"},
    {"UTC", "UTC"},
};

const char *TimezoneLabel(const std::string &id) {
    for (const auto &option : kTimezones) if (id == option.id) return option.label;
    return id.c_str();
}

struct LanguageOpt {
    const char *code;
    const char *title;
    const char *subtitle;
    const char *input_lang;
};

const LanguageOpt kLanguages[] = {
    {"vi-VN", "Tiếng Việt", "Việt Nam", "vi"},
    {"en-GB", "English (UK)", "Tiếng Anh (Vương quốc Anh)", "en"},
    {"en-AU", "English (Australia)", "Tiếng Anh (Úc)", "en"},
    {"en-IN", "English (India)", "Tiếng Anh (Ấn Độ)", "en"},
    {"zh-TW", "繁體中文", "Tiếng Trung (Phồn thể)", "en"},
    {"zh-HK", "繁體中文 (香港)", "Tiếng Trung (Hồng Kông)", "en"},
    {"ja-JP", "日本語", "Tiếng Nhật", "en"},
    {"es-ES", "Español", "Tiếng Tây Ban Nha", "en"},
    {"es-US", "Español (EE. UU.)", "Tiếng Tây Ban Nha (Mỹ)", "en"},
    {"es-419", "Español (Latinoamérica)", "Tiếng Tây Ban Nha (Châu Mỹ La-tinh)", "en"},
    {"fr-FR", "Français", "Tiếng Pháp", "en"},
};

struct RegionOpt { const char *code; const char *name; };
const RegionOpt kRegions[] = {
    {"VN", "Việt Nam"}, {"AU", "Úc"}, {"CN", "Trung Quốc"},
    {"HK", "Hồng Kông"}, {"IN", "Ấn Độ"}, {"JP", "Nhật Bản"},
    {"SG", "Singapore"}, {"ES", "Tây Ban Nha"}, {"FR", "Pháp"},
    {"UZ", "Uzbekistan"}, {"VU", "Vanuatu"}, {"VE", "Venezuela"},
    {"GB", "Vương quốc Anh"}, {"US", "Hoa Kỳ"},
    {"WF", "Wallis và Futuna"}, {"YE", "Yemen"},
    {"ZM", "Zambia"}, {"ZW", "Zimbabwe"},
};

struct CalendarOpt { const char *code; const char *name; };
const CalendarOpt kCalendars[] = {
    {"gregorian", "Lịch Gregory"},
    {"japanese", "Lịch Nhật Bản"},
    {"buddhist", "Lịch Phật giáo"},
};

struct CloudFontEntry {
    std::string name;
    std::string regular_object;
    std::string bold_object;
};

struct LocalFontFamily {
    std::string name;
    std::string regular_path;
    std::string bold_path;
};

bool FileExistsLocal(const std::string &path) {
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string JoinPath(const std::string &left, const std::string &right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    if (left.back() == '/') return left + right;
    return left + "/" + right;
}

std::string HomeDir() {
    const char *home = std::getenv("HOME");
    if (home && home[0]) return home;
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : "/root";
}

bool HasTtfExtension(const std::string &name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".ttf";
}

std::string FontStemFromPath(const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (HasTtfExtension(name)) name.resize(name.size() - 4);
    for (char &c : name) if (c == '_' || c == '-') c = ' ';
    return name;
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

std::string HumanizeFontName(const std::string &value) {
    std::string result;
    result.reserve(value.size() + 4);
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char current = (unsigned char)value[i];
        const unsigned char previous = i ? (unsigned char)value[i - 1] : 0;
        if (i && value[i - 1] != ' ' &&
            ((std::isupper(current) && std::islower(previous)) ||
             (std::isdigit(current) && !std::isdigit(previous)))) {
            result.push_back(' ');
        }
        result.push_back(value[i]);
    }
    return result;
}

void CollectTtfFiles(const std::string &dir, bool skip_cloud,
                     std::vector<std::string> *files) {
    DIR *handle = ::opendir(dir.c_str());
    if (!handle) return;
    while (dirent *entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const std::string path = JoinPath(dir, name);
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!skip_cloud || LowerAscii(name) != "cloud")
                CollectTtfFiles(path, skip_cloud, files);
        } else if (S_ISREG(st.st_mode) && HasTtfExtension(name)) {
            files->push_back(path);
        }
    }
    ::closedir(handle);
}

std::vector<LocalFontFamily> ListFontFamilies(const std::string &dir,
                                              bool skip_cloud) {
    std::vector<std::string> files;
    CollectTtfFiles(dir, skip_cloud, &files);
    std::sort(files.begin(), files.end());

    std::map<std::string, LocalFontFamily> grouped;
    for (const auto &path : files) {
        std::string name = FontStemFromPath(path);
        std::string lower = LowerAscii(name);
        bool bold = false;
        constexpr const char *kRegularSuffix = " regular";
        constexpr const char *kBoldSuffix = " bold";
        if (lower.size() > std::strlen(kRegularSuffix) &&
            lower.compare(lower.size() - std::strlen(kRegularSuffix),
                          std::strlen(kRegularSuffix), kRegularSuffix) == 0) {
            name.resize(name.size() - std::strlen(kRegularSuffix));
        } else if (lower.size() > std::strlen(kBoldSuffix) &&
                   lower.compare(lower.size() - std::strlen(kBoldSuffix),
                                 std::strlen(kBoldSuffix), kBoldSuffix) == 0) {
            name.resize(name.size() - std::strlen(kBoldSuffix));
            bold = true;
        }
        name = HumanizeFontName(name);

        const std::string key = LowerAscii(name);
        auto &family = grouped[key];
        if (family.name.empty()) family.name = name;
        if (bold) {
            if (family.bold_path.empty()) family.bold_path = path;
        } else if (family.regular_path.empty()) {
            family.regular_path = path;
        }
    }

    std::vector<LocalFontFamily> families;
    families.reserve(grouped.size());
    for (auto &item : grouped) {
        auto &family = item.second;
        if (family.regular_path.empty()) {
            family.regular_path = family.bold_path;
            family.bold_path.clear();
        }
        if (!family.regular_path.empty()) families.push_back(std::move(family));
    }
    return families;
}

bool SafeAssetObject(const std::string &object) {
    return !object.empty() && object.front() != '/' && object.find('\\') == std::string::npos &&
           object.find("..") == std::string::npos;
}

std::string CloudCatalogObject() {
    const char *configured = std::getenv("JETSON_FONT_CATALOG_OBJECT");
    std::string object = configured && configured[0]
                             ? configured : "fonts/cloud/catalog.tsv";
    return SafeAssetObject(object) ? object : "fonts/cloud/catalog.tsv";
}

std::string CloudObjectForFile(const std::string &file) {
    if (!SafeAssetObject(file)) return "";
    if (file.rfind("fonts/", 0) == 0) return file;
    return "fonts/cloud/" + file;
}

void TrimAscii(std::string &value) {
    while (!value.empty() && std::isspace((unsigned char)value.back())) value.pop_back();
    size_t first = 0;
    while (first < value.size() && std::isspace((unsigned char)value[first])) ++first;
    if (first) value.erase(0, first);
}

std::vector<CloudFontEntry> ReadCloudFontCatalog(const std::string &path) {
    std::vector<CloudFontEntry> fonts;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string name, regular, bold;
        if (!std::getline(row, name, '\t') || !std::getline(row, regular, '\t')) continue;
        std::getline(row, bold, '\t');
        TrimAscii(name);
        TrimAscii(regular);
        TrimAscii(bold);
        regular = CloudObjectForFile(regular);
        bold = bold.empty() ? "" : CloudObjectForFile(bold);
        if (name.empty() || regular.empty() || !HasTtfExtension(regular) ||
            (!bold.empty() && !HasTtfExtension(bold))) continue;
        fonts.push_back({name, regular, bold});
    }
    return fonts;
}

jetson::platform::ShellCommandResult FetchAssetObject(const std::string &object) {
    if (!SafeAssetObject(object)) return {-1, "Tên object S3 không hợp lệ"};
    const char *configured = std::getenv("JETSON_S3_ASSET_HELPER");
    std::vector<std::string> helpers;
    if (configured && configured[0]) helpers.emplace_back(configured);
    helpers.emplace_back("scripts/s3_assets.py");
    helpers.emplace_back("/opt/jetson-fw/scripts/s3_assets.py");

    std::string helper;
    for (const auto &candidate : helpers) {
        if (FileExistsLocal(candidate)) { helper = candidate; break; }
    }
    if (helper.empty()) return {-1, "Không tìm thấy scripts/s3_assets.py"};

    std::string command = "JETSON_ASSETS_DIR=" +
                          jetson::platform::QuoteShellArgument(jetson::BuiltinAssetsDir()) +
                          " python3 " + jetson::platform::QuoteShellArgument(helper) +
                          " fetch-file " + jetson::platform::QuoteShellArgument(object);
    auto result = jetson::platform::RunShellCommand(command);
    jetson::platform::TrimTrailingWhitespace(result.output);
    return result;
}

std::string CurrentTimezone() {
    // Keep category switching non-blocking: do not shell out to timedatectl
    // while the LVGL event mutex is held.  Prefer our persisted value, then
    // resolve /etc/localtime directly with readlink(2).
    std::string tz = Settings("system", false).GetString("timezone", "");
    if (!tz.empty()) return tz;
    char target[512]{};
    const ssize_t n = ::readlink("/etc/localtime", target, sizeof(target) - 1);
    if (n > 0) {
        target[n] = '\0';
        tz.assign(target);
        auto p = tz.find("zoneinfo/");
        if (p != std::string::npos) tz = tz.substr(p + 9);
    }
    if (tz.empty()) tz = "Asia/Ho_Chi_Minh";
    return tz;
}

std::string FormatBytes(unsigned long long bytes) {
    char buf[32];
    double b = (double)bytes;
    if (bytes < 1024ULL) std::snprintf(buf, sizeof(buf), "%llu B", bytes);
    else if (bytes < 1024ULL * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", b / 1024);
    else if (bytes < 1024ULL * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024 * 1024));
    else std::snprintf(buf, sizeof(buf), "%.1f GB", b / (1024.0 * 1024 * 1024));
    return buf;
}

void SetWifiSheetTranslateY(void *obj, int32_t value) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(obj), value, 0);
}

// lv_obj_create() hands every plain container LV_OBJ_FLAG_CLICKABLE, and LVGL
// does not bubble clicks unless LV_OBJ_FLAG_EVENT_BUBBLE is set. A decorative
// sub-container laid over a clickable row therefore eats the press and the
// row's own LV_EVENT_CLICKED never fires. Call this on anything inside a row
// that is purely visual so the press lands on the row itself.
void MakeDecorative(lv_obj_t *obj) {
    if (!obj) return;
    lv_obj_clear_flag(obj, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                           LV_OBJ_FLAG_CLICK_FOCUSABLE |
                                           LV_OBJ_FLAG_SCROLLABLE));
}

// Bluetooth commands finish on detached worker threads.  Queue their result
// handling onto LVGL's timer thread so list cleanup/layout, switch state
// changes, and Dynamic-Island animations never run concurrently with an LVGL
// event callback.  The heap task owns any captured shared_ptr until it runs.
void RunLvglTask(void *user_data) {
    std::unique_ptr<std::function<void()>> task(
        static_cast<std::function<void()> *>(user_data));
    LvLockGuard lock;
    (*task)();
}

void DispatchToLvgl(std::function<void()> callback) {
    auto task = std::make_unique<std::function<void()>>(std::move(callback));
    LvLockGuard lock;
    if (lv_async_call(RunLvglTask, task.get()) == LV_RESULT_OK) {
        (void)task.release();
        return;
    }
    // Allocation failure inside lv_async_call is rare; completing under the
    // global LVGL lock is still preferable to leaving a radio operation stuck
    // forever in its busy state.
    (*task)();
}

} // namespace

// =========================================================================
// Construction + layout
// =========================================================================

std::shared_ptr<SettingsView> SettingsView::Self() {
    // shared_from_this() yields shared_ptr<OverlayView>; cast down to this type
    // so worker lambdas can call SettingsView members.
    return std::static_pointer_cast<SettingsView>(shared_from_this());
}

SettingsView::SettingsView(lv_obj_t *parent, int width, int height,
                           jetson::IWifiManager &wifi,
                           jetson::IBluetoothManager &bluetooth,
                           ClosedCb on_closed)
    : OverlayView(parent, width, height, u8"Cài đặt", std::move(on_closed)),
      wifi_(wifi), bluetooth_(bluetooth),
      airplane_enabled_(jetson::IsAirplaneModeEnabled()),
      vpn_enabled_(jetson::VpnManager::Instance().CachedEnabled()) {
    // The settings cog already identifies this window. Keep the title string
    // internally (logs/minimize label) but do not spend header space repeating
    // "Cài đặt" on the device.
    if (title_label_) lv_obj_add_flag(title_label_, LV_OBJ_FLAG_HIDDEN);
    BuildShell();
}

SettingsView::~SettingsView() {
    // Stop the fan readout timer before the base class deletes the overlay:
    // it holds a `this` that stops being a SettingsView once this body returns.
    LvLockGuard lock;
    StopFanPoll();
}

void SettingsView::ShowWifiPage() {
    ShowCategory(Cat::Wifi);
}

void SettingsView::ShowBluetoothPage() {
    ShowCategory(Cat::Bluetooth);
}

void SettingsView::BuildShell() {
    const auto &p = jetson::UiTheme::Instance().Palette();

    // The 800x480 panel is landscape, so retain a narrow category rail while
    // making the detail pane behave like one iPhone Settings navigation stack.
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(body_, 8, 0);
    lv_obj_set_style_pad_column(body_, 8, 0);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    int body_h = (height_ - kHeaderHeight) - 16;

    // ---- Sidebar ----
    sidebar_ = lv_obj_create(body_);
    lv_obj_remove_style_all(sidebar_);
    lv_obj_set_size(sidebar_, 170, body_h);
    lv_obj_set_style_bg_color(sidebar_, Color(p.bg), 0);
    lv_obj_set_style_bg_opa(sidebar_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sidebar_, 12, 0);
    lv_obj_set_style_pad_all(sidebar_, 6, 0);
    lv_obj_set_style_pad_row(sidebar_, 4, 0);
    lv_obj_set_flex_flow(sidebar_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sidebar_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(sidebar_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(sidebar_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sidebar_, LV_SCROLLBAR_MODE_ACTIVE);

    // PNG icons from assets/icons/app.
    struct Entry { Cat cat; const char *icon; const char *label; };
    const Entry connectivity[] = {
        {Cat::Wifi, "wifi", "WiFi"},
        {Cat::Bluetooth, "bluetooth", "Bluetooth"},
    };
    for (const auto &e : connectivity) AddSidebarRow(e.cat, e.icon, e.label);

    AddAirplaneRow();
    AddVpnRow();

    const Entry categories[] = {
        {Cat::Sound, "speaker", "Âm thanh"},
        {Cat::General, "settings", "Cài đặt chung"},
        {Cat::Applications, "app", "Ứng dụng"},
    };
    for (const auto &e : categories) AddSidebarRow(e.cat, e.icon, e.label);

    // ---- Detail pane ----
    detail_ = lv_obj_create(body_);
    lv_obj_remove_style_all(detail_);
    lv_obj_set_flex_grow(detail_, 1);
    lv_obj_set_height(detail_, body_h);
    lv_obj_set_style_bg_color(detail_, Color(p.bg), 0);
    lv_obj_set_style_bg_opa(detail_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(detail_, 12, 0);
    lv_obj_set_style_pad_all(detail_, 10, 0);
    lv_obj_set_style_pad_bottom(detail_, 20, 0);
    lv_obj_set_style_pad_row(detail_, 10, 0);
    lv_obj_set_flex_flow(detail_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(detail_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(detail_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(detail_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(detail_, LV_SCROLLBAR_MODE_ACTIVE);

    ShowCategory(Cat::Wifi);
}

void SettingsView::AddAirplaneRow() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    airplane_row_ = lv_obj_create(sidebar_);
    lv_obj_remove_style_all(airplane_row_);
    lv_obj_set_width(airplane_row_, lv_pct(100));
    lv_obj_set_height(airplane_row_, 48);
    lv_obj_set_style_radius(airplane_row_, 10, 0);
    lv_obj_set_style_bg_color(airplane_row_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(airplane_row_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(airplane_row_, 5, 0);
    lv_obj_set_style_pad_right(airplane_row_, 5, 0);
    lv_obj_set_style_pad_column(airplane_row_, 6, 0);
    lv_obj_set_flex_flow(airplane_row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(airplane_row_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(airplane_row_, LV_OBJ_FLAG_SCROLLABLE);

    airplane_icon_bg_ = lv_obj_create(airplane_row_);
    lv_obj_remove_style_all(airplane_icon_bg_);
    lv_obj_set_size(airplane_icon_bg_, 30, 30);
    lv_obj_set_style_radius(airplane_icon_bg_, 8, 0);
    lv_obj_set_style_bg_opa(airplane_icon_bg_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(airplane_icon_bg_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // This is critical system state, so use the code-native glyph instead of
    // relying on the runtime PNG decoder and deployed asset set.
    auto *plane =
        jetson::ui::CreateAirplaneIcon(airplane_icon_bg_, lv_color_white());
    lv_obj_center(plane);

    auto *label = lv_label_create(airplane_row_);
    lv_obj_set_width(label, 1);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label, Color(p.text), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, "Plan\nmode");

    airplane_switch_ = MakeSwitch(airplane_row_, airplane_enabled_, OnAirplaneSwitch);
    lv_obj_set_size(airplane_switch_, 42, 24);
    AirplaneRefreshUi();
}

void SettingsView::AirplaneRefreshUi() {
    if (airplane_switch_) {
        if (airplane_enabled_) lv_obj_add_state(airplane_switch_, LV_STATE_CHECKED);
        else lv_obj_clear_state(airplane_switch_, LV_STATE_CHECKED);
        if (airplane_busy_.load()) lv_obj_add_state(airplane_switch_, LV_STATE_DISABLED);
        else lv_obj_clear_state(airplane_switch_, LV_STATE_DISABLED);
    }
    if (airplane_icon_bg_) {
        lv_obj_set_style_bg_color(airplane_icon_bg_,
                                  Color(airplane_enabled_ ? 0xff9f0a : 0x8e8e93), 0);
    }
    WifiRefreshSwitch();
    BtRefreshSwitch();
    VpnRefreshUi();
}

void SettingsView::AddVpnRow() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    vpn_row_ = lv_obj_create(sidebar_);
    lv_obj_remove_style_all(vpn_row_);
    lv_obj_set_width(vpn_row_, lv_pct(100));
    lv_obj_set_height(vpn_row_, 48);
    lv_obj_set_style_radius(vpn_row_, 10, 0);
    lv_obj_set_style_bg_color(vpn_row_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(vpn_row_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(vpn_row_, 5, 0);
    lv_obj_set_style_pad_right(vpn_row_, 5, 0);
    lv_obj_set_style_pad_column(vpn_row_, 6, 0);
    lv_obj_set_flex_flow(vpn_row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vpn_row_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(vpn_row_, LV_OBJ_FLAG_SCROLLABLE);

    vpn_icon_bg_ = lv_obj_create(vpn_row_);
    lv_obj_remove_style_all(vpn_icon_bg_);
    lv_obj_set_size(vpn_icon_bg_, 30, 30);
    lv_obj_set_style_radius(vpn_icon_bg_, 8, 0);
    lv_obj_set_style_bg_opa(vpn_icon_bg_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(vpn_icon_bg_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // vpn.png is a colored shield; keep its own artwork instead of flattening
    // it to white (the tile behind it already carries the on/off state).
    auto *icon = jetson::ui::CreateAppIcon(vpn_icon_bg_, "vpn", 20);
    lv_obj_center(icon);

    auto *label = lv_label_create(vpn_row_);
    lv_obj_set_width(label, 1);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label, Color(p.text), 0);
    lv_label_set_text(label, "VPN");

    vpn_switch_ = MakeSwitch(vpn_row_, vpn_enabled_, OnVpnSwitch);
    lv_obj_set_size(vpn_switch_, 42, 24);
    VpnRefreshUi();
}

void SettingsView::VpnRefreshUi() {
    if (vpn_switch_) {
        if (vpn_enabled_) lv_obj_add_state(vpn_switch_, LV_STATE_CHECKED);
        else lv_obj_clear_state(vpn_switch_, LV_STATE_CHECKED);
        if (vpn_busy_.load() || airplane_enabled_)
            lv_obj_add_state(vpn_switch_, LV_STATE_DISABLED);
        else
            lv_obj_clear_state(vpn_switch_, LV_STATE_DISABLED);
    }
    if (vpn_icon_bg_) {
        lv_obj_set_style_bg_color(vpn_icon_bg_,
                                  Color(vpn_enabled_ ? 0x30c967 : 0x8e8e93), 0);
    }
}

void SettingsView::RefreshVpnStatus() {
    if (vpn_busy_.exchange(true)) return;
    VpnRefreshUi();
    std::thread([self = Self()]() {
        const auto status = jetson::VpnManager::Instance().QueryStatus();
        LvLockGuard lock;
        self->vpn_busy_ = false;
        self->vpn_enabled_ = status.authenticated
                                 ? status.enabled
                                 : jetson::VpnManager::Instance().CachedEnabled();
        self->VpnRefreshUi();
    }).detach();
}

void SettingsView::AddSidebarRow(Cat cat, const char *icon, const char *label) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = lv_obj_create(sidebar_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 44);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    auto *ic = jetson::ui::CreateAppIcon(row, icon, 20);
    lv_obj_set_style_image_recolor(ic, Color(p.sub_text), 0);
    lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);

    auto *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(lbl, Color(p.text), 0);
    lv_label_set_text(lbl, label);

    auto *ctx = new SideCtx{this, cat};
    lv_obj_set_user_data(row, ctx); // so HighlightSide can read the category
    lv_obj_add_event_cb(row, OnSideClicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, OnSideDeleted, LV_EVENT_DELETE, ctx);
    side_rows_.push_back(row);
}

void SettingsView::HighlightSide(Cat cat) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    for (auto *r : side_rows_) {
        auto *sd = static_cast<SideCtx *>(lv_obj_get_user_data(r));
        bool sel = sd && sd->cat == cat;
        lv_obj_set_style_bg_color(r, sel ? Color(p.accent) : Color(p.row), 0);
        lv_obj_set_style_bg_opa(r, sel ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        auto *ic = lv_obj_get_child(r, 0);  // icon image
        auto *lbl = lv_obj_get_child(r, 1); // text label
        if (ic) lv_obj_set_style_image_recolor(ic, sel ? Color(p.accent) : Color(p.sub_text), 0);
        if (lbl) lv_obj_set_style_text_color(lbl, sel ? Color(p.accent) : Color(p.text), 0);
    }
}

void SettingsView::ShowCategory(Cat c) {
    current_ = c;
    ClearDetail();
    switch (c) {
        case Cat::Display: BuildDisplay(); break;
        case Cat::Sound: BuildSound(); break;
        case Cat::Wifi: BuildWifi(); break;
        case Cat::Bluetooth: BuildBluetooth(); break;
        case Cat::General: BuildGeneral(); break;
        case Cat::Applications: BuildApplications(); break;
    }
    HighlightSide(c);
}

void SettingsView::ClearDetail() {
    // The fan readout timer points at widgets in this pane; kill it before they
    // are deleted rather than letting it tick against dangling pointers.
    StopFanPoll();
    // TelexInput deletes its C++ wrapper from the LV_EVENT_DELETE callback.
    // Delete its root first, then clean the remainder of the pane.
    if (kbd_demo_ && kbd_demo_->obj()) lv_obj_delete(kbd_demo_->obj());
    kbd_demo_ = nullptr;
    if (detail_) lv_obj_clean(detail_);
    lang_vi_btn_ = nullptr;
    lang_en_btn_ = nullptr;
    wifi_switch_ = nullptr;
    wifi_reload_btn_ = nullptr;
    wifi_list_ = nullptr;
    bt_switch_ = nullptr;
    bt_reload_btn_ = nullptr;
    bt_list_ = nullptr;
    bright_slider_ = nullptr; // (declared via locals; reset modal refs below)
    bright_value_label_ = nullptr;
    text_size_slider_ = nullptr;
    text_size_value_label_ = nullptr;
    terminal_size_value_label_ = nullptr;
    night_warmth_slider_ = nullptr;
    vol_slider_ = nullptr;
    mute_switch_ = nullptr;
    vol_icon_ = nullptr;
    font_status_label_ = nullptr;
    // Any open modal belongs to overlay_, not detail_; leave it.
}

lv_obj_t *SettingsView::SectionTitle(const char *text) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *lbl = lv_label_create(detail_);
    lv_obj_set_style_text_font(lbl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(lbl, Color(p.text), 0);
    lv_label_set_text(lbl, text);
    return lbl;
}

lv_obj_t *SettingsView::MakeRow(const char *title, const char *sub) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = lv_obj_create(detail_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, sub ? 64 : 52);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, Color(p.row), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(row, 14, 0);
    lv_obj_set_style_pad_right(row, 14, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    auto *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_width(left, 1);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 2, 0);
    auto *t = lv_label_create(left);
    lv_obj_set_style_text_font(t, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(t, Color(p.text), 0);
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_label_set_text(t, title);
    if (sub) {
        auto *s = lv_label_create(left);
        lv_obj_set_style_text_font(s, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(s, Color(p.sub_text), 0);
        lv_obj_set_width(s, lv_pct(100));
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_label_set_text(s, sub);
    }
    return row;
}

lv_obj_t *SettingsView::MakeSwitch(lv_obj_t *parent, bool on, lv_event_cb_t cb) {
    auto *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 52, 28);
    lv_obj_set_style_bg_color(sw, Color(0x55565a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, Color(0x30c967), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(sw, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 5, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(sw, LV_OPA_30, LV_PART_KNOB);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, this);
    return sw;
}

lv_obj_t *SettingsView::MakeSlider(lv_obj_t *parent, int minv, int maxv, int val,
                                   lv_event_cb_t cb) {
    auto *sl = lv_slider_create(parent);
    lv_obj_set_width(sl, 220);
    lv_obj_set_height(sl, 6);
    lv_slider_set_range(sl, minv, maxv);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, Color(0x68696d), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, Color(0x1597f4), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(sl, 22, LV_PART_KNOB);
    lv_obj_set_style_height(sl, 22, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(sl, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(sl, LV_OPA_30, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, this);
    return sl;
}

lv_obj_t *SettingsView::MakeButton(lv_obj_t *parent, const char *text, uint32_t bg,
                                   lv_event_cb_t cb) {
    auto *b = lv_button_create(parent);
    lv_obj_set_height(b, 40);
    lv_obj_set_style_min_width(b, 96, 0);
    lv_obj_set_style_bg_color(b, Color(bg), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
    auto *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

lv_obj_t *SettingsView::MakeReloadButton(lv_obj_t *parent, lv_event_cb_t cb) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *button = lv_button_create(parent);
    lv_obj_set_size(button, 34, 34);
    lv_obj_set_style_bg_color(button, Color(p.button), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 9, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);
    auto *icon = jetson::ui::CreateAppIcon(button, "reload", 20);
    lv_obj_set_style_image_recolor(icon, Color(p.accent), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_center(icon);
    return button;
}

lv_obj_t *SettingsView::DisplayCard() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *card = lv_obj_create(detail_);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, Color(p.row), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, Color(p.border), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *SettingsView::DisplayRow(lv_obj_t *card, const char *title,
                                   const char *sub, int height) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, height);
    lv_obj_set_style_pad_left(row, 14, 0);
    lv_obj_set_style_pad_right(row, 14, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (title && *title) {
        auto *left = lv_obj_create(row);
        lv_obj_remove_style_all(left);
        lv_obj_set_width(left, 1);
        lv_obj_set_flex_grow(left, 1);
        lv_obj_set_height(left, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(left, 1, 0);
        lv_obj_clear_flag(left, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                                LV_OBJ_FLAG_CLICKABLE));

        auto *label = lv_label_create(left);
        lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(label, Color(p.text), 0);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text(label, title);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        if (sub) {
            auto *caption = lv_label_create(left);
            lv_obj_set_style_text_font(caption, &BUILTIN_SMALL_TEXT_FONT, 0);
            lv_obj_set_style_text_color(caption, Color(p.sub_text), 0);
            lv_obj_set_width(caption, lv_pct(100));
            lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
            lv_label_set_text(caption, sub);
            lv_obj_clear_flag(caption, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    return row;
}

void SettingsView::DisplayDivider(lv_obj_t *card) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_style_bg_color(line, Color(p.border), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_70, 0);
    lv_obj_clear_flag(line, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
}

void SettingsView::DisplayPageHeader(const char *title, bool show_back) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *header = lv_obj_create(detail_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 42);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    if (show_back) {
        auto *back = lv_obj_create(header);
        lv_obj_remove_style_all(back);
        lv_obj_set_size(back, 36, 36);
        lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(back, Color(p.button), 0);
        lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
        lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(back, OnDisplayBack, LV_EVENT_CLICKED, this);
        auto *arrow = lv_label_create(back);
        lv_obj_set_style_text_font(arrow, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(arrow, Color(p.text), 0);
        lv_label_set_text(arrow, LV_SYMBOL_LEFT);
        lv_obj_center(arrow);
    }

    auto *label = lv_label_create(header);
    lv_obj_set_style_text_font(label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label, Color(p.text), 0);
    lv_label_set_text(label, title);
    lv_obj_align(label, show_back ? LV_ALIGN_LEFT_MID : LV_ALIGN_CENTER, show_back ? 48 : 0, 0);
}

void SettingsView::DisplayCaption(const char *text) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *caption = lv_label_create(detail_);
    lv_obj_set_width(caption, lv_pct(100));
    lv_obj_set_style_text_font(caption, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(caption, Color(p.sub_text), 0);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
    lv_label_set_text(caption, text);
}

void SettingsView::MakeDisplayNavigationRow(lv_obj_t *card, const char *title,
                                            const char *value, lv_event_cb_t cb) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = DisplayRow(card, title, nullptr);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(row, 4);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, this);

    auto *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_height(right, LV_SIZE_CONTENT);
    // Reserve a deterministic trailing column.  LV_SIZE_CONTENT allowed the
    // value/chevron group to be laid out partly beyond the card on the panel,
    // which left only the tail of values such as "Không" visible.
    lv_obj_set_width(right, 144);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 8, 0);
    lv_obj_clear_flag(right, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                             LV_OBJ_FLAG_CLICKABLE));
    if (value && *value) {
        auto *status = lv_label_create(right);
        lv_obj_set_width(status, 104);
        lv_obj_set_style_text_font(status, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(status, Color(p.sub_text), 0);
        lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
        lv_label_set_text(status, value);
        lv_obj_clear_flag(status, LV_OBJ_FLAG_CLICKABLE);
    }
    auto *chevron = lv_label_create(right);
    lv_obj_set_style_text_font(chevron, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(chevron, Color(p.sub_text), 0);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_clear_flag(chevron, LV_OBJ_FLAG_CLICKABLE);
}

// =========================================================================
// Panes
// =========================================================================

void SettingsView::BuildDisplay() {
    switch (display_page_) {
        case DisplayPage::Main: BuildDisplayMain(); break;
        case DisplayPage::TextSize: BuildTextSizePage(); break;
        case DisplayPage::NightShift: BuildNightShiftPage(); break;
        case DisplayPage::AutoLock: BuildAutoLockPage(); break;
        case DisplayPage::AlwaysOn: BuildAlwaysOnPage(); break;
    }
}

void SettingsView::BuildDisplayMain() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    DisplayPageHeader("Màn hình & Độ sáng", false);

    auto *text_card = DisplayCard();
    const int font_size = Settings("display", false).GetInt("font_size", kDefaultFontSize);
    char font_value[24];
    if (font_size == kDefaultFontSize) std::snprintf(font_value, sizeof(font_value), "Mặc định");
    else std::snprintf(font_value, sizeof(font_value), "%d%%",
                       font_size * 100 / kDefaultFontSize);
    MakeDisplayNavigationRow(text_card, "Cỡ chữ", font_value, OnOpenTextSize);
    DisplayDivider(text_card);
    auto *bold_row = DisplayRow(text_card, "Chữ đậm", nullptr);
    MakeSwitch(bold_row, Settings("display", false).GetBool("bold_text", false), OnBoldToggle);

    auto *brightness_title = lv_label_create(detail_);
    lv_obj_set_style_text_font(brightness_title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(brightness_title, Color(p.sub_text), 0);
    lv_label_set_text(brightness_title, "ĐỘ SÁNG");

    int v = Settings("display", true).GetInt("brightness", 100);
    if (v < 20) v = 20;
    if (v > 100) v = 100;
    auto *brightness_card = DisplayCard();
    auto *slider_row = DisplayRow(brightness_card, "", nullptr, 58);
    /* Brightness sun icon. Rendered from a PNG (assets/icons/app/sun.png,
     * via the shared app-icon cache) and recolored to the palette at runtime,
     * NOT as a U+2600 text glyph -- the bundled arial.ttf has no Misc-Symbols
     * block, so a literal "☀" floods the log with `ttf_get_glyph_dsc_cb:
     * cache not allocated` and `glyph dsc. not found for U+2600` every frame. */
    auto *sun_small = jetson::ui::CreateAppIcon(slider_row, "sun", 20);
    lv_obj_set_style_image_recolor(sun_small, Color(p.sub_text), 0);
    lv_obj_set_style_image_recolor_opa(sun_small, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_right(sun_small, 10, 0);
    bright_slider_ = MakeSlider(slider_row, 20, 100, v, OnBrightChanged);
    lv_obj_set_flex_grow(bright_slider_, 1);
    lv_obj_set_width(bright_slider_, 1);
    char value[16];
    std::snprintf(value, sizeof(value), "%d%%", v);
    bright_value_label_ = lv_label_create(slider_row);
    lv_obj_set_width(bright_value_label_, 52);
    lv_obj_set_style_text_font(bright_value_label_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(bright_value_label_, Color(p.text), 0);
    lv_obj_set_style_text_align(bright_value_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(bright_value_label_, value);
    DisplayDivider(brightness_card);
    auto *tone_row = DisplayRow(brightness_card, "True Tone", nullptr);
    MakeSwitch(tone_row, Settings("display", false).GetBool("true_tone", false),
               OnTrueToneToggle);
    DisplayCaption("Tự động làm dịu tông màu hiển thị để nội dung dễ nhìn và nhất quán hơn.");

    auto *night_card = DisplayCard();
    MakeDisplayNavigationRow(night_card, "Night Shift",
                             Settings("display", false).GetBool("night_shift", false)
                                 ? "Bật" : "Tắt",
                             OnOpenNightShift);

    auto *lock_card = DisplayCard();
    const int sleep = Settings("display", false).GetInt("sleep_timeout", 0);
    MakeDisplayNavigationRow(lock_card, "Tự động khóa", SleepLabel(sleep), OnOpenAutoLock);
    DisplayDivider(lock_card);
    auto *wake_row = DisplayRow(lock_card, "Chạm để bật", nullptr);
    MakeSwitch(wake_row, Settings("display", false).GetBool("touch_to_wake", true),
               OnTouchWakeToggle);

    auto *always_card = DisplayCard();
    MakeDisplayNavigationRow(always_card, "Màn hình luôn bật",
                             Settings("display", false).GetBool("always_on", true)
                                 ? "Bật" : "Tắt",
                             OnOpenAlwaysOn);
    DisplayCaption("Khi bật, màn hình chờ vẫn hiển thị thông tin bằng độ sáng thấp.");
}

void SettingsView::BuildTextSizePage() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    DisplayPageHeader("Cỡ chữ", true);
    DisplayCaption("Các màn hình trong hệ thống sẽ điều chỉnh theo kích cỡ đọc ưa thích của bạn.");

    auto *preview = DisplayCard();
    auto *preview_row = DisplayRow(preview, "Văn bản mẫu", "Jetson DS-02 • Dễ đọc, rõ ràng", 64);
    (void)preview_row;

    auto *size_card = DisplayCard();
    auto *size_row = DisplayRow(size_card, "", nullptr, 70);
    auto *small_a = lv_label_create(size_row);
    lv_obj_set_style_text_font(small_a, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(small_a, Color(p.sub_text), 0);
    lv_label_set_text(small_a, "A");
    const int size = Settings("display", false).GetInt("font_size", kDefaultFontSize);
    text_size_slider_ = MakeSlider(size_row, 0, 6, FontStepForSize(size), OnTextSizeChanged);
    lv_obj_set_flex_grow(text_size_slider_, 1);
    lv_obj_set_width(text_size_slider_, 1);
    auto *large_a = lv_label_create(size_row);
    lv_obj_set_style_text_font(large_a, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(large_a, Color(p.text), 0);
    lv_label_set_text(large_a, "A");

    char value[20];
    std::snprintf(value, sizeof(value), "%d%%", size * 100 / 28);
    text_size_value_label_ = lv_label_create(detail_);
    lv_obj_set_width(text_size_value_label_, lv_pct(100));
    lv_obj_set_style_text_align(text_size_value_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(text_size_value_label_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(text_size_value_label_, Color(p.sub_text), 0);
    lv_label_set_text(text_size_value_label_, value);
}

void SettingsView::BuildNightShiftPage() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    DisplayPageHeader("Night Shift", true);
    DisplayCaption("Night Shift phủ một tông màu ấm lên màn hình để dịu mắt hơn trong môi trường tối.");

    auto *toggle_card = DisplayCard();
    auto *toggle_row = DisplayRow(toggle_card, "Bật thủ công", nullptr);
    MakeSwitch(toggle_row, Settings("display", false).GetBool("night_shift", false),
               OnNightShiftToggle);

    auto *warmth_title = lv_label_create(detail_);
    lv_obj_set_style_text_font(warmth_title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(warmth_title, Color(p.sub_text), 0);
    lv_label_set_text(warmth_title, "NHIỆT ĐỘ MÀU");
    auto *warmth_card = DisplayCard();
    auto *warmth_row = DisplayRow(warmth_card, "", nullptr, 62);
    auto *less = lv_label_create(warmth_row);
    lv_obj_set_style_text_font(less, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(less, Color(p.sub_text), 0);
    lv_label_set_text(less, "Ít ấm");
    const int warmth = std::clamp(Settings("display", false).GetInt("night_warmth", 55), 0, 100);
    night_warmth_slider_ = MakeSlider(warmth_row, 0, 100, warmth, OnNightWarmthChanged);
    lv_obj_set_flex_grow(night_warmth_slider_, 1);
    lv_obj_set_width(night_warmth_slider_, 1);
    auto *more = lv_label_create(warmth_row);
    lv_obj_set_style_text_font(more, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(more, Color(0xffa33a), 0);
    lv_label_set_text(more, "Ấm hơn");
}

void SettingsView::BuildAutoLockPage() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    DisplayPageHeader("Tự động khóa", true);
    const int current = Settings("display", false).GetInt("sleep_timeout", 0);
    auto *card = DisplayCard();
    for (size_t i = 0; i < sizeof(kSleepOpts) / sizeof(kSleepOpts[0]); ++i) {
        const auto &option = kSleepOpts[i];
        // Seven choices fit inside the shorter content area below the system
        // bar, including the final "Không" option, without clipping its text.
        auto *row = DisplayRow(card, option.label, nullptr, 42);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        auto *ctx = new OptCtx{this, std::to_string(option.seconds)};
        lv_obj_add_event_cb(row, OnSleepSelected, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, OnOptDeleted, LV_EVENT_DELETE, ctx);
        if (current == option.seconds) {
            auto *check = lv_label_create(row);
            lv_obj_set_style_text_font(check, &BUILTIN_ICON_FONT, 0);
            lv_obj_set_style_text_color(check, Color(p.accent), 0);
            lv_label_set_text(check, LV_SYMBOL_OK);
        }
        if (i + 1 < sizeof(kSleepOpts) / sizeof(kSleepOpts[0])) DisplayDivider(card);
    }
}

void SettingsView::BuildAlwaysOnPage() {
    DisplayPageHeader("Màn hình luôn bật", true);
    DisplayCaption("Màn hình chờ giảm độ sáng để hiển thị thời gian và thông tin với mức tiêu thụ điện thấp.");
    DisplayCaption("Màn hình sẽ tự chuyển sang trạng thái chờ sau khoảng thời gian Tự động khóa.");

    auto *options = DisplayCard();
    auto *wallpaper = DisplayRow(options, "Hiển thị hình nền", nullptr);
    MakeSwitch(wallpaper, Settings("display", false).GetBool("aod_wallpaper", true),
               OnAlwaysOnWallpaperToggle);
    DisplayDivider(options);
    auto *blur = DisplayRow(options, "Làm mờ hình nền", nullptr);
    MakeSwitch(blur, Settings("display", false).GetBool("aod_blur", true),
               OnAlwaysOnBlurToggle);
    DisplayDivider(options);
    auto *notifications = DisplayRow(options, "Hiển thị thông báo", nullptr);
    MakeSwitch(notifications,
               Settings("display", false).GetBool("aod_notifications", true),
               OnAlwaysOnNotificationsToggle);

    auto *master = DisplayCard();
    auto *master_row = DisplayRow(master, "Màn hình luôn bật", nullptr);
    MakeSwitch(master_row, Settings("display", false).GetBool("always_on", true),
               OnAlwaysOnToggle);
    DisplayCaption("Khi tắt, màn hình sẽ chuyển sang màu đen sau khi tự động khóa.");
}

void SettingsView::BuildSound() {
    SectionTitle("Âm thanh");
    int vol = Settings("display", true).GetInt("volume", 50);
    bool muted = Settings("display", true).GetBool("muted", false);
    char sub[32];
    std::snprintf(sub, sizeof(sub), "%d%%%s", vol, muted ? " (tắt tiếng)" : "");
    auto *row = MakeRow("Âm lượng", sub);
    const auto &p = jetson::UiTheme::Instance().Palette();
    // Stateful speaker icon: swaps to speaker-mute while muted.
    vol_icon_ = jetson::ui::CreateAppIcon(row, muted ? "speaker-mute" : "speaker", 22);
    lv_obj_set_style_image_recolor(vol_icon_, Color(p.sub_text), 0);
    lv_obj_set_style_image_recolor_opa(vol_icon_, LV_OPA_COVER, 0);
    auto *sl = MakeSlider(row, 0, 100, vol, OnVolChanged);
    lv_obj_set_width(sl, 180);
    vol_slider_ = sl;
    mute_switch_ = MakeSwitch(row, !muted, OnMuteToggle); // on = audible
}

void SettingsView::BuildApplications() {
    if (application_page_ == ApplicationPage::Terminal) {
        BuildTerminalSettings();
    } else {
        BuildApplicationsMain();
    }
}

void SettingsView::BuildApplicationsMain() {
    DisplayPageHeader("Ứng dụng", false);
    auto *card = DisplayCard();
    auto *row = DisplayRow(card, "", nullptr, 72);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(row, 4);
    lv_obj_add_event_cb(row, OnOpenTerminalSettings, LV_EVENT_CLICKED, this);

    auto *icon = jetson::ui::CreateAppIcon(row, "terminal", 36);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *labels = lv_obj_create(row);
    lv_obj_remove_style_all(labels);
    lv_obj_set_width(labels, 1);
    lv_obj_set_flex_grow(labels, 1);
    lv_obj_set_height(labels, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(labels, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(labels, 2, 0);
    lv_obj_clear_flag(labels, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                               LV_OBJ_FLAG_CLICKABLE));

    auto *title = lv_label_create(labels);
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, Color(p.text), 0);
    lv_label_set_text(title, "Terminal");

    auto *subtitle = lv_label_create(labels);
    lv_obj_set_style_text_font(subtitle, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(subtitle, Color(p.sub_text), 0);
    lv_label_set_text(subtitle, "Cài đặt theme");

    auto *chevron = lv_label_create(row);
    lv_obj_set_style_text_font(chevron, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(chevron, Color(p.sub_text), 0);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_clear_flag(chevron, LV_OBJ_FLAG_CLICKABLE);
}

void SettingsView::ApplicationsPageHeader(const char *title) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *header = lv_obj_create(detail_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 42);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    auto *back = lv_obj_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 36, 36);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, Color(p.button), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(back, OnApplicationsBack, LV_EVENT_CLICKED, this);

    auto *arrow = lv_label_create(back);
    lv_obj_set_style_text_font(arrow, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(arrow, Color(p.text), 0);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);

    auto *label = lv_label_create(header);
    lv_obj_set_style_text_font(label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label, Color(p.text), 0);
    lv_label_set_text(label, title);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 48, 0);
}

lv_obj_t *SettingsView::CreateTerminalThemePreview(
    lv_obj_t *parent, const jetson::TerminalTheme &theme) {
    auto *preview = lv_obj_create(parent);
    lv_obj_remove_style_all(preview);
    lv_obj_set_size(preview, 82, 48);
    lv_obj_set_style_radius(preview, 8, 0);
    lv_obj_set_style_bg_color(preview, Color(theme.background), 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(preview, 1, 0);
    lv_obj_set_style_border_color(preview, Color(theme.muted), 0);
    lv_obj_set_style_border_opa(preview, LV_OPA_80, 0);
    lv_obj_clear_flag(preview, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                               LV_OBJ_FLAG_CLICKABLE));

    auto bar = [preview](int x, int y, int w, uint32_t color) {
        auto *line = lv_obj_create(preview);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, w, 5);
        lv_obj_set_pos(line, x, y);
        lv_obj_set_style_radius(line, 2, 0);
        lv_obj_set_style_bg_color(line, Color(color), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_clear_flag(line, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                                LV_OBJ_FLAG_CLICKABLE));
    };
    bar(8, 8, 64, theme.foreground);
    bar(8, 17, 54, theme.muted);
    bar(8, 29, 19, theme.accent);
    bar(31, 29, 16, theme.foreground);
    bar(51, 29, 13, theme.muted);
    bar(68, 29, 5, theme.cursor);
    return preview;
}

void SettingsView::MakeTerminalThemeRow(lv_obj_t *card,
                                        const jetson::TerminalTheme &theme,
                                        bool selected) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = DisplayRow(card, "", nullptr, 66);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    if (selected) {
        lv_obj_set_style_bg_color(row, Color(p.accent), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_10, 0);
    }
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(row, 3);

    CreateTerminalThemePreview(row, theme);

    auto *name = lv_label_create(row);
    lv_obj_set_width(name, 1);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_set_style_text_font(name, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(name, Color(selected ? p.accent : p.text), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, theme.name);
    lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

    auto *check = lv_label_create(row);
    lv_obj_set_width(check, 24);
    lv_obj_set_style_text_align(check, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(check, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(check, Color(p.accent), 0);
    lv_label_set_text(check, selected ? LV_SYMBOL_OK : "");
    lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);

    auto *ctx = new ThemeCtx{this, theme.id};
    lv_obj_add_event_cb(row, OnTerminalThemeSelected, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, OnThemeDeleted, LV_EVENT_DELETE, ctx);
}

void SettingsView::BuildTerminalSettings() {
    ApplicationsPageHeader("Terminal");

    Settings terminal("terminal", false);
    const int size = std::clamp(
        terminal.GetInt("text_size", jetson::kDefaultTerminalTextSize),
        jetson::kMinTerminalTextSize, jetson::kMaxTerminalTextSize);
    const auto &selected_theme = jetson::FindTerminalTheme(terminal.GetString(
        "theme", jetson::kDefaultTerminalTheme));

    auto *appearance = DisplayCard();
    auto *size_row = DisplayRow(appearance, "Text Size", nullptr, 58);

    auto make_size_button = [this, size_row](const char *text, lv_event_cb_t cb) {
        const auto &palette = jetson::UiTheme::Instance().Palette();
        auto *button = lv_obj_create(size_row);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 44, 42);
        lv_obj_set_style_radius(button, 10, 0);
        lv_obj_set_style_bg_color(button, Color(palette.button), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);
        auto *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(label, Color(palette.text), 0);
        lv_label_set_text(label, text);
        lv_obj_center(label);
    };
    make_size_button("−", OnTerminalTextSmaller);

    terminal_size_value_label_ = lv_label_create(size_row);
    lv_obj_set_width(terminal_size_value_label_, 52);
    lv_obj_set_style_text_align(terminal_size_value_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(terminal_size_value_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(terminal_size_value_label_,
                                Color(jetson::UiTheme::Instance().Palette().text), 0);
    lv_label_set_text_fmt(terminal_size_value_label_, "%d", size);

    make_size_button("+", OnTerminalTextLarger);
    DisplayDivider(appearance);
    DisplayRow(appearance, "Font", jetson::BuiltinTerminalFontName().c_str(), 58);

    DisplayCaption("THEME");
    auto *themes = DisplayCard();
    for (size_t i = 0; i < jetson::TerminalThemeCount(); ++i) {
        const auto &theme = jetson::TerminalThemeAt(i);
        MakeTerminalThemeRow(themes, theme, std::strcmp(selected_theme.id, theme.id) == 0);
        if (i + 1 < jetson::TerminalThemeCount()) DisplayDivider(themes);
    }
}

void SettingsView::BuildWifi() {
    // One compact control row: title, reload (only while on), and power.
    // Merely opening the page probes radio state but never enables or scans it.
    auto *top = MakeRow("WiFi", airplane_enabled_ ? "Đang tắt bởi Plan mode" : nullptr);
    wifi_reload_btn_ = MakeReloadButton(top, OnWifiRescan);
    wifi_switch_ = MakeSwitch(top, false, OnWifiSwitch);
    WifiRefreshSwitch();
    AirplaneRefreshUi();

    // Network list.
    wifi_list_ = lv_obj_create(detail_);
    lv_obj_remove_style_all(wifi_list_);
    lv_obj_set_width(wifi_list_, lv_pct(100));
    lv_obj_set_flex_grow(wifi_list_, 1);
    lv_obj_set_style_bg_opa(wifi_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(wifi_list_, 0, 0);
    lv_obj_set_style_pad_row(wifi_list_, 6, 0);
    lv_obj_set_flex_flow(wifi_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(wifi_list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(wifi_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(wifi_list_, LV_SCROLLBAR_MODE_ACTIVE);

    if (airplane_enabled_) {
        wifi_enabled_ = false;
        wifi_nets_.clear();
        WifiRenderList();
    } else {
        WifiRenderList();
        WifiLoadState();
    }
}

void SettingsView::BuildBluetooth() {
    auto *top = MakeRow("Bluetooth",
                        airplane_enabled_ ? "Đang tắt bởi Plan mode" : nullptr);
    bt_reload_btn_ = MakeReloadButton(top, OnBtRescan);
    bt_switch_ = MakeSwitch(top, false, OnBtSwitch);
    BtRefreshSwitch();
    AirplaneRefreshUi();

    bt_list_ = lv_obj_create(detail_);
    lv_obj_remove_style_all(bt_list_);
    lv_obj_set_width(bt_list_, lv_pct(100));
    lv_obj_set_flex_grow(bt_list_, 1);
    lv_obj_set_style_bg_opa(bt_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(bt_list_, 0, 0);
    lv_obj_set_style_pad_row(bt_list_, 6, 0);
    lv_obj_set_flex_flow(bt_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(bt_list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(bt_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(bt_list_, LV_SCROLLBAR_MODE_ACTIVE);

    if (airplane_enabled_) {
        bt_powered_ = false;
        bt_devs_.clear();
        BtRenderList();
    } else {
        BtRenderList();
        BtLoadState();
    }
}

void SettingsView::GeneralPageHeader(const char *title) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *header = lv_obj_create(detail_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 42);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    auto *back = lv_obj_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 36, 36);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, Color(p.button), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(back, OnGeneralBack, LV_EVENT_CLICKED, this);
    auto *arrow = lv_label_create(back);
    lv_obj_set_style_text_font(arrow, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(arrow, Color(p.text), 0);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);

    auto *label = lv_label_create(header);
    lv_obj_set_style_text_font(label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label, Color(p.text), 0);
    lv_label_set_text(label, title);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

void SettingsView::MakeOptionRow(lv_obj_t *card, const char *title, const char *sub,
                                 bool selected, lv_event_cb_t cb,
                                 const std::string &value) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = DisplayRow(card, title, sub, sub ? 58 : 48);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    auto *ctx = new OptCtx{this, value};
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, OnOptDeleted, LV_EVENT_DELETE, ctx);
    if (selected) {
        auto *check = lv_label_create(row);
        lv_obj_set_style_text_font(check, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(check, Color(p.accent), 0);
        lv_label_set_text(check, LV_SYMBOL_OK);
    }
}

void SettingsView::MakeFontRow(lv_obj_t *card, const std::string &name,
                               const std::string &regular_path,
                               const std::string &bold_path,
                               const std::string &regular_object,
                               const std::string &bold_object) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    const bool installed = FileExistsLocal(regular_path);
    const bool selected = installed && regular_path == jetson::BuiltinFontRegularPath();
    auto *row = DisplayRow(card, name.c_str(), installed ? "Đã tải về" : "Trên S3", 54);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    auto *state = lv_label_create(row);
    lv_obj_set_style_text_font(state, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(state, Color(p.accent), 0);
    lv_label_set_text(state, selected ? LV_SYMBOL_OK
                                     : (installed ? LV_SYMBOL_RIGHT : LV_SYMBOL_DOWNLOAD));

    auto *ctx = new FontCtx{this, name, regular_path, bold_path,
                            regular_object, bold_object};
    lv_obj_add_event_cb(row, OnFontSelected, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, OnFontDeleted, LV_EVENT_DELETE, ctx);
}

void SettingsView::BuildGeneral() {
    switch (general_page_) {
        case GeneralPage::Main: BuildGeneralMain(); break;
        case GeneralPage::Keyboard: BuildGeneralKeyboard(); break;
        case GeneralPage::LanguageRegion: BuildLanguageRegion(); break;
        case GeneralPage::LanguagePicker: BuildLanguagePicker(); break;
        case GeneralPage::RegionPicker: BuildRegionPicker(); break;
        case GeneralPage::Calendar: BuildCalendarPicker(); break;
        case GeneralPage::DateTime: BuildGeneralDateTime(); break;
        case GeneralPage::Fonts: BuildFonts(); break;
        case GeneralPage::SystemFonts: BuildLocalFonts(false); break;
        case GeneralPage::MyFonts: BuildLocalFonts(true); break;
        case GeneralPage::CloudFonts: BuildCloudFonts(); break;
        case GeneralPage::Power: BuildGeneralPower(); break;
        case GeneralPage::LockTimeout: BuildGeneralLockTimeout(); break;
        case GeneralPage::Fan: BuildGeneralFan(); break;
        case GeneralPage::About: BuildGeneralAbout(); break;
    }
}

void SettingsView::BuildGeneralMain() {
    DisplayPageHeader("Cài đặt chung", false);
    auto *core = DisplayCard();
    MakeDisplayNavigationRow(core, "Bàn phím",
        Settings("input", false).GetString("kbd_lang", "en") == "vi" ? "Tiếng Việt" : "English",
        OnOpenGeneralKeyboard);
    DisplayDivider(core);
    MakeDisplayNavigationRow(core, "Ngôn ngữ & Vùng", nullptr, OnOpenLanguageRegion);
    DisplayDivider(core);
    MakeDisplayNavigationRow(core, "Ngày & Giờ",
        Settings("display", false).GetBool("clock_24h", true) ? "24 giờ" : "12 giờ",
        OnOpenGeneralDateTime);
    DisplayDivider(core);
    MakeDisplayNavigationRow(core, "Phông chữ", jetson::BuiltinFontName().c_str(), OnOpenFonts);

    auto *device = DisplayCard();
    MakeDisplayNavigationRow(device, "Nguồn & Khóa", nullptr, OnOpenGeneralPower);
    DisplayDivider(device);
    const auto fan = jetson::fan::Read();
    char fan_value[24];
    if (!fan.available) {
        std::snprintf(fan_value, sizeof(fan_value), "Không có");
    } else if (fan.mode == jetson::fan::Mode::Manual) {
        std::snprintf(fan_value, sizeof(fan_value), "%d%%",
                      jetson::fan::PwmToPercent(fan.manual_pwm));
    } else if (fan.mode == jetson::fan::Mode::Auto) {
        // In auto the profile is the setting worth surfacing, not the word
        // "Tự động" -- it is what decides how loud the thing is.
        std::snprintf(fan_value, sizeof(fan_value), "%s",
                      jetson::fan::ProfileLabel(fan.profile));
    } else {
        std::snprintf(fan_value, sizeof(fan_value), "%s",
                      jetson::fan::ModeLabel(fan.mode));
    }
    MakeDisplayNavigationRow(device, "Quạt tản nhiệt", fan_value, OnOpenGeneralFan);
    DisplayDivider(device);
    MakeDisplayNavigationRow(device, "Giới thiệu", BOARD_NAME, OnOpenGeneralAbout);
    DisplayCaption("Các mục trên thay đổi trực tiếp cấu hình của firmware và được lưu sau khi khởi động lại.");
}

void SettingsView::BuildGeneralKeyboard() {
    GeneralPageHeader("Bàn phím");
    const std::string lang = Settings("input", false).GetString("kbd_lang", "en");
    auto *card = DisplayCard();
    auto *row = DisplayRow(card, "Ngôn ngữ gõ",
                           lang == "vi" ? "Tiếng Việt (Telex)" : "English", 58);
    auto *buttons = lv_obj_create(row);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_size(buttons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 6, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);
    lang_vi_btn_ = MakeButton(buttons, "VI", lang == "vi" ? 0x2b6fd6 : 0x55565a, OnLangVi);
    lang_en_btn_ = MakeButton(buttons, "EN", lang == "en" ? 0x2b6fd6 : 0x55565a, OnLangEn);

    DisplayCaption("Thử bàn phím đang chọn");
    kbd_demo_ = new TelexInput(detail_, lv_pct(100), 48);
    kbd_demo_->SetTelex(lang == "vi");
    kbd_demo_->SetPlaceholder(lang == "vi" ? "as → á, aw → ă, dd → đ" : "Type to test the keyboard");
}

void SettingsView::BuildLanguageRegion() {
    GeneralPageHeader("Ngôn ngữ & Vùng");
    Settings system("system", false);
    const std::string language = system.GetString("language", "vi-VN");
    const std::string region = system.GetString("region", "VN");
    const std::string calendar = system.GetString("calendar", "gregorian");
    const char *language_name = "Tiếng Việt";
    const char *region_name = "Việt Nam";
    const char *calendar_name = "Lịch Gregory";
    for (const auto &option : kLanguages) if (language == option.code) language_name = option.title;
    for (const auto &option : kRegions) if (region == option.code) region_name = option.name;
    for (const auto &option : kCalendars) if (calendar == option.code) calendar_name = option.name;

    auto *card = DisplayCard();
    MakeDisplayNavigationRow(card, "Ngôn ngữ", language_name, OnOpenLanguagePicker);
    DisplayDivider(card);
    MakeDisplayNavigationRow(card, "Vùng", region_name, OnOpenRegionPicker);
    DisplayDivider(card);
    MakeDisplayNavigationRow(card, "Lịch", calendar_name, OnOpenCalendarPicker);
    DisplayCaption("Ngôn ngữ gõ và định dạng ngày được áp dụng ngay; vùng và loại lịch được lưu dùng chung cho firmware.");
}

void SettingsView::BuildLanguagePicker() {
    GeneralPageHeader("Chọn ngôn ngữ");
    const std::string selected = Settings("system", false).GetString("language", "vi-VN");
    auto *card = DisplayCard();
    for (size_t i = 0; i < sizeof(kLanguages) / sizeof(kLanguages[0]); ++i) {
        const auto &option = kLanguages[i];
        MakeOptionRow(card, option.title, option.subtitle, selected == option.code,
                      OnLanguageSelected, option.code);
        if (i + 1 < sizeof(kLanguages) / sizeof(kLanguages[0])) DisplayDivider(card);
    }
}

void SettingsView::BuildRegionPicker() {
    GeneralPageHeader("Chọn vùng");
    const std::string selected = Settings("system", false).GetString("region", "VN");
    auto *card = DisplayCard();
    for (size_t i = 0; i < sizeof(kRegions) / sizeof(kRegions[0]); ++i) {
        const auto &option = kRegions[i];
        MakeOptionRow(card, option.name, nullptr, selected == option.code,
                      OnRegionSelected, option.code);
        if (i + 1 < sizeof(kRegions) / sizeof(kRegions[0])) DisplayDivider(card);
    }
}

void SettingsView::BuildCalendarPicker() {
    GeneralPageHeader("Lịch");
    const std::string selected = Settings("system", false).GetString("calendar", "gregorian");
    auto *card = DisplayCard();
    for (size_t i = 0; i < sizeof(kCalendars) / sizeof(kCalendars[0]); ++i) {
        const auto &option = kCalendars[i];
        MakeOptionRow(card, option.name, nullptr, selected == option.code,
                      OnCalendarSelected, option.code);
        if (i + 1 < sizeof(kCalendars) / sizeof(kCalendars[0])) DisplayDivider(card);
    }
}

void SettingsView::BuildGeneralDateTime() {
    GeneralPageHeader("Ngày & Giờ");
    Settings system("system", false);
    const bool h24 = Settings("display", false).GetBool("clock_24h", true);
    const bool automatic = system.GetBool("auto_time", true);
    auto *format = DisplayCard();
    auto *h24_row = DisplayRow(format, "Thời gian 24 giờ", h24 ? "24 giờ" : "12 giờ", 54);
    MakeSwitch(h24_row, h24, On24hToggle);
    DisplayDivider(format);
    auto *auto_row = DisplayRow(format, "Đặt tự động", "Đồng bộ thời gian qua mạng", 58);
    MakeSwitch(auto_row, automatic, OnAutoTimeToggle);

    DisplayCaption("Múi giờ");
    const std::string current = CurrentTimezone();
    auto *zones = DisplayCard();
    if (automatic) {
        auto *row = DisplayRow(zones, "Múi giờ hiện tại", TimezoneLabel(current), 58);
        lv_obj_set_style_opa(row, LV_OPA_60, 0);
    } else {
        for (size_t i = 0; i < sizeof(kTimezones) / sizeof(kTimezones[0]); ++i) {
            MakeOptionRow(zones, kTimezones[i].label, kTimezones[i].id,
                          current == kTimezones[i].id,
                          OnTzSelected, kTimezones[i].id);
            if (i + 1 < sizeof(kTimezones) / sizeof(kTimezones[0])) DisplayDivider(zones);
        }
    }
}

void SettingsView::BuildFonts() {
    GeneralPageHeader("Phông chữ");
    auto *local = DisplayCard();
    MakeDisplayNavigationRow(local, "Phông chữ hệ thống", jetson::BuiltinFontName().c_str(),
                             OnOpenSystemFonts);
    DisplayDivider(local);
    MakeDisplayNavigationRow(local, "Phông chữ của tôi", nullptr, OnOpenMyFonts);
    DisplayCaption("Xem các phông chữ có sẵn trên thiết bị và các phông chữ bạn đã chép vào thư mục cá nhân.");

    auto *cloud = DisplayCard();
    MakeDisplayNavigationRow(cloud, "Phông chữ khác", "S3 / MinIO", OnOpenCloudFonts);
    DisplayCaption("Firmware chỉ tải phông chữ trong danh mục S3 do bạn quản lý; không truy cập nguồn font bên ngoài.");
}

void SettingsView::BuildLocalFonts(bool personal) {
    GeneralPageHeader(personal ? "Phông chữ của tôi" : "Phông chữ hệ thống");
    std::string directory = personal ? JoinPath(HomeDir(), ".jetson-fw/fonts")
                                     : JoinPath(jetson::BuiltinAssetsDir(), "fonts");
    if (personal) {
        const std::string root = JoinPath(HomeDir(), ".jetson-fw");
        ::mkdir(root.c_str(), 0775);
        ::mkdir(directory.c_str(), 0775);
    }
    const auto fonts = ListFontFamilies(directory, !personal);
    if (fonts.empty()) {
        DisplayCaption(personal ? "Chưa có phông chữ. Chép file .ttf vào ~/.jetson-fw/fonts rồi mở lại trang này."
                                : "Không tìm thấy phông chữ hệ thống trong assets/fonts.");
        return;
    }
    auto *card = DisplayCard();
    for (size_t i = 0; i < fonts.size(); ++i) {
        MakeFontRow(card, fonts[i].name, fonts[i].regular_path, fonts[i].bold_path);
        if (i + 1 < fonts.size()) DisplayDivider(card);
    }
}

void SettingsView::SetFontStatus(const std::string &text) {
    font_status_ = text;
    if (font_status_label_) lv_label_set_text(font_status_label_, font_status_.c_str());
}

void SettingsView::BuildCloudFonts() {
    GeneralPageHeader("Phông chữ khác");
    auto *action = DisplayCard();
    auto *row = DisplayRow(action, "Danh mục S3", "fonts/cloud/catalog.tsv", 58);
    auto *refresh = MakeButton(row, font_busy_.load() ? "Đang tải" : "Làm mới", 0x2b6fd6,
                               OnRefreshFontCatalog);
    if (font_busy_.load()) lv_obj_add_state(refresh, LV_STATE_DISABLED);

    font_status_label_ = lv_label_create(detail_);
    lv_obj_set_width(font_status_label_, lv_pct(100));
    lv_obj_set_style_text_font(font_status_label_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(font_status_label_,
                                Color(jetson::UiTheme::Instance().Palette().sub_text), 0);
    lv_label_set_long_mode(font_status_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(font_status_label_, font_status_.c_str());

    const std::string catalog_path = JoinPath(jetson::BuiltinAssetsDir(), CloudCatalogObject());
    const auto fonts = ReadCloudFontCatalog(catalog_path);
    if (fonts.empty()) {
        DisplayCaption("Chưa có danh mục font hợp lệ trên thiết bị. Manifest dùng mỗi dòng: Tên<TAB>regular.ttf<TAB>bold.ttf.");
        if (!font_catalog_requested_ && !font_busy_.load()) {
            font_catalog_requested_ = true;
            RefreshCloudFontCatalog();
        }
        return;
    }

    auto *card = DisplayCard();
    for (size_t i = 0; i < fonts.size(); ++i) {
        const auto &font = fonts[i];
        const std::string regular_path = JoinPath(jetson::BuiltinAssetsDir(), font.regular_object);
        const std::string bold_path = font.bold_object.empty()
                                          ? "" : JoinPath(jetson::BuiltinAssetsDir(), font.bold_object);
        MakeFontRow(card, font.name, regular_path, bold_path,
                    font.regular_object, font.bold_object);
        if (i + 1 < fonts.size()) DisplayDivider(card);
    }
}

void SettingsView::RefreshCloudFontCatalog() {
    if (font_busy_.exchange(true)) return;
    SetFontStatus("Đang tải danh mục phông chữ từ S3...");
    std::thread([self = Self()]() {
        auto result = FetchAssetObject(CloudCatalogObject());
        LvLockGuard lock;
        if (!self) return;
        self->font_busy_ = false;
        self->SetFontStatus(result.Ok() ? "Đã cập nhật danh mục phông chữ."
                                        : "Không tải được danh mục: " + result.output);
        if (self->current_ == Cat::General && self->general_page_ == GeneralPage::CloudFonts)
            self->ShowCategory(Cat::General);
    }).detach();
}

void SettingsView::DownloadAndApplyFont(const FontCtx &font) {
    if (font_busy_.exchange(true)) return;
    SetFontStatus("Đang tải " + font.name + " từ S3...");
    std::thread([self = Self(), font]() {
        auto regular = FetchAssetObject(font.regular_object);
        jetson::platform::ShellCommandResult bold{0, ""};
        if (regular.Ok() && !font.bold_object.empty()) bold = FetchAssetObject(font.bold_object);
        const bool downloaded = regular.Ok() && bold.Ok();
        LvLockGuard lock;
        if (!self) return;
        self->font_busy_ = false;
        if (downloaded && jetson::ApplyBuiltinFontFamily(font.name, font.regular_path,
                                                         font.bold_path)) {
            self->SetFontStatus("Đã tải và áp dụng " + font.name + ".");
        } else {
            const std::string error = !regular.Ok() ? regular.output : bold.output;
            self->SetFontStatus("Không tải được " + font.name + ": " + error);
        }
        if (self->current_ == Cat::General && self->general_page_ == GeneralPage::CloudFonts)
            self->ShowCategory(Cat::General);
    }).detach();
}

void SettingsView::BuildGeneralPower() {
    GeneralPageHeader("Nguồn & Khóa");
    const int sleep = Settings("display", false).GetInt("sleep_timeout", 0);
    auto *lock = DisplayCard();
    MakeDisplayNavigationRow(lock, "Tự động khóa", SleepLabel(sleep), OnOpenGeneralLockTimeout);
    DisplayDivider(lock);
    auto *now = DisplayRow(lock, "Khóa màn hình ngay", nullptr, 52);
    MakeButton(now, "Khóa", 0x2b6fd6, OnLockNow);
    DisplayDivider(lock);
    const bool has_pin = !Settings("system", false).GetString("pin", "").empty();
    auto *pin = DisplayRow(lock, "Mã PIN khóa", has_pin ? "Đã đặt" : "Chưa đặt", 58);
    MakeButton(pin, has_pin ? "Thay đổi" : "Đặt PIN", 0x55565a, OnSetPin);

    auto *power = DisplayCard();
    auto *reboot = DisplayRow(power, "Khởi động lại thiết bị", nullptr, 52);
    MakeButton(reboot, "Khởi động lại", 0xb03a3a, OnReboot);
    DisplayDivider(power);
    auto *shutdown = DisplayRow(power, "Tắt thiết bị", nullptr, 52);
    MakeButton(shutdown, "Tắt máy", 0xb03a3a, OnShutdown);
}

void SettingsView::BuildGeneralLockTimeout() {
    GeneralPageHeader("Tự động khóa");
    const int selected = Settings("display", false).GetInt("sleep_timeout", 0);
    auto *card = DisplayCard();
    for (size_t i = 0; i < sizeof(kSleepOpts) / sizeof(kSleepOpts[0]); ++i) {
        MakeOptionRow(card, kSleepOpts[i].label, nullptr, selected == kSleepOpts[i].seconds,
                      OnSleepSelected, std::to_string(kSleepOpts[i].seconds));
        if (i + 1 < sizeof(kSleepOpts) / sizeof(kSleepOpts[0])) DisplayDivider(card);
    }
}

void SettingsView::BuildGeneralFan() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    GeneralPageHeader("Quạt tản nhiệt");
    const auto fan = jetson::fan::Read();

    if (!fan.available) {
        DisplayCaption("Không tìm thấy quạt PWM trên bo mạch này.");
        return;
    }

    auto *live = DisplayCard();
    auto *live_row = DisplayRow(live, "Đang chạy", nullptr, 58);
    fan_readout_label_ = lv_label_create(live_row);
    lv_obj_set_style_text_font(fan_readout_label_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(fan_readout_label_, Color(p.sub_text), 0);
    lv_obj_set_style_text_align(fan_readout_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(fan_readout_label_, LV_OBJ_FLAG_CLICKABLE);

    auto *mode_title = lv_label_create(detail_);
    lv_obj_set_style_text_font(mode_title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(mode_title, Color(p.sub_text), 0);
    lv_label_set_text(mode_title, "CHẾ ĐỘ");

    auto *mode_card = DisplayCard();
    MakeOptionRow(mode_card, "Tự động", "Theo nhiệt độ CPU và GPU",
                  fan.mode == jetson::fan::Mode::Auto, OnFanModeSelected, "auto");
    DisplayDivider(mode_card);
    MakeOptionRow(mode_card, "Thủ công", "Giữ nguyên tốc độ bạn chọn",
                  fan.mode == jetson::fan::Mode::Manual, OnFanModeSelected, "manual");
    DisplayDivider(mode_card);
    MakeOptionRow(mode_card, "Tắt", "Chỉ dùng khi máy đang rất mát",
                  fan.mode == jetson::fan::Mode::Off, OnFanModeSelected, "off");

    // The profile only shapes the auto curve, so it is noise on the manual and
    // off pages -- show it only where it does something.
    if (fan.mode == jetson::fan::Mode::Auto) {
        auto *profile_title = lv_label_create(detail_);
        lv_obj_set_style_text_font(profile_title, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(profile_title, Color(p.sub_text), 0);
        lv_label_set_text(profile_title, "HỒ SƠ QUẠT");

        auto *profile_card = DisplayCard();
        MakeOptionRow(profile_card, "Im lặng", "Gần như không nghe, tăng tốc từ 50°C",
                      fan.profile == jetson::fan::Profile::Quiet,
                      OnFanProfileSelected, "quiet");
        DisplayDivider(profile_card);
        MakeOptionRow(profile_card, "Cân bằng", "Chạy vừa, tăng tốc từ 46°C",
                      fan.profile == jetson::fan::Profile::Balanced,
                      OnFanProfileSelected, "balanced");
        DisplayDivider(profile_card);
        MakeOptionRow(profile_card, "Mát", "Ồn hơn, giữ máy mát nhất",
                      fan.profile == jetson::fan::Profile::Cool,
                      OnFanProfileSelected, "cool");
        DisplayCaption("Ở mức nghỉ quạt gần như không giúp hạ nhiệt: chạy hết cỡ chỉ mát hơn khoảng 1°C so với tắt hẳn, nên Im lặng là mặc định.");
    }

    auto *speed_title = lv_label_create(detail_);
    lv_obj_set_style_text_font(speed_title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(speed_title, Color(p.sub_text), 0);
    lv_label_set_text(speed_title, "TỐC ĐỘ");

    const bool manual = fan.mode == jetson::fan::Mode::Manual;
    int pct = jetson::fan::PwmToPercent(manual ? fan.manual_pwm : fan.target_pwm);
    pct = std::max(pct, jetson::fan::kMinPercent);

    auto *speed_card = DisplayCard();
    auto *slider_row = DisplayRow(speed_card, "", nullptr, 58);
    /* Fan icon from assets/icons/app/fan.png through the shared icon cache and
     * recolored to the palette -- same reason as the brightness sun: arial.ttf
     * carries no symbol block, so a literal glyph would spam the log. */
    auto *fan_icon = jetson::ui::CreateAppIcon(slider_row, "fan", 28);
    lv_obj_set_style_image_recolor(fan_icon, Color(p.sub_text), 0);
    lv_obj_set_style_image_recolor_opa(fan_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_right(fan_icon, 10, 0);
    fan_slider_ = MakeSlider(slider_row, jetson::fan::kMinPercent, 100, pct, OnFanSpeedChanged);
    lv_obj_set_flex_grow(fan_slider_, 1);
    lv_obj_set_width(fan_slider_, 1);

    char value[16];
    std::snprintf(value, sizeof(value), "%d%%", pct);
    fan_value_label_ = lv_label_create(slider_row);
    lv_obj_set_width(fan_value_label_, 52);
    lv_obj_set_style_text_font(fan_value_label_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(fan_value_label_, Color(p.text), 0);
    lv_obj_set_style_text_align(fan_value_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(fan_value_label_, value);

    // In auto/off the slider is a readout of what the curve is doing, so make
    // it look and behave inert instead of silently ignoring drags.
    if (!manual) {
        lv_obj_add_state(fan_slider_, LV_STATE_DISABLED);
        lv_obj_set_style_opa(fan_slider_, LV_OPA_50, 0);
    }

    DisplayCaption(manual
        ? "Quạt giữ nguyên tốc độ này. Kéo thấp quá có thể làm máy nóng khi tải nặng."
        : "Chuyển sang Thủ công để tự kéo tốc độ. Ở chế độ Tự động, tốc độ do hồ sơ quạt quyết định.");

    if (!fan.daemon) {
        DisplayCaption("Chưa cài dịch vụ jetson-fan, thiết lập sẽ mất sau khi khởi động lại.");
    }

    RefreshFanReadout();
    fan_poll_ = lv_timer_create(OnFanPollTick, 2000, this);
}

/* Call with the LVGL lock held (lv_lock is not recursive). ClearDetail covers
 * navigating away; the destructor covers the whole Settings window closing --
 * an LV_EVENT_DELETE hook could not, because the overlay is torn down by
 * ~OverlayView, after the SettingsView sub-object is already gone. */
void SettingsView::StopFanPoll() {
    if (fan_poll_) lv_timer_del(fan_poll_);
    fan_poll_ = nullptr;
    fan_readout_label_ = nullptr;
    fan_slider_ = nullptr;
    fan_value_label_ = nullptr;
}

void SettingsView::RefreshFanReadout() {
    if (!fan_readout_label_) return;
    const auto fan = jetson::fan::Read();
    char text[96];
    if (fan.rpm > 0) {
        std::snprintf(text, sizeof(text), "%d vòng/phút • %d%% • %d°C",
                      fan.rpm, jetson::fan::PwmToPercent(fan.target_pwm), fan.temp_c);
    } else if (fan.target_pwm > 0) {
        // Tachometer off (tach_enable=0) -- the fan is driven but unmeasured.
        std::snprintf(text, sizeof(text), "%d%% • %d°C",
                      jetson::fan::PwmToPercent(fan.target_pwm), fan.temp_c);
    } else {
        std::snprintf(text, sizeof(text), "Đang dừng • %d°C", fan.temp_c);
    }
    lv_label_set_text(fan_readout_label_, text);
}

void SettingsView::BuildGeneralAbout() {
    GeneralPageHeader("Giới thiệu");
    char hostname[128]{};
    ::gethostname(hostname, sizeof(hostname) - 1);
    struct utsname kernel{};
    ::uname(&kernel);

    auto *device = DisplayCard();
    DisplayRow(device, "Tên thiết bị", hostname[0] ? hostname : BOARD_NAME, 58);
    DisplayDivider(device);
    DisplayRow(device, "Phần cứng", BOARD_NAME, 58);
    DisplayDivider(device);
    char screen[32];
    std::snprintf(screen, sizeof(screen), "%d × %d", width_, height_);
    DisplayRow(device, "Màn hình", screen, 58);
    DisplayDivider(device);
#ifdef JETSON_FW_VERSION
    DisplayRow(device, "Firmware", "jetson-fw " JETSON_FW_VERSION, 58);
#else
    DisplayRow(device, "Firmware", "jetson-fw 0.1.0", 58);
#endif
    DisplayDivider(device);
    DisplayRow(device, "Linux", kernel.release[0] ? kernel.release : "—", 58);

    struct statvfs st{};
    std::string storage = "—";
    if (statvfs("/", &st) == 0) {
        const unsigned long long total = (unsigned long long)st.f_blocks * st.f_frsize;
        const unsigned long long freeb = (unsigned long long)st.f_bavail * st.f_frsize;
        storage = FormatBytes(total > freeb ? total - freeb : 0) + " / " + FormatBytes(total) +
                  " • trống " + FormatBytes(freeb);
    }

    std::string memory = "—";
    unsigned long long total_kb = 0, available_kb = 0;
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemTotal:", 0) == 0)
            total_kb = std::strtoull(line.c_str() + 9, nullptr, 10);
        else if (line.rfind("MemAvailable:", 0) == 0)
            available_kb = std::strtoull(line.c_str() + 13, nullptr, 10);
    }
    if (total_kb) {
        const unsigned long long total = total_kb * 1024ULL;
        const unsigned long long available = available_kb * 1024ULL;
        memory = FormatBytes(total > available ? total - available : 0) + " / " +
                 FormatBytes(total) + " • trống " + FormatBytes(available);
    }
    auto *capacity = DisplayCard();
    DisplayRow(capacity, "Lưu trữ", storage.c_str(), 58);
    DisplayDivider(capacity);
    DisplayRow(capacity, "RAM", memory.c_str(), 58);
}

// =========================================================================
// WiFi
// =========================================================================

void SettingsView::WifiRefreshSwitch() {
    if (wifi_switch_) {
        if (wifi_enabled_) lv_obj_add_state(wifi_switch_, LV_STATE_CHECKED);
        else lv_obj_clear_state(wifi_switch_, LV_STATE_CHECKED);
        if (wifi_busy_.load() || airplane_enabled_)
            lv_obj_add_state(wifi_switch_, LV_STATE_DISABLED);
        else lv_obj_clear_state(wifi_switch_, LV_STATE_DISABLED);
    }
    if (wifi_reload_btn_) {
        if (wifi_enabled_) lv_obj_clear_flag(wifi_reload_btn_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(wifi_reload_btn_, LV_OBJ_FLAG_HIDDEN);
        if (wifi_busy_.load() || airplane_enabled_)
            lv_obj_add_state(wifi_reload_btn_, LV_STATE_DISABLED);
        else
            lv_obj_clear_state(wifi_reload_btn_, LV_STATE_DISABLED);
    }
}

void SettingsView::WifiLoadState() {
    if (airplane_enabled_ || wifi_busy_.exchange(true)) return;
    WifiRefreshSwitch();
    std::thread([self = Self()]() {
        const bool enabled = self->wifi_.IsEnabled();
        DispatchToLvgl([self, enabled]() {
            self->wifi_busy_ = false;
            self->wifi_enabled_ = enabled;
            if (!enabled) {
                self->wifi_nets_.clear();
                self->wifi_scanned_ = false;
            }
            self->WifiRefreshSwitch();
            if (self->wifi_list_) self->WifiRenderList();
        });
    }).detach();
}

void SettingsView::WifiRescan() {
    if (!wifi_list_) return;
    if (airplane_enabled_ || jetson::IsAirplaneModeEnabled()) {
        wifi_enabled_ = false;
        wifi_scanned_ = true;
        wifi_nets_.clear();
        WifiRefreshSwitch();
        WifiRenderList();
        SetStatus("WiFi bị tắt bởi Plan mode");
        return;
    }
    if (wifi_busy_.exchange(true)) return;
    SetStatus("Đang quét WiFi...");
    WifiRefreshSwitch();
    Notify("Đang quét WiFi...", 30000);
    ESP_LOGI(TAG, "WiFi scan requested from Settings");
    std::thread([self = Self()]() {
        const bool enabled = self->wifi_.IsEnabled();
        std::vector<jetson::WifiNetwork> nets;
        std::string active;
        if (enabled) {
            nets = self->wifi_.Scan();
            active = self->wifi_.ActiveSsid();
        }
        std::string error = self->wifi_.LastError();
        const bool scan_failed = enabled && nets.empty() && !error.empty();
        bool final_enabled = enabled;
        if (scan_failed) {
            // A failed discovery must not leave the UI claiming WiFi is usable.
            self->wifi_.Enable(false);
            final_enabled = false;
        }
        DispatchToLvgl([self, final_enabled, scan_failed, nets = std::move(nets),
                        active = std::move(active), error = std::move(error)]() mutable {
            self->wifi_busy_ = false;
            self->wifi_enabled_ = final_enabled;
            self->wifi_nets_ = scan_failed ? std::vector<jetson::WifiNetwork>{}
                                           : std::move(nets);
            self->wifi_scanned_ = final_enabled && !scan_failed;
            self->WifiRefreshSwitch();
            if (self->wifi_list_) self->WifiRenderList();
            if (!final_enabled && scan_failed) {
                const std::string message = "Lỗi quét WiFi: " + error;
                self->SetStatus(message.c_str());
                self->Notify(message.c_str());
                ESP_LOGE(TAG, "WiFi scan failed: %s", error.c_str());
            } else if (!final_enabled) {
                self->SetStatus("WiFi đang tắt");
                self->Notify("WiFi đang tắt");
            } else {
                self->SetStatus(active.empty() ? "Chạm mạng để kết nối"
                                               : ("Đã kết nối: " + active).c_str());
                const std::string message = self->wifi_nets_.empty()
                                                ? "Không tìm thấy mạng WiFi"
                                                : "Đã tìm thấy " +
                                                      std::to_string(self->wifi_nets_.size()) +
                                                      " mạng WiFi";
                self->Notify(message.c_str());
                ESP_LOGI(TAG, "WiFi scan rendered %zu networks", self->wifi_nets_.size());
            }
        });
    }).detach();
}

void SettingsView::WifiRenderList() {
    if (!wifi_list_) return;
    lv_obj_clean(wifi_list_);
    const auto &p = jetson::UiTheme::Instance().Palette();
    if (wifi_nets_.empty()) {
        auto *e = lv_label_create(wifi_list_);
        lv_obj_set_style_text_font(e, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(e, Color(p.sub_text), 0);
        lv_label_set_text(e, airplane_enabled_
                                ? "Plan mode đang bật."
                                : (wifi_busy_.load() && wifi_enabled_)
                                      ? "Đang quét WiFi..."
                                      : !wifi_enabled_
                                            ? "WiFi đang tắt."
                                            : "Không có mạng. Bấm biểu tượng tải lại để quét.");
        return;
    }
    for (const auto &n : wifi_nets_) WifiCreateRow(n);
}

void SettingsView::WifiCreateRow(const jetson::WifiNetwork &n) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = lv_obj_create(wifi_list_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 52);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, n.in_use ? Color(0x1e3a5f) : Color(p.bg), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    auto *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_height(left, 30);
    lv_obj_set_flex_grow(left, 1);
    MakeDecorative(left);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 8, 0);

    auto *check = lv_label_create(left);
    lv_obj_set_width(check, 22);
    lv_obj_set_style_text_font(check, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(check, Color(p.accent), 0);
    lv_label_set_text(check, n.in_use ? LV_SYMBOL_OK : "");

    auto *ssid = lv_label_create(left);
    lv_obj_set_style_text_font(ssid, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(ssid, Color(p.text), 0);
    lv_label_set_text(ssid, n.ssid.c_str());
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(ssid, 1);

    auto *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 132, 32);
    MakeDecorative(right);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right, 8, 0);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    if (n.secured) {
        auto *lock = lv_obj_create(right);
        lv_obj_remove_style_all(lock);
        lv_obj_set_size(lock, 16, 20);
        MakeDecorative(lock);
        auto *shackle = lv_obj_create(lock);
        lv_obj_remove_style_all(shackle);
        MakeDecorative(shackle);
        lv_obj_set_size(shackle, 10, 10);
        lv_obj_set_style_radius(shackle, 6, 0);
        lv_obj_set_style_border_width(shackle, 2, 0);
        lv_obj_set_style_border_color(shackle, Color(p.sub_text), 0);
        lv_obj_set_style_bg_opa(shackle, LV_OPA_TRANSP, 0);
        lv_obj_align(shackle, LV_ALIGN_TOP_MID, 0, 0);
        auto *lockBody = lv_obj_create(lock);
        lv_obj_remove_style_all(lockBody);
        MakeDecorative(lockBody);
        lv_obj_set_size(lockBody, 14, 11);
        lv_obj_set_style_radius(lockBody, 2, 0);
        lv_obj_set_style_bg_color(lockBody, Color(p.sub_text), 0);
        lv_obj_set_style_bg_opa(lockBody, LV_OPA_COVER, 0);
        lv_obj_align(lockBody, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    jetson::ui::CreateSignalBars(right, n.signal);

    auto *ctx = new WifiRowCtx{this, n};

    // The information affordance is separate from the row action, matching
    // iOS: tapping the row connects, tapping the circled i opens properties.
    auto *info = lv_obj_create(right);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, 30, 30);
    lv_obj_set_style_radius(info, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(info, 2, 0);
    lv_obj_set_style_border_color(info, Color(p.accent), 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(info, LV_OBJ_FLAG_CLICKABLE);
    auto *infoLabel = lv_label_create(info);
    lv_obj_set_style_text_font(infoLabel, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(infoLabel, Color(p.accent), 0);
    lv_label_set_text(infoLabel, "i");
    lv_obj_center(infoLabel);
    lv_obj_add_event_cb(info, OnWifiInfoClicked, LV_EVENT_CLICKED, ctx);

    lv_obj_add_event_cb(row, OnWifiRowClicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, OnWifiRowDeleted, LV_EVENT_DELETE, ctx);
}

void SettingsView::WifiOpenConnectSheet(const jetson::WifiNetwork &network) {
    CloseModal();
    modal_ssid_ = network.ssid;
    modal_wifi_ = network;
    const auto &p = jetson::UiTheme::Instance().Palette();

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    const int cardW = std::min(width_ - 32, 620);
    constexpr int sheetH = 286;
    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, cardW, sheetH);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 22, 0);
    lv_obj_set_style_clip_corner(popup_card_, true, 0);
    lv_obj_set_style_pad_all(popup_card_, 16, 0);
    lv_obj_set_style_pad_row(popup_card_, 8, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(popup_card_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(popup_card_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto *header = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 40);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto makeCircleAction = [&](const char *glyph, uint32_t bg, uint32_t fg,
                                lv_event_cb_t cb) {
        auto *button = lv_obj_create(header);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 40, 40);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(button, Color(bg), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        auto *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(label, Color(fg), 0);
        lv_label_set_text(label, glyph);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);
        return button;
    };
    makeCircleAction(LV_SYMBOL_CLOSE, p.button, p.text, OnModalClose);
    popup_confirm_btn_ = makeCircleAction(LV_SYMBOL_OK, p.button, 0xffffff,
                                          OnModalConnect);
    lv_obj_set_style_bg_opa(popup_confirm_btn_, LV_OPA_60, 0);
    lv_obj_add_state(popup_confirm_btn_, LV_STATE_DISABLED);

    auto *wifiIcon = jetson::ui::CreateAppIcon(popup_card_, "wifi", 26);
    lv_obj_set_style_image_recolor(wifiIcon, Color(p.accent), 0);
    lv_obj_set_style_image_recolor_opa(wifiIcon, LV_OPA_COVER, 0);

    auto *title = lv_label_create(popup_card_);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, Color(p.text), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    const std::string titleText = "Kết nối “" + network.ssid + "”";
    lv_label_set_text(title, titleText.c_str());

    auto *subtitle = lv_label_create(popup_card_);
    lv_obj_set_width(subtitle, lv_pct(100));
    lv_obj_set_style_text_font(subtitle, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(subtitle, Color(p.sub_text), 0);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    lv_label_set_text(subtitle, "Nhập mật khẩu để kết nối vào mạng Wi-Fi này.");

    popup_input_ = new TelexInput(popup_card_, cardW - 32, 50);
    popup_input_->SetPassword(true);
    popup_input_->SetMaxLen(63);
    popup_input_->SetPlaceholder("Mật khẩu");
    popup_input_->SetFont(&BUILTIN_SMALL_TEXT_FONT);
    lv_obj_add_event_cb(popup_input_->obj(), OnModalConnect, LV_EVENT_READY, this);
    lv_obj_add_event_cb(popup_input_->obj(), OnModalPasswordChanged,
                        LV_EVENT_VALUE_CHANGED, this);
    popup_input_->Focus();

    // iOS-style bottom sheet: start below the viewport and ease into place.
    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, popup_card_);
    lv_anim_set_values(&slide, sheetH + 16, 0);
    lv_anim_set_time(&slide, 280);
    lv_anim_set_exec_cb(&slide, SetWifiSheetTranslateY);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
    lv_anim_start(&slide);
}

void SettingsView::WifiLoadDetails(const jetson::WifiNetwork &network) {
    modal_wifi_ = network;
    modal_ssid_ = network.ssid;
    const jetson::WifiNetwork fallback = network;
    const std::string ssid = fallback.ssid;
    CloseModal();
    SetStatus(("Đang đọc thông tin " + ssid + "...").c_str());
    std::thread([self = Self(), fallback, ssid]() {
        auto details = self->wifi_.Details(ssid);
        if (details.ssid.empty()) details.ssid = ssid;
        if (details.signal == 0) details.signal = fallback.signal;
        details.connected = details.connected || fallback.in_use;
        details.known = details.known || fallback.known || fallback.in_use;
        if (details.security.empty()) {
            details.security = !fallback.security.empty() ? fallback.security
                              : (fallback.secured ? "WPA/WEP" : "Mở");
        }
        if (details.bssid.empty()) details.bssid = fallback.bssid;

        LvLockGuard lock;
        if (self->current_ == Cat::Wifi && self->overlay_ && self->modal_ssid_ == ssid)
            self->WifiOpenDetails(details);
    }).detach();
}

void SettingsView::WifiOpenDetails(const jetson::WifiDetails &details) {
    CloseModal();
    modal_ssid_ = details.ssid;
    const auto &p = jetson::UiTheme::Instance().Palette();

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    const int cardW = std::min(width_ - 32, 650);
    const int cardH = height_ - 24;
    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, cardW, cardH);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 22, 0);
    lv_obj_set_style_clip_corner(popup_card_, true, 0);
    lv_obj_set_style_pad_all(popup_card_, 12, 0);
    lv_obj_set_style_pad_row(popup_card_, 8, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(popup_card_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto *header = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 42);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto *back = lv_obj_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 40, 40);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, Color(p.button), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    auto *backLabel = lv_label_create(back);
    lv_obj_set_style_text_font(backLabel, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(backLabel, Color(p.text), 0);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT);
    lv_obj_center(backLabel);
    lv_obj_add_event_cb(back, OnModalClose, LV_EVENT_CLICKED, this);

    auto *title = lv_label_create(header);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_font(title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, Color(p.text), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_label_set_text(title, details.ssid.c_str());

    auto *headerSpacer = lv_obj_create(header);
    lv_obj_remove_style_all(headerSpacer);
    lv_obj_set_size(headerSpacer, 40, 40);
    lv_obj_clear_flag(headerSpacer, LV_OBJ_FLAG_SCROLLABLE);

    auto *rows = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(rows);
    lv_obj_set_width(rows, lv_pct(100));
    lv_obj_set_flex_grow(rows, 1);
    lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(rows, 4, 0);
    lv_obj_set_style_pad_row(rows, 10, 0);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(rows, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(rows, LV_SCROLLBAR_MODE_ACTIVE);

    using DetailRows = std::vector<std::pair<std::string, std::string>>;
    auto makeGroup = [&](const DetailRows &items) {
        if (items.empty()) return;
        auto *group = lv_obj_create(rows);
        lv_obj_remove_style_all(group);
        lv_obj_set_size(group, lv_pct(100), static_cast<int>(items.size()) * 42);
        lv_obj_set_style_bg_color(group, Color(p.bg), 0);
        lv_obj_set_style_bg_opa(group, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(group, 14, 0);
        lv_obj_set_style_clip_corner(group, true, 0);
        lv_obj_set_style_pad_all(group, 0, 0);
        lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);

        for (size_t i = 0; i < items.size(); ++i) {
            auto *row = lv_obj_create(group);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, lv_pct(100), 42);
            lv_obj_set_style_pad_left(row, 12, 0);
            lv_obj_set_style_pad_right(row, 12, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            if (i + 1 < items.size()) {
                lv_obj_set_style_border_width(row, 1, 0);
                lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
                lv_obj_set_style_border_color(row, Color(p.border), 0);
            }

            auto *key = lv_label_create(row);
            lv_obj_set_width(key, 170);
            lv_obj_set_style_text_font(key, &BUILTIN_SMALL_TEXT_FONT, 0);
            lv_obj_set_style_text_color(key, Color(p.text), 0);
            lv_label_set_long_mode(key, LV_LABEL_LONG_DOT);
            lv_label_set_text(key, items[i].first.c_str());

            auto *value = lv_label_create(row);
            lv_obj_set_flex_grow(value, 1);
            lv_obj_set_style_text_font(value, &BUILTIN_SMALL_TEXT_FONT, 0);
            lv_obj_set_style_text_color(value, Color(p.sub_text), 0);
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
            lv_label_set_text(value, items[i].second.c_str());
        }
    };

    if (details.known || details.connected) {
        auto *forget = lv_obj_create(rows);
        lv_obj_remove_style_all(forget);
        lv_obj_set_size(forget, lv_pct(100), 44);
        lv_obj_set_style_bg_color(forget, Color(p.bg), 0);
        lv_obj_set_style_bg_opa(forget, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(forget, 14, 0);
        lv_obj_set_style_pad_left(forget, 12, 0);
        lv_obj_add_flag(forget, LV_OBJ_FLAG_CLICKABLE);
        auto *forgetLabel = lv_label_create(forget);
        lv_obj_set_style_text_font(forgetLabel, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(forgetLabel, Color(p.accent), 0);
        lv_label_set_text(forgetLabel, "Quên mạng này");
        lv_obj_align(forgetLabel, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(forget, OnModalForget, LV_EVENT_CLICKED, this);
    }

    DetailRows credentials;
    credentials.emplace_back("Bảo mật",
                             details.security.empty() ? "Mở" : details.security);
    if (details.known)
        credentials.emplace_back("Mật khẩu",
                                 details.password.empty() ? "Không đọc được" : details.password);
    makeGroup(credentials);

    const auto shown = [](const std::string &value) {
        return value.empty() ? std::string("—") : value;
    };
    std::string ipv4 = details.ip_address;
    const auto slash = ipv4.find('/');
    if (slash != std::string::npos) ipv4.erase(slash);

    makeGroup({
        {"Địa chỉ Wi-Fi", shown(details.adapter_address)},
        {"Điểm truy cập", shown(details.bssid)},
    });
    makeGroup({
        {"Địa chỉ IPv4", shown(ipv4)},
        {"Gateway", shown(details.gateway)},
        {"DNS", shown(details.dns)},
    });
    makeGroup({
        {"Trạng thái", details.connected ? "Đã kết nối"
                       : (details.known ? "Đã lưu" : "Khả dụng")},
        {"Tín hiệu", std::to_string(details.signal) + "%"},
        {"Kênh", shown(details.channel)},
        {"Tần số", shown(details.frequency)},
        {"Tốc độ", shown(details.rate)},
    });

    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, popup_card_);
    lv_anim_set_values(&slide, cardH + 16, 0);
    lv_anim_set_time(&slide, 240);
    lv_anim_set_exec_cb(&slide, SetWifiSheetTranslateY);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
    lv_anim_start(&slide);
}

void SettingsView::WifiDoConnect(const std::string &ssid, const std::string &pw,
                                 bool offer_password_retry) {
    if (wifi_busy_.exchange(true)) return;
    SetStatus(("Đang kết nối " + ssid + "...").c_str());
    std::thread([self = Self(), ssid, pw, offer_password_retry]() {
        bool ok = self->wifi_.Connect(ssid, pw);
        std::vector<jetson::WifiNetwork> nets;
        std::string active;
        if (ok) {
            nets = self->wifi_.Scan();
            active = self->wifi_.ActiveSsid();
        }
        const std::string error = self->wifi_.LastError();
        LvLockGuard lock;
        self->wifi_busy_ = false;
        if (ok) {
            self->wifi_enabled_ = true;
            self->wifi_nets_ = std::move(nets);
            self->wifi_scanned_ = true;
            if (self->wifi_list_) self->WifiRenderList();
            self->SetStatus(("Đã kết nối: " + (active.empty() ? ssid : active)).c_str());
            ESP_LOGI(TAG, "WiFi connected: %s", ssid.c_str());
        } else {
            self->SetStatus(("Lỗi: " + error).c_str());
            ESP_LOGE(TAG, "WiFi connection failed for %s: %s", ssid.c_str(), error.c_str());
            if (offer_password_retry && self->current_ == Cat::Wifi && self->overlay_) {
                auto it = std::find_if(
                    self->wifi_nets_.begin(), self->wifi_nets_.end(),
                    [&ssid](const jetson::WifiNetwork &n) { return n.ssid == ssid; });
                if (it != self->wifi_nets_.end()) self->WifiOpenConnectSheet(*it);
            }
        }
    }).detach();
}

void SettingsView::WifiDoForget(const std::string &ssid) {
    if (wifi_busy_.exchange(true)) return;
    std::thread([self = Self(), ssid]() {
        bool ok = self->wifi_.Forget(ssid);
        std::vector<jetson::WifiNetwork> nets;
        std::string active;
        if (ok && self->wifi_.IsEnabled()) {
            nets = self->wifi_.Scan();
            active = self->wifi_.ActiveSsid();
        }
        const std::string error = self->wifi_.LastError();
        LvLockGuard lock;
        self->wifi_busy_ = false;
        if (ok) {
            self->wifi_nets_ = std::move(nets);
            if (self->wifi_list_) self->WifiRenderList();
            self->SetStatus(active.empty() ? ("Đã quên: " + ssid).c_str()
                                           : ("Đã kết nối: " + active).c_str());
        } else {
            self->SetStatus(("Lỗi: " + error).c_str());
            ESP_LOGE(TAG, "forget WiFi failed for %s: %s", ssid.c_str(), error.c_str());
        }
    }).detach();
}

// =========================================================================
// Bluetooth
// =========================================================================

void SettingsView::BtRefreshSwitch() {
    if (bt_switch_) {
        if (bt_powered_) lv_obj_add_state(bt_switch_, LV_STATE_CHECKED);
        else lv_obj_clear_state(bt_switch_, LV_STATE_CHECKED);
        if (bt_busy_.load() || airplane_enabled_)
            lv_obj_add_state(bt_switch_, LV_STATE_DISABLED);
        else lv_obj_clear_state(bt_switch_, LV_STATE_DISABLED);
    }
    if (bt_reload_btn_) {
        if (bt_powered_) lv_obj_clear_flag(bt_reload_btn_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(bt_reload_btn_, LV_OBJ_FLAG_HIDDEN);
        if (bt_busy_.load() || airplane_enabled_)
            lv_obj_add_state(bt_reload_btn_, LV_STATE_DISABLED);
        else
            lv_obj_clear_state(bt_reload_btn_, LV_STATE_DISABLED);
    }
}

void SettingsView::BtLoadState() {
    if (airplane_enabled_ || bt_busy_.exchange(true)) return;
    BtRefreshSwitch();
    std::thread([self = Self()]() {
        const bool powered = self->bluetooth_.IsPowered();
        DispatchToLvgl([self, powered]() {
            self->bt_busy_ = false;
            self->bt_powered_ = powered;
            if (!powered) {
                self->bt_devs_.clear();
                self->bt_scanned_ = false;
            }
            self->BtRefreshSwitch();
            if (self->bt_list_) self->BtRenderList();
        });
    }).detach();
}

void SettingsView::BtRescan() {
    if (!bt_list_) return;
    if (airplane_enabled_ || jetson::IsAirplaneModeEnabled()) {
        bt_powered_ = false;
        bt_scanned_ = true;
        bt_devs_.clear();
        BtRefreshSwitch();
        BtRenderList();
        SetStatus("Bluetooth bị tắt bởi Plan mode");
        return;
    }
    if (bt_busy_.exchange(true)) {
        BtRefreshSwitch();
        BtRenderList();
        return;
    }
    SetStatus("Đang quét Bluetooth...");
    BtRefreshSwitch();
    Notify("Đang tìm kiếm thiết bị Bluetooth...", 30000);
    ESP_LOGI(TAG, "Bluetooth scan requested from Settings");
    std::thread([self = Self()]() {
        const bool powered = self->bluetooth_.IsPowered();
        std::vector<jetson::BtDevice> devs;
        if (powered) devs = self->bluetooth_.Scan(8);
        std::string error = self->bluetooth_.LastError();
        const bool scan_failed = powered && devs.empty() && !error.empty();
        bool final_powered = powered;
        if (scan_failed) {
            self->bluetooth_.PowerOff();
            final_powered = false;
        }
        DispatchToLvgl([self, final_powered, scan_failed,
                        devs = std::move(devs), error = std::move(error)]() mutable {
            self->bt_busy_ = false;
            self->bt_powered_ = final_powered;
            self->bt_devs_ = scan_failed ? std::vector<jetson::BtDevice>{}
                                         : std::move(devs);
            self->bt_scanned_ = final_powered && !scan_failed;
            self->BtRefreshSwitch();
            if (self->bt_list_) self->BtRenderList();
            if (scan_failed) {
                const std::string message = "Lỗi quét Bluetooth: " + error;
                self->SetStatus(message.c_str());
                self->Notify(message.c_str());
                ESP_LOGE(TAG, "Bluetooth scan failed: %s", error.c_str());
            } else if (!final_powered) {
                // Distinguish "adapter is simply off" from a real failure
                // (e.g. bluetoothd not running) so the user sees the cause.
                const std::string message = error.empty()
                                                ? "Bluetooth đang tắt. Bật công tắc Bluetooth để quét."
                                                : "Lỗi Bluetooth: " + error;
                self->SetStatus(message.c_str());
                self->Notify(message.c_str());
            } else {
                self->SetStatus("Chạm thiết bị để kết nối, bấm (i) để ngắt/quên");
                if (self->notification_cb_) {
                    const std::string message = self->bt_devs_.empty()
                                                    ? "Không tìm thấy thiết bị Bluetooth"
                                                    : "Đã tìm thấy " +
                                                          std::to_string(self->bt_devs_.size()) +
                                                          " thiết bị Bluetooth";
                    self->Notify(message.c_str());
                }
                ESP_LOGI(TAG, "Bluetooth scan rendered %zu devices", self->bt_devs_.size());
            }
        });
    }).detach();
}

void SettingsView::BtRenderList() {
    if (!bt_list_) return;
    lv_obj_clean(bt_list_);
    const auto &p = jetson::UiTheme::Instance().Palette();
    if (bt_devs_.empty()) {
        auto *e = lv_label_create(bt_list_);
        lv_obj_set_style_text_font(e, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(e, Color(p.sub_text), 0);
        lv_label_set_text(e, airplane_enabled_
                                ? "Plan mode đang bật."
                                : (bt_busy_.load() && bt_powered_)
                                      ? "Đang tìm kiếm thiết bị Bluetooth..."
                                      : !bt_powered_
                                            ? "Bluetooth đang tắt."
                                            : "Không có thiết bị. Bấm biểu tượng tải lại.");
        return;
    }
    for (const auto &d : bt_devs_) BtCreateRow(d);
}

void SettingsView::BtCreateRow(const jetson::BtDevice &d) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = lv_obj_create(bt_list_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 58);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, d.connected ? Color(0x1e3a5f) : Color(p.bg), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Same anatomy as the WiFi rows, but the leading slot shows the scanned
    // device category (controller-mini / headphones / unknow-device) instead
    // of a bare checkmark; accent color doubles as the connected affordance.
    auto *type_icon =
        jetson::ui::CreateAppIcon(row, jetson::BtKindIconName(d.kind), 22);
    lv_obj_set_style_image_recolor(
        type_icon, d.connected ? Color(p.accent) : Color(p.sub_text), 0);
    lv_obj_set_style_image_recolor_opa(type_icon, LV_OPA_COVER, 0);

    auto *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_width(left, 1);
    lv_obj_set_height(left, 42);
    lv_obj_set_flex_grow(left, 1);
    MakeDecorative(left);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(left, 2, 0);
    auto *name = lv_label_create(left);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_text_font(name, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(name, Color(p.text), 0);
    lv_label_set_text(name, d.name.c_str());
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    auto *addr = lv_label_create(left);
    lv_obj_set_width(addr, lv_pct(100));
    lv_obj_set_style_text_font(addr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(addr, Color(p.sub_text), 0);
    lv_label_set_text(addr, d.address.c_str());
    lv_label_set_long_mode(addr, LV_LABEL_LONG_DOT);

    auto *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 168, 32);
    MakeDecorative(right);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right, 8, 0);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto *tag = lv_label_create(right);
    lv_obj_set_style_text_font(tag, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(tag, d.connected ? Color(p.accent) : Color(p.sub_text), 0);
    lv_label_set_text(tag, d.connected ? "Đã kết nối" : (d.paired ? "Đã pair" : ""));
    jetson::ui::CreateSignalBars(right, jetson::ui::RssiToSignalPercent(d.rssi));

    auto *ctx = new BtRowCtx{this, d};

    // The information affordance is separate from the row action, matching
    // the WiFi list: tapping the row connects, tapping the circled i opens
    // properties (state, forget, disconnect).
    auto *info = lv_obj_create(right);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, 30, 30);
    lv_obj_set_style_radius(info, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(info, 2, 0);
    lv_obj_set_style_border_color(info, Color(p.accent), 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(info, LV_OBJ_FLAG_CLICKABLE);
    auto *infoLabel = lv_label_create(info);
    lv_obj_set_style_text_font(infoLabel, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(infoLabel, Color(p.accent), 0);
    lv_label_set_text(infoLabel, "i");
    lv_obj_center(infoLabel);
    lv_obj_add_event_cb(info, OnBtInfoClicked, LV_EVENT_CLICKED, ctx);

    lv_obj_add_event_cb(row, OnBtRowClicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, OnBtRowDeleted, LV_EVENT_DELETE, ctx);
}

void SettingsView::BtOpenConnectSheet(const jetson::BtDevice &d) {
    CloseModal();
    modal_bt_addr_ = d.address;
    modal_bt_connected_ = false;
    const auto &p = jetson::UiTheme::Instance().Palette();

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    const int cardW = std::min(width_ - 32, 620);
    constexpr int sheetH = 236;
    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, cardW, sheetH);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 22, 0);
    lv_obj_set_style_clip_corner(popup_card_, true, 0);
    lv_obj_set_style_pad_all(popup_card_, 16, 0);
    lv_obj_set_style_pad_row(popup_card_, 8, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(popup_card_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(popup_card_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto *header = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 40);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto makeCircleAction = [&](const char *glyph, uint32_t bg, uint32_t fg,
                                lv_event_cb_t cb) {
        auto *button = lv_obj_create(header);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 40, 40);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(button, Color(bg), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        auto *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(label, Color(fg), 0);
        lv_label_set_text(label, glyph);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);
        return button;
    };
    makeCircleAction(LV_SYMBOL_CLOSE, p.button, p.text, OnModalClose);
    // Pairing needs no password (bluetoothctl's default-agent handles PIN /
    // Just-Works), so unlike the WiFi sheet the confirm is enabled right away.
    popup_confirm_btn_ = makeCircleAction(LV_SYMBOL_OK, p.accent, 0xffffff,
                                          OnModalBtAction);

    auto *btIcon = jetson::ui::CreateAppIcon(popup_card_, "bluetooth", 26);
    lv_obj_set_style_image_recolor(btIcon, Color(p.accent), 0);
    lv_obj_set_style_image_recolor_opa(btIcon, LV_OPA_COVER, 0);

    auto *title = lv_label_create(popup_card_);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, Color(p.text), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    const std::string titleText = "Kết nối “" + d.name + "”";
    lv_label_set_text(title, titleText.c_str());

    auto *subtitle = lv_label_create(popup_card_);
    lv_obj_set_width(subtitle, lv_pct(100));
    lv_obj_set_style_text_font(subtitle, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(subtitle, Color(p.sub_text), 0);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    lv_label_set_text(subtitle,
                      "Ghép đôi và kết nối với thiết bị Bluetooth này.");

    auto *addr = lv_label_create(popup_card_);
    lv_obj_set_width(addr, lv_pct(100));
    lv_obj_set_style_text_font(addr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(addr, Color(p.sub_text), 0);
    lv_obj_set_style_text_align(addr, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(addr, d.address.c_str());

    // iOS-style bottom sheet: start below the viewport and ease into place.
    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, popup_card_);
    lv_anim_set_values(&slide, sheetH + 16, 0);
    lv_anim_set_time(&slide, 280);
    lv_anim_set_exec_cb(&slide, SetWifiSheetTranslateY);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
    lv_anim_start(&slide);
}

void SettingsView::BtOpenDetails(const jetson::BtDevice &d) {
    CloseModal();
    modal_bt_addr_ = d.address;
    modal_bt_connected_ = d.connected;
    const auto &p = jetson::UiTheme::Instance().Palette();

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    const int cardW = std::min(width_ - 32, 650);
    const int cardH = height_ - 24;
    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, cardW, cardH);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 22, 0);
    lv_obj_set_style_clip_corner(popup_card_, true, 0);
    lv_obj_set_style_pad_all(popup_card_, 12, 0);
    lv_obj_set_style_pad_row(popup_card_, 8, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(popup_card_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto *header = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 42);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto *back = lv_obj_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 40, 40);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, Color(p.button), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    auto *backLabel = lv_label_create(back);
    lv_obj_set_style_text_font(backLabel, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(backLabel, Color(p.text), 0);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT);
    lv_obj_center(backLabel);
    lv_obj_add_event_cb(back, OnModalClose, LV_EVENT_CLICKED, this);

    auto *title = lv_label_create(header);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_font(title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, Color(p.text), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_label_set_text(title, d.name.c_str());

    auto *headerSpacer = lv_obj_create(header);
    lv_obj_remove_style_all(headerSpacer);
    lv_obj_set_size(headerSpacer, 40, 40);
    lv_obj_clear_flag(headerSpacer, LV_OBJ_FLAG_SCROLLABLE);

    auto *rows = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(rows);
    lv_obj_set_width(rows, lv_pct(100));
    lv_obj_set_flex_grow(rows, 1);
    lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(rows, 4, 0);
    lv_obj_set_style_pad_row(rows, 10, 0);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(rows, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(rows, LV_SCROLLBAR_MODE_ACTIVE);

    auto makeActionRow = [&](const char *text, uint32_t color, lv_event_cb_t cb) {
        auto *action = lv_obj_create(rows);
        lv_obj_remove_style_all(action);
        lv_obj_set_size(action, lv_pct(100), 44);
        lv_obj_set_style_bg_color(action, Color(p.bg), 0);
        lv_obj_set_style_bg_opa(action, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(action, 14, 0);
        lv_obj_set_style_pad_left(action, 12, 0);
        lv_obj_add_flag(action, LV_OBJ_FLAG_CLICKABLE);
        auto *label = lv_label_create(action);
        lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(label, Color(color), 0);
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(action, cb, LV_EVENT_CLICKED, this);
    };
    makeActionRow(d.connected ? "Ngắt kết nối" : "Kết nối", p.accent,
                  OnModalBtAction);
    if (d.paired || d.connected)
        makeActionRow("Quên thiết bị này", 0xff6b6b, OnModalBtRemove);

    using DetailRows = std::vector<std::pair<std::string, std::string>>;
    auto makeGroup = [&](const DetailRows &items) {
        if (items.empty()) return;
        auto *group = lv_obj_create(rows);
        lv_obj_remove_style_all(group);
        lv_obj_set_size(group, lv_pct(100), static_cast<int>(items.size()) * 42);
        lv_obj_set_style_bg_color(group, Color(p.bg), 0);
        lv_obj_set_style_bg_opa(group, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(group, 14, 0);
        lv_obj_set_style_clip_corner(group, true, 0);
        lv_obj_set_style_pad_all(group, 0, 0);
        lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);

        for (size_t i = 0; i < items.size(); ++i) {
            auto *row = lv_obj_create(group);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, lv_pct(100), 42);
            lv_obj_set_style_pad_left(row, 12, 0);
            lv_obj_set_style_pad_right(row, 12, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            if (i + 1 < items.size()) {
                lv_obj_set_style_border_width(row, 1, 0);
                lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
                lv_obj_set_style_border_color(row, Color(p.border), 0);
            }

            auto *key = lv_label_create(row);
            lv_obj_set_width(key, 170);
            lv_obj_set_style_text_font(key, &BUILTIN_SMALL_TEXT_FONT, 0);
            lv_obj_set_style_text_color(key, Color(p.text), 0);
            lv_label_set_long_mode(key, LV_LABEL_LONG_DOT);
            lv_label_set_text(key, items[i].first.c_str());

            auto *value = lv_label_create(row);
            lv_obj_set_flex_grow(value, 1);
            lv_obj_set_style_text_font(value, &BUILTIN_SMALL_TEXT_FONT, 0);
            lv_obj_set_style_text_color(value, Color(p.sub_text), 0);
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
            lv_label_set_text(value, items[i].second.c_str());
        }
    };

    makeGroup({
        {"Trạng thái", d.connected ? "Đã kết nối"
                       : (d.paired ? "Đã ghép đôi" : "Khả dụng")},
        {"Địa chỉ", d.address},
        {"Ghép đôi", d.paired ? "Có" : "Không"},
        {"Tín hiệu", d.rssi
             ? std::to_string(jetson::ui::RssiToSignalPercent(d.rssi)) + "%"
             : "—"},
    });

    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, popup_card_);
    lv_anim_set_values(&slide, cardH + 16, 0);
    lv_anim_set_time(&slide, 240);
    lv_anim_set_exec_cb(&slide, SetWifiSheetTranslateY);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
    lv_anim_start(&slide);
}

void SettingsView::BtDoAction(const std::string &addr, bool connected) {
    if (bt_busy_.exchange(true)) return;
    SetStatus(connected ? "Đang ngắt..." : "Đang kết nối...");
    std::thread([self = Self(), addr, connected]() {
        bool ok = connected ? self->bluetooth_.Disconnect(addr)
                            : self->bluetooth_.PairAndConnect(addr);
        std::vector<jetson::BtDevice> devs;
        if (ok) devs = self->bluetooth_.Scan(4);
        const std::string error = self->bluetooth_.LastError();
        DispatchToLvgl([self, addr, connected, ok, devs = std::move(devs), error]() mutable {
            self->bt_busy_ = false;
            self->BtRefreshSwitch();
            if (ok) {
                self->bt_powered_ = true;
                self->bt_devs_ = std::move(devs);
                self->bt_scanned_ = true;
                if (self->bt_list_) self->BtRenderList();
                const char *message = connected ? "Đã ngắt kết nối Bluetooth"
                                                : "Đã kết nối Bluetooth";
                self->SetStatus(message);
                self->Notify(message);
            } else {
                const std::string message = "Lỗi Bluetooth: " + error;
                self->SetStatus(message.c_str());
                self->Notify(message.c_str());
                ESP_LOGE(TAG, "Bluetooth action failed for %s: %s",
                         addr.c_str(), error.c_str());
            }
        });
    }).detach();
}

void SettingsView::BtDoRemove(const std::string &addr) {
    if (bt_busy_.exchange(true)) return;
    std::thread([self = Self(), addr]() {
        bool ok = self->bluetooth_.Remove(addr);
        std::vector<jetson::BtDevice> devs;
        if (ok && self->bluetooth_.IsPowered()) devs = self->bluetooth_.Scan(4);
        const std::string error = self->bluetooth_.LastError();
        DispatchToLvgl([self, addr, ok, devs = std::move(devs), error]() mutable {
            self->bt_busy_ = false;
            self->BtRefreshSwitch();
            if (ok) {
                self->bt_devs_ = std::move(devs);
                if (self->bt_list_) self->BtRenderList();
                self->SetStatus("Đã quên thiết bị Bluetooth");
                self->Notify("Đã quên thiết bị Bluetooth");
            } else {
                const std::string message = "Lỗi Bluetooth: " + error;
                self->SetStatus(message.c_str());
                self->Notify(message.c_str());
                ESP_LOGE(TAG, "Bluetooth remove failed for %s: %s",
                         addr.c_str(), error.c_str());
            }
        });
    }).detach();
}

// =========================================================================
// Modal helpers
// =========================================================================

void SettingsView::CloseModal() {
    if (popup_) { lv_obj_del(popup_); popup_ = nullptr; popup_card_ = nullptr; }
    popup_confirm_btn_ = nullptr;
    popup_input_ = nullptr; // freed via its LV_EVENT_DELETE -> delete self
    pin_a_ = nullptr; pin_b_ = nullptr;
    modal_yes_ = nullptr;
}

void SettingsView::OpenConfirmModal(const char *title, const char *msg,
                                    std::function<void()> on_yes) {
    CloseModal();
    modal_yes_ = std::move(on_yes);
    const auto &p = jetson::UiTheme::Instance().Palette();

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);

    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, 300, 160);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 16, 0);
    lv_obj_set_style_pad_all(popup_card_, 16, 0);
    lv_obj_set_style_pad_row(popup_card_, 10, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(popup_card_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto *t = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(t, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(t, Color(p.text), 0);
    lv_label_set_text(t, title);
    auto *m = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(m, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(m, Color(p.sub_text), 0);
    lv_obj_set_width(m, 268);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_label_set_text(m, msg);

    auto *btns = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    MakeButton(btns, "Xác nhận", 0xb03a3a, OnModalConfirmYes);
    MakeButton(btns, "Hủy", 0x3a3a3a, OnModalClose);
}

void SettingsView::OpenPinModal() {
    CloseModal();
    const auto &p = jetson::UiTheme::Instance().Palette();
    std::string cur = Settings("system", true).GetString("pin", "");

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    // Two labeled fields plus actions need their real content height.  The old
    // 230 px card forced the flex children past its lower edge.
    lv_obj_set_size(popup_card_, 330, 330);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 16, 0);
    lv_obj_set_style_pad_all(popup_card_, 14, 0);
    lv_obj_set_style_pad_row(popup_card_, 8, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(popup_card_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto *t = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(t, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(t, Color(p.text), 0);
    lv_label_set_text(t, cur.empty() ? "Đặt PIN (4 số)" : "Đổi PIN (4 số)");

    auto *lbl1 = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(lbl1, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(lbl1, Color(p.sub_text), 0);
    lv_label_set_text(lbl1, "PIN mới");
    pin_a_ = new TelexInput(popup_card_, 302, 44);
    pin_a_->SetTelex(false);
    pin_a_->SetPassword(true);
    pin_a_->SetMaxLen(4);
    pin_a_->SetAcceptedChars("0123456789");
    pin_a_->SetPlaceholder("4 chữ số");
    lv_obj_add_event_cb(pin_a_->obj(), [](lv_event_t *event) {
        auto *self = static_cast<SettingsView *>(lv_event_get_user_data(event));
        if (self->pin_b_) self->pin_b_->Focus();
    }, LV_EVENT_READY, this);
    pin_a_->Focus();

    auto *lbl2 = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(lbl2, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(lbl2, Color(p.sub_text), 0);
    lv_label_set_text(lbl2, "Nhập lại");
    pin_b_ = new TelexInput(popup_card_, 302, 44);
    pin_b_->SetTelex(false);
    pin_b_->SetPassword(true);
    pin_b_->SetMaxLen(4);
    pin_b_->SetAcceptedChars("0123456789");
    pin_b_->SetPlaceholder("Nhập lại 4 chữ số");
    lv_obj_add_event_cb(pin_b_->obj(), OnPinSave, LV_EVENT_READY, this);

    auto *btns = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    MakeButton(btns, "Lưu", 0x2b6fd6, OnPinSave);
    if (!cur.empty()) MakeButton(btns, "Xóa PIN", 0xb03a3a, OnPinClear);
    MakeButton(btns, "Hủy", 0x3a3a3a, OnModalClose);
}

// =========================================================================
// OnStart / OnResize
// =========================================================================

void SettingsView::OnStart() {
    SetStatus("");
    RefreshVpnStatus();
}

void SettingsView::OnResize(int /*w*/, int h) {
    // Runs under the base class lock. Rebuild the current pane into the (already
    // resized) detail container. Sidebar/detail heights are % / flex-grown so
    // they reflow; only the pane content needs rebuilding.
    const int pane_h = std::max(240, h - 16);
    if (sidebar_) lv_obj_set_height(sidebar_, pane_h);
    if (detail_) lv_obj_set_height(detail_, pane_h);
    ShowCategory(current_);
}

// =========================================================================
// Event handlers
// =========================================================================

void SettingsView::OnSideClicked(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<SideCtx *>(lv_event_get_user_data(e));
    if (ctx) {
        if (ctx->cat == Cat::Display) ctx->self->display_page_ = DisplayPage::Main;
        if (ctx->cat == Cat::General) ctx->self->general_page_ = GeneralPage::Main;
        if (ctx->cat == Cat::Applications)
            ctx->self->application_page_ = ApplicationPage::Main;
        ctx->self->ShowCategory(ctx->cat);
    }
}
void SettingsView::OnSideDeleted(lv_event_t *e) {
    delete static_cast<SideCtx *>(lv_event_get_user_data(e));
}
void SettingsView::OnWifiRowDeleted(lv_event_t *e) {
    delete static_cast<WifiRowCtx *>(lv_event_get_user_data(e));
}
void SettingsView::OnBtRowDeleted(lv_event_t *e) {
    delete static_cast<BtRowCtx *>(lv_event_get_user_data(e));
}
void SettingsView::OnOptDeleted(lv_event_t *e) {
    delete static_cast<OptCtx *>(lv_event_get_user_data(e));
}
void SettingsView::OnThemeDeleted(lv_event_t *e) {
    delete static_cast<ThemeCtx *>(lv_event_get_user_data(e));
}
void SettingsView::OnFontDeleted(lv_event_t *e) {
    delete static_cast<FontCtx *>(lv_event_get_user_data(e));
}

void SettingsView::OnBrightChanged(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    int v = lv_slider_get_value(self->bright_slider_);
    if (v < 20) { v = 20; lv_slider_set_value(self->bright_slider_, 20, LV_ANIM_OFF); }
    Settings("display", true).SetInt("brightness", v);
    if (self->brightness_cb_) self->brightness_cb_(v);
    if (self->bright_value_label_) {
        char value[16];
        std::snprintf(value, sizeof(value), "%d%%", v);
        lv_label_set_text(self->bright_value_label_, value);
    }
    self->SetStatus(("Độ sáng: " + std::to_string(v) + "%").c_str());
}

void SettingsView::OnDisplayBack(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->display_page_ = DisplayPage::Main;
    self->ShowCategory(Cat::Display);
}

void SettingsView::OnOpenTextSize(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->display_page_ = DisplayPage::TextSize;
    self->ShowCategory(Cat::Display);
}

void SettingsView::OnOpenNightShift(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->display_page_ = DisplayPage::NightShift;
    self->ShowCategory(Cat::Display);
}

void SettingsView::OnOpenAutoLock(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->display_page_ = DisplayPage::AutoLock;
    self->ShowCategory(Cat::Display);
}

void SettingsView::OnOpenAlwaysOn(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->display_page_ = DisplayPage::AlwaysOn;
    self->ShowCategory(Cat::Display);
}

void SettingsView::OnTextSizeChanged(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const int step = std::clamp((int)lv_slider_get_value(self->text_size_slider_), 0, 6);
    const int size = kFontSizes[step];
    const bool bold = Settings("display", false).GetBool("bold_text", false);
    jetson::ApplyBuiltinTypography(size, bold);
    if (self->text_size_value_label_) {
        char value[20];
        std::snprintf(value, sizeof(value), "%d%%", size * 100 / 28);
        lv_label_set_text(self->text_size_value_label_, value);
    }
    self->SetStatus(("Cỡ chữ: " + std::to_string(size) + " px").c_str());
}

void SettingsView::OnBoldToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    const int size = Settings("display", false).GetInt("font_size", kDefaultFontSize);
    jetson::ApplyBuiltinTypography(size, on);
    self->SetStatus(on ? "Đã bật chữ đậm" : "Đã tắt chữ đậm");
}

void SettingsView::OnTrueToneToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("true_tone", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
    self->SetStatus(on ? "True Tone: Bật" : "True Tone: Tắt");
}

void SettingsView::OnNightShiftToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("night_shift", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
    self->SetStatus(on ? "Night Shift: Bật" : "Night Shift: Tắt");
}

void SettingsView::OnNightWarmthChanged(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const int warmth = lv_slider_get_value(self->night_warmth_slider_);
    Settings("display", true).SetInt("night_warmth", warmth);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
    self->SetStatus(("Độ ấm Night Shift: " + std::to_string(warmth) + "%").c_str());
}

void SettingsView::OnTouchWakeToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("touch_to_wake", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
    self->SetStatus(on ? "Chạm để bật: Bật" : "Chạm để bật: Tắt");
}

void SettingsView::OnAlwaysOnToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("always_on", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
    self->SetStatus(on ? "Màn hình luôn bật: Bật" : "Màn hình luôn bật: Tắt");
}

void SettingsView::OnAlwaysOnWallpaperToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("aod_wallpaper", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
}

void SettingsView::OnAlwaysOnBlurToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("aod_blur", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
}

void SettingsView::OnAlwaysOnNotificationsToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("aod_notifications", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
}

void SettingsView::OnVolChanged(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    int v = lv_slider_get_value(self->vol_slider_);
    Settings("display", true).SetInt("volume", v);
    if (self->volume_cb_) self->volume_cb_(v, !lv_obj_has_state(self->mute_switch_, LV_STATE_CHECKED));
    self->SetStatus(("Âm lượng: " + std::to_string(v) + "%").c_str());
}

void SettingsView::OnMuteToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    bool audible = lv_obj_has_state(self->mute_switch_, LV_STATE_CHECKED);
    Settings("display", true).SetBool("muted", !audible);
    int v = self->vol_slider_ ? lv_slider_get_value(self->vol_slider_) : 0;
    if (self->volume_cb_) self->volume_cb_(v, !audible);
    if (self->vol_icon_)
        jetson::ui::SetAppIcon(self->vol_icon_, audible ? "speaker" : "speaker-mute", 22);
    self->SetStatus(audible ? "Bật tiếng" : "Tắt tiếng");
}

void SettingsView::OnOpenTerminalSettings(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (!self) return;
    self->application_page_ = ApplicationPage::Terminal;
    self->ShowCategory(Cat::Applications);
}

void SettingsView::OnApplicationsBack(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (!self) return;
    self->application_page_ = ApplicationPage::Main;
    self->ShowCategory(Cat::Applications);
}

void SettingsView::OnTerminalTextSmaller(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (!self) return;
    Settings terminal("terminal", true);
    const int current = std::clamp(
        terminal.GetInt("text_size", jetson::kDefaultTerminalTextSize),
        jetson::kMinTerminalTextSize, jetson::kMaxTerminalTextSize);
    const int next = std::max(jetson::kMinTerminalTextSize,
                              current - jetson::kTerminalTextSizeStep);
    terminal.SetInt("text_size", next);
    if (self->terminal_size_value_label_)
        lv_label_set_text_fmt(self->terminal_size_value_label_, "%d", next);
    TerminalView::RefreshOpenTerminals();
    self->SetStatus(("Cỡ chữ Terminal: " + std::to_string(next)).c_str());
}

void SettingsView::OnTerminalTextLarger(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (!self) return;
    Settings terminal("terminal", true);
    const int current = std::clamp(
        terminal.GetInt("text_size", jetson::kDefaultTerminalTextSize),
        jetson::kMinTerminalTextSize, jetson::kMaxTerminalTextSize);
    const int next = std::min(jetson::kMaxTerminalTextSize,
                              current + jetson::kTerminalTextSizeStep);
    terminal.SetInt("text_size", next);
    if (self->terminal_size_value_label_)
        lv_label_set_text_fmt(self->terminal_size_value_label_, "%d", next);
    TerminalView::RefreshOpenTerminals();
    self->SetStatus(("Cỡ chữ Terminal: " + std::to_string(next)).c_str());
}

void SettingsView::OnTerminalThemeSelected(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<ThemeCtx *>(lv_event_get_user_data(e));
    if (!ctx || !ctx->self) return;
    auto *self = ctx->self;
    const std::string theme_id = ctx->theme_id;
    const auto &theme = jetson::FindTerminalTheme(theme_id);
    Settings("terminal", true).SetString("theme", theme_id);
    TerminalView::RefreshOpenTerminals();
    self->SetStatus((std::string("Terminal theme: ") + theme.name).c_str());
    self->ShowCategory(Cat::Applications);
}

void SettingsView::OnAirplaneSwitch(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (!self || !self->airplane_switch_) return;
    const bool enabled = lv_obj_has_state(self->airplane_switch_, LV_STATE_CHECKED);

    if (self->airplane_busy_.exchange(true)) {
        self->AirplaneRefreshUi();
        return;
    }
    if (self->wifi_busy_.load() || self->bt_busy_.load()) {
        self->airplane_busy_ = false;
        self->AirplaneRefreshUi();
        const char *message = "Vui lòng đợi thao tác WiFi/Bluetooth hoàn tất";
        self->SetStatus(message);
        self->Notify(message);
        return;
    }

    // Optimistic UI: show the user's choice at once (the switch stays disabled
    // while busy). The worker below reverts it if the transition fails.
    self->airplane_enabled_ = enabled;
    self->AirplaneRefreshUi();
    self->SetStatus(enabled ? "Đang bật Plan mode..."
                            : "Đang tắt Plan mode...");
    std::thread([self = self->Self(), enabled]() {
        auto result = jetson::SetAirplaneMode(enabled, self->wifi_, self->bluetooth_);

        LvLockGuard lock;
        self->airplane_busy_ = false;
        self->airplane_enabled_ = result.enabled;
        self->wifi_enabled_ = result.wifi_enabled;
        self->bt_powered_ = result.bluetooth_powered;
        self->wifi_nets_.clear();
        self->bt_devs_.clear();
        self->wifi_scanned_ = result.enabled;
        self->bt_scanned_ = result.enabled;
        self->AirplaneRefreshUi();

        if (self->current_ == Cat::Wifi || self->current_ == Cat::Bluetooth)
            self->ShowCategory(self->current_);

        std::string message;
        if (enabled && result.success) {
            message = "Đã bật Plan mode — WiFi và Bluetooth đã tắt";
        } else if (enabled) {
            message = "Không thể bật: một radio vẫn đang hoạt động";
        } else if (result.success) {
            message = "Đã tắt Plan mode";
        } else {
            message = "Đã tắt Plan mode, nhưng chưa khôi phục đủ radio";
        }
        self->SetStatus(result.error.empty()
                            ? message.c_str()
                            : (message + ": " + result.error).c_str());
        self->Notify(message.c_str());
    }).detach();
}

void SettingsView::OnVpnSwitch(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (!self || !self->vpn_switch_) return;
    const bool enabled = lv_obj_has_state(self->vpn_switch_, LV_STATE_CHECKED);

    if (enabled && (self->airplane_enabled_ || self->airplane_busy_.load() ||
                    jetson::IsAirplaneModeEnabled())) {
        self->vpn_enabled_ = false;
        self->VpnRefreshUi();
        const char *message = "Tắt Plan mode trước khi bật VPN";
        self->SetStatus(message);
        self->Notify(message);
        return;
    }
    if (self->vpn_busy_.exchange(true)) {
        self->VpnRefreshUi();
        return;
    }

    self->vpn_enabled_ = enabled;
    self->VpnRefreshUi();
    self->SetStatus(enabled ? "Đang kết nối VPN..." : "Đang ngắt VPN...");
    std::thread([self = self->Self(), enabled]() {
        const auto result = jetson::VpnManager::Instance().SetEnabled(enabled);

        LvLockGuard lock;
        self->vpn_busy_ = false;
        self->vpn_enabled_ = result.enabled;
        self->VpnRefreshUi();

        std::string status;
        std::string notification;
        if (result.success && result.enabled) {
            status = "VPN đã kết nối qua " +
                     jetson::VpnManager::Instance().ExitNode();
            notification = "Kết nối VPN thành công";
        } else if (result.success) {
            status = "VPN đã ngắt kết nối";
            notification = "Đã ngắt kết nối VPN";
        } else {
            status = result.error.empty() ? "Không thể thay đổi trạng thái VPN"
                                          : result.error;
            // Surface the actionable backend reason (not installed, not
            // logged in, exit node unavailable) in the Dynamic Island.
            notification = status;
        }
        self->SetStatus(status.c_str());
        self->Notify(notification.c_str(), result.success ? 2800 : 5000);
    }).detach();
}

void SettingsView::OnWifiSwitch(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (self->airplane_enabled_ || self->airplane_busy_.load() ||
        jetson::IsAirplaneModeEnabled()) {
        self->WifiRefreshSwitch();
        self->SetStatus("Tắt Plan mode trước khi bật WiFi");
        return;
    }
    bool on = lv_obj_has_state(self->wifi_switch_, LV_STATE_CHECKED);
    if (self->wifi_busy_.exchange(true)) {
        self->WifiRefreshSwitch();
        return;
    }
    // Optimistic UI: keep the switch where the user put it; revert on failure.
    self->wifi_enabled_ = on;
    auto previous_nets = self->wifi_nets_;
    self->wifi_nets_.clear();
    self->wifi_scanned_ = false;
    self->WifiRefreshSwitch();
    if (self->wifi_list_) self->WifiRenderList();
    if (on) {
        self->SetStatus("Đang quét WiFi...");
        self->Notify("Đang quét WiFi...", 30000);
    } else {
        self->SetStatus("Đang tắt WiFi...");
    }
    std::thread([self = self->Self(), on,
                 previous_nets = std::move(previous_nets)]() mutable {
        bool ok = self->wifi_.Enable(on);
        std::vector<jetson::WifiNetwork> nets;
        std::string active;
        if (ok && on) {
            nets = self->wifi_.Scan();
            active = self->wifi_.ActiveSsid();
        }
        std::string error = self->wifi_.LastError();
        const bool scan_failed = ok && on && nets.empty() && !error.empty();
        if (scan_failed) {
            self->wifi_.Enable(false);
            ok = false;
        }
        DispatchToLvgl([self, on, ok, scan_failed, nets = std::move(nets),
                        active = std::move(active), error = std::move(error),
                        previous_nets = std::move(previous_nets)]() mutable {
            self->wifi_busy_ = false;
            if (ok) {
                self->wifi_enabled_ = on;
                self->wifi_nets_ = on ? std::move(nets)
                                      : std::vector<jetson::WifiNetwork>{};
                self->wifi_scanned_ = on;
                self->WifiRefreshSwitch();
                if (self->wifi_list_) self->WifiRenderList();
                if (on) {
                    self->SetStatus(active.empty() ? "Chạm mạng để kết nối"
                                                   : ("Đã kết nối: " + active).c_str());
                    const std::string message = self->wifi_nets_.empty()
                                                    ? "Không tìm thấy mạng WiFi"
                                                    : "Đã tìm thấy " +
                                                          std::to_string(self->wifi_nets_.size()) +
                                                          " mạng WiFi";
                    self->Notify(message.c_str());
                } else {
                    self->SetStatus("Đã tắt WiFi");
                }
            } else {
                self->wifi_enabled_ = on ? false : true;
                self->wifi_nets_ = on ? std::vector<jetson::WifiNetwork>{}
                                      : std::move(previous_nets);
                self->wifi_scanned_ = false;
                self->WifiRefreshSwitch();
                if (self->wifi_list_) self->WifiRenderList();
                const std::string message =
                    (scan_failed ? "Lỗi quét WiFi: " : "Lỗi WiFi: ") + error;
                self->SetStatus(message.c_str());
                self->Notify(message.c_str());
                ESP_LOGE(TAG, "WiFi %s failed: %s",
                         scan_failed ? "scan" : "power change", error.c_str());
            }
        });
    }).detach();
}

void SettingsView::OnWifiRescan(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->WifiRescan();
}

void SettingsView::OnWifiRowClicked(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<WifiRowCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    const auto &network = ctx->network;
    if (network.in_use) return;
    if (network.secured && !network.known)
        ctx->self->WifiOpenConnectSheet(network);
    else
        // A saved profile connects silently; if its stored password has gone
        // stale, WifiDoConnect falls back to asking for a new one.
        ctx->self->WifiDoConnect(network.ssid, "", network.secured);
}

void SettingsView::OnWifiInfoClicked(lv_event_t *e) {
    LvLockGuard lock;
    lv_event_stop_bubbling(e);
    auto *ctx = static_cast<WifiRowCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    ctx->self->WifiLoadDetails(ctx->network);
}

void SettingsView::OnBtSwitch(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (self->airplane_enabled_ || self->airplane_busy_.load() ||
        jetson::IsAirplaneModeEnabled()) {
        self->BtRefreshSwitch();
        const char *message = "Tắt Plan mode trước khi bật Bluetooth";
        self->SetStatus(message);
        self->Notify(message);
        return;
    }
    bool on = lv_obj_has_state(self->bt_switch_, LV_STATE_CHECKED);
    if (self->bt_busy_.exchange(true)) {
        self->BtRefreshSwitch();
        return;
    }
    // Optimistic UI: keep the switch where the user put it; revert on failure.
    self->bt_powered_ = on;
    self->bt_scanned_ = false;
    auto previous_devs = self->bt_devs_;
    self->bt_devs_.clear();
    self->BtRefreshSwitch();
    if (self->bt_list_) self->BtRenderList();
    if (on) {
        self->SetStatus("Đang tìm kiếm thiết bị Bluetooth...");
        self->Notify("Đang tìm kiếm thiết bị Bluetooth...", 30000);
    } else {
        self->SetStatus("Đang tắt Bluetooth...");
    }

    std::thread([self = self->Self(), on, previous_devs = std::move(previous_devs)]() mutable {
        bool ok = on ? self->bluetooth_.PowerOn() : self->bluetooth_.PowerOff();
        std::vector<jetson::BtDevice> devs;
        if (ok && on) devs = self->bluetooth_.Scan(8);
        std::string error = self->bluetooth_.LastError();
        // Power-on succeeded but discovery itself can still fail.  Do not
        // present a blank list as success when BlueZ supplied an error.
        const bool scan_failed = ok && on && !error.empty();
        if (scan_failed) {
            self->bluetooth_.PowerOff();
            ok = false;
        }
        DispatchToLvgl([self, on, ok, scan_failed, devs = std::move(devs), error,
                        previous_devs = std::move(previous_devs)]() mutable {
            self->bt_busy_ = false;
            if (ok) {
                self->bt_powered_ = on;
                self->bt_devs_ = std::move(devs);
                self->bt_scanned_ = on;
                self->BtRefreshSwitch();
                if (self->bt_list_) self->BtRenderList();
                if (on) {
                    self->SetStatus("Chạm thiết bị để kết nối, bấm (i) để ngắt/quên");
                    if (self->notification_cb_) {
                        const std::string message = self->bt_devs_.empty()
                                                        ? "Không tìm thấy thiết bị Bluetooth"
                                                        : "Đã tìm thấy " +
                                                              std::to_string(self->bt_devs_.size()) +
                                                              " thiết bị Bluetooth";
                        self->Notify(message.c_str());
                    }
                } else {
                    self->SetStatus("Đã tắt Bluetooth");
                }
            } else {
                self->bt_powered_ = !on; // revert the optimistic toggle
                self->bt_devs_ = on ? std::vector<jetson::BtDevice>{}
                                    : std::move(previous_devs);
                self->bt_scanned_ = false;
                self->BtRefreshSwitch();
                if (self->bt_list_) self->BtRenderList();
                const std::string message =
                    (scan_failed ? "Lỗi quét Bluetooth: " : "Lỗi Bluetooth: ") + error;
                self->SetStatus(message.c_str());
                self->Notify(message.c_str());
                ESP_LOGE(TAG, "Bluetooth power change failed: %s", error.c_str());
            }
        });
    }).detach();
}

void SettingsView::OnBtRescan(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->BtRescan();
}

void SettingsView::OnBtRowClicked(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<BtRowCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    const auto &device = ctx->device;
    // Mirror the WiFi list: tapping the connected device is a no-op (use the
    // circled i to disconnect/forget), tapping anything else offers to connect.
    if (device.connected) return;
    ctx->self->BtOpenConnectSheet(device);
}

void SettingsView::OnBtInfoClicked(lv_event_t *e) {
    LvLockGuard lock;
    lv_event_stop_bubbling(e);
    auto *ctx = static_cast<BtRowCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    ctx->self->BtOpenDetails(ctx->device);
}

void SettingsView::OnLangVi(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    Settings("input", true).SetString("kbd_lang", "vi");
    self->ShowCategory(Cat::General);
}
void SettingsView::OnLangEn(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    Settings("input", true).SetString("kbd_lang", "en");
    self->ShowCategory(Cat::General);
}

void SettingsView::OnGeneralBack(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    switch (self->general_page_) {
        case GeneralPage::LanguagePicker:
        case GeneralPage::RegionPicker:
        case GeneralPage::Calendar:
            self->general_page_ = GeneralPage::LanguageRegion;
            break;
        case GeneralPage::SystemFonts:
        case GeneralPage::MyFonts:
        case GeneralPage::CloudFonts:
            self->general_page_ = GeneralPage::Fonts;
            break;
        case GeneralPage::LockTimeout:
            self->general_page_ = GeneralPage::Power;
            break;
        default:
            self->general_page_ = GeneralPage::Main;
            break;
    }
    self->ShowCategory(Cat::General);
}

#define GENERAL_NAV_HANDLER(name, page)                                      \
    void SettingsView::name(lv_event_t *e) {                                \
        LvLockGuard lock;                                                     \
        auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));  \
        self->general_page_ = GeneralPage::page;                              \
        self->ShowCategory(Cat::General);                                     \
    }

GENERAL_NAV_HANDLER(OnOpenGeneralKeyboard, Keyboard)
GENERAL_NAV_HANDLER(OnOpenLanguageRegion, LanguageRegion)
GENERAL_NAV_HANDLER(OnOpenLanguagePicker, LanguagePicker)
GENERAL_NAV_HANDLER(OnOpenRegionPicker, RegionPicker)
GENERAL_NAV_HANDLER(OnOpenCalendarPicker, Calendar)
GENERAL_NAV_HANDLER(OnOpenGeneralDateTime, DateTime)
GENERAL_NAV_HANDLER(OnOpenFonts, Fonts)
GENERAL_NAV_HANDLER(OnOpenSystemFonts, SystemFonts)
GENERAL_NAV_HANDLER(OnOpenMyFonts, MyFonts)
GENERAL_NAV_HANDLER(OnOpenCloudFonts, CloudFonts)
GENERAL_NAV_HANDLER(OnOpenGeneralPower, Power)
GENERAL_NAV_HANDLER(OnOpenGeneralLockTimeout, LockTimeout)
GENERAL_NAV_HANDLER(OnOpenGeneralFan, Fan)
GENERAL_NAV_HANDLER(OnOpenGeneralAbout, About)

#undef GENERAL_NAV_HANDLER

void SettingsView::OnFanModeSelected(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<OptCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    auto mode = jetson::fan::Mode::Auto;
    if (ctx->value == "manual") mode = jetson::fan::Mode::Manual;
    else if (ctx->value == "off") mode = jetson::fan::Mode::Off;
    jetson::fan::SetMode(mode);
    ctx->self->SetStatus((std::string("Quạt: ") + jetson::fan::ModeLabel(mode)).c_str());
    // Rebuild so the checkmark moves and the slider enables/disables with the
    // new mode. This also tears down and restarts the readout timer.
    ctx->self->ShowCategory(Cat::General);
}

void SettingsView::OnFanProfileSelected(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<OptCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    auto profile = jetson::fan::Profile::Quiet;
    if (ctx->value == "cool") profile = jetson::fan::Profile::Cool;
    else if (ctx->value == "balanced") profile = jetson::fan::Profile::Balanced;
    jetson::fan::SetProfile(profile);
    ctx->self->SetStatus(
        (std::string("Hồ sơ quạt: ") + jetson::fan::ProfileLabel(profile)).c_str());
    ctx->self->ShowCategory(Cat::General);
}

void SettingsView::OnFanSpeedChanged(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (!self->fan_slider_) return;
    int pct = lv_slider_get_value(self->fan_slider_);
    if (pct < jetson::fan::kMinPercent) {
        pct = jetson::fan::kMinPercent;
        lv_slider_set_value(self->fan_slider_, pct, LV_ANIM_OFF);
    }
    jetson::fan::SetManualPwm(jetson::fan::PercentToPwm(pct));
    if (self->fan_value_label_) {
        char value[16];
        std::snprintf(value, sizeof(value), "%d%%", pct);
        lv_label_set_text(self->fan_value_label_, value);
    }
    self->SetStatus(("Quạt: " + std::to_string(pct) + "%").c_str());
}

void SettingsView::OnFanPollTick(lv_timer_t *t) {
    static_cast<SettingsView *>(lv_timer_get_user_data(t))->RefreshFanReadout();
}

void SettingsView::OnLanguageSelected(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<OptCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    Settings("system", true).SetString("language", ctx->value);
    for (const auto &option : kLanguages) {
        if (ctx->value == option.code) {
            Settings("input", true).SetString("kbd_lang", option.input_lang);
            break;
        }
    }
    ctx->self->general_page_ = GeneralPage::LanguageRegion;
    ctx->self->ShowCategory(Cat::General);
}

void SettingsView::OnRegionSelected(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<OptCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    Settings("system", true).SetString("region", ctx->value);
    ctx->self->general_page_ = GeneralPage::LanguageRegion;
    ctx->self->ShowCategory(Cat::General);
}

void SettingsView::OnCalendarSelected(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<OptCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    Settings("system", true).SetString("calendar", ctx->value);
    ctx->self->general_page_ = GeneralPage::LanguageRegion;
    ctx->self->ShowCategory(Cat::General);
}

void SettingsView::On24hToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("clock_24h", on);
    self->SetStatus(on ? "Định dạng 24h" : "Định dạng 12h");
    self->ShowCategory(Cat::General);
}

void SettingsView::OnAutoTimeToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("system", true).SetBool("auto_time", on);
    self->SetStatus(on ? "Đang bật đồng bộ thời gian mạng..."
                       : "Đã tắt đồng bộ thời gian mạng");
    self->ShowCategory(Cat::General);
    std::thread([self = self->Self(), on]() {
        auto result = jetson::platform::RunShellCommand(
            std::string("timedatectl set-ntp ") + (on ? "true" : "false"));
        LvLockGuard lock;
        if (self) self->SetStatus(result.Ok() ? (on ? "Đã bật đồng bộ thời gian mạng"
                                                    : "Đã tắt đồng bộ thời gian mạng")
                                                : "Không thể thay đổi đồng bộ thời gian");
    }).detach();
}

void SettingsView::OnTzSelected(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<OptCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    std::string tz = ctx->value;
    ctx->self->SetStatus(("Đang đặt múi giờ " + tz + "...").c_str());
    std::thread([self = ctx->self->Self(), tz]() {
        RunCapture("timedatectl set-timezone " + jetson::platform::QuoteShellArgument(tz));
        Settings("system", true).SetString("timezone", tz);
        LvLockGuard lock;
        if (self) {
            self->SetStatus(("Múi giờ: " + tz).c_str());
            self->ShowCategory(Cat::General);
        }
    }).detach();
}

void SettingsView::OnSleepSelected(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<OptCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    int secs = 0;
    try { secs = std::stoi(ctx->value); } catch (...) { secs = 0; }
    Settings("display", true).SetInt("sleep_timeout", secs);
    ctx->self->SetStatus(secs == 0 ? "Tự tắt: Không"
                                   : ("Tự tắt sau " + std::to_string(secs) + "s").c_str());
    if (ctx->self->current_ == Cat::Display &&
        ctx->self->display_page_ == DisplayPage::AutoLock) {
        ctx->self->ShowCategory(Cat::Display);
    } else {
        ctx->self->general_page_ = GeneralPage::Power;
        ctx->self->ShowCategory(Cat::General);
    }
}

void SettingsView::OnFontSelected(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<FontCtx *>(lv_event_get_user_data(e));
    if (!ctx || ctx->self->font_busy_.load()) return;
    const FontCtx font = *ctx;
    if (FileExistsLocal(font.regular_path)) {
        if (jetson::ApplyBuiltinFontFamily(font.name, font.regular_path, font.bold_path)) {
            font.self->SetFontStatus("Đã áp dụng " + font.name + ".");
            font.self->ShowCategory(Cat::General);
        }
        return;
    }
    if (!font.regular_object.empty()) font.self->DownloadAndApplyFont(font);
}

void SettingsView::OnRefreshFontCatalog(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->RefreshCloudFontCatalog();
    self->ShowCategory(Cat::General);
}

void SettingsView::OnLockNow(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (self->lock_cb_) self->lock_cb_();
}

void SettingsView::OnSetPin(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->OpenPinModal();
}

void SettingsView::OnReboot(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->OpenConfirmModal("Khởi động lại?", "Thiết bị sẽ khởi động lại ngay.", []() {
        std::thread([]() { sync(); int r = system("reboot"); (void)r; }).detach();
    });
}

void SettingsView::OnShutdown(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->OpenConfirmModal("Tắt máy?", "Thiết bị sẽ tắt ngay.", []() {
        std::thread([]() { sync(); int r = system("poweroff"); (void)r; }).detach();
    });
}

void SettingsView::OnPopupDismiss(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (lv_event_get_target(e) == self->popup_) self->CloseModal();
}

void SettingsView::OnModalClose(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->CloseModal();
}

void SettingsView::OnModalConnect(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    std::string ssid = self->modal_ssid_;
    std::string pw = self->popup_input_ ? self->popup_input_->Text() : "";
    if (self->popup_input_ && pw.empty()) return;
    self->CloseModal();
    self->WifiDoConnect(ssid, pw);
}

void SettingsView::OnModalPasswordChanged(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (!self->popup_confirm_btn_ || !self->popup_input_) return;
    const bool ready = !self->popup_input_->Text().empty();
    const auto &p = jetson::UiTheme::Instance().Palette();
    lv_obj_set_style_bg_color(self->popup_confirm_btn_,
                              Color(ready ? p.accent : p.button), 0);
    lv_obj_set_style_bg_opa(self->popup_confirm_btn_,
                            ready ? LV_OPA_COVER : LV_OPA_60, 0);
    if (ready) lv_obj_clear_state(self->popup_confirm_btn_, LV_STATE_DISABLED);
    else lv_obj_add_state(self->popup_confirm_btn_, LV_STATE_DISABLED);
}

void SettingsView::OnModalForget(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    std::string ssid = self->modal_ssid_;
    self->CloseModal();
    self->WifiDoForget(ssid);
}

void SettingsView::OnModalBtAction(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    std::string addr = self->modal_bt_addr_;
    bool connected = self->modal_bt_connected_;
    self->CloseModal();
    self->BtDoAction(addr, connected);
}

void SettingsView::OnModalBtRemove(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    std::string addr = self->modal_bt_addr_;
    self->CloseModal();
    self->BtDoRemove(addr);
}

void SettingsView::OnModalConfirmYes(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    auto yes = std::move(self->modal_yes_);
    self->CloseModal();
    if (yes) yes();
}

void SettingsView::OnPinSave(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (!self->pin_a_ || !self->pin_b_) return;
    std::string a = self->pin_a_->Text();
    std::string b = self->pin_b_->Text();
    if (a.size() != 4 || a != b) {
        self->SetStatus("PIN không hợp lệ hoặc không khớp (cần 4 ký tự)");
        return;
    }
    Settings("system", true).SetString("pin", a);
    self->SetStatus("Đã lưu PIN");
    self->CloseModal();
    if (self->current_ == Cat::General && self->general_page_ == GeneralPage::Power)
        self->ShowCategory(Cat::General);
}

void SettingsView::OnPinClear(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    Settings("system", true).SetString("pin", "");
    self->SetStatus("Đã xóa PIN");
    self->CloseModal();
    if (self->current_ == Cat::General && self->general_page_ == GeneralPage::Power)
        self->ShowCategory(Cat::General);
}

} // namespace home
