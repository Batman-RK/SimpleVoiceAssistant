////////////////////////////////////////////////////////////////////////////////
//
// (c) 2022 Realtek Semiconductor Corp. All rights reserved.
//
////////////////////////////////////////////////////////////////////////////////

#include <tinyalsa/asoundlib.h>

#include "include/AudioInputStreamConfigs.h"

namespace rtk::media::audio {

#define DEFINE_ENUM_VALUE(type, name) \
  static const type name { #name, ::name }

DEFINE_ENUM_VALUE(Capabilities::AudioFormat, AUDIO_FORMAT_PCM_16_BIT);
DEFINE_ENUM_VALUE(Capabilities::AudioFormat, AUDIO_FORMAT_PCM_32_BIT);
DEFINE_ENUM_VALUE(Capabilities::ChannelMask, AUDIO_CHANNEL_IN_STEREO);

const std::vector<Capabilities> kCapsInputStream[USECASE_CAP_NUM] = {
    [USECASE_CAP_ECHOREF] =
        {{true, AUDIO_FORMAT_PCM_32_BIT, {16000}, {AUDIO_CHANNEL_IN_STEREO}}},
    [USECASE_CAP_RECORD] =
        {{true, AUDIO_FORMAT_PCM_32_BIT, {16000}, {AUDIO_CHANNEL_IN_STEREO}}},
    [USECASE_CAP_MMAP] =
        {{true, AUDIO_FORMAT_PCM_16_BIT, {48000}, {AUDIO_CHANNEL_IN_STEREO}}},
    [USECASE_CAP_AO] =
        {{true, AUDIO_FORMAT_PCM_16_BIT, {48000}, {AUDIO_CHANNEL_IN_STEREO}}},
    [USECASE_CAP_AI] =
        {{true, AUDIO_FORMAT_PCM_16_BIT, {48000}, {AUDIO_CHANNEL_IN_STEREO}},
         {true, AUDIO_FORMAT_PCM_32_BIT, {48000}, {AUDIO_CHANNEL_IN_STEREO}}},
    [USECASE_CAP_DMIC] =
        {{true, AUDIO_FORMAT_PCM_16_BIT, {16000}, {AUDIO_CHANNEL_IN_STEREO}}},
};

const AlsaInputStream::Configuration kConfigInputStream[USECASE_CAP_NUM] = {
    [USECASE_CAP_ECHOREF] =
        {
            "echoref",              /* use case */
            "Mars",                 /* card name */
            0,                      /* card */
            4,                      /* device */
            PCM_IN | PCM_MONOTONIC, /* flags */
            1024,                   /* period size */
            4,                      /* period count */
            0,                      /* start threshold */
            0,                      /* stop threshold */
            0,                      /* avail min */
        },
    [USECASE_CAP_RECORD] =
        {
            "builtinmic",           /* use case */
            "Mars",                 /* card name */
            0,                      /* card */
            3,                      /* device */
            PCM_IN | PCM_MONOTONIC, /* flags */
            1024,                   /* period size */
            4,                      /* period count */
            0,                      /* start threshold */
            0,                      /* stop threshold */
            0,                      /* avail min */
        },
    [USECASE_CAP_MMAP] =
        {
            "mmapcap",                         /* use case */
            "Mars",                            /* card name */
            0,                                 /* card */
            0,                                 /* device */
            PCM_IN | PCM_MONOTONIC | PCM_MMAP, /* flags */
            256,                               /* period size */
            4,                                 /* period count */
            0,                                 /* start threshold */
            INT_MAX,                           /* stop threshold */
            256,                               /* avail min */
        },
    [USECASE_CAP_AO] =
        {
            "aocap",                /* use case */
            "Mars",                 /* card name */
            1,                      /* card */
            0,                      /* device */
            PCM_IN | PCM_MONOTONIC, /* flags */
            4096,                   /* period size */
            4,                      /* period count */
            0,                      /* start threshold */
            0,                      /* stop threshold */
            0,                      /* avail min */
        },
    [USECASE_CAP_AI] =
        {
            "aicap",                /* use case */
            "Mars",                 /* card name */
            0,                      /* card */
            0,                      /* device */
            PCM_IN | PCM_MONOTONIC, /* flags */
            2048,                   /* period size */
            4,                      /* period count */
            0,                      /* start threshold */
            0,                      /* stop threshold */
            0,                      /* avail min */
        },
    [USECASE_CAP_DMIC] = {
        "dmic",                 /* use case */
        "Mars",                 /* card name */
        0,                      /* card */
        2,                      /* device */
        PCM_IN | PCM_MONOTONIC, /* flags */
        1056,                   /* period size */
        2,                      /* period count */
        0,                      /* start threshold */
        0,                      /* stop threshold */
        0,                      /* avail min */
    }};

}  // namespace rtk::media::audio