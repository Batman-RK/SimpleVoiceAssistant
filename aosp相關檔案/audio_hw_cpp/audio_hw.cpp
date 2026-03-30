#define LOG_TAG "rtk_audio_hw"
//#define LOG_NDEBUG 0

#define ATRACE_TAG (ATRACE_TAG_AUDIO | ATRACE_TAG_HAL)
#include <cutils/trace.h>

#include <endian.h>
#include <errno.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>

#include <log/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <audio_utils/resampler.h>
#include <fcntl.h>
#include <device/rpcdev.h>

#include <sound/asound.h>

#include <deque>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <optional>

#include "AudioCaptureThread.h"
#include "AudioCapture.h"
#include "rtk_compress.h"
#include "PatchManager.h"
#include "audio_hw.h"
#include "AudioConfig.h"
#include "AudioDelayManager.h"
#include "Platform_Lib/PLI/PLI.h"
#include "ioctrl/audio/audio_cmd_id.h"

#include "AudioHalHelper.h"
#include "AudioOutputDump.h"
#include "AudioStreamConfigs.h"
#include "AlsaOutputStream.h"
#include "DirectOutputStream.h"
#include "OffloadOutputStream.h"
#include "TunnelOutputStream.h"
#include "TunnelOffloadStream.h"

#include "AudioStreamConfigs.h"
#include "DeviceManager.h"
#include "IClockManager.h"
#include "AudioOutputMixer.h"
#include "AudioPatchManager.h"
#include "AudioPortManager.h"

//#define _ENABLE_DOLBY_HAL 1
#if _ENABLE_DOLBY_HAL
#include <rtk_impl.h>
#include <rtk_ms12_aidk.h>
#endif

#define AUDIO_HAL_4_0
#define TUNNEL_AV_SYNC_V2
#define AAUDIO_SUPPORT

#if PLATFORM_SDK_VERSION < 28
//[From BT] no more vr_bee_hidraw in android P, so disabled it.
//[TBD] needs to check else case at P
#define BLE_ENABLED
#else
#define AUDIO_HAL_4_0_ONLY  /* for after starting P */
#endif

#define RTK_AUDIO_TUNNEL_ADDR_PROP "persist.vendor.rtk.audio.tunnel.addr"

static AudioConfig* mAudioConfig = nullptr;
static AudioDelayManager* mAudioDelayManager = nullptr;
static AudioOutputDevice* mAudioOutputDevice = nullptr;
static AudioPortManager* mAudioPortManager = nullptr;

void setAudioTunnel(unsigned long addr)
{
    char property[PROPERTY_VALUE_MAX] = "0";
    memset(property, 0, sizeof(property));
    snprintf(property, sizeof(property), "%lx", addr);
    property_set(RTK_AUDIO_TUNNEL_ADDR_PROP, property);
}

static bool RTK_AUDIO_DEBUG_PROFILE_G = false;
static bool RTK_AUDIO_DEBUG_OUTPUT_ZERO_LOG= false;
#define RTK_AUDIO_DEBUG_PROFILE_PROP "persist.vendor.rtk.audio.profile"
#define RTK_AUDIO_DEBUG_ZERO_PROP "persist.vendor.rtk.audio.zero"
static bool RTK_AUDIO_DEBUG_G = false;
#define RTK_AUDIO_DEBUG_PROP "persist.vendor.rtk.audio.debug"
#define ATRACE_NAME_AVAIL "aAvail"

#define AUDIO_FUNC_ENTER { \
    if (RTK_AUDIO_DEBUG_G) { \
        ALOGD("%d: %s E", __LINE__, __FUNCTION__); \
    } \
}; AUDIO_FUNC_PROILE

#define AUDIO_FUNC_EXIT { \
    if (RTK_AUDIO_DEBUG_G) { \
        ALOGD("%d: %s X", __LINE__, __FUNCTION__); \
    } \
}

#define AUDIO_LOG(args...) { \
    if (RTK_AUDIO_DEBUG_G) { \
        ALOGD(args); \
    } \
}

class CProfile_audio_hw
{
 public:
    std::string m_func;
    int m_line;
    int64_t startTimeUs;
    int m_timeout;
    bool m_atrace;
 public:
    CProfile_audio_hw(const char *_func, int _line, int timeout=5,bool atrace=false) : m_func(_func), m_line(_line),m_timeout(timeout),m_atrace(atrace){
        startTimeUs = pli_getPTS();
    };

//private:
    ~CProfile_audio_hw(){
        int64_t endTimeUs = pli_getPTS();
        int64_t d = (endTimeUs-startTimeUs)/90;
        if (d>m_timeout || (RTK_AUDIO_DEBUG_PROFILE_G&&d>0) ||(RTK_AUDIO_DEBUG_G == true)) {
            ALOGD("profile, %s:%d,time=%lld ms", m_func.c_str(), m_line, d);
        }
        if (m_atrace)ATRACE_INT(m_func.c_str(), d);
    };
};
#define AUDIO_FUNC_PROILE CProfile_audio_hw __profile_time_audio_hw_(__func__, __LINE__);
#define AUDIO_FUNC_PROILE_TIMEOUT_WRITE(x) CProfile_audio_hw __profile_time_audio_hw_(__func__, __LINE__, x, true);


void resetAudioDebug(void)
{
    char property[PROPERTY_VALUE_MAX];
    property_get(RTK_AUDIO_DEBUG_PROP, property, "-1");
    if (!strcmp(property, "1")) {
        RTK_AUDIO_DEBUG_G = true;
    } else if (!strcmp(property, "0")) {
        RTK_AUDIO_DEBUG_G = false;
    }

    property_get(RTK_AUDIO_DEBUG_PROFILE_PROP, property, "-1");
    if (!strcmp(property, "1")) {
        RTK_AUDIO_DEBUG_PROFILE_G = true;
    } else if (!strcmp(property, "0")) {
        RTK_AUDIO_DEBUG_PROFILE_G = false;
    }
    property_get(RTK_AUDIO_DEBUG_ZERO_PROP, property, "0");
    if (!strcmp(property, "1")) {
        RTK_AUDIO_DEBUG_OUTPUT_ZERO_LOG = true;
    } else if (!strcmp(property, "0")) {
        RTK_AUDIO_DEBUG_OUTPUT_ZERO_LOG= false;
    }
}

#define AUDIO_HW_DURATION_FATE_OUT_BY_FW        5

/* User serviceable */
/* #define to use mmap no-irq mode for playback, #undef for non-mmap irq mode */
#undef PLAYBACK_MMAP        // was #define
/* short period (aka low latency) in milliseconds */
#define SHORT_PERIOD_MS 3   // was 22

/* Constraint imposed by ABE: for playback, all period sizes must be multiples of 24 frames
 * = 500 us at 48 kHz.  It seems to be either 48 or 96 for capture, or maybe it is because the
 * limitation is actually a min number of bytes which translates to a different amount of frames
 * according to the number of channels.
 */
#define ABE_BASE_FRAME_COUNT 24

/* Derived from MM_FULL_POWER_SAMPLING_RATE=48000 and ABE_BASE_FRAME_COUNT=24 */
#define MULTIPLIER_FACTOR 2

/* number of base blocks in a short period (low latency) */
#define SHORT_PERIOD_MULTIPLIER (SHORT_PERIOD_MS * MULTIPLIER_FACTOR)
/* number of frames per short period (low latency) */
#define SHORT_PERIOD_SIZE 256
#define SHORT_PERIOD_COUNT 16

/* number of frames per period for HDMI multichannel output */
#define HDMI_MULTI_PERIOD_SIZE  256
/* number of periods for HDMI multichannel output */
#define HDMI_MULTI_PERIOD_COUNT 64
/* default number of channels for HDMI multichannel output */
#define HDMI_MULTI_DEFAULT_CHANNEL_COUNT 2

#define MMAP_PERIOD_SIZE 256
#define MMAP_MIN_SIZE_FRAMES_MAX 64*1024

/* Number of pseudo periods for low latency playback.
 * These are called "pseudo" periods in that they are not known as periods by ALSA.
 * Formerly, ALSA was configured in MMAP mode with 2 large periods, and this
 * number was set to 4 (2 didn't work).
 * The short periods size and count were only known by the audio HAL.
 * Now for low latency, we are using non-MMAP mode and can set this to 2.
 */
#ifdef PLAYBACK_MMAP
#define PLAYBACK_SHORT_PERIOD_COUNT 4
/* If sample rate converter is required, then use triple-buffering to
 * help mask the variance in cycle times.  Otherwise use double-buffering.
 */
#elif DEFAULT_OUT_SAMPLING_RATE != MM_FULL_POWER_SAMPLING_RATE
#define PLAYBACK_SHORT_PERIOD_COUNT 3
#else
#define PLAYBACK_SHORT_PERIOD_COUNT 2
#endif

/* write function */
#ifdef PLAYBACK_MMAP
#define PCM_WRITE pcm_mmap_write
#else
#define PCM_WRITE pcm_write
#endif

/* User serviceable */
#define CAPTURE_PERIOD_MS 22

/* Number of frames per period for capture.  This cannot be reduced below 96.
 * Possibly related to the following rule in sound/soc/omap/omap-pcm.c:
 *  ret = snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 384);
 *      (where 96 * 4 = 384)
 * The only constraints I can find are periods_min = 2, period_bytes_min = 32.
 * If you define RULES_DEBUG in sound/core/pcm_native.c, you can see which rule
 * caused capture to fail.
 * Decoupling playback and capture period size may have impacts on echo canceler behavior:
 * to be verified.  Currently 96 = 4 x 24 but it could be changed without noticing
 * if we use separate defines.
 */
#define CAPTURE_PERIOD_SIZE (ABE_BASE_FRAME_COUNT * CAPTURE_PERIOD_MS * MULTIPLIER_FACTOR)
/* number of periods for capture */
#define CAPTURE_PERIOD_COUNT 2
/* minimum sleep time in out_write() when write threshold is not reached */
#define MIN_WRITE_SLEEP_US 5000

#define DEFAULT_OUT_SAMPLING_RATE 48000 // 48000 is possible but interacts poorly with HDMI

/* sampling rate when using MM full power port */
#define MM_FULL_POWER_SAMPLING_RATE 48000   // affects MULTIPLIER_FACTOR
/* sampling rate when using VX port for narrow band */
#define VX_NB_SAMPLING_RATE 8000
/* sampling rate when using VX port for wide band */
#define VX_WB_SAMPLING_RATE 16000


#define DUMP_TO_FILE
#ifdef DUMP_TO_FILE
#define DTF_PROPERTY "persist.vendor.rtk.audio.tofile"
#define INDTF_PROPERTY "persist.vendor.rtk.audio.in.tofile"

FILE* fp_deepbuffer_dump;
FILE* fp_lowlatency_dump;
FILE* fp_tunnel_dump;
FILE* fp_indump;
FILE* fp_offload_dump;
FILE* fp_offload_dump_all;
int fp_dumpOutIndex;
int fp_dumpInIndex;
#endif

#define DUMP_ECHOANDINMIC_TO_FILE 1



#define ECHO_REFERNCE_DUMP "persist.vendor.rtk.audio.echo_reference.debug"
#define LEGACY_AUDIO_HAL "persist.vendor.rtk.audio.legacy"
FILE* echo_fp_indump;
FILE* inmic_fp_indump;



#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* for mic select*/
#define AICAP_CONFIG "AICAP"
#define BLE_CONFIG "BLEMIC"
#define DMIC_CONFIG "DMIC"
#define INMIC_CONFIG "INMIC"

char mic_property[PROPERTY_VALUE_MAX];
int alsa_card = -1;
pthread_mutex_t global_lock;
int print_debug_count = 0;

struct pcm_config pcm_config_aaudio = {
    .channels = 2,
    .rate = DEFAULT_OUT_SAMPLING_RATE,
    .period_size = MMAP_PERIOD_SIZE,
    .period_count = 8,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = MMAP_PERIOD_SIZE,
    .stop_threshold = INT32_MAX,
    .avail_min = MMAP_PERIOD_SIZE,
};

struct pcm_config pcm_config_aaudio_capture = {
    .channels = 2,
    .rate = MM_FULL_POWER_SAMPLING_RATE,
    .period_size = MMAP_PERIOD_SIZE,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 0,
    .silence_threshold = 0,
    .silence_size = 0,
    .stop_threshold = INT_MAX,
    .avail_min = MMAP_PERIOD_SIZE,
};

//in config BLEMIC
struct pcm_config pcm_config_blemic = {
    .channels = 1,
    .rate = 16000,
    .period_size = 240,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

//in config DMIC
struct pcm_config pcm_config_dmic = {
    .channels = 2,
    .rate = 16000,
    .period_size = CAPTURE_PERIOD_SIZE,
    .period_count = CAPTURE_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

//in config AI capture
struct pcm_config pcm_config_ai_capture = {
    .channels = 2,
    .rate = 48000,
    .period_size = 2048,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_mic_capture = {
    .channels = 2,
    .rate = 16000,
    .period_size = 1024,
    .period_count = 4,
    .format = PCM_FORMAT_S32_LE,
};

//in config AO capture
struct pcm_config pcm_config_ao_capture = {
    .channels = 2,
    .rate = 48000,
    .period_size = 4096,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

//////////////////////////////////////////////////////////////////////////////////

static int64_t clock_time_ns() {
  struct timespec systime;
  clock_gettime(CLOCK_MONOTONIC, &systime);
  return ((int64_t)systime.tv_sec) * 1000000000 + systime.tv_nsec;
}

//////////////////////////////////////////////////////////////////////////////////

#define AUDIO_HW_SIZE_OF_TMP_DATA (8192)
struct stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    pthread_mutex_t pre_lock; /* acquire before lock to avoid DOS by playback thread */
    struct pcm_config config;
    struct pcm* pcm;
    struct resampler_itfe* resampler;
    OutputTypeE output_type;
    PcmTypeE pcm_type;
    char* buffer;
    size_t buffer_frames;
    int standby;

#ifndef AUDIO_HAL_4_0
    uint32_t sample_rate;
#endif
    audio_channel_mask_t channel_mask;
    audio_channel_mask_t sup_channel_masks[3];

    audio_format_t format;
    audio_output_flags_t flags;

    /* FIXME: workaround for HDMI multi channel channel swap on first playback after opening
     * the output stream: force reopening the pcm driver after writing a few periods. */
    int restart_periods_cnt;

    struct audio_device* dev;

    uint64_t written;
    uint64_t segment_written;
    uint32_t tunneled_bs_size;
    uint64_t tunneled_timestamp;
    uint64_t PRE_STC;
    uint64_t PRE_PTS;
    int64_t zeroSampleToWrite;
    bool hdmiHasPaused;
    int64_t lastPts;

    uint64_t PRE_audio_gap_STC;
    bool audio_gap;

    bool tunneled_firstts;
    bool tunneled_bufwithheader;
    int pre_bytesleft;
    char *tmp_data;

    int rtkaudiofd;
    int64_t signed_frames;

    float stream_volume;


    /* offload */
    audio_offload_info_t offload_info;
    int offload_sample_rate;
    bool offload_first_write;
    bool offload_open;
    bool offload_flag_need_wait_done;
    pthread_cond_t offload_cond_wait_done_init;
    pthread_cond_t offload_cond_wait_done_flush;
    uint64_t offload_writen;
    bool force_audio_master;
    uint64_t pre_timestamp;
    bool is_tunnel;
    bool is_external_syncid;
    bool set_delay;
    uint64_t pre_pos;
    struct timespec pre_pos_ts;

    stream_callback_t offload_callback;
    void* offload_cookie;
    struct timespec timestamp;

    int send_new_metadata;
    int non_blocking;
    int playback_started;
    int offload_state;
    int hdmi_state;
    int64_t time_trigger_pause_hdmi_fade_done;
    pthread_cond_t offload_cond;
    pthread_t offload_thread;
    struct listnode offload_cmd_list;
//    bool offload_thread_blocked;

#ifdef AAUDIO_SUPPORT
    bool bExitThread;
    bool bStopPlay;
    pthread_t mmap_thread;
    pthread_cond_t mmap_cond;
    int mmap_shared_memory_fd; /* file descriptor associated with MMAP NOIRQ shared memory */
#endif

    rtk_compress_offload_t* offload_handle;
    int delay_time[DELAY_MAX];

    bool use_a2dp;
    bool use_usb;
    volatile bool is_writting;
    struct {
      bool valid;
      uint64_t frames;
      struct timespec timestamp;
    } last_presentation_position;

    size_t pcm_avail_write_max;
    REFCLOCK* rcd;
    int source_metadata_usage;
    int handle;
};

struct stream_in {
    struct audio_stream_in stream;
    struct audio_stream_in* ble_stream_in;

    pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    pthread_mutex_t pre_lock; /* acquire before lock to avoid DOS by playback thread */
    struct pcm_config config;
    struct pcm* pcm;
    int device;
    struct resampler_itfe* resampler;
    struct resampler_buffer_provider buf_provider;
    unsigned int requested_rate;
    int standby;
    int source;

    int16_t* read_buf;
    int16_t* temp_buf;
    size_t read_buf_size;
    size_t read_buf_frames;

    int16_t* proc_buf_in;
    int16_t* proc_buf_out;
    size_t proc_buf_size;
    size_t proc_buf_frames;

    int16_t* ref_buf;
    size_t ref_buf_size;
    size_t ref_buf_frames;

    int64_t read_frames;

    int read_status;

    bool aux_channels_changed;
    uint32_t main_channels;
    uint32_t aux_channels;
    struct audio_device* dev;
    audio_input_flags_t flags;

    int AO_CAP_enable;
#ifdef BLE_ENABLED
    bool use_ble;
#endif
    bool use_a2dp;
};

#define AUDIO_HW_HDMI_STATE_PAUSE       0x01
#define AUDIO_HW_HDMI_STATE_RESUME      0x02
#define AUDIO_HW_HDMI_STATE_FLUSH       0x04
#define AUDIO_HW_HDMI_STATE_STANDBY     0x08
#define AUDIO_HW_HDMI_STATE_IS_PAUSED(x) (x&AUDIO_HW_HDMI_STATE_PAUSE)
#define AUDIO_HW_HDMI_STATE_SET(x, state)      {x|=state;}
#define AUDIO_HW_HDMI_STATE_REMOVE(x, state)   {x&=(~state);}

#define STRING_TO_ENUM(string) { #string, string, 1 }
struct string_to_enum {
    const char* name;
    uint32_t value;
    uint32_t enable:1;
};

#ifdef AUDIO_HAL_4_0
#define SUP_FORMAT_INDEX_PCM_16_BIT 0
#define SUP_FORMAT_INDEX_AC3        1
#define SUP_FORMAT_INDEX_E_AC3      2
#define SUP_FORMAT_INDEX_E_AC3_JOC  3
#define SUP_FORMAT_INDEX_AC4        4
#define SUP_FORMAT_INDEX_DTS        5
struct string_to_enum sup_output_formats_name_to_enum_table[] = {
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_16_BIT),
    STRING_TO_ENUM(AUDIO_FORMAT_AC3),
    STRING_TO_ENUM(AUDIO_FORMAT_E_AC3),
    STRING_TO_ENUM(AUDIO_FORMAT_E_AC3_JOC),     //dolby atmos
    STRING_TO_ENUM(AUDIO_FORMAT_AC4),
    //STRING_TO_ENUM(AUDIO_FORMAT_DTS),
};

struct string_to_enum sup_input_formats_name_to_enum_table[] = {
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_16_BIT),
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_32_BIT),
};

const struct string_to_enum out_channels_name_to_enum_table[] = {
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_MONO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_STEREO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_5POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_7POINT1),
};

//////////////////////////////////////////////////////////////////////////////////

#ifdef AUDIO_HAL_4_0_ONLY
//microphone characteristics:
//refer to **/oreo/device/google/marlin/audio_platform_info_tasha_marlin.xml

struct audio_microphone_characteristic_t mic_default[] = {
    {
        .device_id = "builtin_mic_1",
        .id = 0, /* TBD */
        .device = AUDIO_DEVICE_IN_BUILTIN_MIC,
        .address = AUDIO_BOTTOM_MICROPHONE_ADDRESS,
        .channel_mapping = {AUDIO_MICROPHONE_CHANNEL_MAPPING_PROCESSED},
        .location = AUDIO_MICROPHONE_LOCATION_MAINBODY,
        .group = 0,
        .index_in_the_group = 0,
        .sensitivity = -37.0, //AUDIO_MICROPHONE_SENSITIVITY_UNKNOWN
        .max_spl = 132.5, //AUDIO_MICROPHONE_SPL_UNKNOWN
        .min_spl = 28.5,  //AUDIO_MICROPHONE_SPL_UNKNOWN
        .directionality = AUDIO_MICROPHONE_DIRECTIONALITY_OMNI,
        .num_frequency_responses = 93,
        .frequency_responses = {
            {
                97.16, 102.92, 109.02, 115.48, 122.32, 129.57, 137.25, 145.38, 153.99, 163.12,
                172.78, 183.02, 193.87, 205.35, 217.52, 230.41, 244.06, 258.52, 273.84, 290.07,
                307.26, 325.46, 344.75, 365.17, 386.81, 409.73, 434.01, 459.73, 486.97, 515.82,
                546.39, 578.76, 613.06, 649.38, 687.86, 728.62, 771.79, 817.52, 865.96, 917.28,
                971.63, 1029.20, 1090.18, 1154.78, 1223.21, 1295.69, 1372.46, 1453.78, 1539.93, 1631.17,
                1727.83, 1830.21, 1938.65, 2053.53, 2175.20, 2304.09, 2440.62, 2585.23, 2738.42, 2900.68,
                3072.56, 3254.62, 3447.47, 3651.74, 3868.12, 4097.32, 4340.10, 4597.27, 4869.68, 5158.22,
                5463.87, 5787.62, 6130.56, 6493.82, 6878.60, 7286.18, 7717.92, 8175.23, 8659.64, 9172.76,
                9716.28, 10292.01, 10901.84, 11547.82, 12232.07, 12956.87, 13724.61, 14537.84, 15399.27, 16311.73,
                17278.26, 18302.06, 19386.53
            },
            {
                -0.40, -0.40, -0.60, -0.70, -0.40, -0.40, -0.30, -0.30, -0.30, -0.30,
                -0.20, -0.60, -0.90, -0.90, -1.00, -0.70, -0.80, -0.70, -0.70, -0.90,
                -0.70, -0.20, 0.60, 1.40, 1.70, 0.80, -0.80, -2.10, -2.30, -1.70,
                -0.90, 0.50, 1.30, 1.20, 0.80, 0.10, 0.20, 0.40, 2.30, 2.40,
                0.00, -0.40, -0.10, 0.70, -0.40, 1.00, 0.50, 1.40, 2.40, 2.00,
                2.50, 2.70, 1.70, 1.40, 1.70, -1.90, -3.60, 1.70, 2.30, 0.00,
                0.80, -0.30, 0.60, 1.90, 1.40, -1.90, 0.30, 1.70, -0.60, 0.40,
                2.20, 3.60, -4.20, 2.50, 3.60, 8.10, -4.30, 5.70, 7.30, 9.60,
                7.80, 10.20, 16.40, 18.60, 20.10, 22.50, 23.50, 17.60, 17.90, 18.80,
                17.70, 15.10, 14.70
            }
        },
        .geometric_location = {0.0, -1.0, 0.0},
        .orientation = {0.0513, 0.0, 0.0038},
    },
    {
        .device_id = "builtin_mic_2",
        .id = 1, /* TBD */
        .device = AUDIO_DEVICE_IN_BUILTIN_MIC,
        .address = AUDIO_BOTTOM_MICROPHONE_ADDRESS,
        .channel_mapping = {AUDIO_MICROPHONE_CHANNEL_MAPPING_PROCESSED},
        .location = AUDIO_MICROPHONE_LOCATION_MAINBODY,
        .group = 0,
        .index_in_the_group = 0,
        .sensitivity = -37.0, //AUDIO_MICROPHONE_SENSITIVITY_UNKNOWN
        .max_spl = 132.5, //AUDIO_MICROPHONE_SPL_UNKNOWN
        .min_spl = 28.5,  //AUDIO_MICROPHONE_SPL_UNKNOWN
        .directionality = AUDIO_MICROPHONE_DIRECTIONALITY_OMNI,
        .num_frequency_responses = 93,
        .frequency_responses = {
            {
                97.16, 102.92, 109.02, 115.48, 122.32, 129.57, 137.25, 145.38, 153.99, 163.12,
                172.78, 183.02, 193.87, 205.35, 217.52, 230.41, 244.06, 258.52, 273.84, 290.07,
                307.26, 325.46, 344.75, 365.17, 386.81, 409.73, 434.01, 459.73, 486.97, 515.82,
                546.39, 578.76, 613.06, 649.38, 687.86, 728.62, 771.79, 817.52, 865.96, 917.28,
                971.63, 1029.20, 1090.18, 1154.78, 1223.21, 1295.69, 1372.46, 1453.78, 1539.93, 1631.17,
                1727.83, 1830.21, 1938.65, 2053.53, 2175.20, 2304.09, 2440.62, 2585.23, 2738.42, 2900.68,
                3072.56, 3254.62, 3447.47, 3651.74, 3868.12, 4097.32, 4340.10, 4597.27, 4869.68, 5158.22,
                5463.87, 5787.62, 6130.56, 6493.82, 6878.60, 7286.18, 7717.92, 8175.23, 8659.64, 9172.76,
                9716.28, 10292.01, 10901.84, 11547.82, 12232.07, 12956.87, 13724.61, 14537.84, 15399.27, 16311.73,
                17278.26, 18302.06, 19386.53
            },
            {
                -0.40, -0.40, -0.60, -0.70, -0.40, -0.40, -0.30, -0.30, -0.30, -0.30,
                -0.20, -0.60, -0.90, -0.90, -1.00, -0.70, -0.80, -0.70, -0.70, -0.90,
                -0.70, -0.20, 0.60, 1.40, 1.70, 0.80, -0.80, -2.10, -2.30, -1.70,
                -0.90, 0.50, 1.30, 1.20, 0.80, 0.10, 0.20, 0.40, 2.30, 2.40,
                0.00, -0.40, -0.10, 0.70, -0.40, 1.00, 0.50, 1.40, 2.40, 2.00,
                2.50, 2.70, 1.70, 1.40, 1.70, -1.90, -3.60, 1.70, 2.30, 0.00,
                0.80, -0.30, 0.60, 1.90, 1.40, -1.90, 0.30, 1.70, -0.60, 0.40,
                2.20, 3.60, -4.20, 2.50, 3.60, 8.10, -4.30, 5.70, 7.30, 9.60,
                7.80, 10.20, 16.40, 18.60, 20.10, 22.50, 23.50, 17.60, 17.90, 18.80,
                17.70, 15.10, 14.70
            }
        },
        .geometric_location = {0.0, -1.0, 0.0},
        .orientation = {0.0513, 0.0, 0.0038},
    }
};
#endif /* AUDIO_HAL_4_0_ONLY */
#else
const struct string_to_enum out_channels_name_to_enum_table[] = {
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_STEREO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_5POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_7POINT1),
};
#endif

static inline int64_t ns_from_timespec(const struct timespec* ts)
{
    return ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

/**
 * NOTE: when multiple mutexes have to be acquired, always respect the following order:
 *        hw device > in stream > out stream
 */


static int do_input_standby(struct stream_in* in);

static bool is_supported_format(audio_format_t format)
{
    switch (format) {
        case AUDIO_FORMAT_AC3:
        case AUDIO_FORMAT_E_AC3:
        case AUDIO_FORMAT_E_AC3_JOC:
        case AUDIO_FORMAT_AC4:
            return true;
        default:
            break;
    }
    return false;
}


#ifdef TUNNELED_PLAYBACK
static rtk::media::audio::IClockManager *gClockManager =
  rtk::media::audio::IClockManager::getInstance();

static void tunneledPlaybackInit()
{
    rpcdev_init();
    gClockManager->init();
}

static void tunneledPlaybackDeinit()
{
    gClockManager->unInit();
    rpcdev_exit();
}
#endif

void lock_input_stream(struct stream_in* in)
{
    pthread_mutex_lock(&in->pre_lock);
    pthread_mutex_lock(&in->lock);
    pthread_mutex_unlock(&in->pre_lock);
}

void lock_output_stream(struct stream_out* out)
{
    pthread_mutex_lock(&out->pre_lock);
    pthread_mutex_lock(&out->lock);
    pthread_mutex_unlock(&out->pre_lock);
}

bool trylock_output_stream(struct stream_out* out)
{
    bool ret = false;
    pthread_mutex_lock(&out->pre_lock);
    if (pthread_mutex_trylock(&out->lock)==0)
        ret =true;
    pthread_mutex_unlock(&out->pre_lock);
    return ret;
}

#ifdef BLE_ENABLED
static bool isBleMicConnected(void)
{
    bool ret = false;
    char property[PROPERTY_VALUE_MAX];
    if (property_get("rtk.bt.btmic.connected", property, "0") && !strcmp(property, "1")) {
        ret = true;
    }
    ALOGD("isBleMicConnected:%d", ret);
    return ret;
}
#endif

static void selectMic(struct stream_in* streamIn __unused, audio_devices_t device, const char* fun)
{
    memset(mic_property, '\0', sizeof(mic_property));
#ifdef BLE_ENABLED
    bool bBleConnected = false;
    if (isBleMicConnected()) {
        bBleConnected = true;
        ALOGD("%s:%s[%d]: BLEMIC", fun, __FUNCTION__, __LINE__);
        memcpy(mic_property, BLE_CONFIG, sizeof(BLE_CONFIG));
    } else
#endif
#ifdef DMIC_ENABLED
    {
        ALOGD("%s:%s[%d]: DMIC", fun, __FUNCTION__, __LINE__);
        memcpy(mic_property, DMIC_CONFIG, sizeof(DMIC_CONFIG));
    }
#else
    {

        if(device == AUDIO_DEVICE_IN_ECHO_REFERENCE || device == AUDIO_DEVICE_IN_BUILTIN_MIC || device == AUDIO_DEVICE_IN_BACK_MIC){
            ALOGD("%s:%s[%d]: MICCAP", fun, __FUNCTION__, __LINE__);
            memcpy(mic_property, INMIC_CONFIG, sizeof(INMIC_CONFIG));
        }else{
            ALOGD("%s:%s[%d]: AICAP", fun, __FUNCTION__, __LINE__);
            memcpy(mic_property, AICAP_CONFIG, sizeof(AICAP_CONFIG));
        }
    }
#endif
#ifdef BLE_ENABLED
    if (streamIn != NULL) {
        streamIn->use_ble = bBleConnected;
    }
#endif
}

static int check_input_parameters(uint32_t sample_rate, audio_format_t format, int channel_count)
{
    if (format != AUDIO_FORMAT_PCM_16_BIT && format != AUDIO_FORMAT_PCM_32_BIT) {
        return -EINVAL;
    }

    if ((channel_count < 1) || (channel_count > 2)) {
        return -EINVAL;
    }

    switch(sample_rate) {
        case 8000:
        case 11025:
        case 12000:
        case 16000:
        case 22050:
        case 24000:
        case 32000:
        case 44100:
        case 48000:
            break;
        default:
            return -EINVAL;
    }

    return 0;
}

static size_t get_input_buffer_size(uint32_t sample_rate, audio_format_t format, audio_devices_t device, int channel_count)
{
    AUDIO_FUNC_ENTER
    size_t size;
    if (check_input_parameters(sample_rate, format, channel_count) != 0) {
        ALOGE("%s: invalid config", __FUNCTION__);
        return 0;
    }

    if (alsa_card != -1) {
        selectMic(NULL, device, (const char*)__FUNCTION__);
    } else {
        ALOGE("get_input_buffer_size ALSA card id equal -1!!!");
    }

    /* take resampling into account and return the closest majoring
    multiple of 16 frames, as audioflinger expects audio buffers to
    be a multiple of 16 frames */
    ALOGD("get_input_buffer_size: mic_property: %s\n", mic_property);
    if (!strcmp(mic_property, AICAP_CONFIG)) {
        size = (pcm_config_ai_capture.period_size * sample_rate) / pcm_config_ai_capture.rate;
    } else if (!strcmp(mic_property, BLE_CONFIG)) {
        size = (pcm_config_blemic.period_size * sample_rate) / pcm_config_blemic.rate;
    } else if (!strcmp(mic_property, DMIC_CONFIG)) {
        size = (pcm_config_dmic.period_size * sample_rate) / pcm_config_dmic.rate;
    } else if (!strcmp(mic_property, INMIC_CONFIG)) {
        size = (pcm_config_mic_capture.period_size * sample_rate) / pcm_config_mic_capture.rate;
    } else {
        size = (pcm_config_ao_capture.period_size * sample_rate) / pcm_config_ao_capture.rate;
    }

    size = ((size + 15) / 16) * 16;
    ALOGD("get_input_buffer_size = %d\n", size);

    return size * channel_count * sizeof(short);
}

static int out_set_sample_rate(struct audio_stream* stream __unused, uint32_t rate __unused)
{
    //[from header comment]
    /* currently unused - use set_parameters with key
       AUDIO_PARAMETER_STREAM_SAMPLING_RATE */

    return 0;
}

static audio_channel_mask_t out_get_channels(const struct audio_stream* stream)
{
    struct stream_out* out = (struct stream_out*)stream;
    return out->channel_mask;
}

static unsigned int out_get_samplerate(const struct audio_stream* stream)
{
    struct stream_out* out = (struct stream_out*)stream;
    return out->config.rate;
}

static audio_format_t out_get_format(const struct audio_stream* stream)
{
    struct stream_out* out = (struct stream_out*)stream;
    return out->format;
}

static int out_set_format(struct audio_stream* stream, audio_format_t format __unused)
{
#ifdef A2DP_ENABLED
    struct stream_out* out = (struct stream_out*)stream;
    if (out->use_a2dp) {
        ALOGE("setting a2dp format not yet supported (0x%x)", format);
        return -ENOSYS;
    }
#endif
    return 0;
}

static int destroy_mmap_commit_thread(struct stream_out* stream)
{
    struct stream_out* out = stream;
    pthread_join(out->mmap_thread, (void**) NULL);
    pthread_cond_destroy(&out->mmap_cond);
    return 0;
}

/* must be called with hw device and output stream mutexes locked */
static int do_output_standby(struct stream_out* out, int compress_deinit)
{
    AUDIO_FUNC_ENTER
    ALOGI("%s:%d,out=%p, standby=%d,output_type=%d,offload_open=%d,compress_deinit=%d\n",__FUNCTION__,__LINE__,out, out->standby,out->output_type,out->offload_open,compress_deinit);

    if (!out->standby) {
        if (out->output_type != OUTPUT_OFFLOAD) {
            out->standby = 1;

            if (out->pcm) {
#ifdef AAUDIO_SUPPORT
                if (out->output_type == OUTPUT_AAUDIO) {
                    if (out->mmap_shared_memory_fd >= 0) {
                        if (out->bExitThread == false) {
                            out->bStopPlay = true;
                            out->bExitThread = true;
                            destroy_mmap_commit_thread(out);
                            if(out->pcm != NULL) {
                                int ret = pcm_stop(out->pcm);
                                if (ret < 0) {
                                    ALOGE("%s: MMAP pcm_stop failed ret %d", __func__, ret);
                                }
                            }
                        }
                        ALOGD("%s: closing mmap_shared_memory_fd = %d", __func__, out->mmap_shared_memory_fd);
                        close(out->mmap_shared_memory_fd);
                        out->mmap_shared_memory_fd = -1;
                    }
                }
#endif
                if ((out->output_type != OUTPUT_DIRECT_TUNNEL) && (out->output_type != OUTPUT_DIRECT_NONTUNNEL)) {
                    ALOGD("pcm close %d\n", out->pcm_type);
                    if (out->handle != -1) {
                        AudioPatchManager::instance().notifyOutputStreamClosed(out->handle);
                    }
                    pcm_close(out->pcm);
                    out->pcm = NULL;
                }
            }
        }
    }
    AUDIO_FUNC_EXIT
    return 0;
}

static int out_standby(struct audio_stream* stream)
{
    AUDIO_FUNC_ENTER
    struct stream_out* out = (struct stream_out*)stream;
    int status;

    pthread_mutex_lock(&out->dev->audio_device_lock);
    lock_output_stream(out);
    status = do_output_standby(out, 1);
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->audio_device_lock);
    AUDIO_FUNC_EXIT
    return status;
}

static int out_dump(const struct audio_stream* stream __unused, int fd __unused)
{
    return 0;
}

static int out_set_parameters(struct audio_stream* stream, const char* kvpairs)
{
    struct stream_out* out = (struct stream_out*)stream;
    struct audio_device* adev = out->dev;

    ALOGD("out_set_parameters, out=%p, device(%x), kvpairs: %s", stream, adev->out_device, kvpairs);

    if (adev->a2dp_stream_out) {
        //return 0;//adev->a2dp_stream_out->common.set_parameters(adev->a2dp_stream_out,kvpairs);
    }

    struct str_parms* parms;
    char value[32];
    int ret, ret_get;

#ifdef AUDIO_HAL_4_0_ONLY
    ret = -ENOSYS;
#else
    ret = -ENOENT;
#endif

    //VTS test: empty parameter expect return OK!
    if (strcmp(kvpairs, "") == 0) {
        ALOGW("get empty parameters");
        return 0;
    }

    parms = str_parms_create_str(kvpairs);

    ret_get = str_parms_get_str(parms, AUDIO_PARAMETER_DEVICE_CONNECT, value, sizeof(value));
    if (ret_get >= 0) {
        audio_devices_t device = (audio_devices_t)strtoul(value, NULL, 10);
        ALOGE("out_set_parameters, DEVICE_CONNECT: %x", device);
        ret = 0;
    }

    ret_get = str_parms_get_str(parms, AUDIO_PARAMETER_DEVICE_DISCONNECT, value, sizeof(value));
    if (ret_get >= 0) {
        audio_devices_t device = (audio_devices_t)strtoul(value, NULL, 10);
        ALOGE("out_set_parameters, DEVICE_DISCONNECT: %x", device);
        ret = 0;
    }

    ret_get = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret_get >= 0) {
        ALOGD("out_set_parameters, STREAM_ROUTING: %s", value);
        ret = 0;
    }

    ret_get = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_CHANNELS, value, sizeof(value));
    if (ret_get >= 0) {
        audio_channel_mask_t channels = (audio_channel_mask_t)strtoul(value, NULL, 10);
        out->channel_mask = channels;
        ALOGD("out_set_parameters, STREAM_CHANNELS: %x", channels);
        ret = 0;
    }

    ret_get = str_parms_get_str(parms, "exiting", value, sizeof(value));
    if (ret_get >= 0) {
        ALOGD("out_set_parameters:exiting");
        // do nothing
        ret = 0;
    }

#ifdef AUDIO_HAL_4_0
    /*
    case 0: return Result::OK;
    case -EINVAL: return Result::INVALID_ARGUMENTS;
    case -ENODATA: return Result::INVALID_STATE;
    case -ENODEV: return Result::NOT_INITIALIZED;
    case -ENOSYS: return Result::NOT_SUPPORTED;
    default: return Result::INVALID_STATE;
    */

    //hw_av_sync case: hack for VTS, playback will not get hw_av_sync addr from here
    int rcd = 0;
    ret_get = str_parms_get_int(parms, AUDIO_PARAMETER_STREAM_HW_AV_SYNC, &rcd);
    if (ret_get >= 0) {
        ALOGD("out_set_parameters, STREAM_HW_AV_SYNC: %x", rcd);
        gClockManager->resetClock(rcd);
        ret = 0;
    }
#endif

    str_parms_destroy(parms);
    return ret;
}

static char* out_get_parameters(const struct audio_stream* stream, const char* keys)
{
    struct stream_out* out = (struct stream_out*)stream;
    struct str_parms* query = str_parms_create_str(keys);
    char* str;
    char value[256];
    struct str_parms* reply = str_parms_create();
    size_t j;
    int ret;
    bool first, found_key = false;
    audio_format_t forma_query = AUDIO_FORMAT_DEFAULT;

    ALOGD("out_get_parameters, keys: %s", keys);

    //format case:
    //ref: frameworks/av/services/audiopolicy/managerdefault/AudioPolicyManager.cpp, AudioPolicyManager::updateAudioProfiles(){ profiles.hasDynamicFormat() }
    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_FORMAT, value, sizeof(value));
    if (ret >= 0) {
        forma_query = (audio_format_t)atoi(value);
        ALOGD("out_get_parameters, STREAM_FORMAT %x", forma_query);
        if (forma_query==AUDIO_FORMAT_PCM_16_BIT || is_supported_format(forma_query)){
        }else{
            forma_query = out_get_format(stream);
        }
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_CHANNELS, value, sizeof(value));
    if (ret >= 0) {
        ALOGD("out_get_parameters, STREAM_CHANNELS %s", value);
        found_key = true;
        first = true;
        audio_channel_mask_t channels = out_get_channels(stream);
        memset(value, 0, sizeof(value));
        for (j = 0; j < ARRAY_SIZE(out_channels_name_to_enum_table); j++) {
            if (out_channels_name_to_enum_table[j].value == channels) {
                if (!first) {
                    strncat(value, "|", sizeof(value) - strlen(value) - 1);
                }
                strncat(value, out_channels_name_to_enum_table[j].name, sizeof(value) - strlen(value) - 1);
                first = false;
            }
        }
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_CHANNELS, value);
    }

    //sup_channels, reply audio_fw implement
    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value, sizeof(value));
    if (ret >= 0) {
#if 0
        found_key = true;
        first = true;
        i = 0;
        memset(value, 0, sizeof(value));
        for (j = 0; j < ARRAY_SIZE(out_channels_name_to_enum_table); j++) {
            if (!first) {
                strncat(value, "|", sizeof(value) - strlen(value) - 1);
            }
            strncat(value, out_channels_name_to_enum_table[j].name, sizeof(value) - strlen(value) - 1);
            first = false;
        }
#else
        if (forma_query == AUDIO_FORMAT_PCM_16_BIT){
            sprintf(value, "AUDIO_CHANNEL_OUT_MONO|AUDIO_CHANNEL_OUT_STEREO");//AUDIO_CHANNEL_OUT_MONO for VTS only,   vts-tradefed run vts -m VtsHalAudioV5_0Target
        }else{
            sprintf(value, "AUDIO_CHANNEL_OUT_STEREO|AUDIO_CHANNEL_OUT_5POINT1");
        }
#endif
        ALOGD("out_get_parameters, STREAM_SUP_CHANNELS %s", value);
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value);
    }

    //sup_sampling_rates
    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES, value, sizeof(value));
    if (ret >= 0) {
        ALOGD("out_get_parameters, SUP_SAMPLING_RATES %s", value);
        uint32_t samplerate = out_get_samplerate(stream);
        found_key = true;
        memset(value, 0, sizeof(value));
#if 0
        snprintf(value, sizeof(value), "%u", samplerate);
#else
        if (forma_query == AUDIO_FORMAT_PCM_16_BIT){
            snprintf(value, sizeof(value), "44100|48000");
        }else if (forma_query == AUDIO_FORMAT_AC3 || forma_query == AUDIO_FORMAT_E_AC3){
            snprintf(value, sizeof(value), "32000|44100|48000");
        }else if (forma_query == AUDIO_FORMAT_E_AC3_JOC){
            snprintf(value, sizeof(value), "48000");
        }else{
            snprintf(value, sizeof(value), "%u", samplerate);
        }
#endif
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES, value);
    }

    //sup_formats , OUTPUT_OFFLOAD with arc only
    //ref: frameworks/av/services/audiopolicy/managerdefault/AudioPolicyManager.cpp, AudioPolicyManager::updateAudioProfiles(){ profiles.hasDynamicFormat() }
    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_FORMATS, value, sizeof(value));
    if (ret >= 0) {
        ALOGD("out_get_parameters, STREAM_SUP_FORMATS %s", value);
        found_key = true;
        first = true;
        value[0] = '\0';
        for (j = 0; j < ARRAY_SIZE(sup_output_formats_name_to_enum_table); j++) {
            if (out->output_type != OUTPUT_OFFLOAD || /*in VTS testcase will invoke this directly for each codec, reply all codec. vts-tradefed run vts -m VtsHalAudioV5_0Target*/
                (sup_output_formats_name_to_enum_table[j].enable && forma_query == AUDIO_FORMAT_DEFAULT && j != SUP_FORMAT_INDEX_PCM_16_BIT) ) {
                if (!first) {
                    strncat(value, "|", sizeof(value) - strlen(value) - 1);
                }
                strncat(value, sup_output_formats_name_to_enum_table[j].name, sizeof(value) - strlen(value) - 1);
                first = false;
                //break;
            }
        }
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_FORMATS, value);
    }

    if(found_key) {
        str = str_parms_to_str(reply);
    } else {
        //VTS test: if key not found, should return empty
        str = strdup("");
    }
    str_parms_destroy(query);
    str_parms_destroy(reply);

    ALOGD("%s,  ret:%s", __func__,str);
    return str;
}

static int out_set_volume(struct audio_stream_out* stream, float left,
                          float right)
{
    AUDIO_FUNC_PROILE
    bool ret = 0;
    struct stream_out* out = (struct stream_out*)stream;
    out->stream_volume = left;
    ret = 0;
    ALOGD("out_set_volume: [%f, %f] out=%p ret=%d written: %lld/%lld, vol:%f output_type: %i, time: %lld ns", left, right, out, ret,out->written,out->offload_writen, out->stream_volume, out->output_type, clock_time_ns());
    return ret;
}

static int out_add_audio_effect(const struct audio_stream* stream __unused, effect_handle_t effect __unused)
{
    AUDIO_FUNC_ENTER
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream* stream __unused, effect_handle_t effect __unused)
{
    AUDIO_FUNC_ENTER
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out* stream __unused,
                                        int64_t* timestamp __unused)
{
    AUDIO_FUNC_ENTER
    //case -ENOSYS: return Result::NOT_SUPPORTED;
    return -ENOSYS;
}

static int get_presentation_position(const struct audio_stream_out* stream,
        uint64_t* frames, struct timespec* timestamp, bool using_render_position __unused)
{
    AUDIO_FUNC_PROILE
    struct stream_out* out = (struct stream_out*)stream;

    if (trylock_output_stream(out)){
    }else{
        if (out->is_writting && out->last_presentation_position.valid) {
            *frames = out->last_presentation_position.frames;
            *timestamp = out->last_presentation_position.timestamp;
            return 0;
        }else{
            lock_output_stream(out);
        }
    }

    int ret = -ENODATA;
    if (out->pcm) {
        size_t avail;
        if (pcm_get_htimestamp(out->pcm, &avail, timestamp) == 0) {
            std::string traceName(ATRACE_NAME_AVAIL);
            traceName += std::to_string(out->output_type);
            ATRACE_INT(traceName.c_str(), avail);

            struct snd_pcm_status hw_status;
            ret = pcm_ioctl(out->pcm, SNDRV_PCM_IOCTL_STATUS, &hw_status);
            if (ret == 0) {
                //size_t kernel_buffer_size = out->config.period_size * out->config.period_count;
                //ALOGI("pcm[%d]: timestamp tv_sec: %ld, tv_nsec: %ld, kernel.delay: %d, hw.delay: %d, avail: %d, sr: %d",out->pcm_type,timestamp->tv_sec, timestamp->tv_nsec, kernel_buffer_size, hw_status.delay, avail, out->config.rate);
                //int64_t signed_frames = out->written - kernel_buffer_size + avail;
                int64_t signed_frames = out->written - hw_status.delay;

                if (signed_frames >= 0) {
                    *frames = signed_frames;
                    out->signed_frames = signed_frames;
                } else {
                    ret = -ENODATA;
                }
            } else {
                ALOGE("presentation_position() Get hw buffer error!!!");
                ret = -ENODATA;
            }
        }
    }

    out->last_presentation_position.frames = *frames;
    out->last_presentation_position.timestamp = *timestamp;
    out->last_presentation_position.valid = true;
    pthread_mutex_unlock(&out->lock);

    return ret;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                         uint64_t *frames, struct timespec *timestamp)
{
    return get_presentation_position(stream, frames, timestamp, false);
}

static int out_get_render_position(const struct audio_stream_out* stream,
                                   uint32_t* dsp_frames)
{
    ALOGD("%s", __func__);
    uint64_t frames=0;
    struct timespec timestamp;

    int ret = get_presentation_position(stream, &frames, &timestamp, true);
    if (dsp_frames) *dsp_frames = (uint32_t)frames;
    return ret;
}

static void adjust_mmap_period_count(struct pcm_config* config __unused, int32_t min_size_frames __unused)
{
    /*
        int periodCountRequested = (min_size_frames + config->period_size - 1) / config->period_size;
        int periodCount = MMAP_PERIOD_COUNT_MIN;

        ALOGI("%s original config.period_size = %d config.period_count = %d",
                __func__,
                config->period_size,
                config->period_count);

        while (periodCount < periodCountRequested && (periodCount * 2) < MMAP_PERIOD_COUNT_MAX) {
            periodCount *= 2;
        }
        config->period_count = periodCount;

        ALOGI("%s requested config.period_count = %d", __func__, config->period_count);
    */
}

/** audio_stream_out implementation for AAudio **/
#ifdef AAUDIO_SUPPORT
static int pcm_stop_and_check_fading(struct audio_stream_out* stream, int caller_line_)
{
    struct stream_out* out = (struct stream_out*)stream;
    struct timespec timestamp;
    size_t avail = 0;
    int64_t wait = out->time_trigger_pause_hdmi_fade_done - pli_getPTS();
    struct snd_pcm_status hw_status;

    hw_status.delay = 0;
    if (out->pcm && pcm_get_htimestamp(out->pcm, &avail, &timestamp) == 0) {
        std::string traceName(ATRACE_NAME_AVAIL);
        traceName += std::to_string(out->output_type);
        ATRACE_INT(traceName.c_str(), avail);
        pcm_ioctl(out->pcm, SNDRV_PCM_IOCTL_STATUS, &hw_status);
    }
    if (wait > 90){
        wait = wait / 90;
        usleep(wait * 1000);
    }else
        wait = 0;
    out->time_trigger_pause_hdmi_fade_done = 0;
    ALOGI("%s, %sout=%p, wait=%lldms, avail=%zu, %ld, %d",__func__, (wait>=0?"wait, ":""),stream, wait, avail, hw_status.delay, caller_line_);
    int ret = 0;
    if (out->pcm){
        ret = pcm_stop(out->pcm);
    }
    return ret;
}

static int audio_get_mmap_data_fd(int card, int *fd, int dev)
{
    int hw_fd = -1;
    char dev_name[128];
    struct snd_mmap_data mmap_fd;

    memset(&mmap_fd, 0, sizeof(mmap_fd));
    snprintf(dev_name, sizeof(dev_name), "/dev/snd/hwC%dD0", card);
    hw_fd = open(dev_name, O_RDONLY);
    mmap_fd.dev = dev;
    if (hw_fd < 0) {
        ALOGE("hw dep node open %s failed", dev_name);
        return -1;
    }
    if (ioctl(hw_fd, SNDRV_PCM_IOCTL_MMAP_DATA_FD, &mmap_fd) < 0) {
        ALOGE("hw dep node ioctl failed");
        close(hw_fd);
        return -1;
    }

    *fd = mmap_fd.fd;
    close(hw_fd); // mmap_fd should still be valid
    return 0;
}

static void* mmap_thread_loop(void* context)
{
    struct audio_stream_out* stream = (struct audio_stream_out*) context;
    struct stream_out* out = (struct stream_out*)stream;

    ALOGI("%s:%d  enter threadloop bExitThread:%d, bStopPlay:%d", __func__, __LINE__, out->bExitThread, out->bStopPlay);
    while (!out->bExitThread) {
        if (!out->bStopPlay) {
            unsigned int EmptySize = 0;
            EmptySize = pcm_mmap_avail(out->pcm);
            int count = EmptySize/out->config.period_size;

            if (EmptySize >= (out->config.period_size))
                pcm_mmap_commit(out->pcm, 0, count*out->config.period_size);
        }
        usleep(1*1000);
    }
    ALOGI("%s:%d  exit threadloop", __func__, __LINE__);

    return NULL;
}

static int create_mmap_commit_thread(struct stream_out* stream)
{
    struct stream_out* out = stream;
    pthread_cond_init(&out->mmap_cond, (const pthread_condattr_t*) NULL);
    pthread_create(&out->mmap_thread, (const pthread_attr_t*) NULL,
                   mmap_thread_loop, out);
    return 0;
}

static int out_create_mmap_buffer(const struct audio_stream_out *stream,
                                  int32_t min_size_frames,
                                  struct audio_mmap_buffer_info *info)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    const char *step;
    int ret;

    ALOGD("%s: min_size_frames: %d", __func__, min_size_frames);
    pthread_mutex_lock(&adev->audio_device_lock);
    if (info == NULL || min_size_frames <= 0 || min_size_frames > MMAP_MIN_SIZE_FRAMES_MAX) {
        ALOGE("%s: info = %p, min_size_frames = %d", __func__, info, min_size_frames);
        step = "";
        ret = -EINVAL;
        goto exit;
    }
    if (!out->standby) {
        ALOGE("%s,standby = %d", __func__, out->standby);
        step = "";
        ret = -ENOSYS;
        goto exit;
    }

    adjust_mmap_period_count(&out->config, min_size_frames);

    ALOGD("%s: channels:%d, rate:%d, format:%d, period_size:%d, period_count:%d, start_threshold:%d",__func__,
            out->config.channels,
            out->config.rate,
            out->config.format,
            out->config.period_size,
            out->config.period_count,
            out->config.start_threshold);

    out->pcm = pcm_open(adev->alsa_card, out->pcm_type,
            (PCM_OUT | PCM_MONOTONIC | PCM_MMAP | PCM_NOIRQ), &out->config);

    if (out->handle != -1) {
        AudioPatchManager::instance().notifyOutputStreamOpened(
        out->handle, {IAudioOutputMixer::StreamType::ALSA,
              static_cast<uint32_t>(out->pcm_type)});
    } else {
        ALOGW("Lacked the handle");
    }

    if (out->pcm && !pcm_is_ready(out->pcm)) {
        step = "open";
        ret = -ENODEV;
        goto exit;
    }

    {
        unsigned int frames1 = 0;
        unsigned int offset1 = 0;
        ret = pcm_mmap_begin(out->pcm, &info->shared_memory_address, &offset1, &frames1);
        if (ret < 0) {
            step = "begin";
            goto exit;
        }
    }
    info->buffer_size_frames = pcm_get_buffer_size(out->pcm);
    info->burst_size_frames = out->config.period_size;
    info->shared_memory_fd = pcm_get_poll_fd(out->pcm);

    if (adev->alsa_card != -1) {
        ret = audio_get_mmap_data_fd(adev->alsa_card, &info->shared_memory_fd, out->pcm_type);
    } else {
        ret = audio_get_mmap_data_fd(0, &info->shared_memory_fd, out->pcm_type);
    }

    if (ret < 0) {
        // Fall back to non exclusive mode
        info->shared_memory_fd = pcm_get_poll_fd(out->pcm);
    } else {
        out->mmap_shared_memory_fd = info->shared_memory_fd; // for closing later
        ALOGD("%s: opened mmap_shared_memory_fd = %d", __func__, out->mmap_shared_memory_fd);

        // FIXME: indicate exclusive mode support by returning a negative buffer size
        info->buffer_size_frames *= -1;
    }
    memset(info->shared_memory_address, 0, pcm_frames_to_bytes(out->pcm,info->buffer_size_frames));

    ret = pcm_mmap_commit(out->pcm, 0, out->config.period_size);
    if (ret < 0) {
        step = "commit";
        goto exit;
    }

    out->standby = false;
    ret = 0;

    ALOGD("%s: got mmap buffer address %p info->buffer_size_frames %d, info->burst_size_frames %d",
            __func__, info->shared_memory_address, info->buffer_size_frames, info->burst_size_frames);

exit:
    if (ret != 0) {
        if (out->pcm == NULL) {
            ALOGE("%s: %s failed - %d", __func__, step, ret);
        } else {
            ALOGE("%s: %s failed, pcm get error: %s", __func__, step, pcm_get_error(out->pcm));
            pcm_close(out->pcm);
            if (out->handle != -1) {
                AudioPatchManager::instance().notifyOutputStreamClosed(out->handle);
            }
            out->pcm = NULL;
        }
    }
    pthread_mutex_unlock(&adev->audio_device_lock);
    return ret;
}

static uint32_t out_get_sample_rate_aaudio(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    ALOGD("out_get_sample_rate_aaudio %d", out->config.rate);

    return out->config.rate;
}

static int out_get_mmap_position(const struct audio_stream_out *stream,
                                 struct audio_mmap_position *position)
{
    struct stream_out *out = (struct stream_out *)stream;
    AUDIO_LOG("out_get_mmap_position");
    if (position == NULL) {
        return -EINVAL;
    }
    if (out->pcm == NULL) {
        return -ENOSYS;
    }

    struct snd_pcm_status hw_status;
    int ret;
    int64_t audio_frame_us;

    ret = pcm_ioctl(out->pcm, SNDRV_PCM_IOCTL_STATUS, &hw_status);
    if (ret < 0) {
        ALOGE("%s: %s", __func__, pcm_get_error(out->pcm));
        return -ENOSYS;
    }

    struct timespec ts = hw_status.tstamp;
    audio_frame_us = hw_status.audio_tstamp.tv_sec * 1000000LL + hw_status.audio_tstamp.tv_nsec / 1000;
    position->position_frames = audio_frame_us * out->config.rate / 1000000;

    position->time_nanoseconds = ns_from_timespec(&ts);
    AUDIO_LOG("position_frames: %d, time_nanoseconds: %lld",
            position->position_frames,
            position->time_nanoseconds);
    return 0;
}

static size_t out_get_buffer_size_aaudio(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    ALOGD("out_get_buffer_size_aaudio");

#if PLATFORM_SDK_VERSION < 22
    return out->config.period_size * audio_stream_frame_size((const struct audio_stream_out *)stream);
#else
    return out->config.period_size * audio_stream_out_frame_size((const struct audio_stream_out *)stream);
#endif
}

static uint32_t out_get_latency_aaudio(const struct audio_stream_out *stream)
{
    struct stream_out* out = (struct stream_out*)stream;
    struct audio_device* adev = out->dev;
    uint32_t latency = 0;
    if (adev->a2dp_stream_out) {
        latency = adev->a2dp_stream_out->get_latency(adev->a2dp_stream_out);
    } else {
        latency = (out->config.period_count * out->config.period_size * 1000) / (out->config.rate);
    }

    return latency;
}

static ssize_t out_write_aaudio(struct audio_stream_out *stream __unused, const void *buffer __unused, size_t bytes __unused)
{
    return -ENOSYS;
}

static void _update_avr_force_pcm(audio_usage_t usage, int deviceId)
{
    int rtkaudiofd = open ("/dev/rtkaudio", O_RDWR);

    if(rtkaudiofd >= 0) {
        char cmd[128];

        //ACPU log:
        //  [ARPC] skip all only force pcm 0->1
        //  [AO] AMixer3 stop , clean force pcm
        sprintf(cmd, "fw@ skip_all_force_pcm %d %d", (usage == AUDIO_USAGE_GAME), (deviceId+1));
        write(rtkaudiofd, cmd, strlen(cmd)+1);
        close(rtkaudiofd);
        rtkaudiofd = -1;
        ALOGI("%s:%d, update skip_all_force_pcm %d, device %d", __func__,__LINE__, (usage == AUDIO_USAGE_GAME), deviceId);
    }else{
        ALOGE("%s:%d, error, can't open rtkaudio", __func__,__LINE__);
    }
}

static void out_update_source_metadata(struct audio_stream_out *stream,
                                   const struct source_metadata* source_metadata)
{
    struct stream_out *out = (struct stream_out *)stream;

    if (stream == nullptr || source_metadata == nullptr ||
        source_metadata->tracks == nullptr)
        ALOGD("%s:%d,stream=%p source_metadata=%p",__func__,__LINE__,stream, source_metadata);
    else{
        if (out->source_metadata_usage != source_metadata->tracks->usage){
            _update_avr_force_pcm(source_metadata->tracks->usage, out->pcm_type);
            out->source_metadata_usage = source_metadata->tracks->usage;
        }

        ALOGD("%s:%d,stream=%p track_count=%d usage=%d content=%d gain=%f ret=%d",
            __func__,__LINE__,stream,
            source_metadata->track_count,
            source_metadata->tracks->usage,
            source_metadata->tracks->content_type,
            source_metadata->tracks->gain);
    }
}

static void out_update_source_metadata_v7(struct audio_stream_out *stream,
                                      const struct source_metadata_v7* source_metadata)
{
    struct stream_out *out = (struct stream_out *)stream;

    if (stream == nullptr || source_metadata == nullptr ||
        source_metadata->tracks == nullptr){
        ALOGD("%s:%d,stream=%p source_metadata=%p",__func__,__LINE__,stream, source_metadata);
    }else{
        if (out->source_metadata_usage != source_metadata->tracks->base.usage){
            _update_avr_force_pcm(source_metadata->tracks->base.usage, out->pcm_type);
            out->source_metadata_usage = source_metadata->tracks->base.usage;
        }

        ALOGD("%s:%d,stream=%p track_count=%d usage=%d content=%d gain=%f mask=x%x",
            __func__,__LINE__,stream,
            source_metadata->track_count,
            source_metadata->tracks->base.usage,
            source_metadata->tracks->base.content_type,
            source_metadata->tracks->base.gain,
            source_metadata->tracks->channel_mask);
    }
}

static int out_start(const struct audio_stream_out* stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    int ret = -ENOSYS;
    struct timespec timestamp;
    size_t avail = 0;

    ALOGD("out_start");
    if(out->pcm == NULL) {
        ALOGD("out_start, but pcm is NULL!");
        return ret;
    }

    pthread_mutex_lock(&adev->audio_device_lock);
    pcm_prepare(out->pcm);
    pcm_get_htimestamp(out->pcm, &avail, &timestamp);
    pcm_mmap_commit(out->pcm, 0, out->config.period_size);
    out->bStopPlay = false;
    out->bExitThread = false;

    if(out->pcm == NULL)
    {
        ALOGE("%s: pcm stream is null",__func__);
        goto error;
    }
    if (!pcm_is_ready(out->pcm))
    {
        ALOGE("%s: pcm stream is not ready",__func__);
        goto error;
    }
    ret = pcm_start(out->pcm);
    if (ret < 0) {
        ALOGE("%s: MMAP pcm_start failed ret %d", __func__, ret);
    }
    create_mmap_commit_thread(out);

error:
    pthread_mutex_unlock(&adev->audio_device_lock);
    return ret;
}

static int out_stop(const struct audio_stream_out* stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    int ret = -ENOSYS;

    ALOGD("out_stop");
    pthread_mutex_lock(&adev->audio_device_lock);
    if (out->bExitThread == false) {
        out->bStopPlay = true;
        out->bExitThread = true;
    } else {
        ret = 0;
        goto error;
    }
    destroy_mmap_commit_thread(out);
    if(out->pcm == NULL)
    {
        ALOGE("%s: pcm stream is null",__func__);
        goto error;
    }
    ret = pcm_stop_and_check_fading((struct audio_stream_out*)stream, __LINE__);
    if (ret < 0) {
        ALOGE("%s: MMAP pcm_stop failed ret %d", __func__, ret);
    }

error:
    pthread_mutex_unlock(&adev->audio_device_lock);
    return ret;
}
#endif

/** audio_stream_in implementation **/

static int in_start(const struct audio_stream_in* stream)
{
    struct stream_in* in = (struct stream_in*)stream;
    struct audio_device* adev = in->dev;
    int ret = -ENOSYS;

    ALOGD("in_start");
    pthread_mutex_lock(&adev->audio_device_lock);
    if (in->pcm == NULL) {
        ALOGE("%s: pcm stream is null", __func__);
        goto error;
    }
    if (!pcm_is_ready(in->pcm)) {
        ALOGE("%s: pcm stream is not ready", __func__);
        goto error;
    }
    ret = pcm_start(in->pcm);
    if (ret < 0) {
        ALOGE("%s: MMAP pcm_start failed ret %d", __func__, ret);
    }

error:
    pthread_mutex_unlock(&adev->audio_device_lock);
    return ret;
}

static int in_stop(const struct audio_stream_in* stream)
{
    struct stream_in* in = (struct stream_in*)stream;
    struct audio_device* adev = in->dev;
    int ret = -ENOSYS;

    ALOGD("in_stop");
    pthread_mutex_lock(&adev->audio_device_lock);
    if(in->pcm == NULL) {
        ALOGE("%s: pcm stream is null", __func__);
        goto error;
    }
    ret = pcm_stop(in->pcm);
    if (ret < 0) {
        ALOGE("%s: MMAP pcm_stop failed ret %d", __func__, ret);
    }

error:
    pthread_mutex_unlock(&adev->audio_device_lock);
    return ret;
}

static int in_create_mmap_buffer(const struct audio_stream_in* stream,
                                 int32_t min_size_frames,
                                 struct audio_mmap_buffer_info* info)
{
    struct stream_in* in = (struct stream_in*)stream;
    struct audio_device* adev = in->dev;
    int ret = 0;
    unsigned int offset1 = 0;
    unsigned int frames1 = 0;
    const char* step = "";

    ALOGV("in_create_mmap_buffer: min_size_frames=%d", min_size_frames);
    pthread_mutex_lock(&adev->audio_device_lock);
    if (info == NULL || min_size_frames <= 0 || min_size_frames > MMAP_MIN_SIZE_FRAMES_MAX) {
        ALOGE("%s invalid argument info %p min_size_frames %d", __func__, info, min_size_frames);
        ret = -EINVAL;
        goto exit;
    }
    if (!in->standby) {
        ALOGE("%s: standby = %d", __func__, in->standby);
        ret = -ENOSYS;
        goto exit;
    }

    adjust_mmap_period_count(&in->config, min_size_frames);
    // We only support device 0 for pcm_in
    in->pcm = pcm_open(adev->alsa_card, 0,
                       (PCM_IN | PCM_MMAP | PCM_MONOTONIC), &in->config);
    if (in->pcm == NULL || !pcm_is_ready(in->pcm)) {
        step = "open";
        ret = -ENODEV;
        goto exit;
    }

    ret = pcm_mmap_begin(in->pcm, &info->shared_memory_address, &offset1, &frames1);
    if (ret < 0)  {
        step = "begin";
        goto exit;

    }
    info->buffer_size_frames = pcm_get_buffer_size(in->pcm);
    info->burst_size_frames = in->config.period_size;
    info->shared_memory_fd = pcm_get_poll_fd(in->pcm);

    memset(info->shared_memory_address, 0, pcm_frames_to_bytes(in->pcm,
            info->buffer_size_frames));

    ret = pcm_mmap_commit(in->pcm, 0, MMAP_PERIOD_SIZE);
    if (ret < 0) {
        step = "commit";
        goto exit;
    }

    in->standby = false;
    ret = 0;

    ALOGD("%s: got mmap buffer address %p info->buffer_size_frames %d",
          __func__, info->shared_memory_address, info->buffer_size_frames);

exit:
    if (ret != 0) {
        if (in->pcm == NULL) {
            ALOGE("%s: %s - %d", __func__, step, ret);

        } else {
            ALOGE("%s: %s %s", __func__, step, pcm_get_error(in->pcm));
            pcm_close(in->pcm);
            in->pcm = NULL;
        }
    }
    pthread_mutex_unlock(&adev->audio_device_lock);
    return ret;
}

static int in_get_mmap_position(const struct audio_stream_in* stream,
                                struct audio_mmap_position* position)
{
    struct stream_in* in = (struct stream_in*)stream;
    ALOGD("%s", __func__);
    if (position == NULL) {
        ALOGE("position == NULL, return -EINVAL(%d)", -EINVAL);
        return -EINVAL;
    }
    if (in->pcm == NULL) {
        ALOGE("in->pcm == NULL, return -ENOSYS(%d)", -ENOSYS);
        return -ENOSYS;
    }

    struct timespec ts = { 0, 0  };
    int ret = pcm_mmap_get_hw_ptr(in->pcm, (unsigned int*)&position->position_frames, &ts);
    if (ret < 0) {
        ALOGE("%s: %s", __func__, pcm_get_error(in->pcm));
        return ret;
    }
    position->time_nanoseconds = ns_from_timespec(&ts);
    return 0;
}

/* must be called with hw device and input stream mutexes locked */
static int start_input_stream(struct stream_in* in)
{
    resetAudioDebug();
    AUDIO_FUNC_ENTER
    struct audio_device* adev = in->dev;

    ALOGD("%s: mic_property: %s\n", __FUNCTION__, mic_property);

    adev->active_input = in;

    if (in->aux_channels_changed) {
        in->aux_channels_changed = false;
        in->config.channels = popcount(in->main_channels | in->aux_channels);

        if (in->resampler) {
            /* release and recreate the resampler with the new number of channel of the input */
            release_resampler(in->resampler);
            in->resampler = NULL;
            ALOGD("in->requested_rate = %d\n", in->requested_rate);
            ALOGD("in->config.rate = %d\n", in->config.rate);
            create_resampler(in->config.rate,
                                   in->requested_rate,
                                   in->config.channels,
                                   RESAMPLER_QUALITY_DEFAULT,
                                   &in->buf_provider,
                                   &in->resampler);
        }
        ALOGD("%s: New channel configuration, "
              "main_channels = [%04x], aux_channels = [%04x], config.channels = %d",
              __FUNCTION__, in->main_channels, in->aux_channels, in->config.channels);
    }


    ALOGD("%s: rate = %d , channels = %d, format = %d, period_size=%d, period_count = %d",
          __FUNCTION__, in->config.rate , in->config.channels , in->config.format, in->config.period_size , in->config.period_count);

    if (!strcmp(mic_property, AICAP_CONFIG)) {   /* this assumes routing is done previously */
        if (adev->alsa_card != -1) {
            in->pcm = pcm_open(adev->alsa_card, 0, PCM_IN, &in->config);  //card 0 for AI capture
        } else {
            in->pcm = pcm_open(0, 0, PCM_IN, &in->config);  //card 0 for AI capture
        }
    } else if (!strcmp(mic_property, INMIC_CONFIG)) {
        if (AUDIO_DEVICE_IN_ECHO_REFERENCE == (in->device | AUDIO_DEVICE_BIT_IN)) {
            in->pcm = pcm_open(adev->alsa_card, 4, PCM_IN | PCM_MONOTONIC, &in->config);  // card 0 device 4 for ECHO_REFERENCE
        } else {
            in->pcm = pcm_open(adev->alsa_card, 3, PCM_IN | PCM_MONOTONIC, &in->config); // card 0 device 3 for BUILD_IN_MIC
        }
    } else if (!strcmp(mic_property, DMIC_CONFIG)) {
        in->pcm = pcm_open(adev->alsa_card, 2, PCM_IN, &in->config);  // card 0 device 2 for DMIC
    } else {
        in->pcm = pcm_open(adev->alsa_card + 1, 0, PCM_IN, &in->config);  //card 1 for AO capture
    }

    if (!pcm_is_ready(in->pcm)) {
        ALOGE("cannot open pcm_in driver: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        adev->active_input = NULL;
        return -ENOMEM;
    }

#ifdef DUMP_TO_FILE
    char property[PROPERTY_VALUE_MAX];
    if (property_get(INDTF_PROPERTY, property, NULL)) {
        ALOGD("%s = %s\n", INDTF_PROPERTY, property);
        char dumpPath[PROPERTY_VALUE_MAX];
        memset(dumpPath, 0, sizeof(dumpPath));
        snprintf(dumpPath, sizeof(dumpPath), "%sin%d.pcm", property, fp_dumpInIndex);
        ALOGD("open: %s\n", dumpPath);
        fp_indump = fopen(dumpPath , "a+");
        if (!fp_indump) {
            ALOGE("open file failed %s\n", strerror(errno));
        } else {
            fp_dumpInIndex++;
        }
    }
#endif
    /* force read and proc buf reallocation case of frame size or channel count change */
    in->read_buf_frames = 0;
    in->read_buf_size = 0;
    in->proc_buf_frames = 0;
    in->proc_buf_size = 0;
    /* if no supported sample rate is available, use the resampler */
    if (in->resampler) {
        in->resampler->reset(in->resampler);
    }
    return 0;
}

static audio_devices_t in_get_device(const struct audio_stream* stream) {
  struct stream_in* in = (struct stream_in*)stream;
  return (audio_devices_t)(in->device | AUDIO_DEVICE_BIT_IN);
}

static uint32_t in_get_sample_rate(const struct audio_stream* stream)
{
    struct stream_in* in = (struct stream_in*)stream;
    ALOGD("[%s] requested_rate=%d, config_rate=%d", __FUNCTION__, in->requested_rate, in->config.rate);

#ifdef A2DP_ENABLED
    struct audio_device* adev = in->dev;
    if (in->use_a2dp && adev->a2dp_stream_in) {
        return adev->a2dp_stream_in->common.get_sample_rate(stream);
    }
#endif

#ifdef AUDIO_HAL_4_0
    if (AUDIO_DEVICE_IN_BUILTIN_MIC == (in->device | AUDIO_DEVICE_BIT_IN)) {
        ALOGD("[%s] AUDIO_DEVICE_IN_BUILTIN_MIC", __FUNCTION__);
        return in->config.rate;
    } else {
        return in->requested_rate;
    }
#else
    return in->config.rate;
#endif
}

static int in_set_sample_rate(struct audio_stream* stream, uint32_t rate)
{

#ifdef A2DP_ENABLED
    struct stream_in* in = (struct stream_in*)stream;
    struct audio_device* adev = in->dev;
    if (in->use_a2dp && adev->a2dp_stream_in) {
        return adev->a2dp_stream_in->common.set_sample_rate(stream, rate);
    }
#endif

#ifdef AUDIO_HAL_4_0
    //[from header comment]
    /* currently unused - use set_parameters with key
       AUDIO_PARAMETER_STREAM_SAMPLING_RATE */
    return 0;
#else
    struct stream_in* in = (struct stream_in*)stream;
    if(rate < 4000 || rate > 48000) {
        return - EINVAL;
    }
    in->requested_rate = rate;
    return 0;
#endif
}

static size_t in_get_buffer_size(const struct audio_stream* stream)
{
    struct stream_in* in = (struct stream_in*)stream;

#ifdef A2DP_ENABLED
    if (in->use_a2dp) {
        return 320;
    }
#endif

#if PLATFORM_SDK_VERSION < 22
    int size = in->config.period_size * audio_stream_frame_size((const struct audio_stream_in*)stream);
#else
    int size = in->config.period_size * audio_stream_in_frame_size((const struct audio_stream_in*)stream);
#endif
    ALOGI("getsize:%d", size);
    return size;
    /*return get_input_buffer_size(in->requested_rate,
                                 AUDIO_FORMAT_PCM_16_BIT,
                                 popcount(in->main_channels));*/
}

static audio_channel_mask_t in_get_channels(const struct audio_stream* stream)
{
    struct stream_in* in = (struct stream_in*)stream;

#ifdef A2DP_ENABLED
    struct audio_device* adev = in->dev;
    if (in->use_a2dp && adev->a2dp_stream_in) {
        return adev->a2dp_stream_in->common.get_channels(stream);
    }
#endif

    return (audio_channel_mask_t)in->main_channels;
}

static audio_format_t in_get_format(const struct audio_stream* stream)
{
    struct stream_in* in = (struct stream_in*)stream;

#ifdef A2DP_ENABLED
    if (in->use_a2dp) {
        return AUDIO_FORMAT_PCM_16_BIT;
    }
#endif

    if (!in->config.format) {
        return AUDIO_FORMAT_PCM_16_BIT;
    }

    if(in->config.format == PCM_FORMAT_S32_LE){
       return AUDIO_FORMAT_PCM_32_BIT;
    }else{
       return AUDIO_FORMAT_PCM_16_BIT;
    }
}

static int in_set_format(struct audio_stream* stream, audio_format_t format)
{
    struct stream_in* in = (struct stream_in*)stream;

#ifdef A2DP_ENABLED
    if (in->use_a2dp) {
        (void)(stream);
        if (format == AUDIO_FORMAT_PCM_16_BIT) {
            return 0;
        } else {
            return -1;
        }
    }
#endif

    in->config.format = (enum pcm_format)format;
    return 0;
}

/* must be called with hw device and input stream mutexes locked */
static int do_input_standby(struct stream_in* in)
{
    struct audio_device* adev = in->dev;

    if (!in->standby) {
        pcm_close(in->pcm);
        in->pcm = NULL;

        adev->active_input = NULL;
        in->standby = 1;
    }
    return 0;
}

static int in_standby(struct audio_stream* stream)
{
    struct stream_in* in = (struct stream_in*)stream;
    int status;

    pthread_mutex_lock(&in->dev->audio_device_lock);
    lock_input_stream(in);
    status = do_input_standby(in);
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->audio_device_lock);
    return status;
}

static int in_dump(const struct audio_stream* stream __unused, int fd __unused)
{
    return 0;
}

static int in_set_parameters(struct audio_stream* stream, const char* kvpairs)
{
    struct stream_in* in = (struct stream_in*)stream;

#ifdef A2DP_ENABLED
    if (in->use_a2dp) {
        (void)(stream);
        (void)(kvpairs);
        return 0;
    }
#endif

    struct audio_device* adev = in->dev;
    struct str_parms* parms;
    char value[32];
    int ret, ret_get;
    bool do_standby = false;

#ifdef AUDIO_HAL_4_0_ONLY
    /*
    case 0: return Result::OK;
    case -EINVAL: return Result::INVALID_ARGUMENTS;
    case -ENODATA: return Result::INVALID_STATE;
    case -ENODEV: return Result::NOT_INITIALIZED;
    case -ENOSYS: return Result::NOT_SUPPORTED;
    default: return Result::INVALID_STATE;
    */
    ret = -ENOSYS;
#else
    /*
    case 0: return Result::OK;
    case -ENOSYS: return Result::INVALID_STATE;
    default: return Result::INVALID_ARGUMENTS;
    */
    ret = -ENOENT;
#endif
    ALOGD("in_set_parameters, kvpairs: %s", kvpairs);
    //VTS test: empty parameter expect return OK!
    if (strcmp(kvpairs, "") == 0) {
        ALOGW("get empty parameters");
        return 0;
    }

    parms = str_parms_create_str(kvpairs);

    ret_get = str_parms_get_str(parms, AUDIO_PARAMETER_DEVICE_CONNECT, value, sizeof(value));
    if (ret_get >= 0) {
        audio_devices_t device = (audio_devices_t)strtoul(value, NULL, 10);
        ALOGE("in_set_parameters, DEVICE_CONNECT: %x", device);
        ret = 0;
    }

    ret_get = str_parms_get_str(parms, AUDIO_PARAMETER_DEVICE_DISCONNECT, value, sizeof(value));
    if (ret_get >= 0) {
        audio_devices_t device = (audio_devices_t)strtoul(value, NULL, 10);
        ALOGE("in_set_parameters, DEVICE_DISCONNECT: %x", device);
        ret = 0;
    }

    ret_get = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE, value, sizeof(value));

    pthread_mutex_lock(&adev->audio_device_lock);
    lock_input_stream(in);
    if (ret_get >= 0) {
        int val = atoi(value);
        /* no audio source uses val == 0 */
        if ((in->source != val) && (val != 0) && !(in->flags & AUDIO_INPUT_FLAG_MMAP_NOIRQ)) {
            ALOGI("in_set_parameters() source=%d val=%d, do_standby!", in->source, val);
            in->source = val;
            do_standby = true;
        }
        ret = 0;
    }

    ret_get = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret_get >= 0) {
        int val = atoi(value) & ~AUDIO_DEVICE_BIT_IN;
        if ((in->device != val) && (val != 0) && !(in->flags & AUDIO_INPUT_FLAG_MMAP_NOIRQ)) {
            ALOGI("in_set_parameters() device=%d val=%d, do_standby!", in->device, val);
            in->device = val;
            do_standby = true;
            /* make sure new device selection is incompatible with multi-mic pre processing
             * configuration */
            //in_update_aux_channels(in, NULL);
        }
        ret = 0;
    }

    if (do_standby) {
        do_input_standby(in);
    }
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&adev->audio_device_lock);

    str_parms_destroy(parms);
    return ret;
}

static char* in_get_parameters(const struct audio_stream* stream,
                               const char* keys)
{
    struct stream_in* in = (struct stream_in*)stream;

#ifdef A2DP_ENABLED
    if (in->use_a2dp) {
        (void)(stream);
        (void)(keys);
        return strdup("");
    }
#endif

    //struct audio_device *adev = in->dev;
    struct str_parms* parms;
    struct str_parms* out_parms;
    char* str;
    char value[256];
    int ret;
    bool found_key = false;

    parms = str_parms_create_str(keys);
    out_parms = str_parms_create();

    /*
    pthread_mutex_lock(&adev->audio_device_lock);
    lock_input_stream(in); */

    ALOGD("in_get_parameters ,input keys: %s", keys);
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE, value, sizeof(value));
    if (ret >= 0) {
        char str1[20];
        memset(str1, 0, sizeof(str1));
        snprintf(str1, sizeof(str1), "%d", in->source);
        str_parms_add_str(out_parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE, str1);
        found_key = true;
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret >= 0) {
        char str1[20];
        memset(str1, 0, sizeof(str1));
        snprintf(str1, sizeof(str1), "%d", (in->device | AUDIO_DEVICE_BIT_IN));
        str_parms_add_str(out_parms, AUDIO_PARAMETER_STREAM_ROUTING, str1);
        found_key = true;
    }

#ifdef AUDIO_HAL_4_0
    //sup_format case:
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_SUP_FORMATS, value, sizeof(value));
    if (ret >= 0) {
        size_t j;
        audio_format_t sup_format = in_get_format(stream);
        found_key = true;
        bool first = true;
        memset(value, 0, sizeof(value));
        for (j = 0; j < ARRAY_SIZE(sup_input_formats_name_to_enum_table); j++) {
            if (sup_input_formats_name_to_enum_table[j].value == sup_format) {
                if (!first) {
                    strncat(value, "|", sizeof(value) - strlen(value) - 1);
                }
                strncat(value, sup_input_formats_name_to_enum_table[j].name, sizeof(value) - strlen(value) - 1);
                first = false;
                //break;
            }
        }
        str_parms_add_str(out_parms, AUDIO_PARAMETER_STREAM_SUP_FORMATS, value);
    }
#endif

    /*
    pthread_mutex_unlock(&adev->audio_device_lock);
    pthread_mutex_unlock(&in->lock); */

    if(found_key) {
        str = str_parms_to_str(out_parms);
    } else {
        //VTS test: if key not found, should return empty
        str = strdup("");
    }
    str_parms_destroy(out_parms);
    str_parms_destroy(parms);

    return str;
}

static int in_set_gain(struct audio_stream_in* stream __unused, float gain __unused)
{
    return 0;
}

static int get_next_buffer(struct resampler_buffer_provider* buffer_provider,
                           struct resampler_buffer* buffer)
{
    struct stream_in* in;

    if (buffer_provider == NULL || buffer == NULL) {
        return -EINVAL;
    }

    in = (struct stream_in*)((char*)buffer_provider -
                             offsetof(struct stream_in, buf_provider));

    if (in->pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    if (in->read_buf_frames == 0) {
        size_t size_in_bytes = in->config.period_size * in->config.channels * 2;
        if (in->read_buf_size < in->config.period_size) {
            in->read_buf_size = in->config.period_size;
            // KWarning: checked ok by scorpio_ren
            in->read_buf = (int16_t*) realloc(in->read_buf, size_in_bytes);
            ALOG_ASSERT((in->read_buf != NULL),
                        "get_next_buffer() failed to reallocate read_buf");
            if(in->read_buf == NULL) {
                ALOGE("get_next_buffer(): in->read_buf realloc failed");
                return -ENOMEM;
            }
            ALOGV("get_next_buffer(): read_buf %p extended to %d bytes",
                  in->read_buf, size_in_bytes);
        }

        in->read_status = pcm_read(in->pcm, (void*)in->read_buf, size_in_bytes);

        if (in->read_status != 0) {
            ALOGE("get_next_buffer() pcm_read error %d", in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }

        in->read_buf_frames = in->config.period_size;

        if ((popcount(in->main_channels) == 1) && (in->config.channels != 1)) {
            for(size_t i = 0; i < in->read_buf_frames; i++) {
                in->read_buf[i] = in->read_buf[i * 2];
            }
        } else if ((popcount(in->main_channels) == 2) && (in->config.channels == 1)) {
            size_in_bytes = in->config.period_size * popcount(in->main_channels) * 2;
            in->temp_buf = (int16_t*) realloc(in->temp_buf, size_in_bytes);

            if(in->temp_buf != NULL) {
                for(size_t i = 0; i < in->read_buf_frames; i++) {
                    in->temp_buf[i * 2] = in->read_buf[i];
                    in->temp_buf[i * 2 + 1] = in->read_buf[i];
                }
            }
            if(in->temp_buf == NULL) {
                ALOGE("get_next_buffer() in->temp_buf realloc failed");
                return -ENOMEM;
            }
        }
    }

    if ((popcount(in->main_channels) == 2) && (in->config.channels == 1)) {
        buffer->frame_count = (buffer->frame_count > in->read_buf_frames) ? in->read_buf_frames : buffer->frame_count;
        buffer->i16 = in->temp_buf + (in->config.period_size - in->read_buf_frames) * popcount(in->main_channels);
    } else {
        buffer->frame_count = (buffer->frame_count > in->read_buf_frames) ? in->read_buf_frames : buffer->frame_count;
        buffer->i16 = in->read_buf + (in->config.period_size - in->read_buf_frames) * popcount(in->main_channels);
    }

    return in->read_status;
}

static void release_buffer(struct resampler_buffer_provider* buffer_provider,
                           struct resampler_buffer* buffer)
{
    struct stream_in* in;

    if (buffer_provider == NULL || buffer == NULL) {
        return;
    }

    in = (struct stream_in*)((char*)buffer_provider -
                             offsetof(struct stream_in, buf_provider));

    in->read_buf_frames -= buffer->frame_count;
}

/* read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified */
static ssize_t read_frames(struct stream_in* in, void* buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;

    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;
        if (in->resampler != NULL) {
#if PLATFORM_SDK_VERSION < 22
            in->resampler->resample_from_provider(in->resampler,
                                                  (int16_t*)((char*)buffer +
                                                          frames_wr * audio_stream_frame_size(&in->stream)),
                                                  &frames_rd);
#else
            in->resampler->resample_from_provider(in->resampler,
                                                  (int16_t*)((char*)buffer +
                                                          frames_wr * audio_stream_in_frame_size(&in->stream)),
                                                  &frames_rd);
#endif
        } else {
            /*struct resampler_buffer buf = {
                    { raw : NULL, },
                    frame_count : frames_rd,
            };*/
            struct resampler_buffer buf;
            buf.raw = NULL;
            buf.frame_count = frames_rd;
            get_next_buffer(&in->buf_provider, &buf);
            if (buf.raw != NULL) {
#if PLATFORM_SDK_VERSION < 22
                memcpy((char*)buffer +
                       frames_wr * audio_stream_frame_size(&in->stream),
                       buf.raw,
                       buf.frame_count * audio_stream_frame_size(&in->stream));
#else
                memcpy((char*)buffer +
                       frames_wr * audio_stream_in_frame_size(&in->stream),
                       buf.raw,
                       buf.frame_count * audio_stream_in_frame_size(&in->stream));
#endif
                frames_rd = buf.frame_count;
            }
            release_buffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0) {
            return in->read_status;
        }

        frames_wr += frames_rd;
    }

    return frames_wr;
}

static ssize_t in_read(struct audio_stream_in* stream, void* buffer, size_t bytes)
{
    int ret = 0;
    char property[PROPERTY_VALUE_MAX];
    struct stream_in* in = (struct stream_in*)stream;
    struct audio_device* adev = in->dev;

    const size_t frame_size = audio_stream_in_frame_size(stream);
    const size_t frames = bytes / frame_size;

#ifdef A2DP_ENABLED
    if (in->use_a2dp && adev->a2dp_stream_in) {
        return adev->a2dp_stream_in->read(stream, buffer, bytes);
    }
#endif

#if PLATFORM_SDK_VERSION < 22
    size_t frames_rq = bytes / audio_stream_frame_size(stream);
#else
    size_t frames_rq = bytes / audio_stream_in_frame_size(stream);
#endif
    int16_t* read_buffer, *p;
    size_t i;

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the input stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->audio_device_lock);
    lock_input_stream(in);
    if (in->standby) {
        ALOGV("in_read start_input_stream");
        ret = start_input_stream(in);
        if (ret == 0) {
            in->standby = 0;
        }
    }
    pthread_mutex_unlock(&adev->audio_device_lock);

    if (ret < 0) {
        goto exit;
    } else if (in->resampler != NULL) {
        ret = read_frames(in, buffer, frames_rq);
    } else {
        if ((popcount(in->main_channels) == 1) && (in->config.channels != 1)) {
            read_buffer = (int16_t*)malloc(in->config.period_size * in->config.channels * 2);
            if(read_buffer != NULL) {
                ret = pcm_read(in->pcm, read_buffer, bytes * 2);
                p = (int16_t*) buffer;

                for(i = 0; i < frames_rq; i++) {
                    p[i] = (int16_t) read_buffer[i * 2];
                }
                free(read_buffer);
            } else {
                ALOGE("%s: FAIL to malloc in read_buffer, size:%i", __FUNCTION__, in->config.period_size * in->config.channels * 2);
                goto exit;
            }
        } else if ((popcount(in->main_channels) == 2) && (in->config.channels == 1)) {
            read_buffer = (int16_t*)malloc(in->config.period_size * in->config.channels * 2);
            if(read_buffer != NULL) {
                ret = pcm_read(in->pcm, read_buffer, bytes / 2);
                p = (int16_t*) buffer;

                for(i = 0; i < frames_rq; i++) {
                    p[2 * i] = (int16_t) read_buffer[i];
                    p[2 * i + 1] = (int16_t) read_buffer[i];
                }
                free(read_buffer);
            } else {
                ALOGE("%s: FAIL to malloc in read_buffer, size:%i", __FUNCTION__, in->config.period_size * in->config.channels * 2);
                goto exit;
            }
        } else {
            ret = pcm_read(in->pcm, buffer, bytes);
#ifdef DUMP_TO_FILE
            if (fp_indump) {
                int wbRet = fwrite(buffer, bytes, 1, fp_indump);
                AUDIO_LOG("in read to file 1: %d", wbRet);
            }
#endif
        }
    }

    if (ret > 0) {
        ret = 0;
    }

    if (ret == 0 && adev->mic_mute) {
        memset(buffer, 0, bytes);
    }

exit:
#if PLATFORM_SDK_VERSION < 22
    if (ret < 0)
        usleep(bytes * 1000000 / audio_stream_frame_size((const struct audio_stream_in*)stream) /
               in_get_sample_rate(&stream->common));
#else
    if (ret < 0)
        usleep(bytes * 1000000 / audio_stream_in_frame_size((const struct audio_stream_in*)stream) /
               in_get_sample_rate(&stream->common));
#endif

    pthread_mutex_unlock(&in->lock);

    if(property_get(ECHO_REFERNCE_DUMP, property, NULL) && (atoi(property) == 1)){
        if(AUDIO_DEVICE_IN_ECHO_REFERENCE == (in->device | AUDIO_DEVICE_BIT_IN)){
                echo_fp_indump = fopen("/data/local/echo_reference.pcm", "a+");
                if(echo_fp_indump == NULL){
                    ALOGW("echo_fp_indump is NULL %d", errno);
                } else {
                    fwrite(buffer, bytes, 1, echo_fp_indump);
                    ALOGW("echo in_read :%i", bytes);
                    fclose(echo_fp_indump);
                }
        }
        if(AUDIO_DEVICE_IN_BUILTIN_MIC == (in->device | AUDIO_DEVICE_BIT_IN)){
                inmic_fp_indump = fopen("/data/local/builtin_mic.pcm", "a+");
                if(inmic_fp_indump == NULL){
                    ALOGW("inmic_fp_indump is NULL %d", errno);
                } else {
                    fwrite(buffer, bytes, 1, inmic_fp_indump);
                    ALOGW("builtin in_read :%i", bytes);
                    fclose(inmic_fp_indump);
                }
        }
    }

    // AUDIO_LOG("in_read :%i", bytes);

    if (bytes > 0) {
        in->read_frames += frames;
    }
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in* stream __unused)
{
    AUDIO_FUNC_ENTER
    return 0;
}

static int in_get_capture_position(const struct audio_stream_in *stream, int64_t *frames,
                                        int64_t *time)
{
    if (stream == nullptr) {
        ALOGE("%s stream is NULL",__FUNCTION__);
        return -EINVAL;
    } else if (frames == nullptr){
        ALOGE("%s frames is NULL", __FUNCTION__);
        return -EINVAL;
    } else if (time == nullptr){
        ALOGE("%s time is NULL", __FUNCTION__);
        return -EINVAL;
    }

    struct stream_in *in = (struct stream_in *)stream;

    lock_input_stream(in);

    if (in->pcm) {
        struct timespec pcm_timestamp;
        size_t avail_frames;
        if (pcm_get_htimestamp(in->pcm, &avail_frames, &pcm_timestamp) == 0) {
            *frames = in->read_frames + avail_frames;
            *time = pcm_timestamp.tv_sec * 1000000000LL + pcm_timestamp.tv_nsec;
            pthread_mutex_unlock(&in->lock);
            return 0;
        }else{
            ALOGE("%s get timestamp fail", __FUNCTION__);
            pthread_mutex_unlock(&in->lock);
            return -ENOSYS;
        }
    } else {
        ALOGE("%s stream is not pcm",__FUNCTION__);
        pthread_mutex_unlock(&in->lock);
        return -ENOSYS;
    }
}

static int in_add_audio_effect(const struct audio_stream* stream __unused, effect_handle_t effect __unused)
{
    AUDIO_FUNC_ENTER
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream* stream __unused, effect_handle_t effect __unused)
{
    AUDIO_FUNC_ENTER
    return 0;
}

#ifdef AUDIO_HAL_4_0_ONLY
static int adev_get_microphones(const struct audio_hw_device* dev,
                                struct audio_microphone_characteristic_t* mic_array,
                                size_t* mic_count)
{
    AUDIO_FUNC_ENTER
    struct audio_device* adev = (struct audio_device*)dev;

    //check parameters
    if ((mic_array == NULL) || (mic_count == NULL)) {
        return (-EINVAL);
    }

    pthread_mutex_lock(&adev->audio_device_lock);
    //[From **/hardware/libhardware/include/hardware/audio.h]
    //if mic_count is passed as zero, mic_array will not be populated,
    //and mic_count will return the actual number of microphones in the system
    if (*mic_count == 0) {
        *mic_count = sizeof(mic_default) / sizeof(struct audio_microphone_characteristic_t);
        pthread_mutex_unlock(&adev->audio_device_lock);
        return 0;
    }

    memcpy((void*)mic_array, (void*)(&mic_default), sizeof(mic_default));
    *mic_count = sizeof(mic_default) / sizeof(struct audio_microphone_characteristic_t);

    pthread_mutex_unlock(&adev->audio_device_lock);
    AUDIO_FUNC_EXIT
    return 0;
}

static int in_get_active_microphones(const struct audio_stream_in* stream,
                                     struct audio_microphone_characteristic_t* mic_array,
                                     size_t* mic_count)
{
    AUDIO_FUNC_ENTER
    struct stream_in* in = (struct stream_in*)stream;

    //check parameters
    if ((mic_array == NULL) || (mic_count == NULL)) {
        return (-EINVAL);
    }

    lock_input_stream(in);
    //[From **/hardware/libhardware/include/hardware/audio.h]
    //if mic_count is passed as zero, mic_array will not be populated,
    //and mic_count will return the actual number of active microphones.
    if (*mic_count == 0) {
        *mic_count = sizeof(mic_default) / sizeof(struct audio_microphone_characteristic_t);
        pthread_mutex_unlock(&in->lock);
        return 0;
    }

    memcpy((void*)mic_array, (void*)(&mic_default), sizeof(mic_default));
    *mic_count = sizeof(mic_default) / sizeof(struct audio_microphone_characteristic_t);

    pthread_mutex_unlock(&in->lock);

    AUDIO_FUNC_EXIT
    return 0;
}
#endif /* AUDIO_HAL_4_0_ONLY */

#ifdef AAUDIO_SUPPORT
/* Converts pcm_format to audio_format.
 * Parameters:
 *  format  the pcm_format to convert
 *
 * Logs a fatal error if format is not a valid convertible pcm_format.
 */
static audio_format_t audio_format_from_pcm_format(enum pcm_format format)
{
    switch (format) {
    case PCM_FORMAT_S16_LE:
        return AUDIO_FORMAT_PCM_16_BIT;
    case PCM_FORMAT_S24_3LE:
        return AUDIO_FORMAT_PCM_24_BIT_PACKED;
    case PCM_FORMAT_S24_LE:
        return AUDIO_FORMAT_PCM_8_24_BIT;
    case PCM_FORMAT_S32_LE:
        return AUDIO_FORMAT_PCM_32_BIT;
    default:
        LOG_ALWAYS_FATAL("audio_format_from_pcm_format: invalid pcm format %#x", format);
        return (audio_format_t)0;
    }
}
#endif

static int adev_open_output_stream(struct audio_hw_device* dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config* config,
                                   struct audio_stream_out** streamOut,
                                   const char* address)
{
    resetAudioDebug();
    AUDIO_FUNC_PROILE
    ALOGI("%s handle:%x, flags=0x%x, format:0x%x, rate:%d channel:0x%x, devices:0x%x",
        __FUNCTION__, handle, flags, config->format, config->sample_rate, config->channel_mask, devices);
    struct audio_device* adev = (struct audio_device*)dev;
    struct stream_out* out;
    int ret = 0;
    std::optional<uint32_t> streamdelay;

    *streamOut = NULL;
    rtk::media::audio::AudioOutputStream *aos = nullptr;
    AudioOutputMixer::StreamType streamType = AudioOutputMixer::StreamType::ALSA;
    if (audio_is_linear_pcm(config->format)) {
        if (flags & AUDIO_OUTPUT_FLAG_MMAP_NOIRQ) {
          // mmap
        } else if (flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) {
          // tunneled pcm
          aos = new rtk::media::audio::TunnelOutputStream(
              devices, handle, config->format, config->channel_mask,
              config->sample_rate, rtk::media::audio::kCapsPcmDirect,
              rtk::media::audio::kConfigTunnelOutputStream, gClockManager);
          streamdelay = DELAY_TUNNEL_PCM;
          int delay = mAudioDelayManager->getAudioDelay(devices, DELAY_TUNNEL_PCM_SYSTIME_OFFSET,config->channel_mask);
          aos->setPresentationPositionOffset(delay);

          int enable = mAudioDelayManager->getAudioDelay(devices, DELAY_ENABLE_FIXED_DEFER_START,config->channel_mask);
          if (enable > 0)
            delay = mAudioDelayManager->getAudioDelay(devices, DELAY_TUNNEL_PCM_FIXED_DEFER_START,config->channel_mask);
          else
            delay = 0;
          int deferStartMs = mAudioDelayManager->getAudioDelay(devices, DELAY_TUNNEL_PCM_DYNAMIC_DEFER_START,config->channel_mask);
          aos->setExtraDelayMs(delay, deferStartMs);
        } else if (flags & AUDIO_OUTPUT_FLAG_DIRECT) {
          // direct pcm
          aos = new rtk::media::audio::DirectOutputStream(
              devices, handle, config->format, config->channel_mask,
              config->sample_rate, rtk::media::audio::kCapsPcmDirect,
              rtk::media::audio::kConfigDirectOutputStream);
          if(config->channel_mask == AUDIO_CHANNEL_OUT_5POINT1) {
            streamdelay = DELAY_NON_TUNNEL_PCM51CH;
            int delayMs = mAudioDelayManager->getAudioDelay(devices, DELAY_NON_TUNNEL_PCM51CH,config->channel_mask);
            if (delayMs < 0) {
              aos->setPresentationPositionOffset(delayMs);
            }
          } else {
            streamdelay = DELAY_NON_TUNNEL_PCM;
          }
        } else if (flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER) {
          // deep buffer
          aos = new rtk::media::audio::AlsaOutputStream(
              devices, handle, config->format, config->channel_mask,
              config->sample_rate, rtk::media::audio::kCapsPcm,
              rtk::media::audio::kConfigDeepBufferOutputStream);
          int delayMs = mAudioDelayManager->getAudioDelay(devices, DELAY_NON_TUNNEL_PCM_SYSTIME_OFFSET,config->channel_mask);
          if (delayMs != 0) {
            aos->setPresentationPositionOffset(delayMs);
          }
          streamdelay = DELAY_NON_TUNNEL_PCM;
        } else {
          // low latency
           aos = new rtk::media::audio::AlsaOutputStream(
              devices, handle, config->format, config->channel_mask,
              config->sample_rate, rtk::media::audio::kCapsPcm,
              rtk::media::audio::kConfigLowDelayOutputStream);
        }
      } else {
        streamType = AudioOutputMixer::StreamType::OFFLOADED;
        if (flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) {
            aos = new rtk::media::audio::TunnelOffloadStream(
                devices, handle, config->format, config->channel_mask,
                config->sample_rate, rtk::media::audio::kCapsOffload, gClockManager);
            streamdelay = DELAY_TUNNEL_OFFLOAD;
        } else {
            aos = new rtk::media::audio::OffloadOutputStream(
                devices, handle, config->format, config->channel_mask,
                config->sample_rate, rtk::media::audio::kCapsOffload);
            streamdelay = DELAY_NON_TUNNEL_OFFLOAD;
        }
      }

    if (aos) {
      if (property_get_bool("vendor.rtk.audio.dump", 0)) {
          if (flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC)
            aos = new rtk::media::audio::AudioOutputDumpWithAvSyncHeader(aos);
          else
            aos = new rtk::media::audio::AudioOutputDump(aos);
      }
      if (streamdelay.has_value()) {
        mAudioDelayManager->notifyStreamOpened(atoi(address), streamdelay.value(), config->channel_mask, aos);
      }
      *streamOut = rtk::media::audio::bindToLegacyHal(aos);
      aos->setOutputDevice(mAudioOutputDevice);
    //   AudioPatchManager::instance().notifyOutputStreamOpened(handle, aos);
#if _ENABLE_DOLBY_HAL
      rtkms12Aidk_listenOuputStream((void*)*streamOut);
#endif
      ALOGI(
          "%s aos=%p, handle:%x, flags=0x%x, format:0x%x, rate:%d "
          "channel:0x%x, devices:0x%x",
          __FUNCTION__, aos, handle, flags, config->format, config->sample_rate, config->channel_mask,
          devices);

      return 0;
    }

    out = (struct stream_out*)calloc(1, sizeof(struct stream_out));
    if (!out) {
        ALOGE("adev_open_output_stream err");
        return -ENOMEM;
    }
    //ALOGD("adev_open_output_stream out:%p", out);

    // initilize fd to -1 to avoid closing fd 0.
    out->rtkaudiofd = -1;
#ifdef AAUDIO_SUPPORT
    out->mmap_shared_memory_fd = -1; // not open
#endif
    out->pcm = NULL;
    out->source_metadata_usage = -1;

    pthread_mutex_init(&out->lock, (const pthread_mutexattr_t*) NULL);
    pthread_mutex_init(&out->pre_lock, (const pthread_mutexattr_t*) NULL);

#ifndef AUDIO_HAL_4_0
    out->sup_channel_masks[0] = AUDIO_CHANNEL_OUT_STEREO;
    out->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
#endif

    out->offload_first_write = false;
    out->offload_open = false;
    out->flags = flags;
    out->handle = -1;

    pthread_mutex_lock(&adev->audio_device_lock);

    // AAudio support
    if (flags & AUDIO_OUTPUT_FLAG_MMAP_NOIRQ) {
#ifdef AAUDIO_SUPPORT
        out->output_type = OUTPUT_AAUDIO;
        out->handle = handle;
        switch (adev->aaudio_count)
        {
            case 0:
                out->pcm_type = PCM_AAUDIO;
                break;
            case 1:
                out->pcm_type = PCM_AAUDIO_2;
                break;
            default:
                ALOGE("All AAudio Device Busy");
                ret = -EBUSY;
                goto err_open;
        }
        ALOGI("Use AAudio Device %d", adev->aaudio_count);
        adev->aaudio_count++;
        out->stream.common.get_buffer_size = out_get_buffer_size_aaudio;
        out->stream.common.get_sample_rate = out_get_sample_rate_aaudio;
        out->stream.get_latency = out_get_latency_aaudio;
        //out->stream.resume = ;
        //out->stream.pause = ;
        out->stream.write = out_write_aaudio;
        out->stream.start = out_start;
        out->stream.stop = out_stop;
        out->stream.create_mmap_buffer = out_create_mmap_buffer;
        out->stream.get_mmap_position = out_get_mmap_position;
        out->stream.update_source_metadata = out_update_source_metadata;
        out->stream.update_source_metadata_v7 = out_update_source_metadata_v7;

        uint32_t channel_number = audio_channel_count_from_out_mask(config->channel_mask);
        out->config = pcm_config_aaudio;
        if(config->sample_rate != 0) {
            out->config.rate = config->sample_rate;
        }
        if(config->channel_mask != 0) {
            out->config.channels = channel_number;
            out->channel_mask = config->channel_mask;
        }

#if 0   //for aaudio debug
        if (property_get("persist.vendor.rtk.audio.mmap.size", property, NULL)) {
            out->config.period_size = atoi(property);
            ALOGD("[AAUDIO] set mmap size from property %d", out->config.period_size);
        }
#endif

        out->format = audio_format_from_pcm_format(out->config.format);

        out->channel_mask = config->channel_mask;
        ALOGD("AAudio samplerate:%d, format:%d, channel:%d",
              out->config.rate,
              out->config.format,
              channel_number);
#else
        ret = -ENOSYS;
        goto err_open;
#endif
    }

    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.set_volume = out_set_volume;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_presentation_position = out_get_presentation_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;

    out->dev = adev;
    out->standby = 1;
    out->tunneled_bs_size = 0;
    out->tunneled_timestamp = 0;
    out->tunneled_firstts = false;
    out->tunneled_bufwithheader = true;
    out->PRE_STC = 0;
    out->PRE_PTS = 0;
    out->written = 0;
    out->segment_written = 0;
    out->stream_volume = 1.0f;

    out->PRE_audio_gap_STC = 0;
    out->audio_gap = false;

    //out->signed_frames = 0;

    config->format = out->stream.common.get_format(&out->stream.common);
    config->channel_mask = out->stream.common.get_channels(&out->stream.common);
    config->sample_rate = out->stream.common.get_sample_rate(&out->stream.common);

    *streamOut = &out->stream;

    pthread_mutex_unlock(&adev->audio_device_lock);
#if _ENABLE_DOLBY_HAL
    if (ret == 0){
        rtkms12Aidk_listenOuputStream((void*)out);
    }
#endif

    ALOGI("%s end, handle:%x, devices:0x%x flags:0x%x samplerate:%d, format:0x%x, output_type=%d, channel:0x%x",
        __FUNCTION__, handle, devices, flags, config->sample_rate, config->format, out->output_type,config->channel_mask);

    return ret;

err_open:
    free(out);
    pthread_mutex_unlock(&adev->audio_device_lock);
    return ret;
}


static void adev_close_output_stream(struct audio_hw_device* dev,
                                     struct audio_stream_out* stream)
{
    AUDIO_FUNC_ENTER
    struct audio_device* adev = (struct audio_device*)dev;
    struct stream_out* out = (struct stream_out*)stream;

#if _ENABLE_DOLBY_HAL
    rtkms12Aidk_removeOuputStream((void*)out);
#endif
    if (rtk::media::audio::isAudioOutputStream(stream)) {
      mAudioDelayManager->notifyStreamClose(stream);
      rtk::media::audio::closeAudioOutputStream(stream);
      return;
    }

    if (out != nullptr) {
        ALOGD("adev_close_output_stream out=%p output_type %d", out, out->output_type);
    } else {
        ALOGD("stream is invalid");
        return;
    }

    pthread_mutex_lock(&out->lock);
    do_output_standby(out, 1);
    pthread_mutex_unlock(&out->lock);

    switch(out->output_type)
    {
    case OUTPUT_AAUDIO:
        out->handle = -1;
        pthread_mutex_lock(&adev->audio_device_lock);
        adev->aaudio_count--;
        pthread_mutex_unlock(&adev->audio_device_lock);
        ALOGD("adev_close_output_stream OUTPUT_AAUDIO");
        break;

    default:
        break;
    }

#ifdef TUNNELED_PLAYBACK
    if(out->rtkaudiofd != -1) {
        ALOGD("close rtkaudiofd %d", out->rtkaudiofd);
        close (out->rtkaudiofd);
        out->rtkaudiofd = -1;
    }
#endif

    if (out->tmp_data){
        free(out->tmp_data);
        out->tmp_data = NULL;
    }
    if (out->buffer) {
        free(out->buffer);
    }
    if (out->resampler) {
        release_resampler(out->resampler);
    }

    if (out != nullptr) {
        free(out);
    }

    AUDIO_FUNC_EXIT
}

static int set_parameters_to_driver(struct audio_device* adev, int type, int setting_value)
{
    int ret = 0;
    int rtkaudiofd = -1;

    switch (type) {
        case PARAM_DMX_MODE:
            rtkaudiofd = open ("/dev/rtkaudio", O_RDWR);
            if (rtkaudiofd != -1) {
                if (ioctl(rtkaudiofd, RTKAUDIO_IOC_SET_DMX_MODE, &setting_value) < 0) {
                    ALOGE("[HAL] Get FW CAPABILITY failed ioctl failed\n");
                    ret = -1;
                }

                close(rtkaudiofd);
            }
            break;
        case PARAM_DUALMONO_SETTING:
            pthread_mutex_lock(&global_lock);
            if (adev->offload_handle) {
                rtk_compress_offload_t* offload_handle = (rtk_compress_offload_t*) adev->offload_handle;
                rtk_compress_set_parameter(offload_handle, PARAM_DUALMONO_SETTING, setting_value);
            }
            pthread_mutex_unlock(&global_lock);
            break;
        default:
            ALOGI("unknown or unsupported adev set parameter %d", type);
            break;
    }

    return ret;
}

static int get_parameters_from_driver(struct audio_device* adev, int type)
{
    int ret = 0;

    switch (type) {
        case PARAM_SAMPLE_RATE:
            pthread_mutex_lock(&global_lock);
            if (adev->offload_handle) {
                rtk_compress_offload_t* offload_handle = (rtk_compress_offload_t*) adev->offload_handle;
                ret = rtk_compress_get_sample_rate(offload_handle);
            }
            pthread_mutex_unlock(&global_lock);
            break;
        case PARAM_CHANNEL_CONFIG:
            pthread_mutex_lock(&global_lock);
            if (adev->offload_handle) {
                rtk_compress_offload_t* offload_handle = (rtk_compress_offload_t*) adev->offload_handle;
                ret = rtk_compress_get_channel_config(offload_handle);
            }
            pthread_mutex_unlock(&global_lock);
            break;
        case PARAM_IS_DUALMONO:
            pthread_mutex_lock(&global_lock);
            if (adev->offload_handle) {
                rtk_compress_offload_t* offload_handle = (rtk_compress_offload_t*) adev->offload_handle;
                ret = rtk_compress_get_is_dualmono(offload_handle);
            }
            pthread_mutex_unlock(&global_lock);
            break;
        default:
            ALOGI("unknown or unsupported adev get parameter %d", type);
            break;
    }

    return ret;
}

static void setKaraokeRefGain(int value) {
    int rtkaudiofd = open ("/dev/rtkaudio", O_RDWR);
    if(rtkaudiofd != -1) {
        ALOGD("setKaraokeRefGain: %i", value);
        char s1[64];
        memset(s1, 0, 64);
        sprintf(s1, "fw@ bt_mic_ref_adjust_gain %d", value);
        write(rtkaudiofd, s1, sizeof(s1));
        close(rtkaudiofd);
    }
}

static int adev_set_parameters(struct audio_hw_device* dev, const char* kvpairs)
{
    struct audio_device* adev = (struct audio_device*)dev;
    struct str_parms* parms;
    char value[32];
    int ret, ret_get;
    int setting_value;

    ALOGD("%s: input kvpairs: %s", __FUNCTION__, kvpairs);

#ifdef AUDIO_HAL_4_0_ONLY
    ret = -ENOSYS;
#else
    ret = -ENOENT;
#endif

    // VTS test: empty parameter expect return OK!
    if (strcmp(kvpairs, "") == 0) {
        ALOGW("get set parameters");
        return 0;
    }
    parms = str_parms_create_str(kvpairs);

    ret_get = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_BT_NREC, value, sizeof(value));
    if (ret_get >= 0) {
        ALOGD("adev_set_parameters, KEY_BT_NREC %s", value);
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0) {
            adev->bluetooth_nrec = true;
        } else {
            adev->bluetooth_nrec = false;
        }
        ret = 0;
    }

    ret_get = str_parms_get_str(parms, "screen_state", value, sizeof(value));
    if (ret_get >= 0) {
        ALOGD("adev_set_parameters, screen_state %s", value);
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0) {
            adev->screen_off = false;
        } else {
            adev->screen_off = true;
        }
        ret = 0;
    }

    // VTS test: playback will not get hw_av_sync addr from here, but need to return OK for testcase
    ret_get = str_parms_get_str(parms, "hw_av_sync", value, sizeof(value));
    if (ret_get >= 0) {
        ALOGD("adev_set_parameters, hw_av_sync %s", value);
        ret = 0;
    }

    int deviceType = 0;
    int hwStreamId = 0xffff;

    ret_get = str_parms_get_int(parms, "rtk_routing", &deviceType);
    if (ret_get >= 0 ) {
        ret_get = str_parms_get_int(parms, "mixer_index", &hwStreamId);
        if (ret_get >= 0 ) {
            std::shared_ptr<IAudioOutputMixer> mixer = AudioPatchManager::instance().getAudioOutputMixer();
            if (mixer) {
                if (mixer->addMixerInputStreams(static_cast<audio_devices_t>(deviceType), hwStreamId) == 0) {
                    ALOGD("adev_set_parameters, deviceType:0x%x, mixer_index:0x%x Success !!!", deviceType, hwStreamId);
                    ret = 0;
                }
            }
        }
    }

    // add uiaudioout for android_media_AudioSystem_setParameters -38 error
    ret_get = str_parms_get_str(parms, "uiaudioout", value, sizeof(value));
    if (ret_get >= 0) {
        // Todo: here should parse the audio out device
        ret = 0;
    }

    // add restarting for android_media_AudioSystem_setParameters -38 error
    ret_get = str_parms_get_str(parms, "restarting", value, sizeof(value));
    if (ret_get >= 0) {
        // Todo:
        ret = 0;
    }

    // add rotation for android_media_AudioSystem_setParameters -38 error
    ret_get = str_parms_get_str(parms, "rotation", value, sizeof(value));
    if (ret_get >= 0) {
        // Todo:
        ret = 0;
    }

    // add device_folded for android_media_AudioSystem_setParameters -38 error
    ret_get = str_parms_get_str(parms, "device_folded", value, sizeof(value));
    if (ret_get >= 0) {
        // Todo:
        ret = 0;
    }

    int device = 0;
    ret_get = str_parms_get_int(parms, "connect", &device);
    if ((ret_get >= 0 ) && ((device & AUDIO_DEVICE_BIT_IN) == 0)) {
        pthread_mutex_lock(&adev->audio_device_lock);
        adev->out_device |= (audio_devices_t) device;
        ALOGI("current out device=0x%x", adev->out_device);

        AudioPatchManager::instance().connectDevice(static_cast<audio_devices_t>(device));

        if(device & AUDIO_DEVICE_OUT_ALL_A2DP) {
            ALOGD("a2dp connect, device 0x%x", device);
            int target_ret;
            audio_io_handle_t handle = AUDIO_IO_HANDLE_NONE;
            struct audio_config config = AUDIO_CONFIG_INITIALIZER;
            config.sample_rate = DEFAULT_OUT_SAMPLING_RATE;
            config.format = AUDIO_FORMAT_PCM_16_BIT;
            config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
            audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE;
            const char* address = NULL;
            ALOGD("a2dp_open_output_stream: samplerate:%d,format:%d, channel:%d", config.sample_rate, config.format, config.channel_mask);
            target_ret = adev->a2dp_device->open_output_stream(adev->a2dp_device, handle, (audio_devices_t)(device), flags,
                    &config, &adev->a2dp_stream_out, address);
            if(target_ret != 0) {
                ALOGE("a2dp_open_output_stream target_ret:%d", target_ret);
            } else {
                ALOGD("a2dp_open_output_stream adev->a2dp_stream_out=%p", adev->a2dp_stream_out);
                mAudioOutputDevice->addExternalOutput((audio_devices_t)device, adev->a2dp_stream_out);
                if(adev->usb_stream_out) {
                    ALOGD("stop usb captrue thread");
                    stopAudioCaptureThread(adev->usb_stream_out);
                }
                startAudioCaptureThread(device, adev->a2dp_stream_out);
            }
            //mAudioOutputMixer->createMixer(AudioOutputMixer::DeviceType::BLUETOOTH);
        }else if(device & AUDIO_DEVICE_OUT_ALL_USB) {
            ALOGD("usb connect, device 0x%x", device);
            int card = -1;
            ret_get = str_parms_get_int(parms, "card", &card);
            if(ret_get < 0 ) {
                ALOGE("usb address card invalid");
            }
            int tmpDevice = -1;
            ret_get = str_parms_get_int(parms, "device", &tmpDevice);
            if(ret_get < 0 ) {
                ALOGE("usb address device invalid");
            }
            char address[20];
            snprintf(address, 16, "%s%d%s%d", "card=", card, ";device=", tmpDevice);
            ALOGD("usb address: %s", address);

            int target_ret;
            audio_io_handle_t handle = AUDIO_IO_HANDLE_NONE;
            struct audio_config config = AUDIO_CONFIG_INITIALIZER;
            config.sample_rate = DEFAULT_OUT_SAMPLING_RATE;
            config.format = AUDIO_FORMAT_PCM_16_BIT;
            config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
            audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE;
            ALOGD("usb_open_output_stream target_device: samplerate:%d,format:%d, channel:%d", config.sample_rate, config.format, config.channel_mask);
            target_ret = adev->usb_device->open_output_stream(adev->usb_device, handle, (audio_devices_t)(device), flags,
                    &config, &adev->usb_stream_out, address);
            if(target_ret != 0) {
                ALOGE("a2dp_open_output_stream target_ret:%d", target_ret);
            } else {
                mAudioOutputDevice->addExternalOutput((audio_devices_t)device, adev->usb_stream_out);
                if(adev->a2dp_stream_out) {
                    ALOGD("stop a2dp captrue thread");
                    stopAudioCaptureThread(adev->a2dp_stream_out);
                }
                startAudioCaptureThread(device, adev->usb_stream_out);
            }
            // mAudioOutputMixer->createMixer(AudioOutputMixer::DeviceType::USB);
        } else if ( device & AUDIO_DEVICE_OUT_HDMI_ARC) {
            // mAudioOutputMixer->createMixer(AudioOutputMixer::DeviceType::HDMI_ARC);
        }

        ALOGI("%s: connect device: 0x%x", __FUNCTION__, device);
        mAudioOutputDevice->setConnectedDevice((audio_devices_t)device, adev->delayUnmute);
        mAudioDelayManager->notifyDeviceConnected(device);
        pthread_mutex_unlock(&adev->audio_device_lock);
        ret = 0;
    } else if (device & AUDIO_DEVICE_BIT_IN) {
      ret = 0;
    }

    ret_get = str_parms_get_int(parms, "disconnect", &device);
    if ((ret_get >= 0 ) && ((device & AUDIO_DEVICE_BIT_IN) == 0)) {
        pthread_mutex_lock(&adev->audio_device_lock);
        adev->out_device &= (audio_devices_t) (~device);
        ALOGI("current out device=0x%x", adev->out_device);

        AudioPatchManager::instance().disconnectDevice(static_cast<audio_devices_t>(device));

        if (device & AUDIO_DEVICE_OUT_ALL_A2DP)
        {
            ALOGD("a2dp disconnect, device 0x%x", device);
            if(adev->a2dp_stream_out) {
                stopAudioCaptureThread(adev->a2dp_stream_out);
                mAudioOutputDevice->removeExternalOutput((audio_devices_t)device);
                ALOGD("a2dp_close_output_stream dev=%p, out=%p",adev->a2dp_device, adev->a2dp_stream_out);
                adev->a2dp_device->close_output_stream(adev->a2dp_device, adev->a2dp_stream_out);
                if(adev->a2dp_stream_out) {
                    adev->a2dp_stream_out = NULL;
                }
            } else {
                ALOGE("a2dp close fail for null");
            }
            if(adev->usb_stream_out) {
                ALOGD("start usb captrue thread");
                startAudioCaptureThread(AUDIO_DEVICE_OUT_ALL_USB, adev->usb_stream_out);
            }
            // mAudioOutputMixer->releaseMixer(AudioOutputMixer::DeviceType::BLUETOOTH);
        }
        else if (device & AUDIO_DEVICE_OUT_ALL_USB)
        {
            ALOGD("usb disconnect, device 0x%x", device);
            if(adev->usb_stream_out) {
                stopAudioCaptureThread(adev->usb_stream_out);
                mAudioOutputDevice->removeExternalOutput((audio_devices_t)device);
                ALOGD("usb_close_output_stream dev=%p, out=%p",adev->usb_device, adev->usb_stream_out);
                adev->usb_device->close_output_stream(adev->usb_device, adev->usb_stream_out);
                if(adev->usb_stream_out) {
                    adev->usb_stream_out = NULL;
                }
            } else {
                ALOGE("usb close fail for null");
            }

            if(adev->a2dp_stream_out) {
                ALOGD("start a2dp captrue thread");
                startAudioCaptureThread(AUDIO_DEVICE_OUT_ALL_A2DP, adev->a2dp_stream_out);
            }
            // mAudioOutputMixer->releaseMixer(AudioOutputMixer::DeviceType::USB);
        } else if ( device & AUDIO_DEVICE_OUT_HDMI_ARC) {
            // mAudioOutputMixer->releaseMixer(AudioOutputMixer::DeviceType::HDMI_ARC);
        }

        mAudioDelayManager->notifyDeviceDisconnected(device);

        ALOGI("%s: disconnect device: 0x%x", __FUNCTION__, device);
        mAudioOutputDevice->setDisconnectedDevice((audio_devices_t)device, adev->delayUnmute);
        pthread_mutex_unlock(&adev->audio_device_lock);
        ret = 0;
    } else if (device & AUDIO_DEVICE_BIT_IN) {
      ret = 0;
    }

#ifdef AUDIO_HAL_4_0_ONLY
    //BT HP volume : return OK for pass VTS
    //[TBD] HP volume can be set independently?
    ret_get = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_HFP_VOLUME, value, sizeof(value));
    if (ret_get >= 0) {
        ret = 0;
    }
#endif

    ret_get = str_parms_get_int(parms, AUDIO_PARAMETER_STREAM_DOLBY_ATMOS_LOCK, &setting_value);
    if (ret_get >= 0) {
        int rtkaudiofd = -1;
        adev->parameter_atmos_lock = setting_value;
        ALOGI("adev_set_parameters, dolby atmos lock: %x", setting_value);

        rtkaudiofd = open ("/dev/rtkaudio", O_RDWR);
        if(rtkaudiofd != -1) {
            char s1[64];
            sprintf(s1, "fw@ ott_enable %d", setting_value);
            write(rtkaudiofd, s1, sizeof(s1));
            memset(s1, 0, 64);

            sprintf(s1, "fw@ atmos_lock %d", setting_value);
            write(rtkaudiofd, s1, sizeof(s1));
            close(rtkaudiofd);
        }

        ret = 0;
    }

    /* set to offload track */
    ret_get = str_parms_get_int(parms, AUDIO_PARAMETER_DUALMONO_SETTING, &setting_value);
    if (ret_get >= 0) {
        if (adev->dualmono_mode != setting_value) {
            if ((setting_value < 0) || setting_value > 2) {
                ALOGW("[%s] Warning, unknown dualmono_setting mode %d, keep original setting %d" , __FUNCTION__, setting_value, adev->dualmono_mode);
            } else {
                ALOGI("%s, dualmono_setting: %d > %d, handle %p" , __FUNCTION__, adev->dualmono_mode, setting_value, adev->offload_handle);
                set_parameters_to_driver(adev, PARAM_DUALMONO_SETTING, setting_value);
                adev->dualmono_mode = setting_value;
            }
        }

        ret = 0;
    }
    /*************************************************************************/

    /* set to audio driver */
    ret_get = str_parms_get_int(parms, AUDIO_PARAMETER_DMX_MODE, &setting_value);
    if (ret_get >= 0) {
        if (adev->dmx_mode != setting_value) {
            if ((setting_value < 0) || setting_value > 2) {
                ALOGW("[%s] Warning, unknown dmx_mode %d, keep original setting %d" , __FUNCTION__, setting_value, adev->dmx_mode);
            } else {
                ALOGI("%s, dmx_setting: %d > %d" , __FUNCTION__, adev->dmx_mode, setting_value);
                set_parameters_to_driver(adev, PARAM_DMX_MODE, setting_value);
                adev->dmx_mode = setting_value;
            }
        }

        ret = 0;
    }
    /*************************************************************************/
    /* set Spec vocal cancellation */
    ret_get = str_parms_get_int(parms, AUDIO_PARAMETER_SPEC_VOCAL_CANCELLATION, &setting_value);
    if (ret_get >= 0) {
        adev->mSpecVocalCancellation = setting_value;
        if(mAudioDelayManager == nullptr) {
            ALOGW("AudioDelayManager is null");
        } else {
            if(setting_value == 1) {
                mAudioDelayManager->openKaraoke(true);
            } else {
                mAudioDelayManager->openKaraoke(false);
            }
        }
        ALOGD("[%s] mSpecVocalCancellation: %i" , __FUNCTION__, adev->mSpecVocalCancellation);

        if (adev->mSpecVocalCancellation == 1) { // Karaoke on
            if(adev->pKaraokeConfig) {
                int refGain = adev->pKaraokeConfig->GetKaraokeValue(BLE_MIC_REFERENCE_GAIN);
                ALOGD("refGain: %d", refGain);
                setKaraokeRefGain(refGain);
            }
        }

        ret = 0;
    }

    /* set Spec karaoke state */
    ret_get = str_parms_get_int(parms, AUDIO_PARAMETER_SPEC_KARAOKE_STATE, &setting_value);
    if (ret_get >= 0) {
        if(mAudioDelayManager == nullptr) {
            ALOGW("AudioDelayManager is null");
        } else {
            ALOGD("[%s] mSpecKaraokeState: %i" , __FUNCTION__, setting_value);
            if(setting_value == 1) {
                mAudioDelayManager->openKaraoke(true);
            } else {
                mAudioDelayManager->openKaraoke(false);
            }
        }
        ret = 0;
    }

    ret_get = str_parms_get_int(parms, AUDIO_PARAMETER_USB_KARAOKE_MODE, &setting_value);
    if (ret_get >= 0) {
        if(adev->usb_device) {
            ALOGD("set_parameters: usb_karaoke_mode: %i", setting_value);
            ret = adev->usb_device->set_parameters(adev->usb_device, kvpairs);
        }
    }

    ret_get = str_parms_get_int(parms, AUDIO_PARAMETER_USB_KARAOKE_VOLUME, &setting_value);
    if (ret_get >= 0) {
        if(adev->usb_device) {
            ALOGD("set_parameters: usb_karaoke_volume: %i", setting_value);
            ret = adev->usb_device->set_parameters(adev->usb_device, kvpairs);
        }
    }

    ret_get = str_parms_get_int(parms, "mute_state", &setting_value);
    if (ret_get >= 0) {
        ALOGD("set_parameters: mute_state = %d", setting_value);
        mAudioOutputDevice->setMuteState(setting_value);
        ret = 0;
    }
    str_parms_destroy(parms);
    return ret;
}

static char* adev_get_parameters(const struct audio_hw_device* dev,
                                 const char* keys)
{
    struct audio_device* adev = (struct audio_device*)dev;
    struct str_parms* parms;
    struct str_parms* out_parms;
    char* str;
    char value[32];
    int ret;
    int found_key = false;

    AUDIO_LOG("adev_get_parameters, input keys: %s", keys);
    parms = str_parms_create_str(keys);
    out_parms = str_parms_create();

    pthread_mutex_lock(&adev->audio_device_lock);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_BT_NREC, value, sizeof(value));
    if (ret >= 0) {
        found_key = true;
        str_parms_add_str(out_parms, AUDIO_PARAMETER_KEY_BT_NREC,
                          adev->bluetooth_nrec ? "true" : "false");
        str = str_parms_to_str(out_parms);
    }

#ifdef TUNNELED_PLAYBACK
    ret = str_parms_get_str(parms, "hw_av_sync", value, sizeof(value));
    if(ret >= 0) {
        unsigned long clock;
        if(gClockManager->getFreeClock(clock) >= 0) {
            char a[20];
            memset(a, 0, sizeof(a));
            snprintf(a, sizeof(a), "%lu", clock);
            found_key = true;
            str_parms_add_str(out_parms, "hw_av_sync", a);
            str = str_parms_to_str(out_parms);
            gClockManager->resetClock(clock);
            AUDIO_LOG("AUDIO HAL adev_get_parameters hw_av_sync = %lx", clock);
        }
    }
#endif

    ret = str_parms_get_str(parms, "HwAvSyncEAC3Supported", value, sizeof(value));
    if (ret >= 0) {
        found_key = true;
        str_parms_add_int(out_parms, "HwAvSyncEAC3Supported", true);
        str = str_parms_to_str(out_parms);
        AUDIO_LOG("Get HwAvSyncEAC3Supported\n");
    }

    ret = str_parms_get_str(parms, "isAc4PresentationSelectionByIndexSupported", value, sizeof(value));
    if (ret >= 0) {
        found_key = true;
        str_parms_add_int(out_parms, "isAc4PresentationSelectionByIndexSupported", true);
        str = str_parms_to_str(out_parms);
        AUDIO_LOG("Get isAc4PresentationSelectionByIndexSupported\n");
    }

    //used in audiopolicymanager.cpp:handleDeviceConfigChange, don't rechange a2dp satae 0->1
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_A2DP_RECONFIG_SUPPORTED, value, sizeof(value));
    if(ret >= 0) {
        found_key = true;
        str_parms_add_str(out_parms, AUDIO_PARAMETER_A2DP_RECONFIG_SUPPORTED, "1");
        str = str_parms_to_str(out_parms);
        ALOGD("Get AUDIO_PARAMETER_A2DP_RECONFIG_SUPPORTED");
    }

    /*********************** query from audio driver *************************/
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_IS_AVAILABLE_MS12, value, sizeof(value));
    if (ret >= 0) {
        found_key = true;

        str_parms_add_int(out_parms, AUDIO_PARAMETER_IS_AVAILABLE_MS12, adev->is_m12_platform);
        str = str_parms_to_str(out_parms);
        AUDIO_LOG("%s, %s\n", __FUNCTION__, str);
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_SUPPORTED_CHANNEL_COUNT, value, sizeof(value));
    if (ret >= 0) {
        found_key = true;

        str_parms_add_int(out_parms, AUDIO_PARAMETER_SUPPORTED_CHANNEL_COUNT, adev->max_aac_channel);
        str = str_parms_to_str(out_parms);
        AUDIO_LOG("%s, %s\n", __FUNCTION__, str);
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_DMX_MODE, value, sizeof(value));
    if (ret >= 0) {
        found_key = true;

        str_parms_add_int(out_parms, AUDIO_PARAMETER_DMX_MODE, adev->dmx_mode);
        str = str_parms_to_str(out_parms);
        AUDIO_LOG("%s, %s\n", __FUNCTION__, str);
    }
    /*************************************************************************/

    /********************** query from offload track *************************/
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_SAMPLE_RATE, value, sizeof(value));
    if (ret >= 0) {
        int sample_rate = 0;
        found_key = true;
        sample_rate = get_parameters_from_driver(adev, PARAM_SAMPLE_RATE);
        AUDIO_LOG("[%s] Get_Sample_Rate %d, handle %p", __FUNCTION__, sample_rate, adev->offload_handle);

        str_parms_add_int(out_parms, AUDIO_PARAMETER_SAMPLE_RATE, sample_rate);
        str = str_parms_to_str(out_parms);
        AUDIO_LOG("%s, %s\n", __FUNCTION__, str);
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_CHANNEL_CONFIG, value, sizeof(value));
    if (ret >= 0) {
        int ch = 0;
        found_key = true;
        ch = get_parameters_from_driver(adev, PARAM_CHANNEL_CONFIG);
        AUDIO_LOG("[%s] Get_Channel_Config %d, handle %p", __FUNCTION__, ch, adev->offload_handle);

        str_parms_add_int(out_parms, AUDIO_PARAMETER_CHANNEL_CONFIG, ch);
        str = str_parms_to_str(out_parms);
        AUDIO_LOG("%s, %s\n", __FUNCTION__, str);
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_IS_DUALMONO, value, sizeof(value));
    if (ret >= 0) {
        int is_dualmono = 0;
        found_key = true;
        is_dualmono = get_parameters_from_driver(adev, PARAM_IS_DUALMONO);
        AUDIO_LOG("[%s] Get_Is_Dualmono %d, handle %p", __FUNCTION__, is_dualmono, adev->offload_handle);

        str_parms_add_int(out_parms, AUDIO_PARAMETER_IS_DUALMONO, is_dualmono);
        str = str_parms_to_str(out_parms);
        AUDIO_LOG("%s, %s\n", __FUNCTION__, str);
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_DUALMONO_SETTING, value, sizeof(value));
    if (ret >= 0) {
        found_key = true;

        str_parms_add_int(out_parms, AUDIO_PARAMETER_DUALMONO_SETTING, adev->dualmono_mode);
        str = str_parms_to_str(out_parms);
        AUDIO_LOG("%s, %s\n", __FUNCTION__, str);
    }
    /*************************************************************************/

    str_parms_destroy(out_parms);
    str_parms_destroy(parms);
    pthread_mutex_unlock(&adev->audio_device_lock);

    ALOGD("adev_get_parameters, keys: %s, ret: %s", keys,
          (found_key ? str : ""));
    if (found_key) {
        return str;
    } else {
        return strdup("");;
    }
}

static int adev_init_check(const struct audio_hw_device* dev __unused)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device* dev __unused, float volume __unused)
{
    return 0;
}

static int adev_set_master_volume(struct audio_hw_device* dev __unused, float volume __unused)
{
    return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device* dev __unused, float* volume __unused)
{
    return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device* dev __unused, bool muted __unused)
{
    return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device* dev __unused, bool* muted __unused)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device* dev __unused, audio_mode_t mode __unused)
{
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device* dev, bool state)
{
  AUDIO_FUNC_ENTER
  struct audio_device *adev = (struct audio_device *)dev;
  auto wrapDev = rtk::media::audio::DeviceManager::get(adev->in_device);
  if (!wrapDev) {
    ALOGW("%s: no device=0x%x", __FUNCTION__, adev->in_device);
    return 0;
  }
  int ret = wrapDev->setMicMute(state);
  if (ret != -ENOTSUP)
    return ret;

  adev->mic_mute = state;
  return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device* dev, bool* state)
{
  struct audio_device *adev = (struct audio_device *)dev;
  auto wrapDev = rtk::media::audio::DeviceManager::get(adev->in_device);
  if (!wrapDev) {
    ALOGW("%s: no device=0x%x", __FUNCTION__, adev->in_device);
    return 0;
  }
  int ret = wrapDev->getMicMute(state);
  if (ret != -ENOTSUP)
    return ret;

  *state = adev->mic_mute;
  return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device* dev,
        const struct audio_config* config)
{
    struct audio_device* adev = (struct audio_device*)dev;
    int channel_count = popcount(config->channel_mask);
    if (check_input_parameters(config->sample_rate, config->format, channel_count) != 0) {
        return 0;
    }

    return get_input_buffer_size(config->sample_rate, config->format, adev->in_device, channel_count);
}

static int openInputStreamLegacy(
    struct audio_hw_device *dev, audio_io_handle_t handle __unused,
    audio_devices_t devices, struct audio_config *config,
    struct audio_stream_in **streamIn, audio_input_flags_t flags,
    const char *address __unused, audio_source_t source __unused) {
  struct audio_device* adev = (struct audio_device*)dev;
  int ret = 0;

  if (devices == AUDIO_DEVICE_IN_USB_DEVICE || devices == AUDIO_DEVICE_IN_USB_HEADSET) {
    ALOGI("usb_open_input_stream: device 0x%x, sampling rate %d, format %#x, channel mask %#x, "
             "flags %#x", devices, config->sample_rate, config->format, config->channel_mask, flags);
    adev->usb_device->open_input_stream(adev->usb_device, handle, devices,
            config, &adev->usb_stream_in, flags, address, source);
    *streamIn = adev->usb_stream_in;
    ALOGI("[%s] stream_in= %p", __FUNCTION__, *streamIn);
    return ret;
  }

  struct stream_in* in;
  int channel_count = popcount(config->channel_mask);


  ALOGD("%s: flags=0x%x, device=0x%x, format:%d, sample_rate:%d channel_mask:0x%x",
        __FUNCTION__, flags, devices, config->format, config->sample_rate, config->channel_mask);
  if (check_input_parameters(config->sample_rate, config->format, channel_count) != 0) {
    ALOGD("%s: invalid config", __FUNCTION__);
  }

  in = (struct stream_in*)calloc(1, sizeof(struct stream_in));
  if (!in) {
    return -ENOMEM;
  }

  pthread_mutex_init(&in->lock, (const pthread_mutexattr_t*) NULL);
  pthread_mutex_init(&in->pre_lock, (const pthread_mutexattr_t*) NULL);

  in->dev = adev;
  if (!(flags & AUDIO_INPUT_FLAG_MMAP_NOIRQ)) {
    //for MIC select: ble mic > dmic (build in mic)
    ALOGD("%s: ALSA card id:%d", __FUNCTION__, adev->alsa_card);
    selectMic(in, devices, (const char*)__FUNCTION__);
    ALOGD("%s: mic_property: %s\n", __FUNCTION__, mic_property);
  }

  in->stream.common.get_device = in_get_device;
  in->stream.common.get_sample_rate = in_get_sample_rate;
  in->stream.common.set_sample_rate = in_set_sample_rate;
  in->stream.common.get_buffer_size = in_get_buffer_size;
  in->stream.common.get_channels = in_get_channels;
  in->stream.common.get_format = in_get_format;
  in->stream.common.set_format = in_set_format;
  in->stream.common.standby = in_standby;
  in->stream.common.dump = in_dump;
  in->stream.common.set_parameters = in_set_parameters;
  in->stream.common.get_parameters = in_get_parameters;
  in->stream.common.add_audio_effect = in_add_audio_effect;
  in->stream.common.remove_audio_effect = in_remove_audio_effect;
  in->stream.set_gain = in_set_gain;
  in->stream.read = in_read;
  in->stream.get_input_frames_lost = in_get_input_frames_lost;
  in->stream.get_capture_position = in_get_capture_position;
#ifdef AUDIO_HAL_4_0_ONLY
  in->stream.get_active_microphones = in_get_active_microphones;
#endif

  if (flags & AUDIO_INPUT_FLAG_MMAP_NOIRQ) {
    //AAudio support
    in->stream.start = in_start;
    in->stream.stop = in_stop;
    in->stream.create_mmap_buffer = in_create_mmap_buffer;
    in->stream.get_mmap_position = in_get_mmap_position;
    in->config = pcm_config_aaudio_capture;
    uint32_t channel_number = audio_channel_count_from_out_mask(config->channel_mask);
    if (config->sample_rate != 0) {
      in->config.rate = config->sample_rate;
    }
    if (config->channel_mask != 0) {
      in->config.channels = channel_number;
    }

    ALOGD("AAudio samplerate:%d, format:%d, channel:%d", in->config.rate, in->config.format, channel_number);
  }

  in->requested_rate = config->sample_rate;
  in->main_channels = config->channel_mask;

  if (!(flags & AUDIO_INPUT_FLAG_MMAP_NOIRQ)) {
    if (!strcmp(mic_property, AICAP_CONFIG)) {
      ALOGD("AI capture\n");
      memcpy(&in->config, &pcm_config_ai_capture, sizeof(pcm_config_ai_capture));     //for AI capture
      if(config->format == AUDIO_FORMAT_PCM_32_BIT){
        in->config.format = PCM_FORMAT_S32_LE;
      }
    } else if (!strcmp(mic_property, INMIC_CONFIG)) {
      ALOGD("INMIC\n");
      memcpy(&in->config, &pcm_config_mic_capture, sizeof(pcm_config_mic_capture));     //for INMIC
    } else if (!strcmp(mic_property, DMIC_CONFIG)) {
      ALOGD("DMIC\n");
      memcpy(&in->config, &pcm_config_dmic, sizeof(pcm_config_dmic));     //for DMIC
    } else {
      ALOGD("AO capture\n");
      memcpy(&in->config, &pcm_config_ao_capture, sizeof(pcm_config_ao_capture));     //for AO capture
      //refuse open two AO capture at the same time
      if(adev->AO_open_count == 0) {
        adev->AO_open_count++;
        in->AO_CAP_enable = 1;
        ALOGE("AO open:%d\n", adev->AO_open_count);
      } else {
        ALOGE("AO capture already open\n");
        ret = -EINVAL;
        goto err;
      }
    }

    /* initialisation of preprocessor structure array is implicit with the calloc.
     * same for in->aux_channels and in->aux_channels_changed */

    ALOGD("in->requested_rate = %d\n", in->requested_rate);
    ALOGD("in->config.rate = %d\n", in->config.rate);
    ALOGD("in->config.format = %d\n", in->config.format);
  }

  adev->in_device = devices;
  in->dev = adev;
  in->standby = 1;
  in->flags = flags;
  in->device = devices & ~AUDIO_DEVICE_BIT_IN;

  *streamIn = &in->stream;
  return ret;

err:
  if (in->resampler) {
    release_resampler(in->resampler);
  }

  free(in);
  return ret;
}

static int adev_open_input_stream(struct audio_hw_device* dev,
                                  audio_io_handle_t handle __unused,
                                  audio_devices_t devices,
                                  struct audio_config* config,
                                  struct audio_stream_in** streamIn,
                                  audio_input_flags_t flags,
                                  const char* address __unused,
                                  audio_source_t source __unused)
{
  resetAudioDebug();
  AUDIO_FUNC_ENTER
  ALOGD("%s start", __FUNCTION__);
  auto wrapDev = rtk::media::audio::DeviceManager::get(devices);
  if (!wrapDev) {
    ALOGI("%s: ignore devices=0x%x", __FUNCTION__, devices);
    return -EINVAL;
  }
  int ret = wrapDev->openInputStream(dev, handle, devices, config, streamIn,
                                     flags, address, source);
  if (ret != -ENOTSUP) {
    ALOGD("%s end", __FUNCTION__);
    return ret;
  }

  //legacy flow
  return openInputStreamLegacy(dev, handle, devices, config, streamIn, flags,
                               address, source);
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                    struct audio_stream_in *stream) {
  AUDIO_FUNC_ENTER
  ALOGD("%s start", __FUNCTION__);

  auto wrapDev = rtk::media::audio::DeviceManager::get((audio_stream*)stream);
  if (!wrapDev) {
    ALOGW("%s: missing stream=%p", __FUNCTION__, stream);
    return;
  }
  int ret = wrapDev->closeInputStream(stream);
  if (ret != -ENOTSUP) {
    ALOGD("%s end", __FUNCTION__);
    return;
  }

  struct audio_device *adev = (struct audio_device *)dev;

  if (adev->usb_device && adev->usb_stream_in) {
    if (stream == adev->usb_stream_in) {
      ALOGD("usb_close_input_stream, stream=%p, adev->usb_stream_in:%p", stream,
            adev->usb_stream_in);
      adev->usb_device->close_input_stream(adev->usb_device, stream);
      adev->usb_stream_in = NULL;
      return;
    }
  }

  struct stream_in *in = (struct stream_in *)stream;
  in_standby(&stream->common);

  free(in->read_buf);
  free(in->temp_buf);
  if (in->resampler) {
    release_resampler(in->resampler);
  }
  if (in->proc_buf_in) {
    free(in->proc_buf_in);
  }
  if (in->proc_buf_out) {
    free(in->proc_buf_out);
  }
  if (in->ref_buf) {
    free(in->ref_buf);
  }

  if (in->AO_CAP_enable == 1) {
    in->dev->AO_open_count--;
    in->AO_CAP_enable = 0;
    ALOGE("AO close:%d\n", in->dev->AO_open_count);
  }

#ifdef DUMP_TO_FILE
    if (fp_indump) {
        fclose(fp_indump);
        ALOGD("close file\n");
    }
#endif

    free(stream);
    return;
}

int adev_get_audio_port(struct audio_hw_device* dev __unused,
                        struct audio_port* port __unused)
{
    ALOGI("%s", __FUNCTION__);
    return -EINVAL;
}

int adev_get_audio_port_v7(struct audio_hw_device* dev __unused,
                           struct audio_port_v7* port)
{
    ALOGI("%s, port id : %u", __FUNCTION__, port->id);
    if (mAudioPortManager == nullptr) {
        ALOGW("AudioPortManager was not constructed");
    } else if (mAudioPortManager->getPort(port) != 0) {
        ALOGW("Could not find the port(%u)", port->id);
        return -EINVAL;
    }
    return 0;
}

int adev_set_device_connected_state_v7(struct audio_hw_device* dev,
                                       struct audio_port_v7* port,
                                       bool connected)
{
    ALOGI("%s", __FUNCTION__);
    struct audio_device* adev = (struct audio_device*)dev;
    int ret = -1;
    int device = port->ext.device.type;
    ALOGD("Port id : %d, device : %s", port->id, audio_device_to_string(port->ext.device.type));
    if (connected) {
        if (mAudioPortManager != nullptr) {
            mAudioPortManager->addPort(port);
            if (mAudioPortManager->getPort(port) != 0) {
                ALOGW("Could not find the port(%u)", port->id);
            }
        }
        if ((device & AUDIO_DEVICE_BIT_IN) == 0) {
            pthread_mutex_lock(&adev->audio_device_lock);
            adev->out_device |= (audio_devices_t) device;
            ALOGI("current out device=0x%x", adev->out_device);

            AudioPatchManager::instance().connectDevice(static_cast<audio_devices_t>(device));

            if(device & AUDIO_DEVICE_OUT_ALL_A2DP) {
                ALOGD("a2dp connect, device 0x%x", device);
                int target_ret;
                audio_io_handle_t handle = AUDIO_IO_HANDLE_NONE;
                struct audio_config config = AUDIO_CONFIG_INITIALIZER;
                config.sample_rate = DEFAULT_OUT_SAMPLING_RATE;
                config.format = AUDIO_FORMAT_PCM_16_BIT;
                config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
                audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE;
                const char* address = NULL;
                ALOGD("a2dp_open_output_stream: samplerate:%d,format:%d, channel:%d", config.sample_rate, config.format, config.channel_mask);
                target_ret = adev->a2dp_device->open_output_stream(adev->a2dp_device, handle, (audio_devices_t)(device), flags,
                        &config, &adev->a2dp_stream_out, address);
                if(target_ret != 0) {
                    ALOGE("a2dp_open_output_stream target_ret:%d", target_ret);
                } else {
                    ALOGD("a2dp_open_output_stream adev->a2dp_stream_out=%p", adev->a2dp_stream_out);
                    mAudioOutputDevice->addExternalOutput((audio_devices_t)device, adev->a2dp_stream_out);
                    if(adev->usb_stream_out) {
                        ALOGD("stop usb captrue thread");
                        stopAudioCaptureThread(adev->usb_stream_out);
                    }
                    startAudioCaptureThread(device, adev->a2dp_stream_out);
                }
                // mAudioOutputMixer->createMixer(AudioOutputMixer::DeviceType::BLUETOOTH);
            }else if(device & AUDIO_DEVICE_OUT_ALL_USB) {
                ALOGD("usb connect, device 0x%x", device);
                ALOGD("usb address: %s", port->ext.device.address);

                int target_ret;
                audio_io_handle_t handle = AUDIO_IO_HANDLE_NONE;
                struct audio_config config = AUDIO_CONFIG_INITIALIZER;
                config.sample_rate = DEFAULT_OUT_SAMPLING_RATE;
                config.format = AUDIO_FORMAT_PCM_16_BIT;
                config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
                audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE;
                ALOGD("usb_open_output_stream target_device: samplerate:%d,format:%d, channel:%d", config.sample_rate, config.format, config.channel_mask);
                target_ret = adev->usb_device->open_output_stream(adev->usb_device, handle, (audio_devices_t)(device), flags,
                        &config, &adev->usb_stream_out, port->ext.device.address);
                if(target_ret != 0) {
                    ALOGE("a2dp_open_output_stream target_ret:%d", target_ret);
                } else {
                    mAudioOutputDevice->addExternalOutput((audio_devices_t)device, adev->usb_stream_out);
                    if(adev->a2dp_stream_out) {
                        ALOGD("stop a2dp captrue thread");
                        stopAudioCaptureThread(adev->a2dp_stream_out);
                    }
                    startAudioCaptureThread(device, adev->usb_stream_out);
                }
                // mAudioOutputMixer->createMixer(AudioOutputMixer::DeviceType::USB);
            } else if ( device & AUDIO_DEVICE_OUT_HDMI_ARC) {
                // mAudioOutputMixer->createMixer(AudioOutputMixer::DeviceType::HDMI_ARC);
            }

            ALOGI("%s: connect device: 0x%x", __FUNCTION__, device);
            mAudioOutputDevice->setConnectedDevice((audio_devices_t)device, adev->delayUnmute);
            mAudioDelayManager->notifyDeviceConnected(device);
            pthread_mutex_unlock(&adev->audio_device_lock);
            ret = 0;
        } else if (device & AUDIO_DEVICE_BIT_IN) {
            ret = 0;
        }
    } else {
        if (mAudioPortManager != nullptr) {
            mAudioPortManager->removePort(port->id);
        }
        if ((device & AUDIO_DEVICE_BIT_IN) == 0) {
            pthread_mutex_lock(&adev->audio_device_lock);
            adev->out_device &= (audio_devices_t) (~device);
            ALOGI("current out device=0x%x", adev->out_device);

            AudioPatchManager::instance().disconnectDevice(static_cast<audio_devices_t>(device));

            if (device & AUDIO_DEVICE_OUT_ALL_A2DP)
            {
                ALOGD("a2dp disconnect, device 0x%x", device);
                if(adev->a2dp_stream_out) {
                    stopAudioCaptureThread(adev->a2dp_stream_out);
                    mAudioOutputDevice->removeExternalOutput((audio_devices_t)device);
                    ALOGD("a2dp_close_output_stream dev=%p, out=%p",adev->a2dp_device, adev->a2dp_stream_out);
                    adev->a2dp_device->close_output_stream(adev->a2dp_device, adev->a2dp_stream_out);
                    if(adev->a2dp_stream_out) {
                        adev->a2dp_stream_out = NULL;
                    }
                } else {
                    ALOGE("a2dp close fail for null");
                }
                if(adev->usb_stream_out) {
                    ALOGD("start usb captrue thread");
                    startAudioCaptureThread(AUDIO_DEVICE_OUT_ALL_USB, adev->usb_stream_out);
                }
                // mAudioOutputMixer->releaseMixer(AudioOutputMixer::DeviceType::BLUETOOTH);
            }
            else if (device & AUDIO_DEVICE_OUT_ALL_USB)
            {
                ALOGD("usb disconnect, device 0x%x", device);
                if(adev->usb_stream_out) {
                    stopAudioCaptureThread(adev->usb_stream_out);
                    mAudioOutputDevice->removeExternalOutput((audio_devices_t)device);
                    ALOGD("usb_close_output_stream dev=%p, out=%p",adev->usb_device, adev->usb_stream_out);
                    adev->usb_device->close_output_stream(adev->usb_device, adev->usb_stream_out);
                    if(adev->usb_stream_out) {
                        adev->usb_stream_out = NULL;
                    }
                } else {
                    ALOGE("usb close fail for null");
                }

                if(adev->a2dp_stream_out) {
                    ALOGD("start a2dp captrue thread");
                    startAudioCaptureThread(AUDIO_DEVICE_OUT_ALL_A2DP, adev->a2dp_stream_out);
                }
                // mAudioOutputMixer->releaseMixer(AudioOutputMixer::DeviceType::USB);
            } else if ( device & AUDIO_DEVICE_OUT_HDMI_ARC) {
                // mAudioOutputMixer->releaseMixer(AudioOutputMixer::DeviceType::HDMI_ARC);
            }

            mAudioDelayManager->notifyDeviceDisconnected(device);

            ALOGI("%s: disconnect device: 0x%x", __FUNCTION__, device);
            mAudioOutputDevice->setDisconnectedDevice((audio_devices_t)device, adev->delayUnmute);
            pthread_mutex_unlock(&adev->audio_device_lock);
            ret = 0;
        } else if (device & AUDIO_DEVICE_BIT_IN) {
            ret = 0;
        }
    }
    return ret;
}

int adev_set_audio_port_config(struct audio_hw_device* dev __unused,
                               const struct audio_port_config* port)
{
    switch (port->type) {
      case AUDIO_PORT_TYPE_DEVICE:
        ALOGI("set_audio_port_config: id=%d, type=%d (device), device-type=%s",
              port->id, port->type,
              audio_device_to_string(port->ext.device.type));
        break;
      case AUDIO_PORT_TYPE_MIX:
        ALOGI("set_audio_port_config id=%d, type=%d (mix), io-handle=%d", port->id,
              port->type, port->ext.mix.handle);
        break;
      default:
        ALOGI("set_audio_port_config: id=%d, type=%d", port->id, port->type);
        break;
    }
    return 0;
}

static int adev_dump(const audio_hw_device_t* device __unused, int fd)
{
    AUDIO_FUNC_ENTER
    const size_t SIZE = 64;
    const size_t LENGTH = 1024;
    char buffer[SIZE] = {0};
    char result[LENGTH] = {0};
    snprintf(buffer, SIZE, "\naudio device::dump\n");
    strncat(result, buffer, SIZE - 1);
    strncat(result, buffer, SIZE - 1);
    write(fd, result, strlen(result));
    return 0;
}

static int adev_close(hw_device_t* device)
{
    AUDIO_FUNC_ENTER
    struct audio_device* adev = (struct audio_device*)device;

#ifdef TUNNELED_PLAYBACK
    tunneledPlaybackDeinit();
#endif

#ifdef BLE_ENABLED
    audio_hw_device_close(adev->target_device);
#endif

    if (adev->a2dp_device) {
        audio_hw_device_close(adev->a2dp_device);
        rtk::media::audio::DeviceManager::unregisterDevice(
            rtk::media::audio::kA2dpDevices);
    }

    if (adev->usb_device) {
        audio_hw_device_close(adev->usb_device);
        rtk::media::audio::DeviceManager::unregisterDevice(
            rtk::media::audio::kUsbDevices);
    }

    if(mAudioConfig)
        delete mAudioConfig;

    if(mAudioDelayManager)
        delete mAudioDelayManager;

    if(mAudioOutputDevice)
        delete mAudioOutputDevice;

    if(mAudioPortManager)
        delete mAudioPortManager;

    if(adev->pKaraokeConfig)
        delete adev->pKaraokeConfig;

    free(device);
    rtk::media::audio::DeviceManager::unregisterDevice(
        rtk::media::audio::kPrimaryDevices);
    pthread_mutex_destroy(&global_lock);
    return 0;
}

/* Auto detect ALSA card */
static void detect_alsa_card_number(struct audio_device* adev)
{
    const char* cards = "/proc/asound/cards";
    int tries = 10;
    FILE* fp = NULL;

    ALOGD("Start detect alsa cards\n");

    while (--tries) {
        if ((fp = fopen(cards, "r")) == NULL) {
            ALOGE("Cannot open %s file to get list of sound cards", cards);
            usleep(10000);
            continue;
        }
        break;
    }

    if(fp != NULL) {
        char* line = NULL;
        size_t len = 0;
        int line_no = 0;
        char* ptr, *saveptr;
        int card_number = -1;
        ssize_t bytes_read;
        while ((bytes_read = getline(&line, &len, fp)) != -1) {
            ALOGD("Line %d:%s", line_no, line);
            if (line_no++ % 2) {
                continue;
            }
            ptr = strtok_r(line, " [", &saveptr);
            if (!ptr) {
                continue;
            }

            card_number = atoi(ptr);
            ALOGD("token1 %s:%d", ptr, card_number);

            ptr = strtok_r(saveptr, " [", &saveptr);
            if (!ptr) {
                continue;
            }
            ALOGD("token2 %s:%d", ptr, card_number);

            if (!strncmp(ptr, "Mars", 4)) {
                adev->alsa_card = card_number;
                ALOGD("find alsa card number %d", adev->alsa_card);
                break;
            }
        }
        ALOGD("End detect alsa cards\n");

        if (line) {
            free(line);
        }
        fclose(fp);
    }
}

#ifdef BLE_ENABLED
int ble_open(struct audio_device* adev)
{
    AUDIO_FUNC_ENTER
    int status;

    status = hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, "vr_bee_hidraw", &adev->target_module);
    if (status != 0) {
        ALOGE("failed to open module %s.%s (%s)", AUDIO_HARDWARE_MODULE_ID, "vr_bee_hidraw", strerror(-status));
        return status;
    }

    status = audio_hw_device_open(adev->target_module, &adev->target_device);
    if (status != 0) {
        ALOGE("failed to open hidraw device (%s)", strerror(-status));
        return status;
    }

    ALOGI("open hidraw module %s.vr_bee_hidraw", AUDIO_HARDWARE_MODULE_ID);
    return 0;
}
#endif

int a2dp_device_open(struct audio_device* adev)
{
    AUDIO_FUNC_ENTER
    int status;
    status = hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID,
                                    "bluetooth-rtk",
                                    &adev->a2dp_module);
    if (status != 0) {
        ALOGE("failed to open module %s.%s (%s)", AUDIO_HARDWARE_MODULE_ID, "bluetooth-rtk", strerror(-status));
        return status;
    }
    status = audio_hw_device_open(adev->a2dp_module, &adev->a2dp_device);
    if (status != 0) {
        ALOGE("failed to open a2dp device (%s)", strerror(-status));
        return status;
    }

    rtk::media::audio::DeviceManager::registerDevice(
        rtk::media::audio::kA2dpDevices, adev->a2dp_device);

    ALOGI("%s, open a2dp module %s.bluetooth-rtk, dev=%p",__func__, AUDIO_HARDWARE_MODULE_ID, adev->a2dp_device);
    return 0;
}

int usb_device_open(struct audio_device* adev)
{
    AUDIO_FUNC_ENTER
    int status;
    status = hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID,
                                    "usb-rtk",
                                    &adev->usb_module);
    if (status != 0) {
        ALOGE("failed to open module %s.%s (%s)", AUDIO_HARDWARE_MODULE_ID, "usb-rtk", strerror(-status));
        return status;
    }
    status = audio_hw_device_open(adev->usb_module, &adev->usb_device);
    if (status != 0) {
        ALOGE("failed to open usb device (%s)", strerror(-status));
        return status;
    }

    rtk::media::audio::DeviceManager::registerDevice(
        rtk::media::audio::kUsbDevices, adev->usb_device);

    ALOGI("%s, open usb module %s.usb-rtk, dev=%p",__func__, AUDIO_HARDWARE_MODULE_ID, adev->usb_device);
    return 0;
}

#if _ENABLE_DOLBY_HAL
int _audiohal_callback(
    CALLBACK_TYPE_AUDIOHAL type,
    void *stream,
    CALLBACK_AUDIOHAL_DATA data)
{
    int ret = -1;
    struct audio_stream_out *out = (struct audio_stream_out *)stream;

    if (out == NULL || out->set_volume == NULL){
        ALOGE("%s:%d, error, type=%d, stream=%p",__func__,__LINE__, type, stream);
    }else if (type == CALLBACK_TYPE_AUDIOHAL_setPcmPlaybackStreamGain){
        out->set_volume(out, data.setPcmPlaybackStreamGain, data.setPcmPlaybackStreamGain);
        ret = 0;
    }
    return ret;
}
#endif

static int adev_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    resetAudioDebug();
    AUDIO_FUNC_ENTER

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0) {
        ALOGE("adev_open: name is not AUDIO_HARDWARE_INTERFACE: %s", name);
        return -EINVAL;
    }

    struct audio_device* adev;
    adev = static_cast<audio_device*>(calloc(1, sizeof(struct audio_device)));
    if (!adev) {
        ALOGE("adev_open: calloc fail");
        return -ENOMEM;
    }

    pthread_mutex_init(&adev->audio_device_lock, (const pthread_mutexattr_t*) NULL);
    pthread_mutex_init(&global_lock, (const pthread_mutexattr_t*) NULL);

    int status = 0;

#ifdef BLE_ENABLED
    status = ble_open(adev);
    if (status != 0) {
        free(adev);
        return status;
    }
#endif

    status = a2dp_device_open(adev);
    if (status != 0) {
        free(adev);
        return status;
    }

    status = usb_device_open(adev);
    if (status != 0) {
        free(adev);
        return status;
    }

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_3_2;
    adev->hw_device.common.module = (struct hw_module_t*) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.get_master_volume = adev_get_master_volume;
    adev->hw_device.set_master_mute = adev_set_master_mute;
    adev->hw_device.get_master_mute = adev_get_master_mute;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;
#ifdef AUDIO_HAL_4_0_ONLY
    adev->hw_device.get_microphones = adev_get_microphones;
#endif

    adev->hw_device.create_audio_patch = adev_create_audio_patch;
    adev->hw_device.release_audio_patch = adev_release_audio_patch;
    adev->hw_device.get_audio_port = adev_get_audio_port;
    adev->hw_device.get_audio_port_v7 = adev_get_audio_port_v7;
    adev->hw_device.set_device_connected_state_v7 = adev_set_device_connected_state_v7;
    adev->hw_device.set_audio_port_config = adev_set_audio_port_config;
    adev->patch_manager = create_patch_manager(adev);

    /* Set the default route before the PCM stream is opened */
    pthread_mutex_lock(&adev->audio_device_lock);

    adev->active_input = NULL;
    adev->in_device = AUDIO_DEVICE_IN_BUILTIN_MIC;
    adev->out_device = AUDIO_DEVICE_OUT_SPEAKER;
    adev->bluetooth_nrec = true;
    adev->AO_open_count = 0; // AO capture count
    list_init(&adev->stream_out_list);

    adev->offload_handle = NULL;
    adev->dualmono_mode = MAIN_ONLY;
    adev->dmx_mode = DMX_LTRT;

    adev->alsa_card = -1;
    detect_alsa_card_number(adev);
    alsa_card = adev->alsa_card;  // for get_input_buffer_size use
    memset(mic_property, '\0', sizeof(mic_property)); // for mic select

#ifdef TUNNELED_PLAYBACK
    tunneledPlaybackInit();
#endif

    adev->aaudio_count = 0;

    pthread_mutex_unlock(&adev->audio_device_lock);

    *device = &adev->hw_device.common;

    mAudioOutputDevice = new AudioOutputDevice();
    mAudioOutputDevice->initAudioOutDevice();

#if _ENABLE_DOLBY_HAL
    RTKIMP_TYPE_MS12_VER ms12Version = RTKIMP_TYPE_MS12_VER_V26;
    int rtkaudiofd = -1;
    int caps = 0;
    rtkaudiofd = open ("/dev/rtkaudio", O_RDWR);
    if (rtkaudiofd != -1) {

        if (ioctl(rtkaudiofd, RTKAUDIO_IOC_OMX_CREATE_DECODER, NULL) < 0) {
            ALOGE("[HAL] Create decoder ioctl failed\n");
            //return -1;
        }

//https://wiki.realtek.com/pages/viewpage.action?spaceKey=MDKB&title=%5BAudio%5D%5BDoc%5D+Query+audio+firmware+capability
        if (ioctl(rtkaudiofd, RTKAUDIO_IOC_GET_FW_CAPABILITY, &caps) < 0) {
            ALOGE("[HAL] Get FW CAPABILITY failed ioctl failed\n");
            //return -1;
        } else {
            if (caps & 0x7) {
                adev->is_m12_platform = 1;
            } else {
                adev->is_m12_platform = 0;
            }

            if (caps & 0x38) {
                adev->max_aac_channel = ((caps & 0x38) >> 3) + 1;
            } else {
                adev->max_aac_channel = 0;
            }

            if (((caps & 0x3800) >> 11) == 0){
                ms12Version = RTKIMP_TYPE_MS12_VER_V24;
            }else if (((caps & 0x3800) >> 11) == 1){
                ms12Version = RTKIMP_TYPE_MS12_VER_V26;
            }
        }

        close(rtkaudiofd);
    }
{
    char property[256];
#define RTK_AUDIO_DEBUG_MSD_HAL_PROP "persist.vendor.rtk.audio.msd_hal"
    property_get(RTK_AUDIO_DEBUG_MSD_HAL_PROP, property, "1");
    std::shared_ptr<::CRTKIMP_CallBack> ms12ServerCb = nullptr;
    if (!strcmp(property, "1")) {
        ms12ServerCb = regist_ms12_service(ms12Version);
        rtkms12Aidk_setCallbackPcmPlaybackStreamGain(_audiohal_callback);
        rtkms12Aidk_setConfigSupportDolby(adev->is_m12_platform);
    }
    ALOGD("%s:%d, %s, enable=%d ms12cb=%p(%ld) caps=0x%x ms12=%d",
         __FUNCTION__, __LINE__,RTK_AUDIO_DEBUG_MSD_HAL_PROP,atoi(property),
         ms12ServerCb.get(), ms12ServerCb.use_count(), caps, ms12Version);
}
#endif

    mAudioConfig = new AudioConfig();
    mAudioDelayManager = new AudioDelayManager();
    mAudioDelayManager->setOutputDevice(mAudioOutputDevice);

    mAudioPortManager = new AudioPortManager();

    adev->pKaraokeConfig = new KaraokeConfig();

    rtk::media::audio::DeviceManager::registerDevice(
        rtk::media::audio::kPrimaryDevices, &adev->hw_device);

    AUDIO_FUNC_EXIT
    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "RTK Audio HAL",
        .author = "RTK",
        .methods = &hal_module_methods,
    },
};
