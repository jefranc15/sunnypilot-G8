#include <cstdio>
#include "system/camerad/cameras/camera_common.h"
#include "system/camerad/cameras/spectra.h"

#include <poll.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "common/params.h"
#include "common/swaglog.h"


ExitHandler do_exit;

// for debugging
const bool env_debug_frames = getenv("DEBUG_FRAMES") != nullptr;
const bool env_log_raw_frames = getenv("LOG_RAW_FRAMES") != nullptr;
const bool env_ctrl_exp_from_params = getenv("CTRL_EXP_FROM_PARAMS") != nullptr;


class CameraState {
public:
  SpectraCamera camera;
  int exposure_time = 5;
  bool dc_gain_enabled = false;
  int dc_gain_weight = 0;
  int gain_idx = 0;
  float analog_gain_frac = 0;

  float cur_ev[3] = {};
  float best_ev_score = 0;
  int new_exp_g = 0;
  int new_exp_t = 0;

  Rect ae_xywh = {};
  float measured_grey_fraction = 0;
  float target_grey_fraction = 0.125;

  float fl_pix = 0;
  std::unique_ptr<PubMaster> pm;

  CameraState(SpectraMaster *master, const CameraConfig &config) : camera(master, config) {};
  ~CameraState();
  void init(VisionIpcServer *v);
  void update_exposure_score(float desired_ev, int exp_t, int exp_g_idx, float exp_gain);
  void set_camera_exposure(float grey_frac);
  void set_exposure_rect();
  void sendState();

  float get_gain_factor() const {
    return (1 + dc_gain_weight * (camera.sensor->dc_gain_factor-1) / camera.sensor->dc_gain_max_weight);
  }
};

void CameraState::init(VisionIpcServer *v) {
  camera.camera_open(v);

  if (!camera.enabled) return;

  fl_pix = camera.cc.focal_len / camera.sensor->pixel_size_mm / camera.sensor->out_scale;
  set_exposure_rect();

  dc_gain_weight = camera.sensor->dc_gain_min_weight;
  gain_idx = camera.sensor->analog_gain_rec_idx;
  analog_gain_frac = camera.sensor->sensor_analog_gains[gain_idx];

  // G8_IMX363_AE_SEED_V1
  // LG donor mode starts MAIN at 1684 coarse lines / 1x.
  // Seeding CameraState to the actual sensor state prevents AE from
  // believing the sensor started at openpilot's generic 5-line default.
  if (getenv("G8_AGNOS") != nullptr &&
      camera.cc.stream_type == VISION_STREAM_ROAD &&
      camera.sensor->frame_width == 2016 &&
      camera.sensor->frame_height == 1136) {
    exposure_time = camera.sensor->exposure_time_max;
    fprintf(stderr,
            "G8_IMX363_AE_SEED_V1 exposure=%d gain_idx=%d gain=%.3f\n",
            exposure_time, gain_idx, analog_gain_frac);
    fflush(stderr);
  }

  // G8_IMX351_WIDE_AE_V1
  // Explicit WIDE harness uses logical ROAD but physical LG IMX351.
  // Seed controller bookkeeping to the donor-programmed hardware state:
  // coarse=2660 lines, analog gain=1x. This block does not perform I2C.
  const bool g8_imx351_wide_ae_seed =
      getenv("G8_AGNOS") != nullptr &&
      getenv("G8_CAMERA_TARGET_WIDE") != nullptr &&
      camera.cc.stream_type == VISION_STREAM_ROAD &&
      camera.sensor->frame_width == 2328 &&
      camera.sensor->frame_height == 1312;
  if (g8_imx351_wide_ae_seed) {
    exposure_time = 2660;
    gain_idx = camera.sensor->analog_gain_min_idx;
    analog_gain_frac = camera.sensor->sensor_analog_gains[gain_idx];
    dc_gain_enabled = false;
    dc_gain_weight = camera.sensor->dc_gain_min_weight;
    fprintf(stderr,
            "G8_IMX351_WIDE_AE_V1 SEED exposure=%d gain_idx=%d gain=%.6f sensor_write=0\n",
            exposure_time, gain_idx, analog_gain_frac);
    fflush(stderr);
  }

  // G8_IMX520_AUTO_AE_V1
  // The LG donor mode starts IMX520 at 2572 integration lines and 1x gain.
  // CameraState normally starts exposure_time at 5, so seed the controller
  // to the real hardware state before enabling the bounded front-only AE test.
  const bool g8_imx520_auto_ae =
      getenv("G8_AGNOS") != nullptr &&
      camera.cc.stream_type == VISION_STREAM_DRIVER;

  // G8_IMX520_DRIVER_METADATA_V1
  // The physical LG IMX520 donor mode starts at 2572 integration lines,
  // analog gain index 0 (1x). Seed CameraState to that real hardware state
  // for the logical DRIVER stream even when AE remains frozen. This changes
  // metadata/controller bookkeeping only; it performs no sensor I2C write.
  const bool g8_imx520_driver_metadata =
      getenv("G8_AGNOS") != nullptr &&
      camera.cc.stream_type == VISION_STREAM_DRIVER;

  if (g8_imx520_auto_ae || g8_imx520_driver_metadata) {
    exposure_time = 2572;

    // G8_COLOR_CAL_V1
    // Production DRIVER calibration: retain maximum donor coarse integration
    // and move only one proven analog-gain step, 1.0x -> 1.142857x (0x0080).
    // The bounded AUTO_AE test keeps its original 1x seed.
    const bool g8_imx520_prod_cal =
        g8_imx520_driver_metadata && !g8_imx520_auto_ae;
    // G8_DRIVER_PREVIEW_CAL_V2
    // Stage 9E at 1.142857x remained dark with max output Y=199. Test the next
    // already-proven Sony gain point: index 2 = 1.333333x / register 0x0100.
    gain_idx = g8_imx520_prod_cal ?
        std::min(camera.sensor->analog_gain_min_idx + 2,
                 camera.sensor->analog_gain_max_idx) :
        camera.sensor->analog_gain_min_idx;

    analog_gain_frac = camera.sensor->sensor_analog_gains[gain_idx];
    dc_gain_enabled = false;
    dc_gain_weight = camera.sensor->dc_gain_min_weight;

    if (g8_imx520_prod_cal) {
      auto g8_cal_regs =
          camera.sensor->getExposureRegisters(exposure_time, gain_idx, false);
      camera.sensors_i2c(g8_cal_regs.data(), g8_cal_regs.size(),
                         CAM_SENSOR_PACKET_OPCODE_SENSOR_CONFIG,
                         camera.sensor->data_word);
      fprintf(stderr,
              "G8_COLOR_CAL_V1 IMX520_WRITE logical_camera=%d exposure=%d gain_idx=%d gain=%.6f regs=%zu\n",
              camera.cc.camera_num, exposure_time, gain_idx,
              analog_gain_frac, g8_cal_regs.size());
      fflush(stderr);
    }

    if (g8_imx520_auto_ae) {
      fprintf(stderr,
              "G8_IMX520_AUTO_AE_V1 SEED exposure=%d gain_idx=%d gain=%.6f\n",
              exposure_time, gain_idx, analog_gain_frac);
      fflush(stderr);
    }
    if (g8_imx520_driver_metadata) {
      fprintf(stderr,
              "G8_IMX520_DRIVER_METADATA_V1 SEED logical_camera=%d exposure=%d gain_idx=%d gain=%.6f sensor_write=%d\n",
              camera.cc.camera_num, exposure_time, gain_idx, analog_gain_frac,
              g8_imx520_prod_cal ? 1 : 0);
      fflush(stderr);
    }
  }

  cur_ev[0] = cur_ev[1] = cur_ev[2] = get_gain_factor() * camera.sensor->sensor_analog_gains[gain_idx] * exposure_time;

  pm = std::make_unique<PubMaster>(std::vector{camera.cc.publish_name});
}

CameraState::~CameraState() {}

void CameraState::set_exposure_rect() {
  // G8_IMX351_WIDE_AE_V1
  // The explicit WIDE harness carries ROAD CameraConfig geometry while the
  // physical output is 2328x1312 IMX351. Do not project ROAD intrinsics into
  // that buffer. Use a conservative centered 50% rectangle, guaranteed inside
  // the actual VisionIPC output, then let the normal exposure estimator run.
  if (getenv("G8_AGNOS") != nullptr &&
      getenv("G8_CAMERA_TARGET_WIDE") != nullptr &&
      camera.sensor->frame_width == 2328 &&
      camera.sensor->frame_height == 1312) {
    const int w = (int)camera.buf.out_img_width;
    const int h = (int)camera.buf.out_img_height;
    ae_xywh = (Rect){w / 4, h / 4, w / 2, h / 2};
    fprintf(stderr,
            "G8_IMX351_WIDE_AE_V1 ROI x=%d y=%d w=%d h=%d frame=%dx%d\n",
            ae_xywh.x, ae_xywh.y, ae_xywh.w, ae_xywh.h, w, h);
    fflush(stderr);
    return;
  }
  // set areas for each camera, shouldn't be changed
  std::vector<std::pair<Rect, float>> ae_targets = {
    // (Rect, F)
    std::make_pair((Rect){96, 400, 1734, 524}, 567.0),  // wide
    std::make_pair((Rect){96, 160, 1734, 986}, 2648.0), // road
    std::make_pair((Rect){96, 242, 1736, 906}, 567.0)   // driver
  };
  int h_ref = 1208;
  /*
    exposure target intrinsics is
    [
      [F, 0, 0.5*ae_xywh[2]]
      [0, F, 0.5*H-ae_xywh[1]]
      [0, 0, 1]
    ]
  */
  auto ae_target = ae_targets[camera.cc.camera_num];
  Rect xywh_ref = ae_target.first;
  float fl_ref = ae_target.second;

  ae_xywh = (Rect){
    std::max(0, (int)camera.buf.out_img_width / 2 - (int)(fl_pix / fl_ref * xywh_ref.w / 2)),
    std::max(0, (int)camera.buf.out_img_height / 2 - (int)(fl_pix / fl_ref * (h_ref / 2 - xywh_ref.y))),
    std::min((int)(fl_pix / fl_ref * xywh_ref.w), (int)camera.buf.out_img_width / 2 + (int)(fl_pix / fl_ref * xywh_ref.w / 2)),
    std::min((int)(fl_pix / fl_ref * xywh_ref.h), (int)camera.buf.out_img_height / 2 + (int)(fl_pix / fl_ref * (h_ref / 2 - xywh_ref.y)))
  };
}

void CameraState::update_exposure_score(float desired_ev, int exp_t, int exp_g_idx, float exp_gain) {
  float score = camera.sensor->getExposureScore(desired_ev, exp_t, exp_g_idx, exp_gain, gain_idx);
  if (score < best_ev_score) {
    new_exp_t = exp_t;
    new_exp_g = exp_g_idx;
    best_ev_score = score;
  }
}

void CameraState::set_camera_exposure(float grey_frac) {
  // G8 R1: keep the LG sensor at the exposure programmed by its CamX mode table.
  // Enable AE after first-frame bring-up is proven.
  const bool g8_imx520_auto_ae =
      getenv("G8_AGNOS") != nullptr &&
      camera.cc.stream_type == VISION_STREAM_DRIVER;

  // G8_IMX363_ROAD_AE_V2
  // Allow normal camerad AE to control the real Sony IMX363 MAIN sensor.
  const bool g8_imx363_auto_ae =
      getenv("G8_AGNOS") != nullptr &&
      camera.cc.stream_type == VISION_STREAM_ROAD &&
      camera.sensor->frame_width == 2016 &&
      camera.sensor->frame_height == 1136;

  // G8_IMX351_WIDE_AE_V1
  // Same controller as MAIN; only sensor limits/register conversion differ.
  const bool g8_imx351_auto_ae =
      getenv("G8_AGNOS") != nullptr &&
      getenv("G8_CAMERA_TARGET_WIDE") != nullptr &&
      camera.cc.stream_type == VISION_STREAM_ROAD &&
      camera.sensor->frame_width == 2328 &&
      camera.sensor->frame_height == 1312;

  // Keep other G8 camera paths frozen unless explicitly enabled.
  if (getenv("G8_AGNOS") != nullptr &&
      !g8_imx520_auto_ae &&
      !g8_imx363_auto_ae &&
      !g8_imx351_auto_ae) return;
  if (!camera.enabled) return;

  // Let the first few donor-programmed frames settle before issuing dynamic
  // exposure writes. The controller was seeded to that same hardware state.
  if (g8_imx520_auto_ae && camera.buf.cur_frame_data.frame_id < 6) return;
  if (g8_imx351_auto_ae && camera.buf.cur_frame_data.frame_id < 6) return;
  std::vector<double> target_grey_minimums = {0.1, 0.1, 0.125}; // wide, road, driver

  const float dt = 0.05;

  const float ts_grey = 10.0;
  const float ts_ev = 0.05;

  const float k_grey = (dt / ts_grey) / (1.0 + dt / ts_grey);
  const float k_ev = (dt / ts_ev) / (1.0 + dt / ts_ev);

  // It takes 3 frames for the commanded exposure settings to take effect. The first frame is already started by the time
  // we reach this function, the other 2 are due to the register buffering in the sensor.
  // Therefore we use the target EV from 3 frames ago, the grey fraction that was just measured was the result of that control action.
  // TODO: Lower latency to 2 frames, by using the histogram outputted by the sensor we can do AE before the debayering is complete

  const auto &sensor = camera.sensor;
  // Offset idx by one to not get stuck in self loop
  const float cur_ev_ = cur_ev[(camera.buf.cur_frame_data.frame_id - 1) % 3] * sensor->ev_scale;

  // Scale target grey between min and 0.4 depending on lighting conditions
  float new_target_grey = std::clamp(0.4 - 0.3 * log2(1.0 + sensor->target_grey_factor*cur_ev_) / log2(6000.0), target_grey_minimums[camera.cc.camera_num], 0.4);
  float target_grey = (1.0 - k_grey) * target_grey_fraction + k_grey * new_target_grey;
  if (g8_imx520_auto_ae || g8_imx363_auto_ae || g8_imx351_auto_ae) {
    target_grey = std::max(target_grey, 0.20f);
  }

  float desired_ev = std::clamp(cur_ev_ / sensor->ev_scale * target_grey / grey_frac, sensor->min_ev, sensor->max_ev);
  float k = (1.0 - k_ev) / 3.0;
  desired_ev = (k * cur_ev[0]) + (k * cur_ev[1]) + (k * cur_ev[2]) + (k_ev * desired_ev);

  best_ev_score = 1e6;
  new_exp_g = 0;
  new_exp_t = 0;

  // Hysteresis around high conversion gain
  // We usually want this on since it results in lower noise, but turn off in very bright day scenes
  bool enable_dc_gain = dc_gain_enabled;
  if (!enable_dc_gain && target_grey < sensor->dc_gain_on_grey) {
    enable_dc_gain = true;
    dc_gain_weight = sensor->dc_gain_min_weight;
  } else if (enable_dc_gain && target_grey > sensor->dc_gain_off_grey) {
    enable_dc_gain = false;
    dc_gain_weight = sensor->dc_gain_max_weight;
  }

  if (enable_dc_gain && dc_gain_weight < sensor->dc_gain_max_weight) {dc_gain_weight += 1;}
  if (!enable_dc_gain && dc_gain_weight > sensor->dc_gain_min_weight) {dc_gain_weight -= 1;}

  if (g8_imx520_auto_ae || g8_imx363_auto_ae || g8_imx351_auto_ae) {
    enable_dc_gain = false;
    dc_gain_weight = sensor->dc_gain_min_weight;
  }

  std::string gain_bytes, time_bytes;
  if (env_ctrl_exp_from_params) {
    static Params params;
    gain_bytes = params.get("CameraDebugExpGain");
    time_bytes = params.get("CameraDebugExpTime");
  }

  if (gain_bytes.size() > 0 && time_bytes.size() > 0) {
    // Override gain and exposure time
    gain_idx = std::stoi(gain_bytes);
    exposure_time = std::stoi(time_bytes);

    new_exp_g = gain_idx;
    new_exp_t = exposure_time;
    enable_dc_gain = false;
  } else {
    // Simple brute force optimizer to choose sensor parameters to reach desired EV
    int min_g = std::max(gain_idx - 1, sensor->analog_gain_min_idx);
    int max_g = std::min(gain_idx + 1, sensor->analog_gain_max_idx);
    for (int g = min_g; g <= max_g; g++) {
      float gain = sensor->sensor_analog_gains[g] * get_gain_factor();

      // Compute optimal time for given gain
      int t = std::clamp(int(std::round(desired_ev / gain)), sensor->exposure_time_min, sensor->exposure_time_max);

      // Only go below recommended gain when absolutely necessary to not overexpose
      if (g < sensor->analog_gain_rec_idx && t > 20 && g < gain_idx) {
        continue;
      }

      update_exposure_score(desired_ev, t, g, gain);
    }
  }

  measured_grey_fraction = grey_frac;
  target_grey_fraction = target_grey;

  analog_gain_frac = sensor->sensor_analog_gains[new_exp_g];
  gain_idx = new_exp_g;
  exposure_time = new_exp_t;
  dc_gain_enabled = enable_dc_gain;

  float gain = analog_gain_frac * get_gain_factor();
  cur_ev[camera.buf.cur_frame_data.frame_id % 3] = exposure_time * gain;

  // LOGE("ae - camera %d, cur_t %.5f, sof %.5f, dt %.5f", camera.cc.camera_num, 1e-9 * nanos_since_boot(), 1e-9 * camera.buf.cur_frame_data.timestamp_sof, 1e-9 * (nanos_since_boot() - camera.buf.cur_frame_data.timestamp_sof));

  auto exp_reg_array = sensor->getExposureRegisters(exposure_time, new_exp_g, dc_gain_enabled);
  if (g8_imx520_auto_ae) {
    fprintf(stderr,
            "G8_IMX520_AUTO_AE_V1 STEP frame=%u measured=%.6f target=%.6f desired_ev=%.3f exposure=%d gain_idx=%d gain=%.6f regs=%zu\n",
            camera.buf.cur_frame_data.frame_id,
            grey_frac, target_grey, desired_ev,
            exposure_time, new_exp_g, analog_gain_frac, exp_reg_array.size());
    fflush(stderr);
  }

  if (g8_imx363_auto_ae &&
      (camera.buf.cur_frame_data.frame_id % 30) == 0) {
    fprintf(stderr,
            "G8_IMX363_ROAD_AE_V2 STEP frame=%u measured=%.6f target=%.6f desired_ev=%.3f exposure=%d gain_idx=%d gain=%.6f regs=%zu\n",
            camera.buf.cur_frame_data.frame_id,
            grey_frac, target_grey, desired_ev,
            exposure_time, new_exp_g, analog_gain_frac,
            exp_reg_array.size());
    fflush(stderr);
  }
  if (g8_imx351_auto_ae &&
      (camera.buf.cur_frame_data.frame_id % 30) == 0) {
    fprintf(stderr,
            "G8_IMX351_WIDE_AE_V1 STEP frame=%u measured=%.6f target=%.6f desired_ev=%.3f exposure=%d gain_idx=%d gain=%.6f regs=%zu\n",
            camera.buf.cur_frame_data.frame_id,
            grey_frac, target_grey, desired_ev,
            exposure_time, new_exp_g, analog_gain_frac,
            exp_reg_array.size());
    fflush(stderr);
  }
  camera.sensors_i2c(exp_reg_array.data(), exp_reg_array.size(), CAM_SENSOR_PACKET_OPCODE_SENSOR_CONFIG, camera.sensor->data_word);
}

void CameraState::sendState() {
  camera.buf.sendFrameToVipc();
  if (getenv("G8_CAMERA_VIPC_TEST") != nullptr) {
    static int g8_vipc_publish_count = 0;
    g8_vipc_publish_count++;

    const FrameMetadata &g8_meta = camera.buf.cur_frame_data;
    fprintf(stderr,
      "G8_VIPC_PUBLISH count=%d frame=%u req=%u buf_idx=%d sof=%llu eof=%llu\n",
      g8_vipc_publish_count,
      g8_meta.frame_id,
      g8_meta.request_id,
      camera.buf.cur_buf_idx,
      (unsigned long long)g8_meta.timestamp_sof,
      (unsigned long long)g8_meta.timestamp_eof);
    fflush(stderr);

    // G8_IMX520_EXP_HALF_V1
    // Controlled dynamic-exposure proof only. LG mode 1640x924 starts at
    // 0x0202/0x0203 = 0x0A0C (2572 lines). At publish #30, write exactly
    // half that integration: 0x0506 (1286 lines). Gain registers stay untouched.
    if (g8_vipc_publish_count == 30 &&
        getenv("G8_IMX520_EXPOSURE_TEST") != nullptr &&
        getenv("G8_CAMERA_TARGET_DRIVER") != nullptr) {
      const struct i2c_random_wr_payload g8_imx520_exp_half[] = {
        {0x0202, 0x05},
        {0x0203, 0x06},
      };
      fprintf(stderr,
              "G8_IMX520_EXP_HALF_V1 WRITE publish=%d lines=1286 reg0202=0x05 reg0203=0x06 writes=2\n",
              g8_vipc_publish_count);
      fflush(stderr);
      camera.sensors_i2c(g8_imx520_exp_half, 2,
                         CAM_SENSOR_PACKET_OPCODE_SENSOR_CONFIG,
                         camera.sensor->data_word);
      fprintf(stderr, "G8_IMX520_EXP_HALF_V1 APPLIED lines=1286\n");
      fflush(stderr);
    }

    // G8_IMX520_EXP_RESTORE_V2
    // Stage 6B: restore the exact LG donor mode integration at publish #60.
    if (g8_vipc_publish_count == 60 &&
        getenv("G8_IMX520_EXPOSURE_TEST") != nullptr &&
        getenv("G8_CAMERA_TARGET_DRIVER") != nullptr) {
      const struct i2c_random_wr_payload g8_imx520_exp_restore[] = {
        {0x0202, 0x0A},
        {0x0203, 0x0C},
      };
      fprintf(stderr,
              "G8_IMX520_EXP_RESTORE_V2 WRITE publish=%d lines=2572 reg0202=0x0A reg0203=0x0C writes=2\n",
              g8_vipc_publish_count);
      fflush(stderr);
      camera.sensors_i2c(g8_imx520_exp_restore, 2,
                         CAM_SENSOR_PACKET_OPCODE_SENSOR_CONFIG,
                         camera.sensor->data_word);
      fprintf(stderr, "G8_IMX520_EXP_RESTORE_V2 APPLIED lines=2572\n");
      fflush(stderr);
    }

    // G8_IMX520_GAIN_LADDER_V2
    // Bounded full-range analog gain validation through the real IMX520 API.
    // Keep coarse integration fixed at 1286 lines to reduce highlight clipping.
    // Sequence: 1x -> 2x -> 4x -> 8x -> 1x.
    if (getenv("G8_IMX520_GAIN_LADDER_TEST") != nullptr &&
        getenv("G8_CAMERA_TARGET_DRIVER") != nullptr) {
      int ladder_idx = -1;
      if (g8_vipc_publish_count == 15) ladder_idx = 0;
      if (g8_vipc_publish_count == 35) ladder_idx = 4;
      if (g8_vipc_publish_count == 55) ladder_idx = 6;
      if (g8_vipc_publish_count == 75) ladder_idx = 7;
      if (g8_vipc_publish_count == 95) ladder_idx = 0;

      if (ladder_idx >= 0) {
        const float ladder_gain = camera.sensor->sensor_analog_gains[ladder_idx];
        auto ladder_regs = camera.sensor->getExposureRegisters(1286, ladder_idx, false);
        fprintf(stderr,
                "G8_IMX520_GAIN_LADDER_V2 WRITE publish=%d exposure=1286 gain_idx=%d gain=%.6f regs=%zu\n",
                g8_vipc_publish_count, ladder_idx, ladder_gain, ladder_regs.size());
        fflush(stderr);
        camera.sensors_i2c(ladder_regs.data(), ladder_regs.size(),
                           CAM_SENSOR_PACKET_OPCODE_SENSOR_CONFIG,
                           camera.sensor->data_word);
        fprintf(stderr,
                "G8_IMX520_GAIN_LADDER_V2 APPLIED publish=%d gain_idx=%d gain=%.6f\n",
                g8_vipc_publish_count, ladder_idx, ladder_gain);
        fflush(stderr);
      }
    }

    // Controlled continuous-stream test. Exit through the normal
    // camerad loop/destructors after stable queue recycling.
    // G8_IMX520_AE_DYNAMIC_V1
    // Preserve the proven 120-frame bound for every normal VIPC test.
    // Only the explicit IMX520 dynamic-AE gate gets a longer 240-frame run.
    const bool g8_imx520_ae_dynamic =
        getenv("G8_AGNOS") != nullptr &&
        getenv("G8_CAMERA_TARGET_DRIVER") != nullptr &&
        getenv("G8_IMX520_AUTO_AE_TEST") != nullptr &&
        getenv("G8_IMX520_AE_DYNAMIC_TEST") != nullptr;
    // G8_IMX520_DMON_BOUND_V1
    // Give the explicit DRIVER-model consumer test enough bounded time for
    // QCOM model loading/inference. Normal VIPC tests remain 120 frames;
    // dynamic AE remains 240. This gate does not enable AE.
    const bool g8_imx520_dmon_test =
        getenv("G8_AGNOS") != nullptr &&
        getenv("G8_CAMERA_TARGET_DRIVER") != nullptr &&
        getenv("G8_IMX520_DRIVER_STREAM_TEST") != nullptr &&
        getenv("G8_IMX520_DMON_TEST") != nullptr;
    const int g8_vipc_publish_limit = g8_imx520_dmon_test ? 600 :
        (g8_imx520_ae_dynamic ? 240 : 120);
    if (g8_vipc_publish_count >= g8_vipc_publish_limit) {
      fprintf(stderr, "G8_VIPC_TEST_DONE publishes=%d limit=%d dynamic_ae=%d dmon=%d\n",
              g8_vipc_publish_count, g8_vipc_publish_limit,
              g8_imx520_ae_dynamic ? 1 : 0, g8_imx520_dmon_test ? 1 : 0);
      fflush(stderr);
      do_exit = true;
    }
  }

  MessageBuilder msg;
  auto framed = (msg.initEvent().*camera.cc.init_camera_state)();
  const FrameMetadata &meta = camera.buf.cur_frame_data;
  framed.setFrameId(meta.frame_id);
  framed.setRequestId(meta.request_id);
  framed.setTimestampEof(meta.timestamp_eof);
  framed.setTimestampSof(meta.timestamp_sof);
  framed.setIntegLines(exposure_time);
  framed.setGain(analog_gain_frac * get_gain_factor());
  framed.setHighConversionGain(dc_gain_enabled);
  framed.setMeasuredGreyFraction(measured_grey_fraction);
  framed.setTargetGreyFraction(target_grey_fraction);
  framed.setProcessingTime(meta.processing_time);

  const float ev = cur_ev[meta.frame_id % 3];
  const float perc = util::map_val(ev, camera.sensor->min_ev, camera.sensor->max_ev, 0.0f, 100.0f);
  framed.setExposureValPercent(perc);
  framed.setSensor(camera.sensor->image_sensor);

  // Log raw frames for road camera
  if (env_log_raw_frames && camera.cc.stream_type == VISION_STREAM_ROAD && meta.frame_id % 100 == 5) {  // no overlap with qlog decimation
    framed.setImage(get_raw_frame_image(&camera.buf));
  }

  // G8_IMX351_WIDE_AE_V1
  // set_exposure_rect() supplies a physical-IMX351-safe rectangle for the
  // explicit WIDE harness. Feed the normal measured-grey estimator into the
  // same controller used by MAIN. Non-WIDE paths are unchanged.
  set_camera_exposure(calculate_exposure_value(
      &camera.buf, ae_xywh, 2,
      camera.cc.stream_type != VISION_STREAM_DRIVER ? 2 : 4));

  // Send the message
  pm->send(camera.cc.publish_name, msg);
}

void camerad_thread() {
  // TODO: centralize enabled handling

  VisionIpcServer v("camerad");

  // *** initial ISP init ***
  SpectraMaster m;
  m.init();

  // *** per-cam init ***
  std::vector<std::unique_ptr<CameraState>> cams;
  for (const auto &config : ALL_CAMERA_CONFIGS) {
    // G8_IMX520_DRIVER_STREAM_V1
    // Explicit Stage-7 mode remains DRIVER-only.
    const bool g8_driver_stream_test =
        getenv("G8_AGNOS") != nullptr &&
        getenv("G8_CAMERA_TARGET_DRIVER") != nullptr &&
        getenv("G8_IMX520_DRIVER_STREAM_TEST") != nullptr;
    if (getenv("G8_AGNOS") != nullptr) {
      const bool g8_explicit_target =
          getenv("G8_CAMERA_TARGET_DRIVER") != nullptr ||
          getenv("G8_CAMERA_TARGET_WIDE") != nullptr;
      if (g8_driver_stream_test) {
        if (config.stream_type != VISION_STREAM_DRIVER) continue;
      } else if (g8_explicit_target) {
        // Preserve all earlier ROAD-harness target tests.
        if (config.stream_type != VISION_STREAM_ROAD) continue;
      } else {
        // G8_DUAL_ROAD_DRIVER_V2: production manager launch uses ROAD + DRIVER.
        // WIDE remains intentionally deferred to the next stage.
        if (config.stream_type == VISION_STREAM_WIDE_ROAD) continue;
      }
    }
    if (g8_driver_stream_test) {
      fprintf(stderr,
              "G8_IMX520_DRIVER_STREAM_V1 SELECT logical_camera=%d stream=%d phy=%u output=%d\n",
              config.camera_num, (int)config.stream_type, config.phy, (int)config.output_type);
      fflush(stderr);
    }
    auto cam = std::make_unique<CameraState>(&m, config);
    cam->init(&v);
    cams.emplace_back(std::move(cam));
  }

  // G8_IMX520_PROBE_V1 thread guard
  if (getenv("G8_CAMERA_PROBE_ONLY") != nullptr) {
    fprintf(stderr, "G8_PROBE_ONLY_THREAD_DONE cams=%zu\n", cams.size());
    fflush(stderr);
    return;
  }

  // G8 IMX520 Stage 2 thread guard
  if (getenv("G8_CAMERA_CONFIG_ONLY") != nullptr) {
    fprintf(stderr, "G8_CONFIG_ONLY_THREAD_DONE cams=%zu\n", cams.size());
    fflush(stderr);
    return;
  }

  // G8 IMX520 Stage 3 thread guard
  if (getenv("G8_CAMERA_PHY_ONLY") != nullptr) {
    fprintf(stderr, "G8_PHY_ONLY_THREAD_DONE cams=%zu\n", cams.size());
    fflush(stderr);
    return;
  }

  // G8 IMX520 Stage 4 thread guard
  if (getenv("G8_CAMERA_START_ONLY") != nullptr) {
    fprintf(stderr, "G8_START_ONLY_THREAD_DONE cams=%zu\n", cams.size());
    fflush(stderr);
    return;
  }

  // G8_IMX351_QUEUE_ONLY_V3
  // camera_open() has completed VisionIPC allocation, camera buffer mapping,
  // and initial IFE request scheduling. Stop before listener/sensor stream-on.
  if (getenv("G8_CAMERA_QUEUE_ONLY") != nullptr) {
    fprintf(stderr, "G8_QUEUE_ONLY_DONE cams=%zu\n", cams.size());
    fflush(stderr);
    return;
  }

  v.start_listener();

  // start devices
  LOG("-- Starting devices");
  for (auto &cam : cams) cam->camera.sensors_start();

  // G8_IMX520_STREAMON_V1
  // Stage 5A: prove sensor STREAMON/CAM_START_DEV, then return before
  // entering the request-manager SOF polling loop. CameraState/SpectraCamera
  // destructors perform sensors_stop() and the normal camera teardown.
  if (getenv("G8_CAMERA_STREAMON_ONLY") != nullptr) {
    const char *target =
      getenv("G8_CAMERA_TARGET_DRIVER") != nullptr ? "IMX520" :
      (getenv("G8_CAMERA_TARGET_WIDE") != nullptr ? "IMX351" : "IMX363");

    for (auto &cam : cams) {
      fprintf(stderr,
              "G8_IMX520_STREAMON_V1 START_DONE sensor=%d target=%s enabled=%d sensor_started=%d\n",
              cam->camera.cc.camera_num,
              target,
              cam->camera.enabled ? 1 : 0,
              cam->camera.sensor_started ? 1 : 0);
    }
    fflush(stderr);
    return;
  }

  const bool g8_first_sof = getenv("G8_CAMERA_FIRST_SOF") != nullptr;
  if (g8_first_sof) {
    for (auto &cam : cams) {
      if (!cam->camera.enabled || !cam->camera.sensor_started) {
        fprintf(stderr, "G8_SENSOR_START_FAILED enabled=%d started=%d\n",
                cam->camera.enabled ? 1 : 0, cam->camera.sensor_started ? 1 : 0);
        fflush(stderr);
        return;
      }
    }
    fprintf(stderr, "G8_WAITING_FOR_SOF cams=%zu\n", cams.size());
    fflush(stderr);
  }

  int g8_sof_poll_timeouts = 0;

  // poll events
  LOG("-- Dequeueing Video events");
  while (!do_exit) {
    struct pollfd fds[1] = {{.fd = m.video0_fd, .events = POLLPRI}};
    int ret = poll(fds, std::size(fds), 1000);
    if (ret < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      LOGE("poll failed (%d - %d)", ret, errno);
      break;
    }

    if (ret == 0 && g8_first_sof) {
      g8_sof_poll_timeouts++;
      fprintf(stderr, "G8_SOF_POLL_TIMEOUT count=%d\n", g8_sof_poll_timeouts);
      fflush(stderr);
      if (g8_sof_poll_timeouts >= 3) {
        fprintf(stderr, "G8_NO_SOF_TIMEOUT\n");
        fflush(stderr);
        return;
      }
      continue;
    }

    if (!(fds[0].revents & POLLPRI)) continue;

    struct v4l2_event ev = {0};
    ret = HANDLE_EINTR(ioctl(fds[0].fd, VIDIOC_DQEVENT, &ev));
    if (ret == 0) {
      if (ev.type == V4L_EVENT_CAM_REQ_MGR_EVENT) {
        struct cam_req_mgr_message *event_data = (struct cam_req_mgr_message *)ev.u.data;

        // Stage 5 milestone: first request-manager SOF. Return immediately;
        // destructors call sensors_stop() and the normal camera teardown.
        if (g8_first_sof && getenv("G8_CAMERA_FIRST_IFE") == nullptr) {
          fprintf(stderr,
                  "G8_FIRST_SOF session=0x%X link=0x%X frame=%llu req=%llu timestamp_ns=%llu sof_status=%d\n",
                  event_data->session_hdl,
                  event_data->u.frame_msg.link_hdl,
                  (unsigned long long)event_data->u.frame_msg.frame_id,
                  (unsigned long long)event_data->u.frame_msg.request_id,
                  (unsigned long long)event_data->u.frame_msg.timestamp,
                  event_data->u.frame_msg.sof_status);
          fflush(stderr);
          return;
        }
        if (env_debug_frames) {
          printf("sess_hdl 0x%6X, link_hdl 0x%6X, frame_id %lu, req_id %lu, timestamp %.2f ms, sof_status %d\n", event_data->session_hdl, event_data->u.frame_msg.link_hdl,
                 event_data->u.frame_msg.frame_id, event_data->u.frame_msg.request_id, event_data->u.frame_msg.timestamp/1e6, event_data->u.frame_msg.sof_status);
          do_exit = do_exit || event_data->u.frame_msg.frame_id > (1*20);
        }

        for (auto &cam : cams) {
          if (event_data->session_hdl == cam->camera.session_handle) {
            bool publish_frame = cam->camera.handle_camera_event(event_data);

            if (getenv("G8_CAMERA_FIRST_IFE") != nullptr && cam->camera.g8_first_ife_complete) {
              fprintf(stderr, "G8_FIRST_IFE_TEST_DONE sensor=%d\n", cam->camera.cc.camera_num);
              fflush(stderr);
              return;
            }

            if (publish_frame) {
              cam->sendState();
            }
            break;
          }
        }
      } else {
        LOGE("unhandled event %d\n", ev.type);
      }
    } else {
      LOGE("VIDIOC_DQEVENT failed, errno=%d", errno);
    }
  }
}
