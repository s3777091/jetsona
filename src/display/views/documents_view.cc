#include "display/views/documents_view.h"
#include "display/common/lvgl_utils.h"
#include "fonts.h"
#include "display/theme/ui_theme.h"
#include "esp_log.h"

#include <lvgl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace home {

#define TAG "DocumentsView"

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {
bool CaseLess(const std::string &a, const std::string &b) {
    auto cmp = [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) <
               std::tolower(static_cast<unsigned char>(y));
    };
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), cmp);
}

bool IsDirectory(const std::string &path) {
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string LowerExtension(const std::string &name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return {};

    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

struct ProgrammingIconRule {
    const char *extension;
    const char *filename;
};

// Map source-file extensions to the 74x74 PNG set in assets/icons/programming.
// Some languages share an extension (for example MATLAB/Objective-C use .m and
// V/Verilog use .v); use the most common choice and retain unambiguous aliases.
const char *ProgrammingIconFor(const std::string &name) {
    const std::string ext = LowerExtension(name);
    if (ext.empty()) return nullptr;

    static constexpr ProgrammingIconRule kRules[] = {
        {"adb", "ada.png"}, {"ads", "ada.png"}, {"ada", "ada.png"},
        {"asm", "assembly.png"}, {"s", "assembly.png"},
        {"as", "assemblyscript.png"},
        {"bash", "bash.png"},
        {"c", "c.png"}, {"h", "c.png"},
        {"clj", "clojure.png"}, {"cljs", "clojure.png"},
        {"cljc", "clojure.png"}, {"edn", "clojure.png"},
        {"cob", "cobol.png"}, {"cbl", "cobol.png"},
        {"coffee", "coffeescript.png"}, {"cson", "coffeescript.png"},
        {"iced", "coffeescript.png"},
        {"lisp", "common-lisp.png"}, {"lsp", "common-lisp.png"},
        {"cl", "common-lisp.png"},
        {"cc", "cplusplus.png"}, {"cpp", "cplusplus.png"},
        {"cxx", "cplusplus.png"}, {"hh", "cplusplus.png"},
        {"hpp", "cplusplus.png"}, {"hxx", "cplusplus.png"},
        {"ipp", "cplusplus.png"}, {"tpp", "cplusplus.png"},
        {"cs", "csharp.png"}, {"csx", "csharp.png"},
        {"cr", "crystal.png"},
        {"css", "css.png"},
        {"d", "d.png"}, {"di", "d.png"},
        {"dart", "dart.png"},
        {"dpr", "delphi.png"}, {"dproj", "delphi.png"},
        {"ex", "elixir.png"}, {"exs", "elixir.png"},
        {"elm", "elm.png"},
        {"erl", "erlang.png"}, {"hrl", "erlang.png"},
        {"f", "fortran.png"}, {"for", "fortran.png"},
        {"f77", "fortran.png"}, {"f90", "fortran.png"},
        {"f95", "fortran.png"}, {"f03", "fortran.png"},
        {"fs", "fsharp.png"}, {"fsx", "fsharp.png"}, {"fsi", "fsharp.png"},
        {"gd", "gdscript.png"},
        {"go", "go.png"},
        {"graphql", "graphql.png"}, {"gql", "graphql.png"},
        {"groovy", "groovy.png"}, {"gvy", "groovy.png"},
        {"gradle", "groovy.png"},
        {"hs", "haskell.png"}, {"lhs", "haskell.png"},
        {"hx", "haxe.png"}, {"hxml", "haxe.png"},
        {"html", "html5.png"}, {"htm", "html5.png"},
        {"xhtml", "html5.png"},
        {"java", "java.png"}, {"jar", "java.png"},
        {"js", "javascript.png"}, {"mjs", "javascript.png"},
        {"cjs", "javascript.png"}, {"jsx", "javascript.png"},
        {"jl", "julia.png"},
        {"kt", "kotlin.png"}, {"kts", "kotlin.png"},
        {"vi", "labview.png"}, {"vim", "labview.png"},
        {"tex", "latex.png"}, {"ltx", "latex.png"},
        {"sty", "latex.png"}, {"cls", "latex.png"},
        {"lua", "lua.png"},
        {"m", "matlab.png"},
        {"nim", "nim.png"}, {"nims", "nim.png"}, {"nimble", "nim.png"},
        {"nix", "nix.png"},
        {"mm", "objective-c.png"},
        {"ml", "ocaml.png"}, {"mli", "ocaml.png"},
        {"mll", "ocaml.png"}, {"mly", "ocaml.png"},
        {"pas", "pascal.png"}, {"pp", "pascal.png"},
        {"pl", "perl.png"}, {"pm", "perl.png"}, {"t", "perl.png"},
        {"php", "php.png"},
        {"ps1", "powershell.png"}, {"psm1", "powershell.png"},
        {"psd1", "powershell.png"},
        {"pro", "prolog.png"}, {"prolog", "prolog.png"},
        {"purs", "purescript.png"},
        {"py", "python.png"}, {"pyw", "python.png"},
        {"pyi", "python.png"}, {"pyx", "python.png"}, {"pyc", "python.png"},
        {"qs", "qsharp.png"},
        {"r", "r.png"},
        {"rkt", "racket.png"}, {"rktd", "racket.png"},
        {"rktl", "racket.png"},
        {"re", "reason.png"}, {"rei", "reason.png"},
        {"rb", "ruby.png"}, {"rake", "ruby.png"}, {"gemspec", "ruby.png"},
        {"rs", "rust.png"},
        {"sass", "sass.png"}, {"scss", "sass.png"},
        {"scala", "scala.png"}, {"sc", "scala.png"},
        {"scm", "scheme.png"}, {"ss", "scheme.png"},
        {"sb", "scratch.png"}, {"sb2", "scratch.png"}, {"sb3", "scratch.png"},
        {"sh", "shell.png"}, {"zsh", "shell.png"},
        {"ksh", "shell.png"}, {"fish", "shell.png"},
        {"sol", "solidity.png"},
        {"sql", "sql.png"},
        {"swift", "swift.png"},
        {"tcl", "tcl.png"}, {"tk", "tcl.png"},
        {"ts", "typescript.png"}, {"tsx", "typescript.png"},
        {"mts", "typescript.png"}, {"cts", "typescript.png"},
        {"v", "v.png"}, {"vsh", "v.png"},
        {"vala", "vala.png"}, {"vapi", "vala.png"},
        {"vb", "vbdotnet.png"}, {"vbs", "vbdotnet.png"},
        {"vh", "verilog.png"}, {"sv", "verilog.png"}, {"svh", "verilog.png"},
        {"vhd", "vhdl.png"}, {"vhdl", "vhdl.png"},
        {"wasm", "webassembly.png"}, {"wat", "webassembly.png"},
        {"wl", "wolfram.png"}, {"nb", "wolfram.png"},
        {"xml", "xml.png"}, {"xsd", "xml.png"},
        {"xsl", "xml.png"}, {"xslt", "xml.png"},
        {"yml", "yaml.png"}, {"yaml", "yaml.png"},
        {"zig", "zig.png"},
    };

    for (const auto &rule : kRules) {
        if (ext == rule.extension) return rule.filename;
    }
    return nullptr;
}

constexpr char kProgrammingIconDirectory[] = "assets/icons/programming/";
constexpr int kProgrammingIconScale = 50 * 256 / 74;

// Non-programming files retain the coloured file-type badge fallback.
struct FileBadge { uint32_t color; std::string label; };

FileBadge BadgeFor(const std::string &name) {
    const std::string ext = LowerExtension(name);

    // Images.
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" ||
        ext == "bmp" || ext == "svg" || ext == "webp" || ext == "ico" ||
        ext == "tiff" || ext == "tif") return {0x34A853, "IMG"};
    // Video.
    if (ext == "mp4" || ext == "mkv" || ext == "mov" || ext == "avi" ||
        ext == "webm" || ext == "m4v" || ext == "flv" || ext == "wmv")
        return {0x8E44AD, "VID"};
    // Audio.
    if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "ogg" ||
        ext == "aac" || ext == "m4a" || ext == "wma") return {0xE67E22, "AUD"};
    // Archives.
    if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "rar" ||
        ext == "7z" || ext == "xz" || ext == "bz2" || ext == "tgz")
        return {0xF39C12, "ZIP"};
    // Documents.
    if (ext == "pdf") return {0xE5392A, "PDF"};
    if (ext == "doc" || ext == "docx") return {0x2B579A, "DOC"};
    if (ext == "xls" || ext == "xlsx") return {0x217346, "XLS"};
    if (ext == "ppt" || ext == "pptx") return {0xD24726, "PPT"};
    if (ext == "txt" || ext == "log" || ext == "conf" || ext == "ini" ||
        ext == "cfg") return {0x84848a, "TXT"};
    // Code.
    if (ext == "js" || ext == "mjs" || ext == "cjs" || ext == "jsx")
        return {0xF7DF1E, "JS"};
    if (ext == "ts" || ext == "tsx") return {0x3178C6, "TS"};
    if (ext == "py" || ext == "pyw" || ext == "pyc") return {0x3776AB, "PY"};
    if (ext == "c") return {0x5C6BC0, "C"};
    if (ext == "h") return {0x5C6BC0, "H"};
    if (ext == "cc" || ext == "cpp" || ext == "cxx" || ext == "hpp" || ext == "hxx")
        return {0x00599C, "C++"};
    if (ext == "rs") return {0xDEA584, "RS"};
    if (ext == "go") return {0x00ADD8, "GO"};
    if (ext == "java") return {0xE76F00, "JAVA"};
    if (ext == "jar") return {0xE76F00, "JAR"};
    if (ext == "html" || ext == "htm") return {0xE34F26, "HTML"};
    if (ext == "css" || ext == "scss" || ext == "sass") return {0x2965F1, "CSS"};
    if (ext == "json") return {0x3a3a3a, "JSON"};
    if (ext == "xml") return {0x0060AF, "XML"};
    if (ext == "md" || ext == "markdown") return {0x519ABA, "MD"};
    if (ext == "csv") return {0x1B8E2B, "CSV"};
    if (ext == "yml" || ext == "yaml") return {0xCB171E, "YML"};
    if (ext == "sh" || ext == "bash" || ext == "zsh") return {0x4EAA25, "SH"};
    if (ext == "iso" || ext == "img" || ext == "bin") return {0x6b7280, "BIN"};

    // Unknown: show the real (uppercased) extension on a neutral chip.
    std::string up;
    for (unsigned char c : ext) {
        up.push_back((char)std::toupper(c));
        if (up.size() == 4) break;
    }
    if (up.empty()) up = "FILE";
    return {0x6b7280, std::move(up)};
}

// Pick dark text on light badges (e.g. JS yellow) so the label stays readable.
bool LightBadge(uint32_t rgb) {
    int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return (r * 299 + g * 587 + b * 114) / 1000 > 150;
}

// The service often runs as root, so HOME points at /root even though the real
// desktop files live in /home/<user>.  Prefer an explicit override, then the
// login user, and finally the first real home directory on the device.
std::string ResolveFilesHome() {
    if (const char *p = std::getenv("JETSON_FILES_HOME"); p && IsDirectory(p)) return p;

    const char *user = std::getenv("SUDO_USER");
    if (!user || !*user || std::strcmp(user, "root") == 0) user = std::getenv("USER");
    if (user && *user && std::strcmp(user, "root") != 0) {
        if (struct passwd *pw = ::getpwnam(user); pw && pw->pw_dir && IsDirectory(pw->pw_dir)) {
            return pw->pw_dir;
        }
    }

    if (const char *h = std::getenv("HOME"); h && *h && std::strcmp(h, "/root") != 0 &&
                                             IsDirectory(h)) {
        return h;
    }

    if (DIR *homes = ::opendir("/home")) {
        std::string found;
        while (dirent *de = ::readdir(homes)) {
            if (de->d_name[0] == '.') continue;
            std::string candidate = std::string("/home/") + de->d_name;
            if (IsDirectory(candidate)) { found = std::move(candidate); break; }
        }
        ::closedir(homes);
        if (!found.empty()) return found;
    }

    if (const char *h = std::getenv("HOME"); h && IsDirectory(h)) return h;
    return "/";
}
} // namespace

DocumentsView::DocumentsView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Tệp", std::move(on_closed)) {
    root_path_ = ResolveFilesHome();
    current_path_ = root_path_;
    history_.push_back(current_path_);
    hist_idx_ = 0;
    body_w_ = width_ - 16;
    body_h_ = (height_ - kHeaderHeight) - 16;
    ESP_LOGI(TAG, "opening file browser at %s", root_path_.c_str());
    BuildBody();
    Rescan();
    BuildGrid();
}

DocumentsView::~DocumentsView() {
    LvglLockGuard lock;
    if (nav_timer_) { lv_timer_del(nav_timer_); nav_timer_ = nullptr; }
    if (popup_) { lv_obj_del(popup_); popup_ = nullptr; popup_card_ = nullptr; }
    // Delete image widgets before programming_icon_cache_ releases the PNG
    // descriptors they reference. The base class later deletes the empty grid.
    if (grid_) lv_obj_clean(grid_);
    cells_.clear();
    ctxs_.clear();
}

std::string DocumentsView::BaseName(const std::string &path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string DocumentsView::FormatSize(long bytes) {
    char buf[24];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%ld B", bytes);
    } else if (bytes < 1024L * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024L * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return buf;
}

std::string DocumentsView::ExtUpper(const std::string &name) {
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return "FILE";
    std::string ext = name.substr(dot + 1);
    if (ext.size() > 4) ext = ext.substr(0, 4);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return ext;
}

void DocumentsView::BuildBody() {
    const auto &p = jetson::UiTheme::Instance().Palette();

    // Stack the toolbar above the grid; be explicit so the layout does not
    // depend on LVGL's default object arrangement.
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body_, 8, 0);

    BuildToolbar();

    grid_ = lv_obj_create(body_);
    lv_obj_remove_style_all(grid_);
    lv_obj_set_size(grid_, body_w_, body_h_ - 44 - 8);
    lv_obj_set_flex_flow(grid_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid_, 10, 0);
    lv_obj_set_style_pad_column(grid_, 10, 0);
    lv_obj_add_flag(grid_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(grid_, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(grid_, LV_DIR_VER);
}

void DocumentsView::BuildToolbar() {
    const auto &p = jetson::UiTheme::Instance().Palette();

    toolbar_ = lv_obj_create(body_);
    lv_obj_remove_style_all(toolbar_);
    lv_obj_set_size(toolbar_, body_w_, 44);
    lv_obj_set_flex_flow(toolbar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(toolbar_, 8, 0);
    lv_obj_clear_flag(toolbar_, LV_OBJ_FLAG_SCROLLABLE);

    auto mk_arrow = [&](lv_obj_t **out, const char *glyph, lv_event_cb_t cb) {
        auto *b = lv_button_create(toolbar_);
        lv_obj_set_size(b, 40, 36);
        lv_obj_set_style_bg_color(b, Color(p.button), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        auto *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(l, Color(p.text), 0);
        lv_label_set_text(l, glyph);
        lv_obj_center(l);
        *out = b;
    };
    mk_arrow(&back_btn_, LV_SYMBOL_LEFT, OnBack);
    mk_arrow(&fwd_btn_, LV_SYMBOL_RIGHT, OnForward);

    path_label_ = lv_label_create(toolbar_);
    lv_obj_set_style_text_font(path_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(path_label_, Color(p.text), 0);
    lv_label_set_long_mode(path_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(path_label_, 1);
    lv_obj_set_style_text_align(path_label_, LV_TEXT_ALIGN_LEFT, 0);
    UpdatePathLabel();
    UpdateNavButtons();
}

void DocumentsView::ClearGrid() {
    // Also removes the dedicated empty-state label (which is not a file cell).
    if (grid_) lv_obj_clean(grid_);
    cells_.clear();
    ctxs_.clear();
}

void DocumentsView::BuildGrid() {
    ClearGrid();
    const auto &p = jetson::UiTheme::Instance().Palette();
    constexpr int gap = 10;
    const int columns = std::max(4, (body_w_ + gap) / (118 + gap));
    const int cellW = std::max(92, (body_w_ - gap * (columns - 1)) / columns);
    const int cellH = 106;

    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto &e = entries_[i];
        auto *cell = lv_obj_create(grid_);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, cellW, cellH);
        lv_obj_set_style_radius(cell, 10, 0);
        lv_obj_set_style_bg_color(cell, Color(p.row), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(cell, true, 0);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cell, 4, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

        // Glyph (folder shape vs. file tile with the extension).
        auto *glyph = lv_obj_create(cell);
        lv_obj_remove_style_all(glyph);
        lv_obj_set_size(glyph, 56, 56);
        lv_obj_clear_flag(glyph, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                                 LV_OBJ_FLAG_CLICKABLE));
        if (e.is_dir) {
            // Folder: a blue body with a small tab on the top-left.
            auto *body = lv_obj_create(glyph);
            lv_obj_remove_style_all(body);
            lv_obj_set_size(body, 54, 40);
            lv_obj_set_style_radius(body, 6, 0);
            lv_obj_set_style_bg_color(body, Color(0x3dabf5), 0);
            lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
            lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_clear_flag(body, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                                    LV_OBJ_FLAG_CLICKABLE));
            auto *tab = lv_obj_create(glyph);
            lv_obj_remove_style_all(tab);
            lv_obj_set_size(tab, 24, 10);
            lv_obj_set_style_radius(tab, 4, 0);
            lv_obj_set_style_bg_color(tab, Color(0x3dabf5), 0);
            lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
            lv_obj_align(tab, LV_ALIGN_TOP_LEFT, 1, 4);
            lv_obj_clear_flag(tab, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                                   LV_OBJ_FLAG_CLICKABLE));
        } else {
            // Source code uses the matching language artwork from the bundled
            // programming icon set. If an asset is missing (or this is not a
            // programming extension), fall back to the existing file badge.
            const char *programming_filename = ProgrammingIconFor(e.name);
            LvglImage *programming_icon = LoadProgrammingIcon(programming_filename);
            if (programming_icon) {
                auto *icon = lv_image_create(glyph);
                lv_image_set_src(icon, programming_icon->image_dsc());
                lv_image_set_scale(icon, kProgrammingIconScale);
                lv_obj_center(icon);
                lv_obj_clear_flag(icon, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                                        LV_OBJ_FLAG_CLICKABLE));
            } else {
                const FileBadge fb = BadgeFor(e.name);
                auto *tile = lv_obj_create(glyph);
                lv_obj_remove_style_all(tile);
                lv_obj_set_size(tile, 56, 54);
                lv_obj_set_style_radius(tile, 8, 0);
                lv_obj_set_style_bg_color(tile, Color(fb.color), 0);
                lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
                lv_obj_align(tile, LV_ALIGN_CENTER, 0, 0);
                lv_obj_clear_flag(tile, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                                        LV_OBJ_FLAG_CLICKABLE));
                auto *ext = lv_label_create(tile);
                // 24px font keeps 4-char labels (PDF/JSON/FILE) inside the chip.
                lv_obj_set_style_text_font(ext, &BUILTIN_ICON_FONT, 0);
                lv_obj_set_style_text_color(ext,
                    LightBadge(fb.color) ? Color(0x222222) : lv_color_white(), 0);
                lv_label_set_text(ext, fb.label.c_str());
                lv_obj_center(ext);
            }
        }

        // Filename (wrap to ~2 lines, clip to the cell width).
        auto *name = lv_label_create(cell);
        lv_obj_set_style_text_font(name, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(name, Color(p.sub_text), 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(name, cellW - 6);
        lv_obj_set_height(name, 34);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(name, e.name.c_str());

        auto *ctx = new CellCtx{this, i};
        lv_obj_add_event_cb(cell, OnEntryClicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(cell, OnEntryDeleted, LV_EVENT_DELETE, ctx);
        cells_.push_back(cell);
        ctxs_.push_back(ctx);
    }

    if (entries_.empty()) {
        auto *empty = lv_label_create(grid_);
        lv_obj_set_style_text_font(empty, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(empty, Color(p.sub_text), 0);
        lv_obj_set_width(empty, body_w_ - 16);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(empty, "Thư mục này đang trống");
        SetStatus("");
    } else {
        SetStatus("");
    }
}

void DocumentsView::Rescan() {
    // Caller holds the LVGL lock (DisplayLockGuard during construction, or
    // LvglLockGuard from the click handlers), so it is not re-taken here.
    entries_.clear();
    DIR *d = opendir(current_path_.c_str());
    if (!d) {
        ESP_LOGE(TAG, "cannot open directory: %s", current_path_.c_str());
        SetStatus("Không mở được thư mục");
        return;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        if (!name.empty() && name[0] == '.') continue; // skip hidden
        std::string full = current_path_ + "/" + name;
        struct stat st;
        bool is_dir = false;
        long sz = 0;
        if (::stat(full.c_str(), &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            sz = (long)st.st_size;
        } else {
#ifdef DT_DIR
            is_dir = (de->d_type == DT_DIR);
#endif
        }
        entries_.push_back({name, is_dir, sz});
    }
    closedir(d);
    std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
        if (a.is_dir != b.is_dir) return a.is_dir; // folders first
        return CaseLess(a.name, b.name);
    });
    ESP_LOGI(TAG, "listed %zu entries in %s", entries_.size(), current_path_.c_str());
}

void DocumentsView::NavigateTo(const std::string &path, bool push_history) {
    ESP_LOGI(TAG, "navigate to %s", path.c_str());
    if (push_history) {
        history_.resize(hist_idx_ + 1);
        history_.push_back(path);
        hist_idx_ = history_.size() - 1;
    }
    current_path_ = path;
    Rescan();
    BuildGrid();
    UpdatePathLabel();
    UpdateNavButtons();
}

void DocumentsView::ScheduleNavigate(const std::string &path) {
    // Defer so the clicked cell's event finishes before BuildGrid() frees it.
    if (nav_timer_) { lv_timer_del(nav_timer_); nav_timer_ = nullptr; }
    nav_pending_ = path;
    nav_timer_ = lv_timer_create(OnNavTimer, 0, this);
    lv_timer_set_repeat_count(nav_timer_, 1);
}

void DocumentsView::GoBack() {
    if (hist_idx_ == 0) return;
    hist_idx_--;
    NavigateTo(history_[hist_idx_], false);
}

void DocumentsView::GoForward() {
    if (hist_idx_ + 1 >= history_.size()) return;
    hist_idx_++;
    NavigateTo(history_[hist_idx_], false);
}

void DocumentsView::UpdateNavButtons() {
    if (back_btn_) {
        if (hist_idx_ > 0) lv_obj_clear_state(back_btn_, LV_STATE_DISABLED);
        else lv_obj_add_state(back_btn_, LV_STATE_DISABLED);
    }
    if (fwd_btn_) {
        if (hist_idx_ + 1 < history_.size()) lv_obj_clear_state(fwd_btn_, LV_STATE_DISABLED);
        else lv_obj_add_state(fwd_btn_, LV_STATE_DISABLED);
    }
}

void DocumentsView::UpdatePathLabel() {
    if (!path_label_) return;
    std::string label = (current_path_ == root_path_) ? "Home" : BaseName(current_path_);
    lv_label_set_text(path_label_, label.c_str());
}

LvglImage *DocumentsView::LoadProgrammingIcon(const char *filename) {
    if (!filename || !*filename) return nullptr;

    auto [it, inserted] = programming_icon_cache_.try_emplace(filename);
    if (inserted) {
        it->second = LvglImageFromFile(std::string(kProgrammingIconDirectory) + filename);
    }
    return it->second.get();
}

void DocumentsView::OpenPopup(size_t index) {
    if (popup_) ClosePopup();
    if (index >= entries_.size()) return;
    popup_index_ = index;
    const auto &p = jetson::UiTheme::Instance().Palette();
    const Entry &e = entries_[index];

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, 260, 150);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 16, 0);
    lv_obj_set_style_pad_all(popup_card_, 14, 0);
    lv_obj_set_style_pad_row(popup_card_, 6, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(popup_card_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto *name = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(name, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(name, Color(p.text), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 232);
    lv_label_set_text(name, e.name.c_str());

    char line[64];
    std::snprintf(line, sizeof(line), "Loại: %s", e.is_dir ? "Thư mục" : ExtUpper(e.name).c_str());
    auto *type = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(type, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(type, Color(p.sub_text), 0);
    lv_label_set_text(type, line);

    if (!e.is_dir) {
        std::snprintf(line, sizeof(line), "Dung lượng: %s", FormatSize(e.size).c_str());
    } else {
        std::snprintf(line, sizeof(line), "Dung lượng: —");
    }
    auto *size = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(size, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(size, Color(p.sub_text), 0);
    lv_label_set_text(size, line);

    auto *close = lv_button_create(popup_card_);
    lv_obj_set_width(close, 120);
    lv_obj_set_style_bg_color(close, Color(p.accent), 0);
    lv_obj_set_style_radius(close, 10, 0);
    lv_obj_add_event_cb(close, OnPopupClose, LV_EVENT_CLICKED, this);
    auto *cl = lv_label_create(close);
    lv_obj_set_style_text_font(cl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_label_set_text(cl, "Đóng");
    lv_obj_center(cl);
}

void DocumentsView::ClosePopup() {
    if (popup_) { lv_obj_del(popup_); popup_ = nullptr; popup_card_ = nullptr; }
}

void DocumentsView::OnStart() {
    // Folder cells are directly clickable; no permanent instruction strip.
    SetStatus("");
}

void DocumentsView::OnResize(int w, int h) {
    // Runs under the base class's lock (OnZoomBtn -> ToggleZoom -> OnResize),
    // so the lock is not re-taken here.
    body_w_ = w - 16;
    body_h_ = h - 16;
    if (toolbar_) lv_obj_set_width(toolbar_, body_w_);
    if (grid_) lv_obj_set_size(grid_, body_w_, body_h_ - 44 - 8);
    // Recalculate touch-target widths for the new window size.
    BuildGrid();
}

void DocumentsView::OnBack(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<DocumentsView *>(lv_event_get_user_data(e));
    self->GoBack();
}
void DocumentsView::OnForward(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<DocumentsView *>(lv_event_get_user_data(e));
    self->GoForward();
}

void DocumentsView::OnEntryClicked(lv_event_t *e) {
    LvglLockGuard lock;
    auto *ctx = static_cast<CellCtx *>(lv_event_get_user_data(e));
    auto *self = ctx->self;
    if (ctx->index >= self->entries_.size()) return;
    const Entry &en = self->entries_[ctx->index];
    if (en.is_dir) {
        // Defer: navigating rebuilds the grid and frees this cell, which must
        // not happen while its CLICKED event is still being delivered.
        self->ScheduleNavigate(self->current_path_ + "/" + en.name);
    } else {
        self->OpenPopup(ctx->index);
    }
}

void DocumentsView::OnNavTimer(lv_timer_t *t) {
    LvglLockGuard lock;
    auto *self = static_cast<DocumentsView *>(lv_timer_get_user_data(t));
    lv_timer_del(t);
    self->nav_timer_ = nullptr;
    self->NavigateTo(self->nav_pending_, true);
}

void DocumentsView::OnEntryDeleted(lv_event_t *e) {
    auto *ctx = static_cast<CellCtx *>(lv_event_get_user_data(e));
    delete ctx;
}

void DocumentsView::OnPopupDismiss(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<DocumentsView *>(lv_event_get_user_data(e));
    // Only dismiss when the click landed on the backdrop, not the card/button.
    if (lv_event_get_target(e) == self->popup_) self->ClosePopup();
}

void DocumentsView::OnPopupClose(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<DocumentsView *>(lv_event_get_user_data(e));
    self->ClosePopup();
}

} // namespace home
