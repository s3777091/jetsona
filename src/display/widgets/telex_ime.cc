#include "display/widgets/telex_ime.h"
#include "display/common/lvgl_utils.h"
#include "fonts.h"
#include "lvgl_runtime.h"

#include <cctype>
#include <cstring>
#include <unordered_map>

namespace home {

namespace {

using jetson::ui::Color;

// ---- UTF-8 helpers (Vietnamese precomposed chars are 2- or 3-byte) ----

// Length of the codepoint that ENDS at byte offset `end` (exclusive).
size_t LastCpLen(const std::string &b, size_t end) {
    if (end == 0) return 0;
    size_t i = end - 1;
    while (i > 0 && (static_cast<unsigned char>(b[i]) & 0xC0) == 0x80) --i;
    return end - i;
}

size_t CodepointCount(const std::string &b) {
    size_t n = 0;
    for (unsigned char c : b) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

bool IsAsciiAlpha(const std::string &cp) {
    return cp.size() == 1 && std::isalpha((unsigned char)cp[0]);
}

// Vietnamese letters are all multi-byte; anything >= 0x80 we treat as a letter.
bool IsLetter(const std::string &cp) {
    if (cp.empty()) return false;
    if (cp[0] & 0x80) return true; // Vietnamese precomposed -> letter
    return std::isalpha((unsigned char)cp[0]) != 0;
}

// The 12 vowel bases that can carry a tone.
bool IsVowel(const std::string &cp) {
    static const char *v[] = {
        "a", "\xc4\x83", "\xc3\xa2",         // a ă â
        "e", "\xc3\xaa",                      // e ê
        "i", "o", "\xc3\xb4", "\xc6\xa1",     // i o ô ơ
        "u", "\xc6\xb0", "y"                  // u ư y
    };
    for (auto s : v) if (cp == s) return true;
    return false;
}

// Tone index: s=0 (acute), f=1 (grave), r=2 (hook), x=3 (tilde), j=4 (dot).
const char *ToneFor(const std::string &base, int ti) {
    static const struct { const char *base; const char *t[5]; } table[] = {
        {"a",   {"\xc3\xa1","\xc3\xa0","\xe1\xba\xa3","\xc3\xa3","\xe1\xba\xa1"}},
        {"\xc4\x83", {"\xe1\xba\xaf","\xe1\xba\xb1","\xe1\xba\xb3","\xe1\xba\xb5","\xe1\xba\xb7"}}, // ă
        {"\xc3\xa2", {"\xe1\xba\xa5","\xe1\xba\xa7","\xe1\xba\xa9","\xe1\xba\xab","\xe1\xba\xad"}}, // â
        {"e",   {"\xc3\xa9","\xc3\xa8","\xe1\xba\xbb","\xe1\xba\xbd","\xe1\xba\xb9"}},
        {"\xc3\xaa", {"\xe1\xba\xbf","\xe1\xbb\x81","\xe1\xbb\x83","\xe1\xbb\x85","\xe1\xbb\x87"}}, // ê
        {"i",   {"\xc3\xad","\xc3\xac","\xe1\xbb\x89","\xc4\xa9","\xe1\xbb\x8b"}},
        {"o",   {"\xc3\xb3","\xc3\xb2","\xe1\xbb\x8f","\xc3\xb5","\xe1\xbb\x8d"}},
        {"\xc3\xb4", {"\xe1\xbb\x91","\xe1\xbb\x93","\xe1\xbb\x95","\xe1\xbb\x97","\xe1\xbb\x99"}}, // ô
        {"\xc6\xa1", {"\xe1\xbb\x9b","\xe1\xbb\x9d","\xe1\xbb\x9f","\xe1\xbb\xa1","\xe1\xbb\xa3"}}, // ơ
        {"u",   {"\xc3\xba","\xc3\xb9","\xe1\xbb\xa7","\xc5\xa9","\xe1\xbb\xa5"}},
        {"\xc6\xb0", {"\xe1\xbb\xa9","\xe1\xbb\xab","\xe1\xbb\xad","\xe1\xbb\xaf","\xe1\xbb\xb1"}}, // ư
        {"y",   {"\xc3\xbd","\xe1\xbb\xb3","\xe1\xbb\xb7","\xe1\xbb\xb9","\xe1\xbb\xb5"}},
    };
    for (const auto &row : table) {
        if (base == row.base) {
            if (ti >= 0 && ti < 5) return row.t[ti];
            return nullptr;
        }
    }
    return nullptr;
}

// Offset of the last vowel base in buf_, scanning backward over letters.
// Returns npos if a non-letter is hit first (word boundary) or no vowel found.
size_t LastVowelOffset(const std::string &buf) {
    size_t end = buf.size();
    while (end > 0) {
        size_t len = LastCpLen(buf, end);
        size_t start = end - len;
        std::string cp = buf.substr(start, len);
        if (IsVowel(cp)) return start;
        if (!IsLetter(cp)) return std::string::npos;
        end = start;
    }
    return std::string::npos;
}

std::string LastCp(const std::string &buf) {
    if (buf.empty()) return "";
    size_t len = LastCpLen(buf, buf.size());
    return buf.substr(buf.size() - len, len);
}

} // namespace

// ---- TelexIme ----

void TelexIme::AppendCodepoint(const std::string &cp) {
    if (CodepointCount(buf_) >= max_len_) return;
    buf_ += cp;
}

void TelexIme::ReplaceLast(const std::string &rep) {
    if (buf_.empty()) return;
    size_t len = LastCpLen(buf_, buf_.size());
    buf_.replace(buf_.size() - len, len, rep);
}

void TelexIme::Feed(char c) {
    if (!enabled_) { AppendCodepoint(std::string(1, c)); return; }

    // Telex mode works in lowercase; output is lowercase Vietnamese.
    char lc = (char)std::tolower((unsigned char)c);

    // Tone marks: place on the last vowel of the current word.
    if (lc == 's' || lc == 'f' || lc == 'r' || lc == 'x' || lc == 'j') {
        int ti = (lc == 's') ? 0 : (lc == 'f') ? 1 : (lc == 'r') ? 2
                 : (lc == 'x') ? 3 : 4;
        size_t off = LastVowelOffset(buf_);
        if (off != std::string::npos) {
            // Length of the codepoint starting at off.
            unsigned char h = (unsigned char)buf_[off];
            size_t cplen = 1;
            if ((h & 0x80) != 0) {
                if ((h & 0xE0) == 0xC0) cplen = 2;
                else if ((h & 0xF0) == 0xE0) cplen = 3;
                else if ((h & 0xF8) == 0xF0) cplen = 4;
            }
            std::string cp = buf_.substr(off, cplen);
            const char *toned = ToneFor(cp, ti);
            if (toned) { buf_.replace(off, cplen, toned); return; }
        }
        AppendCodepoint(std::string(1, lc));
        return;
    }

    // 'w' transforms: a->ă, o->ơ, u->ư.
    if (lc == 'w') {
        std::string cp = LastCp(buf_);
        if (cp == "a") { ReplaceLast("\xc4\x83"); return; } // ă
        if (cp == "o") { ReplaceLast("\xc6\xa1"); return; } // ơ
        if (cp == "u") { ReplaceLast("\xc6\xb0"); return; } // ư
        AppendCodepoint("w");
        return;
    }

    // Doubling: aa->â, ee->ê, oo->ô, dd->đ.
    std::string cp = LastCp(buf_);
    if (cp.size() == 1 && cp[0] == lc) {
        if (lc == 'a') { ReplaceLast("\xc3\xa2"); return; } // â
        if (lc == 'e') { ReplaceLast("\xc3\xaa"); return; } // ê
        if (lc == 'o') { ReplaceLast("\xc3\xb4"); return; } // ô
        if (lc == 'd') { ReplaceLast("\xc4\x91"); return; } // đ
    }

    AppendCodepoint(std::string(1, lc));
}

void TelexIme::Backspace() {
    if (buf_.empty()) return;
    size_t len = LastCpLen(buf_, buf_.size());
    buf_.erase(buf_.size() - len);
}

// ---- TelexInput ----

TelexInput::TelexInput(lv_obj_t *parent, int w, int h) {
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, w, h);
    lv_obj_set_style_bg_color(root_, Color(0x2a2a2a), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(root_, Color(0x333333), 0);
    lv_obj_set_style_border_width(root_, 1, 0);
    lv_obj_set_style_radius(root_, 10, 0);
    lv_obj_set_style_pad_all(root_, 8, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE));

    label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label_, Color(0xe6e6e6), 0);
    lv_label_set_long_mode(label_, LV_LABEL_LONG_DOT);
    // Fill the root's content area whatever width the caller used (px or pct).
    lv_obj_set_width(label_, lv_pct(100));
    lv_obj_align(label_, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_add_event_cb(root_, OnKey, LV_EVENT_KEY, this);
    lv_obj_add_event_cb(root_, OnClicked, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(root_, OnDeleted, LV_EVENT_DELETE, this);

    if (auto *g = jetson::LvglRuntime::Instance().keypad_group()) {
        lv_group_add_obj(g, root_);
    }
    Refresh();
}

TelexInput::~TelexInput() {
    // root_ is either already being destroyed (OnDeleted -> delete this) or the
    // parent took it down; do not touch LVGL here.
}

void TelexInput::SetPlaceholder(const char *text) {
    placeholder_ = text ? text : "";
    Refresh();
}

void TelexInput::Focus() {
    if (root_) lv_group_focus_obj(root_);
}

void TelexInput::Refresh() {
    if (!label_) return;
    std::string display;
    if (password_) {
        display = std::string(CodepointCount(ime_.Text()), '*');
    } else {
        display = ime_.Text();
    }
    if (display.empty() && !placeholder_.empty()) {
        lv_label_set_text(label_, placeholder_.c_str());
        lv_obj_set_style_text_color(label_, Color(0x6e6e6e), 0);
    } else {
        lv_label_set_text(label_, display.c_str());
        lv_obj_set_style_text_color(label_, Color(0xe6e6e6), 0);
    }
}

void TelexInput::OnKey(lv_event_t *e) {
    auto *self = static_cast<TelexInput *>(lv_event_get_user_data(e));
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
        self->ime_.Backspace();
        self->Refresh();
    } else if (key == LV_KEY_ENTER) {
        lv_obj_send_event(self->root_, LV_EVENT_READY, nullptr);
    } else if (key >= 0x20 && key < 0x7F) {
        self->ime_.Feed((char)key);
        self->Refresh();
    }
}

void TelexInput::OnClicked(lv_event_t *e) {
    auto *self = static_cast<TelexInput *>(lv_event_get_user_data(e));
    if (self->root_) lv_group_focus_obj(self->root_);
}

void TelexInput::OnDeleted(lv_event_t *e) {
    auto *self = static_cast<TelexInput *>(lv_event_get_user_data(e));
    delete self;
}

} // namespace home
