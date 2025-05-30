// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/media/capture/desktop_capture_device.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <memory>
#include <utility>

#include "base/check_op.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/message_loop/message_pump_type.h"
#include "base/metrics/histogram_macros.h"
#include "base/notreached.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/tick_clock.h"
#include "base/timer/timer.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "content/browser/media/capture/desktop_capture_device_uma_types.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/desktop_capture.h"
#include "content/public/browser/desktop_media_id.h"
#include "content/public/browser/device_service.h"
#include "content/public/common/content_switches.h"
#include "media/base/video_util.h"
#include "media/capture/content/capture_resolution_chooser.h"
#include "media/webrtc/webrtc_features.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/device/public/mojom/wake_lock.mojom.h"
#include "services/device/public/mojom/wake_lock_provider.mojom.h"
#include "third_party/libyuv/include/libyuv/scale_argb.h"
#include "third_party/webrtc/modules/desktop_capture/cropped_desktop_frame.h"
#include "third_party/webrtc/modules/desktop_capture/cropping_window_capturer.h"
#include "third_party/webrtc/modules/desktop_capture/desktop_and_cursor_composer.h"
#include "third_party/webrtc/modules/desktop_capture/desktop_capture_options.h"
#include "third_party/webrtc/modules/desktop_capture/desktop_capturer.h"
#include "third_party/webrtc/modules/desktop_capture/desktop_frame.h"
#include "third_party/webrtc/modules/desktop_capture/fake_desktop_capturer.h"
#include "third_party/webrtc/modules/desktop_capture/mouse_cursor_monitor.h"
#include "ui/gfx/icc_profile.h"

#if BUILDFLAG(IS_CHROMEOS_LACROS)
#include "content/browser/media/capture/desktop_capturer_lacros.h"
#endif

namespace content {

namespace {

// Maximum CPU time percentage of a single core that can be consumed for desktop
// capturing. This means that on systems where screen scraping is slow we may
// need to capture at frame rate lower than requested. This is necessary to keep
// UI responsive.
const int kDefaultMaximumCpuConsumptionPercentage = 50;

webrtc::DesktopRect ComputeLetterboxRect(
    const webrtc::DesktopSize& max_size,
    const webrtc::DesktopSize& source_size) {
  gfx::Rect result = media::ComputeLetterboxRegion(
      gfx::Rect(0, 0, max_size.width(), max_size.height()),
      gfx::Size(source_size.width(), source_size.height()));
  return webrtc::DesktopRect::MakeLTRB(
      result.x(), result.y(), result.right(), result.bottom());
}

bool IsFrameUnpackedOrInverted(webrtc::DesktopFrame* frame) {
  return frame->stride() !=
      frame->size().width() * webrtc::DesktopFrame::kBytesPerPixel;
}

void BindWakeLockProvider(
    mojo::PendingReceiver<device::mojom::WakeLockProvider> receiver) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  GetDeviceService().BindWakeLockProvider(std::move(receiver));
}

int GetMaximumCpuConsumptionPercentage() {
  int max_cpu_consumption_percentage = kDefaultMaximumCpuConsumptionPercentage;

  std::string string_value =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          switches::kWebRtcMaxCpuConsumptionPercentage);
  int tmp_percentage = 0;
  if (base::StringToInt(string_value, &tmp_percentage)) {
    // If the max cpu percentage provided by the user is outside [1, 100] then
    // |max_cpu_consumption_percentage_| is left to the default value. Same if
    // no value is provided by the user, i.e. |string_value| will be empty and
    // base::StringToInt will set |tmp_percentage| to 0.
    if (tmp_percentage > 0 && tmp_percentage <= 100)
      max_cpu_consumption_percentage = tmp_percentage;
  }

  UMA_HISTOGRAM_BOOLEAN("WebRTC.DesktopCapture.MaxCpuConsumptionIsDefault",
                        max_cpu_consumption_percentage ==
                            kDefaultMaximumCpuConsumptionPercentage);
  return max_cpu_consumption_percentage;
}

void LogDesktopCaptureZeroHzIsActive(DesktopMediaID::Type capturer_type,
                                     bool zero_hz_is_active) {
  if (capturer_type == DesktopMediaID::TYPE_SCREEN) {
    UMA_HISTOGRAM_BOOLEAN("WebRTC.DesktopCapture.IsZeroHzActive.Screen",
                          zero_hz_is_active);
  } else {
    UMA_HISTOGRAM_BOOLEAN("WebRTC.DesktopCapture.IsZeroHzActive.Window",
                          zero_hz_is_active);
  }
}

void LogDesktopCaptureFrameIsRefresh(DesktopMediaID::Type capturer_type,
                                     bool is_refresh_frame) {
  if (capturer_type == DesktopMediaID::TYPE_SCREEN) {
    UMA_HISTOGRAM_BOOLEAN("WebRTC.DesktopCapture.FrameIsRefresh.Screen",
                          is_refresh_frame);
  } else {
    UMA_HISTOGRAM_BOOLEAN("WebRTC.DesktopCapture.FrameIsRefresh.Window",
                          is_refresh_frame);
  }
}

}  // namespace

class DesktopCaptureDevice::Core : public webrtc::DesktopCapturer::Callback {
 public:
  Core(scoped_refptr<base::SingleThreadTaskRunner> task_runner,
       std::unique_ptr<webrtc::DesktopCapturer> capturer,
       DesktopMediaID::Type type,
       bool zero_hertz_is_supported);

  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  ~Core() override;

  // Implementation of VideoCaptureDevice methods.
  void AllocateAndStart(const media::VideoCaptureParams& params,
                        std::unique_ptr<Client> client);
  void RequestRefreshFrame();

  void SetNotificationWindowId(gfx::NativeViewId window_id);

  void SetMockTimeForTesting(
      scoped_refptr<base::SingleThreadTaskRunner> task_runner,
      const base::TickClock* tick_clock);

  base::WeakPtr<Core> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  // webrtc::DesktopCapturer::Callback interface.
  // A side-effect of this method is to schedule the next frame.
  void OnCaptureResult(
    webrtc::DesktopCapturer::Result result,
    std::unique_ptr<webrtc::DesktopFrame> frame) override;

  // Sends the last received frame (stored in |output_frame_|) to the client
  // using the colorspace in |last_frame_color_space_|.
  // Does not schedule the next frame.
  void SendLastReceivedFrameToClient(bool is_refresh_frame);

  // Method that is scheduled on |task_runner_| to be called on regular interval
  // to capture a frame.
  void OnCaptureTimer();

  // Captures a frame. Upon completion, schedules the next frame.
  void CaptureFrame();

  // Schedules a timer for the next call to |CaptureFrame|. This method assumes
  // that |CaptureFrame| has already been called at least once before.
  void ScheduleNextCaptureFrame();

  void RequestWakeLock();

  base::TimeTicks NowTicks() const;

  bool zero_hertz_is_supported() const { return zero_hertz_is_supported_; }

  // Task runner used for capturing operations.
  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;

  // The underlying DesktopCapturer instance used to capture frames.
  std::unique_ptr<webrtc::DesktopCapturer> desktop_capturer_;

  // The device client which proxies device events to the controller. Accessed
  // on the task_runner_ thread.
  std::unique_ptr<Client> client_;

  // Requested video capture frame rate.
  float requested_frame_rate_;

  // Inverse of the requested frame rate.
  base::TimeDelta requested_frame_duration_;

  // Records time of last call to CaptureFrame.
  base::TimeTicks capture_start_time_;

  // Size of frame most recently captured from the source.
  webrtc::DesktopSize last_frame_size_;

  // DesktopFrame into which captured frames are stored; either intact or
  // possibly after being down-scaled and/or letterboxed, depending upon the
  // caller's requested capture capabilities. The output frame is black when
  // |output_frame_is_black_| is set. This can happen when a minimized window
  // is shared.
  std::unique_ptr<webrtc::DesktopFrame> output_frame_;

  // True when the |output_frame_->data()| contains only zeros. Tracking this is
  // an optimization to avoid re-clearing |output_frame_| during stretches where
  // we are only sending black frames.
  bool output_frame_is_black_ = false;

  // Copy of the colorspace used for the most recent frame sent to the client.
  gfx::ColorSpace output_frame_color_space_;

  // Determines the size of frames to deliver to the |client_|.
  media::CaptureResolutionChooser resolution_chooser_;

  raw_ptr<const base::TickClock> tick_clock_ = nullptr;

  // Timer used to capture the frame.
  std::unique_ptr<base::OneShotTimer> capture_timer_;

  // See above description of kDefaultMaximumCpuConsumptionPercentage.
  int max_cpu_consumption_percentage_;

  // True when waiting for |desktop_capturer_| to capture current frame.
  bool capture_in_progress_;

  // True if the first capture call has returned. Used to log the first capture
  // result.
  bool first_capture_returned_;

  // True if the first capture permanent error has been logged. Used to log the
  // first capture permanent error.
  bool first_permanent_error_logged;

  // The type of the capturer.
  DesktopMediaID::Type capturer_type_;

  // True if we support dropping captured frames where the updated region
  // contains no change since last captured frame. To support this 0Hz mode,
  // the utilized capturer implementation must updates the
  // |DesktopFrame::updated_region()| desktop region for each captured frame.
  const bool zero_hertz_is_supported_;

  // The system time when we receive the first frame.
  base::TimeTicks first_ref_time_;

  // TODO(jiayl): Remove wake_lock_ when there is an API to keep the
  // screen from sleeping for the drive-by web.
  mojo::Remote<device::mojom::WakeLock> wake_lock_;

  base::WeakPtrFactory<Core> weak_factory_{this};
};

DesktopCaptureDevice::Core::Core(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    std::unique_ptr<webrtc::DesktopCapturer> capturer,
    DesktopMediaID::Type type,
    bool zero_hertz_is_supported)
    : task_runner_(task_runner),
      desktop_capturer_(std::move(capturer)),
      capture_timer_(new base::OneShotTimer()),
      max_cpu_consumption_percentage_(GetMaximumCpuConsumptionPercentage()),
      capture_in_progress_(false),
      first_capture_returned_(false),
      first_permanent_error_logged(false),
      capturer_type_(type),
      zero_hertz_is_supported_(zero_hertz_is_supported) {}

DesktopCaptureDevice::Core::~Core() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  client_.reset();
  desktop_capturer_.reset();
}

void DesktopCaptureDevice::Core::AllocateAndStart(
    const media::VideoCaptureParams& params,
    std::unique_ptr<Client> client) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK_GT(params.requested_format.frame_size.GetArea(), 0);
  DCHECK_GT(params.requested_format.frame_rate, 0);
  DCHECK(desktop_capturer_);
  DCHECK(client);
  DCHECK(!client_);

  client_ = std::move(client);
  requested_frame_rate_ = params.requested_format.frame_rate;
  requested_frame_duration_ = base::Microseconds(static_cast<int64_t>(
      static_cast<double>(base::Time::kMicrosecondsPerSecond) /
          requested_frame_rate_ +
      0.5 /* round to nearest int */));

  // Pass the min/max resolution and fixed aspect ratio settings from |params|
  // to the CaptureResolutionChooser.
  const auto constraints = params.SuggestConstraints();
  resolution_chooser_.SetConstraints(constraints.min_frame_size,
                                     constraints.max_frame_size,
                                     constraints.fixed_aspect_ratio);
  VLOG(2) << __func__ << " (requested_frame_rate=" << requested_frame_rate_
          << ", max_frame_size=" << constraints.max_frame_size.ToString()
          << ")";

  DCHECK(!wake_lock_);
  RequestWakeLock();

  desktop_capturer_->Start(this);
  // Assume it will be always started successfully for now.
  client_->OnStarted();

  CaptureFrame();
}

void DesktopCaptureDevice::Core::RequestRefreshFrame() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  TRACE_EVENT0("webrtc", __func__);
  VLOG(2) << __func__ << " is called by the client";
  // Simply send the last received frame, if we ever received one. Don't
  // schedule a new frame.
  if (output_frame_) {
    SendLastReceivedFrameToClient(/*is_refresh_frame=*/true);
  }
}

void DesktopCaptureDevice::Core::SetNotificationWindowId(
    gfx::NativeViewId window_id) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(window_id);
  desktop_capturer_->SetExcludedWindow(window_id);
}

void DesktopCaptureDevice::Core::SetMockTimeForTesting(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    const base::TickClock* tick_clock) {
  tick_clock_ = tick_clock;
  capture_timer_ = std::make_unique<base::OneShotTimer>(tick_clock_);
  capture_timer_->SetTaskRunner(task_runner);
}

void DesktopCaptureDevice::Core::OnCaptureResult(
    webrtc::DesktopCapturer::Result result,
    std::unique_ptr<webrtc::DesktopFrame> frame) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(client_);
  DCHECK(capture_in_progress_);
  TRACE_EVENT0("webrtc", __func__);
  capture_in_progress_ = false;

  bool success = result == webrtc::DesktopCapturer::Result::SUCCESS;

  if (!first_capture_returned_) {
    first_capture_returned_ = true;
    if (capturer_type_ == DesktopMediaID::TYPE_SCREEN) {
      IncrementDesktopCaptureCounter(success ? FIRST_SCREEN_CAPTURE_SUCCEEDED
                                             : FIRST_SCREEN_CAPTURE_FAILED);
    } else {
      IncrementDesktopCaptureCounter(success ? FIRST_WINDOW_CAPTURE_SUCCEEDED
                                             : FIRST_WINDOW_CAPTURE_FAILED);
    }
  }

  if (!success) {
    VLOG(2) << __func__ << " [ERROR]";
    if (result == webrtc::DesktopCapturer::Result::ERROR_PERMANENT) {
      if (!first_permanent_error_logged) {
        first_permanent_error_logged = true;
        if (capturer_type_ == DesktopMediaID::TYPE_SCREEN) {
          IncrementDesktopCaptureCounter(SCREEN_CAPTURER_PERMANENT_ERROR);
        } else {
          IncrementDesktopCaptureCounter(WINDOW_CAPTURER_PERMANENT_ERROR);
        }
      }
      client_->OnError(media::VideoCaptureError::
                           kDesktopCaptureDeviceWebrtcDesktopCapturerHasFailed,
                       FROM_HERE, "The desktop capturer has failed.");
      return;
    }
    // Continue capturing frames in the temporary error case.
    ScheduleNextCaptureFrame();
    return;
  }
  DCHECK(frame);

  // Continue capturing frames when there are no changes in updated regions
  // since the last captured frame but don't send the same frame again to the
  // client. Clients may call RequestRefreshFrame() to ask for a copy of the
  // last captured frame. Check |output_frame_| to ensure that at least one
  // valid frame has already been captured.
  // |zero_hertz_is_supported()| can be false in combination with capturers that
  // do not support the 0Hz mode, e.g. Windows capturers using the WGC API.
  const bool zero_hertz_is_active = zero_hertz_is_supported() &&
                                    output_frame_ &&
                                    frame->updated_region().is_empty();
  VLOG(2) << __func__ << " [SUCCESS]" << (zero_hertz_is_active ? "[0Hz]" : "");
  if (zero_hertz_is_supported()) {
    LogDesktopCaptureZeroHzIsActive(capturer_type_, zero_hertz_is_active);
  }
  if (zero_hertz_is_active) {
    ScheduleNextCaptureFrame();
    return;
  }

  // If the frame size has changed, drop the output frame (if any), and
  // determine the new output size.
  if (!last_frame_size_.equals(frame->size())) {
    output_frame_.reset();
    resolution_chooser_.SetSourceSize(
        gfx::Size(frame->size().width(), frame->size().height()));
    last_frame_size_ = frame->size();
    VLOG(2) << "  last_frame_size=(" << last_frame_size_.width() << "x"
            << last_frame_size_.height() << ")";
  }
  // Align to 2x2 pixel boundaries, as required by OnIncomingCapturedData() so
  // it can convert the frame to I420 format.
  webrtc::DesktopSize output_size(
      resolution_chooser_.capture_size().width() & ~1,
      resolution_chooser_.capture_size().height() & ~1);
  if (output_size.is_empty()) {
    // Even RESOLUTION_POLICY_ANY_WITHIN_LIMIT is used, a non-empty size should
    // be guaranteed.
    output_size.set(2, 2);
  }
  VLOG(2) << "  output_size=(" << output_size.width() << "x"
          << output_size.height() << ")";

  gfx::ColorSpace frame_color_space;
  if (!frame->icc_profile().empty()) {
    gfx::ICCProfile icc_profile = gfx::ICCProfile::FromData(
        frame->icc_profile().data(), frame->icc_profile().size());
    frame_color_space = icc_profile.GetColorSpace();
  }
  output_frame_color_space_ = frame_color_space;

  if (frame->size().width() <= 1 || frame->size().height() <= 1) {
    // On OSX We receive a 1x1 frame when the shared window is minimized. It
    // cannot be subsampled to I420 and will be dropped downstream. So we
    // replace it with a black frame to avoid the video appearing frozen at the
    // last frame.
    if (!output_frame_ || !output_frame_->size().equals(output_size)) {
      // The new frame will be black by default.
      output_frame_ = std::make_unique<webrtc::BasicDesktopFrame>(output_size);
      output_frame_is_black_ = true;
    }
    if (!output_frame_is_black_) {
      size_t total_bytes = webrtc::DesktopFrame::kBytesPerPixel *
                           output_size.width() * output_size.height();
      memset(output_frame_->data(), 0, total_bytes);
      output_frame_is_black_ = true;
    }
  } else {
    // Scaling frame with odd dimensions to even dimensions will cause
    // blurring. See https://crbug.com/737278.
    // Since chromium always requests frames to be with even dimensions,
    // i.e. for I420 format and video codec, always cropping captured frame
    // to even dimensions.
    const int32_t frame_width = frame->size().width();
    const int32_t frame_height = frame->size().height();
    // TODO(braveyao): remove the check once |CreateCroppedDesktopFrame| can
    // do this check internally.
    if (frame_width & 1 || frame_height & 1) {
      frame = webrtc::CreateCroppedDesktopFrame(
          std::move(frame),
          webrtc::DesktopRect::MakeWH(frame_width & ~1, frame_height & ~1));
    }
    DCHECK(frame);
    DCHECK(!frame->size().is_empty());

    if (!frame->size().equals(output_size)) {
      VLOG(2) << "  Downscaling: frame->size=(" << frame->size().width() << "x"
              << frame->size().height() << ")";
      // Down-scale and/or letterbox to the target format if the frame does
      // not match the output size.

      // Allocate a buffer of the correct size to scale the frame into.
      // |output_frame_| is cleared whenever the output size changes, so we
      // don't need to worry about clearing out stale pixel data in
      // letterboxed areas.
      if (!output_frame_) {
        output_frame_ =
            std::make_unique<webrtc::BasicDesktopFrame>(output_size);
      }
      DCHECK(output_frame_->size().equals(output_size));

      // TODO(wez): Optimize this to scale only changed portions of the
      // output, using ARGBScaleClip().
      const webrtc::DesktopRect output_rect =
          ComputeLetterboxRect(output_size, frame->size());
      uint8_t* output_rect_data =
          output_frame_->GetFrameDataAtPos(output_rect.top_left());
      libyuv::ARGBScale(frame->data(), frame->stride(), frame->size().width(),
                        frame->size().height(), output_rect_data,
                        output_frame_->stride(), output_rect.width(),
                        output_rect.height(), libyuv::kFilterBilinear);
      output_frame_is_black_ = false;
    } else if (IsFrameUnpackedOrInverted(frame.get())) {
      VLOG(2) << "  FrameUnpackedOrInverted";
      // If |frame| is not packed top-to-bottom then create a packed
      // top-to-bottom copy. This is required if the frame is inverted (see
      // crbug.com/306876), or if |frame| is cropped form a larger frame (see
      // crbug.com/437740).
      if (!output_frame_) {
        output_frame_ =
            std::make_unique<webrtc::BasicDesktopFrame>(output_size);
      }
      output_frame_->CopyPixelsFrom(
          *frame, webrtc::DesktopVector(),
          webrtc::DesktopRect::MakeSize(frame->size()));
      output_frame_is_black_ = false;
    } else {
      VLOG(2) << "  output_frame_ = std::move(frame)";
      // If the captured frame matches the output size, we can use the incoming
      // frame as is without any modifications.
      output_frame_ = std::move(frame);
      output_frame_is_black_ = false;
    }
  }

  // Immediately send the new frame to the client and ask for a new frame.
  SendLastReceivedFrameToClient(/*is_refresh_frame=*/false);
  ScheduleNextCaptureFrame();
}

void DesktopCaptureDevice::Core::SendLastReceivedFrameToClient(
    bool is_refresh_frame) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  TRACE_EVENT0("webrtc", __func__);
  LogDesktopCaptureFrameIsRefresh(capturer_type_, is_refresh_frame);

  size_t output_bytes = output_frame_->size().width() *
                        output_frame_->size().height() *
                        webrtc::DesktopFrame::kBytesPerPixel;

  VLOG(2) << __func__ << " [output_size=(" << output_frame_->size().width()
          << "x" << output_frame_->size().height() << ")]";
  base::TimeTicks now = NowTicks();
  if (first_ref_time_.is_null())
    first_ref_time_ = now;
  client_->OnIncomingCapturedData(
      output_frame_->data(), output_bytes,
      media::VideoCaptureFormat(gfx::Size(output_frame_->size().width(),
                                          output_frame_->size().height()),
                                requested_frame_rate_,
                                media::PIXEL_FORMAT_ARGB),
      output_frame_color_space_, 0 /* clockwise_rotation */, false /* flip_y */,
      now, now - first_ref_time_);
}

void DesktopCaptureDevice::Core::OnCaptureTimer() {
  DCHECK(task_runner_->BelongsToCurrentThread());

  if (!client_)
    return;

  CaptureFrame();
}

void DesktopCaptureDevice::Core::CaptureFrame() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(!capture_in_progress_);
  TRACE_EVENT0("webrtc", __func__);
  VLOG(2) << __func__;

  capture_start_time_ = NowTicks();
  capture_in_progress_ = true;

  desktop_capturer_->CaptureFrame();
}

void DesktopCaptureDevice::Core::ScheduleNextCaptureFrame() {
  // Make sure CaptureFrame() was called at least once before.
  DCHECK(!capture_start_time_.is_null());

  base::TimeDelta last_capture_duration = NowTicks() - capture_start_time_;
  VLOG(2) << __func__ << " [last_capture_duration="
          << last_capture_duration.InMilliseconds() << "]";

  // Limit frame-rate to reduce CPU consumption.
  base::TimeDelta capture_period =
      std::max((last_capture_duration * 100) / max_cpu_consumption_percentage_,
               requested_frame_duration_);

  VLOG(2) << "  timer(dT="
          << (capture_period - last_capture_duration).InMilliseconds() << ")";
  // Schedule a task for the next frame.
  capture_timer_->Start(FROM_HERE, capture_period - last_capture_duration, this,
                        &Core::OnCaptureTimer);
}

void DesktopCaptureDevice::Core::RequestWakeLock() {
  mojo::Remote<device::mojom::WakeLockProvider> wake_lock_provider;
  auto receiver = wake_lock_provider.BindNewPipeAndPassReceiver();
  // TODO(https://crbug.com/823869): Fix DesktopCaptureDeviceTest and remove
  // this conditional.
  if (BrowserThread::IsThreadInitialized(BrowserThread::UI)) {
    GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(&BindWakeLockProvider, std::move(receiver)));
  }

  wake_lock_provider->GetWakeLockWithoutContext(
      device::mojom::WakeLockType::kPreventDisplaySleep,
      device::mojom::WakeLockReason::kOther, "Native desktop capture",
      wake_lock_.BindNewPipeAndPassReceiver());

  wake_lock_->RequestWakeLock();
}

base::TimeTicks DesktopCaptureDevice::Core::NowTicks() const {
  return tick_clock_ ? tick_clock_->NowTicks() : base::TimeTicks::Now();
}

// static
std::unique_ptr<media::VideoCaptureDevice> DesktopCaptureDevice::Create(
    const DesktopMediaID& source) {
  auto options = desktop_capture::CreateDesktopCaptureOptions();
  std::unique_ptr<webrtc::DesktopCapturer> capturer;
  std::unique_ptr<media::VideoCaptureDevice> result;

#if BUILDFLAG(IS_WIN)
  options.set_allow_cropping_window_capturer(true);

  // We prefer to allow the WGC and DXGI capturers to embed the cursor when
  // possible. The DXGI implementation uses this switch in combination with
  // internal checks for support of if it is possible to embed the cursor.
  // Note that, very few graphical adapters support embedding the cursor into
  // the captured frame in combination with DXGI; hence most cursors will be
  // added separately by a desktop and cursor composer even if this option is
  // set to true. GDI does not use this option.
  // TODO(crbug.com/1421656): Possibly remove this flag. Keeping for now to
  // force non embedded cursor for all capture APIs on Windows.
  static BASE_FEATURE(kAllowWinCursorEmbedded, "AllowWinCursorEmbedded",
                      base::FEATURE_ENABLED_BY_DEFAULT);
  if (base::FeatureList::IsEnabled(kAllowWinCursorEmbedded)) {
    options.set_prefer_cursor_embedded(true);
  }
  if (base::FeatureList::IsEnabled(features::kWebRtcAllowWgcDesktopCapturer)) {
    options.set_allow_wgc_capturer(true);

    // 0Hz support is by default disabled for WGC but it can be enabled using
    // the `kWebRtcAllowWgcZeroHz` feature flag. When enabled, the WGC capturer
    // will compare the pixel values of the new frame and the previous frame and
    // update the DesktopRegion part of the frame to reflect if the content has
    // changed or not. DesktopFrame::updated_region() will be empty if nothing
    // has changed and contain one (damage) region corresponding to the complete
    // screen or window being captured if any change is detected.
    options.set_allow_wgc_zero_hertz(
        base::FeatureList::IsEnabled(features::kWebRtcAllowWgcZeroHz));
  }
#endif

  // For browser tests, to create a fake desktop capturer.
  if (source.id == DesktopMediaID::kFakeId) {
    capturer = std::make_unique<webrtc::FakeDesktopCapturer>();
    result.reset(new DesktopCaptureDevice(std::move(capturer), source.type));
    return result;
  }

  switch (source.type) {
    case DesktopMediaID::TYPE_SCREEN: {
#if BUILDFLAG(IS_CHROMEOS_LACROS)
      // TODO(https://crbug.com/1094460): Handle options.
      std::unique_ptr<webrtc::DesktopCapturer> screen_capturer =
          std::make_unique<DesktopCapturerLacros>(
              DesktopCapturerLacros::CaptureType::kScreen,
              webrtc::DesktopCaptureOptions());
#else
      std::unique_ptr<webrtc::DesktopCapturer> screen_capturer(
          webrtc::DesktopCapturer::CreateScreenCapturer(options));
#endif
      if (screen_capturer && screen_capturer->SelectSource(source.id)) {
        capturer = std::make_unique<webrtc::DesktopAndCursorComposer>(
            std::move(screen_capturer), options);
        IncrementDesktopCaptureCounter(SCREEN_CAPTURER_CREATED);
        IncrementDesktopCaptureCounter(
            source.audio_share ? SCREEN_CAPTURER_CREATED_WITH_AUDIO
                               : SCREEN_CAPTURER_CREATED_WITHOUT_AUDIO);
      }
      break;
    }

    case DesktopMediaID::TYPE_WINDOW: {
#if BUILDFLAG(IS_CHROMEOS_LACROS)
      std::unique_ptr<webrtc::DesktopCapturer> window_capturer(
          new DesktopCapturerLacros(DesktopCapturerLacros::CaptureType::kWindow,
                                    webrtc::DesktopCaptureOptions()));
#else
      std::unique_ptr<webrtc::DesktopCapturer> window_capturer =
          webrtc::DesktopCapturer::CreateWindowCapturer(options);
#endif
      if (window_capturer && window_capturer->SelectSource(source.id)) {
        capturer = std::make_unique<webrtc::DesktopAndCursorComposer>(
            std::move(window_capturer), options);
        IncrementDesktopCaptureCounter(WINDOW_CAPTURER_CREATED);
      }
      break;
    }

    default: { NOTREACHED(); }
  }

  if (capturer)
    result.reset(new DesktopCaptureDevice(std::move(capturer), source.type));

  return result;
}

DesktopCaptureDevice::~DesktopCaptureDevice() {
  DCHECK(!core_);
}

void DesktopCaptureDevice::AllocateAndStart(
    const media::VideoCaptureParams& params,
    std::unique_ptr<Client> client) {
  thread_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&Core::AllocateAndStart, core_->GetWeakPtr(),
                                params, std::move(client)));
}

void DesktopCaptureDevice::StopAndDeAllocate() {
  if (core_) {
    // This thread should mostly be an idle observer. Stopping it should be
    // fast.
    base::ScopedAllowBaseSyncPrimitivesOutsideBlockingScope allow_thread_join;
    thread_.task_runner()->DeleteSoon(FROM_HERE, core_.release());
    thread_.Stop();
  }
}

void DesktopCaptureDevice::RequestRefreshFrame() {
  // Refresh request shall have no effect after the capturer has been stopped.
  if (!core_) {
    return;
  }
  thread_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&Core::RequestRefreshFrame, core_->GetWeakPtr()));
}

void DesktopCaptureDevice::SetNotificationWindowId(
    gfx::NativeViewId window_id) {
  // This may be called after the capturer has been stopped.
  if (!core_)
    return;
  thread_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&Core::SetNotificationWindowId,
                                core_->GetWeakPtr(), window_id));
}

DesktopCaptureDevice::DesktopCaptureDevice(
    std::unique_ptr<webrtc::DesktopCapturer> capturer,
    DesktopMediaID::Type type)
    : thread_("desktopCaptureThread") {
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  // On Windows/OSX the thread must be a UI thread.
  base::MessagePumpType thread_type = base::MessagePumpType::UI;
#else
  base::MessagePumpType thread_type = base::MessagePumpType::DEFAULT;
#endif

  bool zero_hertz_is_supported = true;
#if BUILDFLAG(IS_WIN)
  if (base::FeatureList::IsEnabled(features::kWebRtcAllowWgcDesktopCapturer)) {
    // TODO(https://crbug.com/1421242): Finalize 0Hz support for WGC.
    // This feature flag is disabled by default.
    zero_hertz_is_supported =
        base::FeatureList::IsEnabled(features::kWebRtcAllowWgcZeroHz);
  } else {
    // TODO(https://crbug.com/1421656): 0Hz mode seems to cause a flickering
    // cursor in some setups. This flag allows us to disable 0Hz when needed.
    zero_hertz_is_supported =
        base::FeatureList::IsEnabled(features::kWebRtcAllowDxgiGdiZeroHz);
  }
#endif

  thread_.StartWithOptions(base::Thread::Options(thread_type, 0));

  core_ = std::make_unique<Core>(thread_.task_runner(), std::move(capturer),
                                 type, zero_hertz_is_supported);
}

void DesktopCaptureDevice::SetMockTimeForTesting(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    const base::TickClock* tick_clock) {
  core_->SetMockTimeForTesting(task_runner, tick_clock);
}

}  // namespace content
