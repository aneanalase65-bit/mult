#pragma once
#include <memory>
#include <cstdint>
namespace oboe {
enum class Result { OK, ErrorDisconnected, ErrorClosed, Other };
inline const char* convertToText(Result) { return ""; }
enum class Direction { Input, Output };
enum class PerformanceMode { LowLatency };
enum class SharingMode { Exclusive };
enum class AudioFormat { Float };
enum ChannelCount { Stereo = 2 };
enum class Usage { Media };
enum class InputPreset { Camcorder, Unprocessed };
enum class SampleRateConversionQuality { Medium };
enum class DataCallbackResult { Continue };
template <class T> struct ResultWithValue { Result error() const { return Result::OK; } T value() const { return T(); } };
class AudioStream {
 public:
  int getChannelCount() { return 2; } int getSampleRate() { return 48000; } int getFramesPerBurst() { return 192; }
  void setBufferSizeInFrames(int) {} void requestStart() {} void stop() {} void close() {}
  ResultWithValue<int32_t> read(void*, int32_t, int64_t) { return {}; }
};
class AudioStreamDataCallback { public: virtual ~AudioStreamDataCallback() = default;
  virtual DataCallbackResult onAudioReady(AudioStream*, void*, int32_t) = 0; };
class AudioStreamErrorCallback { public: virtual ~AudioStreamErrorCallback() = default;
  virtual void onErrorAfterClose(AudioStream*, Result) {} };
class AudioStreamBuilder {
 public:
  AudioStreamBuilder* setDirection(Direction) { return this; }
  AudioStreamBuilder* setPerformanceMode(PerformanceMode) { return this; }
  AudioStreamBuilder* setSharingMode(SharingMode) { return this; }
  AudioStreamBuilder* setFormat(AudioFormat) { return this; }
  AudioStreamBuilder* setChannelCount(int) { return this; }
  AudioStreamBuilder* setSampleRate(int) { return this; }
  AudioStreamBuilder* setSampleRateConversionQuality(SampleRateConversionQuality) { return this; }
  AudioStreamBuilder* setInputPreset(InputPreset) { return this; }
  AudioStreamBuilder* setUsage(Usage) { return this; }
  AudioStreamBuilder* setDeviceId(int) { return this; }
  AudioStreamBuilder* setDataCallback(std::shared_ptr<AudioStreamDataCallback>) { return this; }
  AudioStreamBuilder* setErrorCallback(std::shared_ptr<AudioStreamErrorCallback>) { return this; }
  Result openStream(std::shared_ptr<AudioStream>&) { return Result::OK; }
};
}
