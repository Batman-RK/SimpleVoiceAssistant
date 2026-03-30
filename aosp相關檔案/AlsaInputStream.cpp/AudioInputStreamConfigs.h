////////////////////////////////////////////////////////////////////////////////
//
// (c) 2022 Realtek Semiconductor Corp. All rights reserved.
//
////////////////////////////////////////////////////////////////////////////////
#ifndef __REALTEK_8F5C95C3_03DC_4969_9049_735E57218FE3_H__
#define __REALTEK_8F5C95C3_03DC_4969_9049_735E57218FE3_H__

#include "AlsaInputStream.h"

namespace rtk::media::audio {

typedef enum {
  USECASE_CAP_ECHOREF,
  USECASE_CAP_RECORD, /*common record usecase*/
  USECASE_CAP_MMAP,
  USECASE_CAP_AO,
  USECASE_CAP_AI,
  USECASE_CAP_DMIC,
  USECASE_CAP_NUM
} InUseCase;

extern const AlsaInputStream::Configuration kConfigInputStream[];
extern const std::vector<Capabilities> kCapsInputStream[];

static constexpr uint32_t kPrimaryInputDevices = static_cast<uint32_t>(
    (AUDIO_DEVICE_IN_ECHO_REFERENCE | AUDIO_DEVICE_IN_BUILTIN_MIC |
     AUDIO_DEVICE_IN_BACK_MIC | AUDIO_DEVICE_IN_WIRED_HEADSET |
     AUDIO_DEVICE_IN_HDMI_ARC | AUDIO_DEVICE_IN_LOOPBACK) &
    ~AUDIO_DEVICE_BIT_IN);

static constexpr uint32_t kUsbInputDevices = static_cast<uint32_t>(
    (AUDIO_DEVICE_IN_USB_DEVICE | AUDIO_DEVICE_IN_USB_HEADSET) &
    ~AUDIO_DEVICE_BIT_IN);

static constexpr uint32_t kA2dpInputDevices = static_cast<uint32_t>(
    AUDIO_DEVICE_IN_BLUETOOTH_A2DP & ~AUDIO_DEVICE_BIT_IN);

}  // namespace rtk::media::audio

#endif /* __REALTEK_8F5C95C3_03DC_4969_9049_735E57218FE3_H__ */