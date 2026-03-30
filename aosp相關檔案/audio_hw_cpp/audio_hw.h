/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef RTK_AUDIO_HW_H
#define RTK_AUDIO_HW_H

#include <cutils/list.h>

#include <hardware/audio.h>
#include <tinyalsa/asoundlib.h>
#include "KaraokeConfig.h"

#define TUNNELED_PLAYBACK

#ifdef TUNNELED_PLAYBACK
#include "AudioRPC_System.h"
#endif

#if PLATFORM_SDK_VERSION > 28
#define A2DP_ENABLED        /* for after starting Q*/
#define USB_ENABLED
#endif

#define ADSP_MUTE 100
#define ADSP_0DB  389

typedef enum OutputTypeTag {
    OUTPUT_LOW_LATENCY,   // low latency output stream
    OUTPUT_DEEP_BUF,      // deep PCM buffers output stream
    OUTPUT_DIRECT_TUNNEL,
    OUTPUT_AAUDIO,
    OUTPUT_OFFLOAD,
    OUTPUT_A2DP_LOW_LATENCY,
    OUTPUT_A2DP_DEEP_BUF,
    OUTPUT_A2DP_DIRECT,
    OUTPUT_DIRECT_NONTUNNEL
} OutputTypeE;

// pcm type should be fixed as follow.
typedef enum PcmTypeTag {
    PCM_LOW_LATENCY = 0,
    PCM_DEEP_BUFFER,
    PCM_DIRECT_TUNNEL,
    PCM_AAUDIO,
    PCM_AAUDIO_2,
    PCM_DIRECT_NONTUNNEL
} PcmTypeE;

typedef enum AudioDelayTag {
    // The next index must be matched with audio_fw, DON'T change.
    DELAY_NON_TUNNEL_PCM = 0,               // value base on ms
    DELAY_NON_TUNNEL_OFFLOAD = 1,           // value base on ms
    DELAY_TUNNEL_PCM = 2,                   // value base on 90K
    DELAY_TUNNEL_OFFLOAD = 3,               // value base on 90K
    DELAY_NON_TUNNEL_PCM_DDP2CH = 4,        // value base on ms
    DELAY_TUNNEL_PCM_DDP2CH = 5,            // value base on 90K

    // The next index is followed for A2DP
    DELAY_NON_TUNNEL_PCM_BT,                // value base on ms
    DELAY_NON_TUNNEL_OFFLOAD_BT,            // value base on ms
    DELAY_TUNNEL_PCM_BT,                    // value base on 90K
    DELAY_TUNNEL_OFFLOAD_BT,                // value base on 90K
    DELAY_NON_TUNNEL_PCM_DDP2CH_BT,         // value base on ms
    DELAY_TUNNEL_PCM_DDP2CH_BT,             // value base on 90K

    // The next index is followed for USB
    DELAY_NON_TUNNEL_PCM_USB,                // value base on ms
    DELAY_NON_TUNNEL_OFFLOAD_USB,            // value base on ms
    DELAY_TUNNEL_PCM_USB,                    // value base on 90K
    DELAY_TUNNEL_OFFLOAD_USB,                // value base on 90K
    DELAY_NON_TUNNEL_PCM_DDP2CH_USB,         // value base on ms
    DELAY_TUNNEL_PCM_DDP2CH_USB,             // value base on 90K
    DELAY_NON_TUNNEL_PCM51CH,                // value base on ms
    DELAY_TUNNEL_PCM51CH,                    // value base on 90K

    // for karaoke
    DELAY_NON_TUNNEL_PCM_KARAOKE,           // value base on ms
    DELAY_TUNNEL_PCM_KARAOKE,               // value base on ms
    DELAY_NON_TUNNEL_OFFLOAD_KARAOKE,       // value base on ms
    DELAY_TUNNEL_OFFLOAD_KARAOKE,           // value base on ms

    //PTS offset for TUNNEL PCM
    DELAY_TUNNEL_PCM_SYSTIME_OFFSET,                // value base on ms
    DELAY_TUNNEL_PCM_SYSTIME_OFFSET_ARC,                    // value base on ms
    DELAY_NON_TUNNEL_PCM_SYSTIME_OFFSET,            // value base on ms
    DELAY_NON_TUNNEL_OFFLOAD_SYSTIME_OFFSET,        // value base on ms

    //extra delay
    DELAY_ENABLE_FIXED_DEFER_START,                 // if >0, apply below extra_delay value, boolean
    DELAY_TUNNEL_PCM_FIXED_DEFER_START,             // value base on ms

    //dynamic defer start
    DELAY_TUNNEL_PCM_DYNAMIC_DEFER_START,           // value base on ms
    DELAY_TUNNEL_OFFLOAD_DYNAMIC_DEFER_START,       // value base on ms

    DELAY_MAX,
} AudioDelayE;

static const char *AudioDelayTagToString[] = {
    // The next index must be matched with audio_fw, DON'T change.
    "DELAY_NON_TUNNEL_PCM",        // value base on ms
    "DELAY_NON_TUNNEL_OFFLOAD",    // value base on ms
    "DELAY_TUNNEL_PCM",            // value base on 90K
    "DELAY_TUNNEL_OFFLOAD",        // value base on 90K
    "DELAY_NON_TUNNEL_PCM_DDP2CH", // value base on ms
    "DELAY_TUNNEL_PCM_DDP2CH",     // value base on 90K

    // The next index is followed for A2DP
    "DELAY_NON_TUNNEL_PCM_BT",        // value base on ms
    "DELAY_NON_TUNNEL_OFFLOAD_BT",    // value base on ms
    "DELAY_TUNNEL_PCM_BT",            // value base on 90K
    "DELAY_TUNNEL_OFFLOAD_BT",        // value base on 90K
    "DELAY_NON_TUNNEL_PCM_DDP2CH_BT", // value base on ms
    "DELAY_TUNNEL_PCM_DDP2CH_BT",     // value base on 90K

    // The next index is followed for USB
    "DELAY_NON_TUNNEL_PCM_USB",        // value base on ms
    "DELAY_NON_TUNNEL_OFFLOAD_USB",    // value base on ms
    "DELAY_TUNNEL_PCM_USB",            // value base on 90K
    "DELAY_TUNNEL_OFFLOAD_USB",        // value base on 90K
    "DELAY_NON_TUNNEL_PCM_DDP2CH_USB", // value base on ms
    "DELAY_TUNNEL_PCM_DDP2CH_USB",     // value base on 90K
    "DELAY_NON_TUNNEL_PCM51CH",        // value base on ms
    "DELAY_TUNNEL_PCM51CH",            // value base on 90K
    // for karaoke
    "DELAY_NON_TUNNEL_PCM_KARAOKE",           // value base on ms
    "DELAY_TUNNEL_PCM_KARAOKE",               // value base on ms
    "DELAY_NON_TUNNEL_OFFLOAD_KARAOKE",       // value base on ms
    "DELAY_TUNNEL_OFFLOAD_KARAOKE",           // value base on ms
    //PTS offset for TUNNEL PCM
    "DELAY_TUNNEL_PCM_SYSTIME_OFFSET",                // value base on ms
    "DELAY_TUNNEL_PCM_SYSTIME_OFFSET_ARC",            // value base on ms
    "DELAY_NON_TUNNEL_PCM_SYSTIME_OFFSET",            // value base on ms
    "DELAY_NON_TUNNEL_OFFLOAD_SYSTIME_OFFSET",        // value base on ms
    //extra delay
    "DELAY_ENABLE_FIXED_DEFER_START",                 // if >0, apply below extra_delay value, boolean
    "DELAY_TUNNEL_PCM_FIXED_DEFER_START",             // value base on ms
    //dynamic defer start
    "DELAY_TUNNEL_PCM_DYNAMIC_DEFER_START",           // value base on ms
    "DELAY_TUNNEL_OFFLOAD_DYNAMIC_DEFER_START",       // value base on ms

    "DELAY_MAX",
};

#define DSP389_VOLUME_MIN (DELAY_MAX+100)   // value base on 10000

/* For AAUDIO */
struct snd_mmap_data {
    int dev;
    int fd;
};

#define SNDRV_PCM_IOCTL_MMAP_DATA_FD _IOWR('U', 0xd2, struct snd_mmap_data)

/* for adev parameter */
typedef enum params_type {
    PARAM_IS_MS12,
    PARAM_MAX_AAC_CHANNEL,
    PARAM_DMX_MODE,
    PARAM_SAMPLE_RATE,
    PARAM_CHANNEL_CONFIG,
    PARAM_IS_DUALMONO,
    PARAM_DUALMONO_SETTING
} params_type;

typedef enum dualmono_mode {
    MAIN_AND_SUB,
    MAIN_ONLY,
    SUB_ONLY
} dualmono_mode;

/* for adev parameter */
typedef enum dmx_mode {
    DMX_LTRT,
    DMX_LORO,
    DMX_ARIB
} dmx_mode;

/* Key Value */
/* control by audio driver */
#define AUDIO_PARAMETER_IS_AVAILABLE_MS12       "is_available_ms12"
#define AUDIO_PARAMETER_SUPPORTED_CHANNEL_COUNT "supported_channel_count"
#define AUDIO_PARAMETER_DMX_MODE                "dmx_mode"
#define AUDIO_PARAMETER_STREAM_DOLBY_ATMOS_LOCK "hdmi_dolby_atmos_lock"
/* control by offload track */
#define AUDIO_PARAMETER_SAMPLE_RATE             "sample_rate"
#define AUDIO_PARAMETER_CHANNEL_CONFIG          "channel_config"
#define AUDIO_PARAMETER_IS_DUALMONO             "is_dualmono"
#define AUDIO_PARAMETER_DUALMONO_SETTING        "dualmono_setting"

#define AUDIO_PARAMETER_SPEC_MMAP_PERIOD_SIZE   "spec_mmap_period_size"
#define AUDIO_PARAMETER_SPEC_VOCAL_CANCELLATION "spec_vocal_cancellation"
#define AUDIO_PARAMETER_SPEC_KARAOKE_STATE      "spec_karaoke_state"

#define AUDIO_PARAMETER_USB_KARAOKE_MODE        "usb_karaoke_mode"
#define AUDIO_PARAMETER_USB_KARAOKE_VOLUME      "usb_karaoke_volume"

/*************************************************************************/

struct audio_device {
    struct audio_hw_device hw_device;
    hw_module_t const* target_module;
    audio_hw_device_t* target_device;

#ifdef A2DP_ENABLED
    hw_module_t const* a2dp_module;
    audio_hw_device_t* a2dp_device;
    audio_stream_out* a2dp_stream_out;
    audio_stream_in* a2dp_stream_in;
#endif

#ifdef USB_ENABLED
    hw_module_t const* usb_module;
    audio_hw_device_t* usb_device;
    audio_stream_out* usb_stream_out;
    audio_stream_in* usb_stream_in;
#endif

    pthread_mutex_t audio_device_lock;

    int out_device;
    audio_devices_t in_device;
    struct stream_in* active_input;
    bool mic_mute;
    bool bluetooth_nrec;
    bool screen_off;
    int AO_open_count;

    /* auto detect ALSA card number */
    int alsa_card;
    void* patch_manager;

    int parameter_atmos_lock;

    void* offload_handle;
    int dmx_mode;
    int dualmono_mode;
    int is_m12_platform;
    int max_aac_channel;

    int aaudio_count;
    int mSpecMmapPeriodSize;
    int mSpecVocalCancellation;

    struct listnode stream_out_list;
    bool delayUnmute;

    KaraokeConfig* pKaraokeConfig;
};

#endif // RTK_AUDIO_HW_H
