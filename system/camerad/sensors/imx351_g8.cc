#include <cstdio>
#include <cmath>
#include <algorithm>
#include <iterator>
#include <cstdlib>

#include "system/camerad/sensors/sensor.h"
#include "system/camerad/sensors/imx351_g8_registers.h"

#include <media/msm_camsensor_sdk.h>

IMX351G8::IMX351G8() {
  // LG G8 rear ultrawide: slot 2 / CSIPHY1.
  image_sensor = cereal::FrameData::ImageSensor::UNKNOWN;
  pixel_size_mm = 0.0010f;
  data_word = false;

  out_scale = 1;
  frame_width = 2328;
  frame_height = 1312;
  frame_stride = frame_width * 10 / 8;
  extra_height = 0;
  frame_offset = 0;
  registers_offset = -1;
  stats_offset = -1;
  hdr_offset = -1;

  start_reg_array.assign(std::begin(start_reg_array_imx351_g8),
                         std::end(start_reg_array_imx351_g8));
  stop_reg_array.assign(std::begin(stop_reg_array_imx351_g8),
                        std::end(stop_reg_array_imx351_g8));

  init_reg_array.assign(std::begin(init_array_imx351_g8),
                        std::end(init_array_imx351_g8));
  init_reg_array.insert(init_reg_array.end(),
                        std::begin(mode_2328x1312_30_imx351_g8),
                        std::end(mode_2328x1312_30_imx351_g8));

  probe_reg_addr = 0x0016;
  probe_expected_data = 0x0351;

  bits_per_pixel = 10;
  // Provisional for transport bring-up. Verify from first real frame.
  bayer_pattern = CAM_ISP_PATTERN_BAYER_BGBGBG;
  mipi_format = CAM_FORMAT_MIPI_RAW_10;
  frame_data_type = CSI_RAW10;
  mclk_frequency = 19200000;

  readout_time_ns = 30000000;
  black_level = 64;

  // G8_IMX351_MAIN_COLOR_V1
  // Safe A/B baseline remains the current IMX363 MAIN matrix by default.
  color_correct_matrix = {
    0x00000092, 0x00000FEF, 0x00000FFF,
    0x00000FF7, 0x000000B2, 0x00000FD7,
    0x00000000, 0x00000FBA, 0x000000C6,
  };

  // G8_IMX351_NATIVE_CCM_V1
  // Opt-in LG/QTI IMX351 CC13 daylight node (4650-7000 K), extracted from
  // com.qti.tuned.imx351.bin and converted to Qualcomm signed Q7.
  //
  // Float source:
  //   1.7199  -0.7524   0.0325
  //  -0.1651   1.3173  -0.1522
  //  -0.0140  -0.8976   1.9115
  //
  // Default remains the MAIN-parity baseline unless explicitly enabled.
  if (getenv("G8_IMX351_NATIVE_CCM") != nullptr) {
    color_correct_matrix = {
      0x000000DC, 0x00000FA0, 0x00000004,
      0x00000FEB, 0x000000A9, 0x00000FED,
      0x00000FFE, 0x00000F8D, 0x000000F5,
    };
    fprintf(stderr, "G8_IMX351_NATIVE_CCM_V1 enabled=1 node=4650-7000K\n");
    fflush(stderr);
  }

  linearization_pts.clear();
  linearization_lut.clear();
  vignetting_lut.clear();

  gamma_lut_rgb.clear();
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

  // G8_IMX351_NO_OS04_AE_V1
  // MAIN-parity controller baseline, using IMX351's Sony register family.
  // LG donor mode: frame length 0x0A78 (2680), coarse 0x0A64 (2660), gain 1x.
  ev_scale = 1.0f;
  exposure_time_min = 1;
  exposure_time_max = 2660;
  dc_gain_factor = 1.0f;
  dc_gain_min_weight = 0;
  dc_gain_max_weight = 1;
  dc_gain_on_grey = 0.0f;
  dc_gain_off_grey = 1.0f;

  // G8_IMX351_LG_NATIVE_GAIN_V1
  // Proven from LG's native com.qti.sensor.imx351.so:
  //   realToRegGain: reg = trunc(1024 - 1024 / real_gain), clamp real_gain 1..16
  //   regToRealGain: real = 1024 / (1024 - reg), clamp reg <= 0x03C0
  analog_gain_min_idx = 0;
  analog_gain_rec_idx = 0;
  analog_gain_max_idx = 8;
  sensor_analog_gains[0] = 1.0f;          // 0x0000
  sensor_analog_gains[1] = 1.142857143f;  // 0x0080
  sensor_analog_gains[2] = 1.333333333f;  // 0x0100
  sensor_analog_gains[3] = 1.6f;          // 0x0180
  sensor_analog_gains[4] = 2.0f;          // 0x0200
  sensor_analog_gains[5] = 2.666666667f;  // 0x0280
  sensor_analog_gains[6] = 4.0f;          // 0x0300
  sensor_analog_gains[7] = 8.0f;          // 0x0380
  sensor_analog_gains[8] = 16.0f;         // 0x03C0

  analog_gain_cost_delta = 0;
  analog_gain_cost_low = 0.4f;
  analog_gain_cost_high = 6.4f;

  min_ev = exposure_time_min * sensor_analog_gains[analog_gain_min_idx];
  max_ev = exposure_time_max * sensor_analog_gains[analog_gain_max_idx];
  target_grey_factor = 0.01f;
}

std::vector<i2c_random_wr_payload> IMX351G8::getExposureRegisters(
    int exposure_time, int new_exp_g, bool dc_gain_enabled) const {
  (void)dc_gain_enabled;

  const uint32_t coarse = static_cast<uint32_t>(
      std::clamp(exposure_time, exposure_time_min, exposure_time_max));
  const int gain_idx =
      std::clamp(new_exp_g, analog_gain_min_idx, analog_gain_max_idx);

  // G8_IMX351_SONY_AE_BASELINE_V1
  // G8_IMX351_LG_NATIVE_GAIN_V1: exact LG/QTI IMX351 analogue-gain encoding.
  // Native realToRegGain is trunc(1024 - 1024 / gain), with analogue gain
  // clamped to 1x..16x. 0x0204/0x0205 are the verified global gain registers.
  static constexpr uint32_t analog_gain_reg[] = {
    0x0000,  // 1.000000x
    0x0080,  // 1.142857x
    0x0100,  // 1.333333x
    0x0180,  // 1.600000x
    0x0200,  // 2.000000x
    0x0280,  // 2.666667x
    0x0300,  // 4.000000x
    0x0380,  // 8.000000x
    0x03C0,  // 16.000000x
  };

  const uint32_t gain_reg = analog_gain_reg[gain_idx];

  static uint32_t log_counter = 0;
  if ((log_counter++ % 30) == 0) {
    fprintf(stderr,
            "G8_IMX351_AE_WRITE_V1 coarse=%u gain_idx=%d gain=%.3f reg=0x%04X\n",
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

float IMX351G8::getExposureScore(
    float desired_ev, int exp_t, int exp_g_idx,
    float exp_gain, int gain_idx) const {
  float score = std::abs(desired_ev - (exp_t * exp_gain));

  const float gain_cost =
      exp_g_idx > analog_gain_rec_idx ?
      analog_gain_cost_high :
      analog_gain_cost_low;

  score += std::abs(exp_g_idx - analog_gain_rec_idx) * gain_cost;
  score += std::abs(exp_g_idx - gain_idx) * 3.0f;
  return score;
}

int IMX351G8::getSlaveAddress(int port) const {
  (void)port;
  return 0x34;
}
