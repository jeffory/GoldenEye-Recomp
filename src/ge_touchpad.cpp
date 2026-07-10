// ge - TouchPad singleton storage. See ge_touchpad.h.

#include "ge_touchpad.h"

namespace ge {

TouchPad& TouchPad::Get() {
  static TouchPad instance;
  return instance;
}

}  // namespace ge
