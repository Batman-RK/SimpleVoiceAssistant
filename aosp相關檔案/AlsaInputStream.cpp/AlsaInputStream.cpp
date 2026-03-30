////////////////////////////////////////////////////////////////////////////////
//
// (c) 2022 Realtek Semiconductor Corp. All rights reserved.
//
////////////////////////////////////////////////////////////////////////////////

#include <cutils/str_parms.h>
#include <tinyalsa/asoundlib.h>

#include <time.h>
#include <mutex>
#include <string>
#include <thread>

#include "AlsaInputStream.h"
#include "AudioInputStreamConfigs.h"
#include "AudioStreamUtils.h"

#include <fcntl.h>
#include <unistd.h>

using namespace rtk::media::audio;

extern struct audio_microphone_characteristic_t mic_default[2];

AlsaInputStream::AlsaInputStream(audio_devices_t device, audio_source_t source,
                                 audio_format_t fmt, audio_channel_mask_t ch,
                                 uint32_t sr,
                                 const std::vector<Capabilities>& caps,
                                 const Configuration& configs)
    : AudioInputStream(source),
      BaseAudioStream(configs.name, device, fmt, ch, sr, caps),
      mConfigs(configs),
      mAudioFd(-1) {
  mAudioFd = open("/dev/rtkaudio", O_RDWR);
  if (mAudioFd < 0) {
    loge("failed to open rtkaudio fd: ret=%d", mAudioFd);
  } else {
    logi("open rtkaudio fd success: ret=%d, audio source %d", mAudioFd, source);
  }
  logi("%s opened", __FUNCTION__);
}

AlsaInputStream::~AlsaInputStream() {
  if (mAudioFd >= 0) {
    logi("close AudioFd");
    close(mAudioFd);
  }
  std::lock_guard<std::mutex> lck(mDeviceLck);
  onCloseDevice_l();
}

ssize_t AlsaInputStream::read(void* buffer, size_t bytes) {
  int32_t ret = 0;

  {
    std::lock_guard<std::mutex> lck(mDeviceLck);
    if (!mDeviceHandle && openDevice_l() != 0) {
      return -1;
    }
    ret = onRead_l(buffer, bytes);
  }

  std::lock_guard<std::mutex> lck(mStreamLck);

  if (bytes > 0 && ret == 0) {
    mCapInfo.totalCapframes += bytes / audioBytesPerFrame();
    mCallback->onReadDone(this, buffer, bytes);
    logv("read: %zd/%llu", bytes,
         mCapInfo.totalCapframes * audioBytesPerFrame());
    return bytes;
  } else if (ret < 0) {
    int32_t ms = bytes * 1000 / audioBytesPerFrame() / getSampleRate();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    logw("read err: %d/%zd", ret, bytes);
  }

  return 0;
}

ssize_t AlsaInputStream::onRead_l(void* buffer, size_t bytes) {
  struct pcm* pcm = reinterpret_cast<struct pcm*>(mDeviceHandle);
  return pcm_read(pcm, buffer, bytes);
}

int32_t AlsaInputStream::openDevice_l() {
  if (mDeviceHandle) return 0;

  unsigned int channel = audio_channel_count_from_in_mask(mChannel);
  enum pcm_format format = audioFormat2PcmFormat(mFormat);
  struct pcm_config config {
    channel, mSampleRate, mConfigs.period_size, mConfigs.period_count, format,
        mConfigs.start_threshold, mConfigs.stop_threshold, 0, 0,
        mConfigs.avail_min
  };

  int pcmcard = findAlsaCardNumberByName(mConfigs.card_name);
  if (pcmcard != -1) {
    logi("Alsa card number: %d", pcmcard);
  } else {
    logw("Can not detect the alsa card number, using the default");
    pcmcard = mConfigs.card;
  }

  return onOpenDevice_l(pcmcard, mConfigs.device, mConfigs.flags, &config);
}

int AlsaInputStream::onOpenDevice_l(uint32_t card, uint32_t device,
                                    uint32_t flags, struct pcm_config* config) {
  struct pcm* pcm = NULL;

  /* set audio source to audio fw */
  if (device == kConfigInputStream[USECASE_CAP_RECORD].device) {
    if (mAudioFd == -1) {
      loge("AudioFd is -1. Can not set audio dmic source");
    } else {
      char s1[256];
      memset(s1, 0, 256);

      snprintf(s1, (sizeof(s1) - 1), "fw@ dmic_data_source %d", mSource);
      ::write(mAudioFd, s1, (strlen(s1) + 1));

      logi("Set audio dmic source %d", mSource);
    }
  }

  pcm = pcm_open(card, device, flags, config);
  if (pcm && !pcm_is_ready(pcm)) {
    loge(
        "failed to open audio device: %d/%d, flags=0x%08x, "
        "rate=%d, channels=%d, error message : %s",
        card, device, flags, config->rate, config->channels,
        pcm_get_error(pcm));
    return -1;
  }

  mDeviceHandle = pcm;

  logv(
      "open audio device: %d/%d, flags=0x%08x, rate=%d, "
      "channels=%d",
      card, device, flags, config->rate, config->channels);

  return 0;
}

int AlsaInputStream::onCloseDevice_l() {
  struct pcm*& pcm = reinterpret_cast<struct pcm*&>(mDeviceHandle);
  if (pcm) {
    pcm_close(pcm);
    pcm = nullptr;
    logi("closed pcm");
    return 0;
  }
  return 1;
}

int AlsaInputStream::standby() {
  std::lock_guard<std::mutex> lck(mDeviceLck);
  return standby_l();
}

int AlsaInputStream::standby_l() {
  BaseAudioStream::standby();
  return mDeviceHandle == nullptr ? 0 : onCloseDevice_l();
}

int AlsaInputStream::getCapturePosition(int64_t* frames, int64_t* time) {
  if (!frames || !time) return -EINVAL;

  struct timespec pcmTs;
  size_t availFrames;

  {
    std::lock_guard<std::mutex> lck(mDeviceLck);
    struct pcm* pcm = reinterpret_cast<struct pcm*>(mDeviceHandle);

    if (!pcm) {
      logw("%s fail for no device open", __FUNCTION__);
      return -ENOSYS;
    }
    if (pcm_get_htimestamp(pcm, &availFrames, &pcmTs) != 0) {
      logw("%s fail for get timestamp fail", __FUNCTION__);
      return -ENOSYS;
    }
  }

  std::lock_guard<std::mutex> lck(mStreamLck);

  *frames = mCapInfo.totalCapframes + availFrames;
  if (mCapInfo.firstCapTime == 0LL) {
    *time = ts2ns(&pcmTs);
    mCapInfo.firstCapTime = *time;
    mCapInfo.firstCapFrames = *frames;
  } else {
    int64_t actual = ts2ns(&pcmTs),
            calculate = mCapInfo.firstCapTime +
                        (*frames - mCapInfo.firstCapFrames) * 1e9 / mSampleRate,
            period = LLONG_MAX;  // mConfig.period_size * 1e9 / mSampleRate;
    *time = llabs(actual - calculate) <= period ? calculate : actual;
    if (*time != calculate) {
      logw("jitter outof range:%lld/%lld us", actual / 1000, calculate / 1000);
    }
  }

  logv("%s: %lld/%lld", __func__, *frames, *time);

  return 0;
}

int AlsaInputStream::getActiveMicrophones(
    struct audio_microphone_characteristic_t* mic_array, size_t* mic_count) {
  if (!mic_array || !mic_count) return -EINVAL;

  if (*mic_count == 0) {
    *mic_count =
        sizeof(mic_default) / sizeof(struct audio_microphone_characteristic_t);
    return 0;
  }

  memcpy((void*)mic_array, (void*)(&mic_default), sizeof(mic_default));
  *mic_count =
      sizeof(mic_default) / sizeof(struct audio_microphone_characteristic_t);

  return 0;
}

size_t AlsaInputStream::onGetBufferSize() {
  return mConfigs.period_size * audioBytesPerFrame();
}

int AlsaInputStream::onSetParameters(struct str_parms* params) {
  int ret = -ENOSYS;
  bool doStandby = false;
  {
    std::lock_guard<std::mutex> stLck(mStreamLck);

    int32_t source;
    if (str_parms_get_int(params, AUDIO_PARAMETER_STREAM_INPUT_SOURCE,
                          &source) >= 0) {
      /* no audio source uses val == 0 */
      if (mSource != source && source != 0) {
        logi("in_set_parameters() source=%d val=%d, do_standby!", mSource,
             source);
        mSource = static_cast<audio_source_t>(source);
        doStandby = true;
      }
      ret = 0;
    }

    int32_t device;
    if (str_parms_get_int(params, AUDIO_PARAMETER_STREAM_ROUTING, &device) >=
        0) {
      if (static_cast<int32_t>(mDevice) != device && device != 0) {
        logi("in_set_parameters() device=%d val=%d, do_standby!", mDevice,
             device);
        mDevice = static_cast<audio_devices_t>(device);
        doStandby = true;
      }
      ret = 0;
    }
  }

  if (doStandby) {
    std::lock_guard<std::mutex> devLck(mDeviceLck);
    standby_l();
  }

  return ret;
}

void AlsaInputStream::onGetParameters(struct str_parms* params,
                                      struct str_parms* reply) {
  std::lock_guard<std::mutex> lck(mStreamLck);

  BaseAudioStream::onGetParameters(params, reply);

  if (str_parms_has_key(params, AUDIO_PARAMETER_STREAM_INPUT_SOURCE)) {
    std::string oStr(std::to_string((int)AUDIO_SOURCE_DEFAULT));
    str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_INPUT_SOURCE, oStr.c_str());
  }
  if (str_parms_has_key(params, AUDIO_PARAMETER_STREAM_ROUTING)) {
    std::string oStr(std::to_string((uint32_t)mDevice));
    str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_ROUTING, oStr.c_str());
  }
}
