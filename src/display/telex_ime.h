#pragma once

/* Vietnamese Telex IME + a small LVGL text-input widget that owns its key
 * handling.
 *
 * TelexIme is a pure (no-LVGL) transform: feed it ASCII keystrokes and it
 * composes Vietnamese with precomposed UTF-8 (e.g. "as" -> "á", "aw" -> "ă",
 * "dd" -> "đ", "ows" -> "ớ"). It implements the standard Telex letter
 * transforms (aw/aa/dd/ee/oo/ow/uw) and the five tone marks (s f r x j) placed
 * on the last vowel of the current word. It is a practical subset, not a full
 * IME — already-toned vowels are not re-toggled, but the common cases work.
 *
 * TelexInput is an lv_obj-based widget (NOT lv_textarea) so it fully owns key
 * handling and never double-inserts. It is added to the keypad group so the USB
 * keyboard types into it; printable keys go through TelexIme, Backspace deletes
 * one codepoint, Enter emits LV_EVENT_READY. SetTelex(false) makes it a plain
 * ASCII field (used for passwords / PINs, which must not be transformed).
 *
 * Terminal is intentionally NOT converted to TelexInput: it is a PTY shell and
 * transforming keystrokes would corrupt commands. */

#include <lvgl.h>
#include <cstddef>
#include <string>

namespace home {

class TelexIme {
public:
    TelexIme() = default;

    void SetEnabled(bool on) { enabled_ = on; }
    bool Enabled() const { return enabled_; }

    // Feed one keystroke (ASCII). In Telex mode the char may transform the
    // tail of buf_ instead of appending.
    void Feed(char c);
    // Delete the last UTF-8 codepoint.
    void Backspace();
    void Clear() { buf_.clear(); }
    const std::string &Text() const { return buf_; }
    void SetMaxLen(size_t n) { max_len_ = n; }

private:
    std::string buf_;
    bool enabled_ = false;
    size_t max_len_ = 64;

    void AppendCodepoint(const std::string &cp);
    // Replace the last UTF-8 codepoint of buf_ with `rep`. No-op if buf_ empty.
    void ReplaceLast(const std::string &rep);
};

class TelexInput {
public:
    TelexInput(lv_obj_t *parent, int width, int height);
    ~TelexInput();

    lv_obj_t *obj() const { return root_; }

    void SetTelex(bool on) { ime_.SetEnabled(on); }
    void SetPassword(bool on) { password_ = on; Refresh(); }
    void SetMaxLen(size_t n) { ime_.SetMaxLen(n); }
    void SetPlaceholder(const char *text);
    void Clear() { ime_.Clear(); Refresh(); }
    const std::string &Text() const { return ime_.Text(); }
    void Focus();

private:
    lv_obj_t *root_ = nullptr;   // clickable container, member of the keypad group
    lv_obj_t *label_ = nullptr;
    std::string placeholder_;
    TelexIme ime_;
    bool password_ = false;

    void Refresh();  // repaint label_ from ime_.Text() (or dots if password_)

    static void OnKey(lv_event_t *e);
    static void OnClicked(lv_event_t *e);
    static void OnDeleted(lv_event_t *e);
};

} // namespace home