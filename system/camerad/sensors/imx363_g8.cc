#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdio>

#include "system/camerad/sensors/sensor.h"
#include "system/camerad/sensors/imx363_g8_registers.h"

#include <media/msm_camsensor_sdk.h>

IMX363G8::IMX363G8() {
  // LG G8 ThinQ / alphalm rear normal camera, DTS slot 0.
  // LG CamX module data:
  //   Sony IMX363, I2C write address 0x20
  //   sensor ID register 0x0016, expected 0x0363
  //   2016x1136 @ 30 fps, RAW10, BGGR, 4 D-PHY lanes
  //   MCLK 19.2 MHz
  image_sensor = cereal::FrameData::ImageSensor::UNKNOWN;
  // G8_IMX363_STANDALONE_PROFILE_V3
  ev_scale = 1.0f;

  pixel_size_mm = 0.0014f;
  data_word = false;

  out_scale = 1;
  frame_width = 2016;
  frame_height = 1136;
  frame_stride = frame_width * 10 / 8;
  extra_height = 0;
  frame_offset = 0;
  registers_offset = -1;
  stats_offset = -1;
  hdr_offset = -1;

  start_reg_array.assign(std::begin(start_reg_array_imx363_g8),
                         std::end(start_reg_array_imx363_g8));
  stop_reg_array.assign(std::begin(stop_reg_array_imx363_g8),
                        std::end(stop_reg_array_imx363_g8));

  init_reg_array.assign(std::begin(init_array_imx363_g8),
                        std::end(init_array_imx363_g8));
  init_reg_array.insert(init_reg_array.end(),
                        std::begin(mode_2016x1136_30_imx363_g8),
                        std::end(mode_2016x1136_30_imx363_g8));

  probe_reg_addr = 0x0016;
  probe_expected_data = 0x0363;

  bits_per_pixel = 10;
  bayer_pattern = CAM_ISP_PATTERN_BAYER_BGBGBG;  // BGGR
  mipi_format = CAM_FORMAT_MIPI_RAW_10;
  frame_data_type = CSI_RAW10;
  mclk_frequency = 19200000;

  readout_time_ns = 22000000;

  // LG CamX color-level data: 10-bit white=1023, R/Gr/B/Gb pedestal=64.
  black_level = 64;

  // R1 freezes AE at the exposure from the LG mode table.
  // Keep CameraState initialization numerically safe.
  exposure_time_min = 1;
  exposure_time_max = 1684;
  dc_gain_factor = 1.0f;
  dc_gain_min_weight = 0;
  dc_gain_max_weight = 1;
  dc_gain_on_grey = 0.0f;
  dc_gain_off_grey = 1.0f;
  // G8_IMX363_REAL_AE_V1
  // Sony analog gain code follows:
  // gain = 1024 / (1024 - register_code)
  analog_gain_min_idx = 0;
  analog_gain_rec_idx = 0;
  analog_gain_max_idx = 7;

  sensor_analog_gains[0] = 1.0f;          // 0x0000
  sensor_analog_gains[1] = 1.142857143f;  // 0x0080
  sensor_analog_gains[2] = 1.333333333f;  // 0x0100
  sensor_analog_gains[3] = 1.6f;          // 0x0180
  sensor_analog_gains[4] = 2.0f;          // 0x0200
  sensor_analog_gains[5] = 2.666666667f;  // 0x0280
  sensor_analog_gains[6] = 4.0f;          // 0x0300
  sensor_analog_gains[7] = 8.0f;          // 0x0380

  // Prefer shutter time before increasing analog gain.
  analog_gain_cost_delta = 0;
  analog_gain_cost_low = 0.4f;
  analog_gain_cost_high = 6.4f;

  min_ev = exposure_time_min * sensor_analog_gains[analog_gain_min_idx];
  max_ev = exposure_time_max * sensor_analog_gains[analog_gain_max_idx];

  target_grey_factor = 0.01f;
  // G8_IMX363_ISP_BASELINE_V3 - no OS04 photometric profile
  // LG IMX363 CC13 IPE, 4600K-7000K, transplanted to IFE
  // Float matrix:
  //   1.6743  -0.6819   0.0077
  //  -0.2361   1.4427  -0.2065
  //  -0.0190  -0.9885   2.0074
  color_correct_matrix = {0x00000092, 0x00000FEF, 0x00000FFF, 0x00000FF7, 0x000000B2, 0x00000FD7, 0x00000000, 0x00000FBA, 0x000000C6};
  linearization_pts.clear();
  linearization_lut.clear();
  vignetting_lut.clear();
  // G8_IMX363_LG_GAMMA16_V1
  // Extracted from LG/QTI com.qti.tuned.imx363.bin
  gamma_lut_rgb = {
    0, 64, 108, 144, 176, 205, 232, 258,
    282, 304, 326, 347, 367, 386, 405, 423,
    441, 458, 475, 491, 507, 523, 538, 553,
    568, 583, 597, 611, 625, 638, 651, 665,
    678, 690, 703, 715, 728, 740, 752, 764,
    775, 787, 798, 810, 821, 832, 843, 854,
    865, 875, 886, 896, 907, 917, 927, 937,
    947, 957, 967, 977, 987, 996, 1006, 1015,
    1024
  };
  prepare_gamma_lut();
  // G8_IMX363_RUNTIME_PROFILE_V2
  fprintf(stderr,
          "G8_IMX363_RUNTIME_PROFILE_V2 frame=%ux%u black=%u ev_scale=%.3f gainidx=%d/%d/%d ",
          frame_width, frame_height, black_level, ev_scale,
          analog_gain_min_idx, analog_gain_rec_idx, analog_gain_max_idx);
  fprintf(stderr,
          "ccm_n=%zu ccm0=0x%08x gamma_n=%zu g0=0x%08x g32=0x%08x g63=0x%08x ",
          color_correct_matrix.size(), color_correct_matrix.empty() ? 0 : color_correct_matrix[0],
          gamma_lut_rgb.size(), gamma_lut_rgb[0], gamma_lut_rgb[32], gamma_lut_rgb[63]);
  fprintf(stderr,
          "lin_n=%zu lin0=0x%08x vig_n=%zu",
          linearization_lut.size(), linearization_lut.empty() ? 0 : linearization_lut[0],
          vignetting_lut.size());
  fputc(10, stderr);
  fflush(stderr);
}

std::vector<i2c_random_wr_payload> IMX363G8::getExposureRegisters(
    int exposure_time, int new_exp_g, bool dc_gain_enabled) const {
  (void)dc_gain_enabled;

  const uint32_t coarse = static_cast<uint32_t>(
      std::clamp(exposure_time, exposure_time_min, exposure_time_max));

  const int gain_idx =
      std::clamp(new_exp_g, analog_gain_min_idx, analog_gain_max_idx);

  // Sony IMX363 analogue gain codes.
  // LG/QTI IMX363 RealToRegisterGain:
  //   reg = floor(512 - 512 / real_gain)
  // Valid analog range is 1.0x .. 8.0x.
  static constexpr uint32_t analog_gain_reg[] = {
    0x0000,  // 1.000000x
    0x0040,  // 1.142857x
    0x0080,  // 1.333333x
    0x00C0,  // 1.600000x
    0x0100,  // 2.000000x
    0x0140,  // 2.666667x
    0x0180,  // 4.000000x
    0x01C0,  // 8.000000x
  };

  const uint32_t gain_reg = analog_gain_reg[gain_idx];

  static uint32_t log_counter = 0;
  if ((log_counter++ % 30) == 0) {
    fprintf(stderr,
            "G8_IMX363_AE_WRITE_V1 coarse=%u gain_idx=%d gain=%.3f reg=0x%04X\n",
            coarse, gain_idx, sensor_analog_gains[gain_idx], gain_reg);
    fflush(stderr);
  }

  return {
    {0x0202, coarse >> 8},
    {0x0203, coarse & 0xFF},
    {0x0204, gain_reg >> 8},
    {0x0205, gain_reg & 0xFF},
  };
}

float IMX363G8::getExposureScore(
    float desired_ev, int exp_t, int exp_g_idx,
    float exp_gain, int gain_idx) const {

  float score = std::abs(desired_ev - (exp_t * exp_gain));

  const float gain_cost =
      exp_g_idx > analog_gain_rec_idx ?
      analog_gain_cost_high :
      analog_gain_cost_low;

  score += std::abs(exp_g_idx - analog_gain_rec_idx) * gain_cost;

  // Avoid large gain-index jumps from one frame to the next.
  score += std::abs(exp_g_idx - gain_idx) * 3.0f;

  return score;
}

int IMX363G8::getSlaveAddress(int port) const {
  (void)port;
  return 0x20;
}
