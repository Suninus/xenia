/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_XINPUT_XINPUT_DRIVER_H_
#define XENIA_HID_XINPUT_XINPUT_DRIVER_H_

#include "xenia/common.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/nop/nop_hid-private.h"

namespace xe {
namespace hid {
namespace xinput {

class XInputInputDriver : public InputDriver {
 public:
  XInputInputDriver(InputSystem* input_system);
  ~XInputInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

 protected:
};

}  // namespace xinput
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_XINPUT_XINPUT_DRIVER_H_
