#include <thread>

#include <opus/opus_multistream.h>

#include "platform/common.h"

#include "audio.h"
#include "config.h"
#include "main.h"
#include "thread_safe.h"
#include "utility.h"

namespace audio {
  using namespace std::literals;
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<std::int16_t>>>;

  struct audio_ctx_t {
    // We want to change the sink for the first stream only
    std::unique_ptr<std::atomic_bool> sink_flag;

    std::unique_ptr<platf::audio_control_t> control;

    bool restore_sink;
    platf::sink_t sink;
  };

  static int
  start_audio_control(audio_ctx_t &ctx);
  static void
  stop_audio_control(audio_ctx_t &);

  int
  map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;

  opus_stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51,
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51,
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71,
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71,
      2048000,
    },
  };

  auto control_shared = safe::make_shared<audio_ctx_t>(start_audio_control, stop_audio_control);

  void
  encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    auto stream = &stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];

    // Encoding takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    opus_t opus { opus_multistream_encoder_create(
      stream->sampleRate,
      stream->channelCount,
      stream->streams,
      stream->coupledStreams,
      stream->mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr) };

    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream->bitrate));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));

    auto frame_size = config.packetDuration * stream->sampleRate / 1000;
    while (auto sample = samples->pop()) {
      buffer_t packet { 1400 };

      int bytes = opus_multistream_encode(opus.get(), sample->data(), frame_size, std::begin(packet), packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));
    }
  }

  void
  capture(safe::mail_t mail, config_t config, void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    auto stream = &stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];

    auto ref = control_shared.ref();
    if (!ref) {
      return;
    }

    auto &control = ref->control;
    if (!control) {
      shutdown_event->view();

      return;
    }

    // Order of priority:
    // 1. Config
    // 2. Virtual if available
    // 3. Host
    std::string *sink = &ref->sink.host;
    if (!config::audio.sink.empty()) {
      sink = &config::audio.sink;
    }
    else if (ref->sink.null) {
      auto &null = *ref->sink.null;
      switch (stream->channelCount) {
        case 2:
          sink = &null.stereo;
          break;
        case 6:
          sink = &null.surround51;
          break;
        case 8:
          sink = &null.surround71;
          break;
      }
    }

    // Only the first to start a session may change the default sink
    if (!ref->sink_flag->exchange(true, std::memory_order_acquire)) {
      ref->restore_sink = !config.flags[config_t::HOST_AUDIO];

      // If the sink is empty (Host has no sink!), definately switch to the virtual.
      if (ref->sink.host.empty()) {
        if (control->set_sink(*sink)) {
          return;
        }
      }
      // If the client requests audio on the host, don't change the default sink
      else if (!config.flags[config_t::HOST_AUDIO] && control->set_sink(*sink)) {
        return;
      }
    }

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    auto samples = std::make_shared<sample_queue_t::element_type>(30);
    std::thread thread { encodeThread, samples, config, channel_data };

    auto fg = util::fail_guard([&]() {
      samples->stop();
      thread.join();

      shutdown_event->view();
    });

    auto frame_size = config.packetDuration * stream->sampleRate / 1000;
    int samples_per_frame = frame_size * stream->channelCount;

    auto mic = control->microphone(stream->mapping, stream->channelCount, stream->sampleRate, frame_size);
    if (!mic) {
      BOOST_LOG(error) << "Couldn't create audio input"sv;

      return;
    }

    while (!shutdown_event->peek()) {
      std::vector<std::int16_t> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);
      switch (status) {
        case platf::capture_e::ok:
          break;
        case platf::capture_e::timeout:
          continue;
        case platf::capture_e::reinit:
          mic.reset();
          mic = control->microphone(stream->mapping, stream->channelCount, stream->sampleRate, frame_size);
          if (!mic) {
            BOOST_LOG(error) << "Couldn't re-initialize audio input"sv;

            return;
          }
          return;
        default:
          return;
      }

      samples->raise(std::move(sample_buffer));
    }
  }

  int
  map_stream(int channels, bool quality) {
    int shift = quality ? 1 : 0;
    switch (channels) {
      case 2:
        return STEREO + shift;
      case 6:
        return SURROUND51 + shift;
      case 8:
        return SURROUND71 + shift;
    }
    return STEREO;
  }

  int
  start_audio_control(audio_ctx_t &ctx) {
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);

    // The default sink has not been replaced yet.
    ctx.restore_sink = false;

    if (!(ctx.control = platf::audio_control())) {
      return 0;
    }

    auto sink = ctx.control->sink_info();
    if (!sink) {
      // Let the calling code know it failed
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);

    fg.disable();
    return 0;
  }

  void
  stop_audio_control(audio_ctx_t &ctx) {
    // restore audio-sink if applicable
    if (!ctx.restore_sink) {
      return;
    }

    const std::string &sink = config::audio.sink.empty() ? ctx.sink.host : config::audio.sink;
    if (!sink.empty()) {
      // Best effort, it's allowed to fail
      ctx.control->set_sink(sink);
    }
  }
}  // namespace audio
