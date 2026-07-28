#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "system/camerad/sensors/sensor.h"
#include "system/camerad/sensors/imx520_g8_registers.h"

#include <media/msm_camsensor_sdk.h>

IMX520G8::IMX520G8() : OS04C10() {
  // LG G8 front RGB: slot 1 / CSIPHY2.
  image_sensor = cereal::FrameData::ImageSensor::UNKNOWN;
  pixel_size_mm = 0.00122f;
  data_word = false;

  out_scale = 1;
  frame_width = 1640;
  frame_height = 924;
  frame_stride = frame_width * 10 / 8;
  extra_height = 0;
  frame_offset = 0;
  registers_offset = -1;
  stats_offset = -1;
  hdr_offset = -1;

  start_reg_array.assign(std::begin(start_reg_array_imx520_g8),
                         std::end(start_reg_array_imx520_g8));
  stop_reg_array.assign(std::begin(stop_reg_array_imx520_g8),
                        std::end(stop_reg_array_imx520_g8));

  init_reg_array.assign(std::begin(init_array_imx520_g8),
                        std::end(init_array_imx520_g8));
  init_reg_array.insert(init_reg_array.end(),
                        std::begin(mode_1640x924_30_imx520_g8),
                        std::end(mode_1640x924_30_imx520_g8));

  probe_reg_addr = 0x0016;
  probe_expected_data = 0x0520;

  bits_per_pixel = 10;

  // G8_IMX520_BAYER_LOCK_BGGR_V1
  // LG donor uses 0x0101=0x03 orientation, matching the proven IMX363
  // orientation. Four-pattern first-frame sweep eliminates GRBG/GBRG and
  // supports BGGR over RGGB. Keep the hardware-facing ISP CFA fixed.
  bayer_pattern = CAM_ISP_PATTERN_BAYER_BGBGBG;  // BGGR

  mipi_format = CAM_FORMAT_MIPI_RAW_10;
  frame_data_type = CSI_RAW10;
  mclk_frequency = 19200000;

  readout_time_ns = 30000000;
  black_level = 64;

  // Freeze AE until IMX520 exposure writes are integrated explicitly.
  exposure_time_min = 1;
  exposure_time_max = 2572;
  dc_gain_factor = 1.0f;
  dc_gain_min_weight = 0;
  dc_gain_max_weight = 1;
  dc_gain_on_grey = 0.0f;
  dc_gain_off_grey = 1.0f;
  // G8_IMX520_GAIN_LADDER_V2
  // LG stock analog gain encoding: reg = 1024 - (1024 / gain), analog max 8x.
  // Exact register-domain points avoid rounding ambiguity.
  analog_gain_min_idx = 0;
  analog_gain_rec_idx = 0;
  analog_gain_max_idx = 7;
  sensor_analog_gains[0] = 1.0f;                 // reg 0x0000
  sensor_analog_gains[1] = 1.1428571429f;        // reg 0x0080
  sensor_analog_gains[2] = 1.3333333333f;        // reg 0x0100
  sensor_analog_gains[3] = 1.6f;                 // reg 0x0180
  sensor_analog_gains[4] = 2.0f;                 // reg 0x0200
  sensor_analog_gains[5] = 2.6666666667f;        // reg 0x0280
  sensor_analog_gains[6] = 4.0f;                 // reg 0x0300
  sensor_analog_gains[7] = 8.0f;                 // reg 0x0380
  min_ev = 1.0f;
  max_ev = 20576.0f;                             // 2572 lines * 8x
  target_grey_factor = 0.01f;

  // G8_IMX520_GAMMA_MODERATE_V1 - DRIVER-only A/B test
  // Replace inherited OS04C10 nonlinear gamma with a moderate 2x/(1+x) tone curve.
  // G8_IMX520_NATIVE_CCM_V1
  // LG/QTI IMX520 CC13 daylight node.
  //
  // Extracted independently from com.qti.tuned.imx520.bin. In each of five
  // repeated tuning branches this matrix is immediately preceded by the
  // float CCT trigger pair 4500 K / 9000 K:
  //
  //   1.709274  -0.749651   0.040377
  //  -0.218656   1.580145  -0.361489
  //  -0.061817  -0.781592   1.843410
  //
  // Converted to Qualcomm signed Q7. Opt-in only so the pre-existing DRIVER
  // color profile remains the default/fallback.
  if (getenv("G8_IMX520_NATIVE_CCM") != nullptr) {
    color_correct_matrix = {
      0x000000DB, 0x00000FA0, 0x00000005,
      0x00000FE4, 0x000000CA, 0x00000FD2,
      0x00000FF8, 0x00000F9C, 0x000000EC,
    };
    fprintf(stderr,
            "G8_IMX520_NATIVE_CCM_V1 enabled=1 node=4500-9000K "
            "ccm=[DB,FA0,005;FE4,CA,FD2;FF8,F9C,EC]\n");
    fflush(stderr);
  }

  // G8_IMX520_MAIN_CCM_AB_V1
  // DRIVER-only A/B test using the completed MAIN/IMX363 custom CCM.
  // This intentionally comes after G8_IMX520_NATIVE_CCM_V1, so when both
  // gates are present this matrix is the effective DRIVER CCM.
  // MAIN Q7:
  //   092 FEF FFF
  //   FF7 0B2 FD7
  //   000 FBA 0C6
  if (getenv("G8_IMX520_MAIN_CCM_AB") != nullptr) {
    color_correct_matrix = {
      0x00000092, 0x00000FEF, 0x00000FFF,
      0x00000FF7, 0x000000B2, 0x00000FD7,
      0x00000000, 0x00000FBA, 0x000000C6,
    };
    fprintf(stderr,
            "G8_IMX520_MAIN_CCM_AB_V1 enabled=1 "
            "ccm=[92,FEF,FFF;FF7,B2,FD7;000,FBA,C6]\n");
    fflush(stderr);
  }

  gamma_lut_rgb.clear();

  // Preserve the current DRIVER gamma as the default/fallback.
  for (int i = 0; i < 65; i++) {
    gamma_lut_rgb.push_back((5 * i * 1023 + (128 + 3 * i) / 2) / (128 + 3 * i));
  }

  // G8_IMX520_NATIVE_GAMMA_V1
  // Exact LG/QTI gamma16 curve found three times in com.qti.tuned.imx520.bin
  // as 65 little-endian float values in the 0..1024 domain. The identical
  // curve is also present three times in the LG IMX351 and IMX363 tuned blobs.
  // Opt-in only for safe DRIVER A/B testing.
  if (getenv("G8_IMX520_NATIVE_GAMMA") != nullptr) {
    gamma_lut_rgb = {
      0, 64, 108, 144, 176, 205, 232, 258,
      282, 304, 326, 347, 367, 386, 405, 423,
      441, 458, 475, 491, 507, 523, 538, 553,
      568, 583, 597, 611, 625, 638, 651, 665,
      678, 690, 703, 715, 728, 740, 752, 764,
      775, 787, 798, 810, 821, 832, 843, 854,
      865, 875, 886, 896, 907, 917, 927, 937,
      947, 957, 967, 977, 987, 996, 1006, 1015,
      1024,
    };
    fprintf(stderr,
            "G8_IMX520_NATIVE_GAMMA_V1 enabled=1 "
            "curve=LG_QTI_GAMMA16_65PT g0=0 g32=678 g64=1024\n");
    fflush(stderr);
  }

  prepare_gamma_lut();
}

std::vector<i2c_random_wr_payload> IMX520G8::getExposureRegisters(int exposure_time, int new_exp_g, bool dc_gain_enabled) const {
  (void)dc_gain_enabled;  // IMX520 digital/DC gain is not enabled yet.
  const uint32_t coarse = static_cast<uint32_t>(std::clamp(exposure_time, exposure_time_min, exposure_time_max));
  const int gain_idx = std::clamp(new_exp_g, analog_gain_min_idx, analog_gain_max_idx);
  static constexpr uint32_t analog_gain_reg[] = {
    0x0000,  // 1.000000x
    0x0080,  // 1.142857x
    0x0100,  // 1.333333x
    0x0180,  // 1.600000x
    0x0200,  // 2.000000x
    0x0280,  // 2.666667x
    0x0300,  // 4.000000x
    0x0380,  // 8.000000x
  };
  const uint32_t gain_reg = analog_gain_reg[gain_idx];

  return {
    {0x0202, coarse >> 8}, {0x0203, coarse & 0xFF},
    {0x0204, gain_reg >> 8}, {0x0205, gain_reg & 0xFF},
  };
}

int IMX520G8::getSlaveAddress(int port) const {
  (void)port;
  return 0x20;
}
