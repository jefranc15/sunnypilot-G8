#pragma once

#include "common/util.h"
#include "cereal/gen/cpp/log.capnp.h"
#include "msgq/visionipc/visionipc_server.h"

#include "media/cam_isp_ife.h"


typedef enum {
  ISP_RAW_OUTPUT,   // raw frame from sensor
  ISP_IFE_PROCESSED,  // fully processed image through the IFE
  ISP_BPS_PROCESSED,  // fully processed image through the BPS
} SpectraOutputType;

const bool G8_AGNOS_CAMERA = getenv("G8_AGNOS") != nullptr;
const bool G8_WIDE_CAMERA_TEST = G8_AGNOS_CAMERA && getenv("G8_CAMERA_TARGET_WIDE") != nullptr;
const bool G8_DRIVER_CAMERA_TEST = G8_AGNOS_CAMERA && getenv("G8_CAMERA_TARGET_DRIVER") != nullptr;

// For the comma 3X three camera platform

struct CameraConfig {
  int camera_num;
  VisionStreamType stream_type;
  float focal_len;  // millimeters
  const char *publish_name;
  cereal::FrameData::Builder (cereal::Event::Builder::*init_camera_state)();
  bool enabled;
  uint32_t phy;
  bool vignetting_correction;
  SpectraOutputType output_type;
  bool staggered_sof;  // SOF is staggered (half-period offset) from other cameras
};

// NOTE: to be able to disable road and wide road, we still have to configure the sensor over i2c
// If you don't do this, the strobe GPIO is an output (even in reset it seems!)
const CameraConfig WIDE_ROAD_CAMERA_CONFIG = {
  .camera_num = 0,
  .stream_type = VISION_STREAM_WIDE_ROAD,
  .focal_len = 1.71,
  .publish_name = "wideRoadCameraState",
  .init_camera_state = &cereal::Event::Builder::initWideRoadCameraState,
  .enabled = !getenv("DISABLE_WIDE_ROAD"),
  .phy = CAM_ISP_IFE_IN_RES_PHY_0,
  .vignetting_correction = false,
  .output_type = ISP_IFE_PROCESSED,
  .staggered_sof = false,
};

const CameraConfig ROAD_CAMERA_CONFIG = {
  .camera_num = G8_DRIVER_CAMERA_TEST ? 1 : (G8_WIDE_CAMERA_TEST ? 2 : (G8_AGNOS_CAMERA ? 0 : 1)),
  .stream_type = VISION_STREAM_ROAD,
  .focal_len = G8_DRIVER_CAMERA_TEST ? 1.71f : (G8_AGNOS_CAMERA ? 4.23f : 8.0f),
  .publish_name = "roadCameraState",
  .init_camera_state = &cereal::Event::Builder::initRoadCameraState,
  .enabled = !getenv("DISABLE_ROAD"),
  .phy = (uint32_t)(G8_DRIVER_CAMERA_TEST ? CAM_ISP_IFE_IN_RES_PHY_2 : (G8_WIDE_CAMERA_TEST ? CAM_ISP_IFE_IN_RES_PHY_1 : (G8_AGNOS_CAMERA ? CAM_ISP_IFE_IN_RES_PHY_0 : CAM_ISP_IFE_IN_RES_PHY_1))),
  .vignetting_correction = G8_AGNOS_CAMERA ? false : true,
  .output_type = ISP_IFE_PROCESSED,
  .staggered_sof = false,
};

const CameraConfig DRIVER_CAMERA_CONFIG = {
  .camera_num = 2,
  .stream_type = VISION_STREAM_DRIVER,
  .focal_len = 1.71,
  .publish_name = "driverCameraState",
  .init_camera_state = &cereal::Event::Builder::initDriverCameraState,
  .enabled = !getenv("DISABLE_DRIVER"),
  .phy = CAM_ISP_IFE_IN_RES_PHY_2,
  .vignetting_correction = false,
  // G8_IMX520_DRIVER_STREAM_V1: keep G8 front on the proven IFE path.
  .output_type = G8_AGNOS_CAMERA ? ISP_IFE_PROCESSED : ISP_BPS_PROCESSED,
  .staggered_sof = G8_AGNOS_CAMERA ? false : true,
};

const CameraConfig ALL_CAMERA_CONFIGS[] = {WIDE_ROAD_CAMERA_CONFIG, ROAD_CAMERA_CONFIG, DRIVER_CAMERA_CONFIG};
