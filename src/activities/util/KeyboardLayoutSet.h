#pragma once

// Which keyboard layouts the language key cycles through, and which one a
// keyboard opens on.
//
// Ported from crosspoint-reader (PR #2858 by winst0niuss, with Uri Tauber).
// Reworked here without the persisted layout mask and its settings screen: the
// set is the UI language's layout plus English, so a reader whose interface is
// in Russian gets ЙЦУКЕН and QWERTY, and one in English gets QWERTY alone (and
// no language key, since there is nowhere to switch to). The upstream API is
// kept so the settings screen can be added later without touching the keyboard.
//
// Only the layouts our UI fonts can draw are listed. The SDK also has Hebrew and
// Arabic, but Inter UI carries neither script and the UI text path does no bidi
// or shaping, so offering them would put boxes on the keys.

#include <I18n.h>
#include <components/keyboard/keyboard.h>

#include <cstdint>

namespace keyboard_layouts {

struct LayoutInfo {
  freeink::ui::KeyboardLayoutId id;
  Language language;
};

// Table position is the bit a layout occupies in the enabled() mask.
inline constexpr LayoutInfo ALL[] = {
    {freeink::ui::KeyboardLayoutId::QwertyEn, Language::EN},
    {freeink::ui::KeyboardLayoutId::AzertyFr, Language::FR},
    {freeink::ui::KeyboardLayoutId::QwertzDe, Language::DE},
    {freeink::ui::KeyboardLayoutId::SpanishEs, Language::ES},
    {freeink::ui::KeyboardLayoutId::CyrillicRu, Language::RU},
    {freeink::ui::KeyboardLayoutId::CyrillicUk, Language::UK},
    {freeink::ui::KeyboardLayoutId::CyrillicBe, Language::BE},
    {freeink::ui::KeyboardLayoutId::CyrillicKk, Language::KK},
};
inline constexpr uint8_t COUNT = sizeof(ALL) / sizeof(ALL[0]);
static_assert(COUNT <= 16, "the enabled-layout mask is uint16_t");

// Bit i set: ALL[i] is reachable. Always includes English, which is what URL
// and password fields need whatever the UI language is.
uint16_t enabled();

// The UI language's layout (English when the language has none).
freeink::ui::KeyboardLayoutId startingLayout();

// The enabled layout after `current`, wrapping; `current` when it is the only one.
freeink::ui::KeyboardLayoutId next(freeink::ui::KeyboardLayoutId current);

}  // namespace keyboard_layouts
