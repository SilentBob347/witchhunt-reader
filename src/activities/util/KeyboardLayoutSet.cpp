#include "KeyboardLayoutSet.h"

namespace keyboard_layouts {
namespace {

constexpr uint16_t bitAt(const uint8_t i) { return static_cast<uint16_t>(1u << i); }

uint8_t indexOf(const freeink::ui::KeyboardLayoutId id) {
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (ALL[i].id == id) return i;
  }
  return COUNT;
}

freeink::ui::KeyboardLayoutId forLanguage(const Language language) {
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (ALL[i].language == language) return ALL[i].id;
  }
  return freeink::ui::KeyboardLayoutId::QwertyEn;
}

uint16_t layoutBit(const freeink::ui::KeyboardLayoutId id) {
  const uint8_t i = indexOf(id);
  return i < COUNT ? bitAt(i) : 0;
}

}  // namespace

uint16_t enabled() {
  // An English UI collapses to one layout, and the language key disappears.
  return static_cast<uint16_t>(layoutBit(forLanguage(I18N.getLanguage())) |
                               layoutBit(freeink::ui::KeyboardLayoutId::QwertyEn));
}

freeink::ui::KeyboardLayoutId startingLayout() { return forLanguage(I18N.getLanguage()); }

freeink::ui::KeyboardLayoutId next(const freeink::ui::KeyboardLayoutId current) {
  const uint16_t mask = enabled();
  const uint8_t from = indexOf(current);
  // A current layout the table does not list still has to lead somewhere.
  const uint8_t start = from < COUNT ? from : 0;
  for (uint8_t step = 1; step <= COUNT; ++step) {
    const uint8_t i = static_cast<uint8_t>((start + step) % COUNT);
    if (mask & bitAt(i)) return ALL[i].id;
  }
  return current;
}

}  // namespace keyboard_layouts
