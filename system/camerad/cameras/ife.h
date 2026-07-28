#pragma once

#include <cstdio>
#include <cstdlib>

#include "cdm.h"

// G8_CAMERA_DYNAMIC_AWB_V3
// Shared live IFE-WB state for the three G8 camera roles.
// Field mapping was proven experimentally: gain0=G, gain1=B, gain2=R.
// Each stream starts from its already-proven fixed color-calibration value.
inline uint32_t g8_road_awb_gain_b = 0xCE;
inline uint32_t g8_road_awb_gain_r = 0xB4;
inline uint32_t g8_driver_awb_gain_b = 0xD0;
inline uint32_t g8_driver_awb_gain_r = 0xD8;
inline uint32_t g8_wide_awb_gain_b = 0xD8;
inline uint32_t g8_wide_awb_gain_r = 0xB4;

#include "system/camerad/cameras/hw.h"
#include "system/camerad/sensors/sensor.h"

int build_common_ife_bps(uint8_t *dst, const CameraConfig cam, const SensorInfo *s, std::vector<uint32_t> &patches, bool ife) {
  uint8_t *start = dst;

  /*
    Common between IFE and BPS.
  */

  // IFE -> BPS addresses
  /*
  std::map<uint32_t, uint32_t> addrs = {
    {0xf30, 0x3468},
  };
  */

  // YUV
  dst += write_cont(dst, ife ? 0xf30 : 0x3468, {
    0x00680208,
    0x00000108,
    0x00400000,
    0x03ff0000,
    0x025d1d93,
    0x00000010,
    0x02000000,
    0x03ff0000,
    0x003f1d66,
    0x0000025b,
    0x02000000,
    0x03ff0000,
  });

  return dst - start;
}

int build_update(uint8_t *dst, const CameraConfig cam, const SensorInfo *s, std::vector<uint32_t> &patches) {
  uint8_t *start = dst;

  // init sequence
  dst += write_random(dst, {
    0x2c, 0xffffffff,
    0x30, 0xffffffff,
    0x34, 0xffffffff,
    0x38, 0xffffffff,
    0x3c, 0xffffffff,
  });

  // demux cfg
  dst += write_cont(dst, 0x560, {
    0x00000001,
    0x04440444,
    0x04450445,
    0x04440444,
    0x04450445,
    0x000000ca,
    0x0000009c,
  });

  // white balance
  // G8_IFE_WB_SWEEP_V1
  // Qualcomm IFE WB register packing currently used by this port:
  //   gain0 = word0[15:0]
  //   gain1 = word0[31:16]
  //   gain2 = word1[15:0]
  // Baseline is 0x80 / 0x80 / 0x80. Channel identities are intentionally
  // not assumed; Stage 9A identifies them empirically one field at a time.
  // G8_COLOR_CAL_V1
  // Stage-9 calibration from the same OLED target captured by IMX363,
  // IMX351, IMX520 and an iPhone reference. IFE field mapping was proven:
  // gain0=G, gain1=B, gain2=R.
  //
  // Non-G8 keeps the original unity WB.
  // G8 defaults are now sensor/stream-specific:
  //   IMX363 ROAD:   G=0x80 B=0xCE R=0xB4
  //   IMX351 WIDE:   G=0x80 B=0xD8 R=0xB4
  //   IMX520 DRIVER: G=0x80 B=0xD0 R=0xC0
  uint32_t g8_wb_gain0 = 0x80;
  uint32_t g8_wb_gain1 = 0x80;
  uint32_t g8_wb_gain2 = 0x80;

  const bool g8_color_cal = getenv("G8_AGNOS") != nullptr;
  if (g8_color_cal) {
    const bool g8_is_driver = cam.stream_type == VISION_STREAM_DRIVER;
    const bool g8_is_wide =
        getenv("G8_CAMERA_TARGET_WIDE") != nullptr ||
        cam.stream_type == VISION_STREAM_WIDE_ROAD;

    if (g8_is_driver) {
      // G8_DRIVER_WB_ROOM_V1
      // Compromise WB after OLED-neutral calibration was too red in room light.
      g8_wb_gain1 = 0xD0;  // B
      g8_wb_gain2 = 0xD8;  // R
    } else if (g8_is_wide) {
      g8_wb_gain1 = 0xD8;  // B
      g8_wb_gain2 = 0xB4;  // R
    } else {
      g8_wb_gain1 = 0xCE;  // B
      g8_wb_gain2 = 0xB4;  // R
    }
  }

  // G8_CAMERA_DYNAMIC_AWB_V3
  // Apply the independent live WB state for ROAD / DRIVER / WIDE.
  // Explicit G8_IFE_WB_TEST still runs below and remains the final override.
  // G8_CAMERA_TARGET_WIDE is included so the established standalone WIDE
  // harness (which can publish through ROAD) receives the WIDE AWB state.
  const bool g8_awb_all = getenv("G8_DYNAMIC_AWB_ALL") != nullptr;
  const bool g8_awb_rear = getenv("G8_DYNAMIC_AWB_REAR") != nullptr;
  const bool g8_is_driver_awb = cam.stream_type == VISION_STREAM_DRIVER;
  const bool g8_is_wide_awb =
      cam.stream_type == VISION_STREAM_WIDE_ROAD ||
      getenv("G8_CAMERA_TARGET_WIDE") != nullptr;
  const bool g8_is_road_awb =
      cam.stream_type == VISION_STREAM_ROAD &&
      !g8_is_wide_awb && !g8_is_driver_awb;

  const bool g8_dynamic_awb_driver =
      getenv("G8_AGNOS") != nullptr && g8_is_driver_awb &&
      (g8_awb_all || getenv("G8_DYNAMIC_AWB_DRIVER") != nullptr);
  const bool g8_dynamic_awb_road =
      getenv("G8_AGNOS") != nullptr && g8_is_road_awb &&
      (g8_awb_all || g8_awb_rear || getenv("G8_DYNAMIC_AWB_ROAD") != nullptr);
  const bool g8_dynamic_awb_wide =
      getenv("G8_AGNOS") != nullptr && g8_is_wide_awb &&
      (g8_awb_all || g8_awb_rear || getenv("G8_DYNAMIC_AWB_WIDE") != nullptr);

  if (g8_dynamic_awb_driver) {
    g8_wb_gain1 = g8_driver_awb_gain_b;
    g8_wb_gain2 = g8_driver_awb_gain_r;
  } else if (g8_dynamic_awb_wide) {
    g8_wb_gain1 = g8_wide_awb_gain_b;
    g8_wb_gain2 = g8_wide_awb_gain_r;
  } else if (g8_dynamic_awb_road) {
    g8_wb_gain1 = g8_road_awb_gain_b;
    g8_wb_gain2 = g8_road_awb_gain_r;
  }

  const bool g8_wb_test =
      getenv("G8_AGNOS") != nullptr &&
      getenv("G8_IFE_WB_TEST") != nullptr;

  if (g8_wb_test) {
    auto parse_gain = [](const char *name, uint32_t fallback) {
      const char *v = getenv(name);
      if (v == nullptr || *v == '\0') return fallback;
      char *end = nullptr;
      unsigned long x = strtoul(v, &end, 0);
      if (end == v || *end != '\0') return fallback;
      if (x > 0x0FFFUL) x = 0x0FFFUL;
      return static_cast<uint32_t>(x);
    };

    g8_wb_gain0 = parse_gain("G8_IFE_WB_GAIN0", g8_wb_gain0);
    g8_wb_gain1 = parse_gain("G8_IFE_WB_GAIN1", g8_wb_gain1);
    g8_wb_gain2 = parse_gain("G8_IFE_WB_GAIN2", g8_wb_gain2);

    static bool g8_wb_logged = false;
    if (!g8_wb_logged) {
      fprintf(stderr,
              "G8_IFE_WB_SWEEP_V1 gain0=0x%X gain1=0x%X gain2=0x%X\n",
              g8_wb_gain0, g8_wb_gain1, g8_wb_gain2);
      fflush(stderr);
      g8_wb_logged = true;
    }
  }

  dst += write_cont(dst, 0x6fc, {
    (g8_wb_gain1 << 16) | g8_wb_gain0,
    g8_wb_gain2,
    0x00000000,
    0x00000000,
  });

  // G8_IMX363_DYNAMIC_CCM_V3
  //
  // Smooth LG IMX363 CCT-dependent color correction.
  //
  // The matrices below are the real LG CC13 matrices extracted from
  // com.qti.tuned.imx363.bin and converted to Qualcomm signed Q7.
  //
  // The B/R -> CCT mapping is currently an empirical G8 approximation.
  // Unlike V2 there are no hard matrix jumps: coefficients are linearly
  // interpolated between LG tuning nodes.
  if (getenv("G8_AGNOS") != nullptr &&
      getenv("G8_DYNAMIC_CCM_ROAD") != nullptr &&
      g8_is_road_awb) {

    const int g8_r = g8_road_awb_gain_r > 0 ?
                     (int)g8_road_awb_gain_r : 1;

    // Q10 B/R ratio.
    const int g8_ratio_q10 =
        ((int)g8_road_awb_gain_b * 1024 + g8_r / 2) / g8_r;

    // Empirical B/R anchor positions.
    //
    // 0.980 -> LG daylight / ~5500 K
    // 1.120 -> LG neutral  / ~3900 K
    // 1.240 -> LG warm     / ~3000 K
    // 1.320 -> LG tungsten / ~2450 K
    static constexpr int g8_ratio_nodes[4] = {
      1004, 1147, 1270, 1352
    };

    static constexpr int g8_cct_nodes[4] = {
      5500, 3900, 3000, 2450
    };

    // Signed Qualcomm Q7 coefficients.
    //
    // Node 0: LG CC13 4600-7000 K
    // Node 1: LG CC13 3550-4250 K
    // Node 2: LG CC13 2750-3200 K
    // Node 3: LG CC13 <=2450 K
    static constexpr int g8_ccm_nodes[4][9] = {
      {
         214,  -87,    1,
         -30,  185,  -26,
          -2, -127,  257,
      },
      {
         226,  -92,   -7,
         -41,  185,  -16,
          -1, -122,  251,
      },
      {
         230, -131,   29,
         -28,  136,   20,
           9, -189,  308,
      },
      {
         209, -127,   46,
         -35,  145,   18,
          -6, -185,  318,
      },
    };

    int g8_lo = 0;
    int g8_hi = 0;
    int g8_num = 0;
    int g8_den = 1;

    if (g8_ratio_q10 <= g8_ratio_nodes[0]) {
      g8_lo = g8_hi = 0;

    } else if (g8_ratio_q10 >= g8_ratio_nodes[3]) {
      g8_lo = g8_hi = 3;

    } else {
      for (int i = 0; i < 3; ++i) {
        if (g8_ratio_q10 >= g8_ratio_nodes[i] &&
            g8_ratio_q10 <  g8_ratio_nodes[i + 1]) {

          g8_lo = i;
          g8_hi = i + 1;
          g8_num = g8_ratio_q10 - g8_ratio_nodes[i];
          g8_den = g8_ratio_nodes[i + 1] - g8_ratio_nodes[i];
          break;
        }
      }
    }

    auto g8_lerp_i = [](int a, int b, int num, int den) {
      if (num <= 0 || a == b) return a;

      long long x = (long long)(b - a) * num;

      if (x >= 0) {
        x += den / 2;
      } else {
        x -= den / 2;
      }

      return a + (int)(x / den);
    };

    int g8_ccm_q7[9];

    for (int i = 0; i < 9; ++i) {
      g8_ccm_q7[i] =
          g8_lerp_i(g8_ccm_nodes[g8_lo][i],
                    g8_ccm_nodes[g8_hi][i],
                    g8_num,
                    g8_den);
    }

    int g8_cct =
        g8_lerp_i(g8_cct_nodes[g8_lo],
                  g8_cct_nodes[g8_hi],
                  g8_num,
                  g8_den);

    std::vector<uint32_t> g8_ccm;
    g8_ccm.reserve(9);

    for (int i = 0; i < 9; ++i) {
      // Qualcomm CC register uses signed 12-bit two's complement.
      g8_ccm.push_back((uint32_t)(g8_ccm_q7[i] & 0x0FFF));
    }

    // Per-frame IFE color-correction update.
    dst += write_cont(dst, 0x760, g8_ccm);

    // Do not spam every frame. Log when the estimated illuminant has moved
    // enough to be meaningful.
    static int g8_last_logged_ratio = -10000;
    static int g8_last_logged_cct = -10000;

    const int g8_ratio_change =
        g8_ratio_q10 > g8_last_logged_ratio ?
        g8_ratio_q10 - g8_last_logged_ratio :
        g8_last_logged_ratio - g8_ratio_q10;

    const int g8_cct_change =
        g8_cct > g8_last_logged_cct ?
        g8_cct - g8_last_logged_cct :
        g8_last_logged_cct - g8_cct;

    if (g8_last_logged_ratio < 0 ||
        g8_ratio_change >= 8 ||
        g8_cct_change >= 100) {

      fprintf(stderr,
              "G8_IMX363_DYNAMIC_CCM_V3 "
              "B=0x%X R=0x%X ratio=%.3f "
              "nodes=%d->%d mix=%.3f approx_cct=%dK "
              "ccm=[%d,%d,%d;%d,%d,%d;%d,%d,%d]\n",
              g8_road_awb_gain_b,
              g8_road_awb_gain_r,
              (double)g8_ratio_q10 / 1024.0,
              g8_lo,
              g8_hi,
              g8_den > 0 ? (double)g8_num / (double)g8_den : 0.0,
              g8_cct,
              g8_ccm_q7[0], g8_ccm_q7[1], g8_ccm_q7[2],
              g8_ccm_q7[3], g8_ccm_q7[4], g8_ccm_q7[5],
              g8_ccm_q7[6], g8_ccm_q7[7], g8_ccm_q7[8]);

      fflush(stderr);

      g8_last_logged_ratio = g8_ratio_q10;
      g8_last_logged_cct = g8_cct;
    }
  }

  // module config/enables (e.g. enable debayer, white balance, etc.)
  const bool has_linearization =
      !s->linearization_pts.empty() && !s->linearization_lut.empty();
  const bool has_vignetting =
      cam.vignetting_correction && !s->vignetting_lut.empty();

  dst += write_cont(dst, 0x40, {
    (0x00000c06 & (has_linearization ? ~0U : ~0x00000002U)) |
        ((uint32_t)has_vignetting << 8),
  });
  dst += write_cont(dst, 0x44, {
    0x00000000,
  });
  dst += write_cont(dst, 0x48, {
    (1 << 3) | (1 << 1),
  });
  dst += write_cont(dst, 0x4c, {
    0x00000019,
  });
  dst += write_cont(dst, 0xf00, {
    0x00000000,
  });

  // cropping
  dst += write_cont(dst, 0xe0c, {
    0x00000e00,
  });
  dst += write_cont(dst, 0xe2c, {
    0x00000e00,
  });

  // black level scale + offset
  dst += write_cont(dst, 0x6b0, {
    ((uint32_t)(1 << 11) << 0xf) | (s->black_level << (14 - s->bits_per_pixel)),
    0x0,
    0x0,
  });

  return dst - start;
}


int build_initial_config(uint8_t *dst, const CameraConfig cam, const SensorInfo *s, std::vector<uint32_t> &patches, uint32_t out_width, uint32_t out_height) {
  uint8_t *start = dst;

  // start with the every frame config
  dst += build_update(dst, cam, s, patches);

  uint64_t addr;

  const bool has_linearization =
      !s->linearization_pts.empty() && !s->linearization_lut.empty();
  const bool has_vignetting =
      cam.vignetting_correction && !s->vignetting_lut.empty();

  // setup
  dst += write_cont(dst, 0x478, {
    0x00000004,
    0x004000c0,
  });
  dst += write_cont(dst, 0x488, {
    0x00000000,
    0x00000000,
    0x00000f0f,
  });
  dst += write_cont(dst, 0x49c, {
    0x00000001,
  });
  dst += write_cont(dst, 0xce4, {
    0x00000000,
    0x00000000,
  });

  // linearization
  if (has_linearization) {
    dst += write_cont(dst, 0x4dc, {
      0x00000000,
    });
    dst += write_cont(dst, 0x4e0, s->linearization_pts);
    dst += write_cont(dst, 0x4f0, s->linearization_pts);
    dst += write_cont(dst, 0x500, s->linearization_pts);
    dst += write_cont(dst, 0x510, s->linearization_pts);

    dst += write_dmi(dst, &addr,
                     s->linearization_lut.size() * sizeof(uint32_t),
                     0xc24, 9);
    patches.push_back(addr - (uint64_t)start);
  }

  // vignetting correction
  if (has_vignetting) {
    dst += write_cont(dst, 0x6bc, {
      0x0b3c0000,
      0x00670067,
      0xd3b1300c,
      0x13b1300c,
    });
    dst += write_cont(dst, 0x6d8, {
      0xec4e4000,
      0x0100c003,
    });

    dst += write_dmi(dst, &addr,
                     s->vignetting_lut.size() * sizeof(uint32_t),
                     0xc24, 14);
    patches.push_back(addr - (uint64_t)start);

    dst += write_dmi(dst, &addr,
                     s->vignetting_lut.size() * sizeof(uint32_t),
                     0xc24, 15);
    patches.push_back(addr - (uint64_t)start);
  }

  // debayer
  dst += write_cont(dst, 0x6f8, {
    0x00000100,
  });
  dst += write_cont(dst, 0x71c, {
    0x00008000,
    0x08000066,
  });

  // color correction
  dst += write_cont(dst, 0x760, s->color_correct_matrix);

  // gamma
  dst += write_cont(dst, 0x798, {
    0x00000000,
  });
  dst += write_dmi(dst, &addr, s->gamma_lut_rgb.size()*sizeof(uint32_t), 0xc24, 26);  // G
  patches.push_back(addr - (uint64_t)start);
  dst += write_dmi(dst, &addr, s->gamma_lut_rgb.size()*sizeof(uint32_t), 0xc24, 28);  // B
  patches.push_back(addr - (uint64_t)start);
  dst += write_dmi(dst, &addr, s->gamma_lut_rgb.size()*sizeof(uint32_t), 0xc24, 30);  // R
  patches.push_back(addr - (uint64_t)start);

  // output size/scaling
  dst += write_cont(dst, 0xa3c, {
    0x00000003,
    ((out_width - 1) << 16) | (s->frame_width - 1),
    0x30036666,
    0x00000000,
    0x00000000,
    s->frame_width - 1,
    ((out_height - 1) << 16) | (s->frame_height - 1),
    0x30036666,
    0x00000000,
    0x00000000,
    s->frame_height - 1,
  });
  dst += write_cont(dst, 0xa68, {
    0x00000003,
    ((out_width / 2 - 1) << 16) | (s->frame_width - 1),
    0x3006cccc,
    0x00000000,
    0x00000000,
    s->frame_width - 1,
    ((out_height / 2 - 1) << 16) | (s->frame_height - 1),
    0x3006cccc,
    0x00000000,
    0x00000000,
    s->frame_height - 1,
  });

  // cropping
  dst += write_cont(dst, 0xe10, {
    out_height - 1,
    out_width - 1,
  });
  dst += write_cont(dst, 0xe30, {
    out_height / 2 - 1,
    out_width - 1,
  });
  dst += write_cont(dst, 0xe18, {
    0x0ff00000,
    0x00000016,
  });
  dst += write_cont(dst, 0xe38, {
    0x0ff00000,
    0x00000017,
  });

  dst += build_common_ife_bps(dst, cam, s, patches, true);

  return dst - start;
}


