#include <cstdio>
#include "cdm.h"

#include <algorithm>
#include <stdint.h>
#include <cassert>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "media/cam_defs.h"
#include "media/cam_isp.h"
#include "media/cam_icp.h"
#include "media/cam_isp_ife.h"
#include "media/cam_sync.h"

#include "common/util.h"
#include "common/swaglog.h"
#include "system/camerad/cameras/ife.h"
#include "system/camerad/cameras/nv12_info.h"
#include "system/camerad/cameras/spectra.h"
#include "system/camerad/cameras/bps_blobs.h"


// ************** low level camera helpers ****************

int do_cam_control(int fd, int op_code, void *handle, int size) {
  struct cam_control camcontrol = {0};
  camcontrol.op_code = op_code;
  camcontrol.handle = (uint64_t)handle;
  if (size == 0) {
    camcontrol.size = 8;
    camcontrol.handle_type = CAM_HANDLE_MEM_HANDLE;
  } else {
    camcontrol.size = size;
    camcontrol.handle_type = CAM_HANDLE_USER_POINTER;
  }

  int ret = HANDLE_EINTR(ioctl(fd, VIDIOC_CAM_CONTROL, &camcontrol));
  if (ret == -1) {
    LOGE("VIDIOC_CAM_CONTROL error: op_code %d - errno %d", op_code, errno);
  }
  return ret;
}

int do_sync_control(int fd, uint32_t id, void *handle, uint32_t size) {
  struct cam_private_ioctl_arg arg = {
    .id = id,
    .size = size,
    .ioctl_ptr = (uint64_t)handle,
  };
  int ret = HANDLE_EINTR(ioctl(fd, CAM_PRIVATE_IOCTL_CMD, &arg));

  int32_t ioctl_result = static_cast<int32_t>(arg.result);
  if (ret < 0) {
    LOGE("CAM_SYNC error: id %u - errno %d - ret %d - ioctl_result %d", id, errno, ret, ioctl_result);
    return ret;
  }
  if (ioctl_result != 0) {
    LOGE("CAM_SYNC error: id %u - errno %d - ret %d - ioctl_result %d", id, errno, ret, ioctl_result);
    return ioctl_result;
  }
  return ret;
}

std::optional<int32_t> device_acquire(int fd, int32_t session_handle, void *data, uint32_t num_resources) {
  struct cam_acquire_dev_cmd cmd = {
    .session_handle = session_handle,
    .handle_type = CAM_HANDLE_USER_POINTER,
    .num_resources = (uint32_t)(data ? num_resources : 0),
    .resource_hdl = (uint64_t)data,
  };
  int err = do_cam_control(fd, CAM_ACQUIRE_DEV, &cmd, sizeof(cmd));
  return err == 0 ? std::make_optional(cmd.dev_handle) : std::nullopt;
}

int device_config(int fd, int32_t session_handle, int32_t dev_handle, uint64_t packet_handle) {
  struct cam_config_dev_cmd cmd = {
    .session_handle = session_handle,
    .dev_handle = dev_handle,
    .packet_handle = packet_handle,
  };
  return do_cam_control(fd, CAM_CONFIG_DEV, &cmd, sizeof(cmd));
}

int device_control(int fd, int op_code, int session_handle, int dev_handle) {
  // start stop and release are all the same
  struct cam_start_stop_dev_cmd cmd { .session_handle = session_handle, .dev_handle = dev_handle };
  return do_cam_control(fd, op_code, &cmd, sizeof(cmd));
}

void *alloc_w_mmu_hdl(int video0_fd, int len, uint32_t *handle, int align, int flags, int mmu_hdl, int mmu_hdl2) {
  struct cam_mem_mgr_alloc_cmd mem_mgr_alloc_cmd = {0};
  mem_mgr_alloc_cmd.len = len;
  mem_mgr_alloc_cmd.align = align;
  mem_mgr_alloc_cmd.flags = flags;
  mem_mgr_alloc_cmd.num_hdl = 0;
  if (mmu_hdl != 0) {
    mem_mgr_alloc_cmd.mmu_hdls[0] = mmu_hdl;
    mem_mgr_alloc_cmd.num_hdl++;
  }
  if (mmu_hdl2 != 0) {
    mem_mgr_alloc_cmd.mmu_hdls[1] = mmu_hdl2;
    mem_mgr_alloc_cmd.num_hdl++;
  }

  do_cam_control(video0_fd, CAM_REQ_MGR_ALLOC_BUF, &mem_mgr_alloc_cmd, sizeof(mem_mgr_alloc_cmd));
  *handle = mem_mgr_alloc_cmd.out.buf_handle;

  void *ptr = NULL;
  if (mem_mgr_alloc_cmd.out.fd > 0) {
    ptr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, mem_mgr_alloc_cmd.out.fd, 0);
    assert(ptr != MAP_FAILED);
  }

  // LOGD("allocated: %x %d %llx mapped %p", mem_mgr_alloc_cmd.out.buf_handle, mem_mgr_alloc_cmd.out.fd, mem_mgr_alloc_cmd.out.vaddr, ptr);

  return ptr;
}

void release(int video0_fd, uint32_t handle) {
  struct cam_mem_mgr_release_cmd mem_mgr_release_cmd = {0};
  mem_mgr_release_cmd.buf_handle = handle;

  int ret = do_cam_control(video0_fd, CAM_REQ_MGR_RELEASE_BUF, &mem_mgr_release_cmd, sizeof(mem_mgr_release_cmd));
  assert(ret == 0);
}

static cam_cmd_power *power_set_wait(cam_cmd_power *power, int16_t delay_ms) {
  cam_cmd_unconditional_wait *unconditional_wait = (cam_cmd_unconditional_wait *)((char *)power + (sizeof(struct cam_cmd_power) + (power->count - 1) * sizeof(struct cam_power_settings)));
  unconditional_wait->cmd_type = CAMERA_SENSOR_CMD_TYPE_WAIT;
  unconditional_wait->delay = delay_ms;
  unconditional_wait->op_code = CAMERA_SENSOR_WAIT_OP_SW_UCND;
  return (struct cam_cmd_power *)(unconditional_wait + 1);
}

// *** MemoryManager ***

void *MemoryManager::alloc_buf(int size, uint32_t *handle) {
  void *ptr;
  auto &cache = cached_allocations[size];
  if (!cache.empty()) {
    ptr = cache.front();
    cache.pop();
    *handle = handle_lookup[ptr];
  } else {
    ptr = alloc_w_mmu_hdl(video0_fd, size, handle);
    handle_lookup[ptr] = *handle;
    size_lookup[ptr] = size;
  }
  memset(ptr, 0, size);
  return ptr;
}

void MemoryManager::free(void *ptr) {
  cached_allocations[size_lookup[ptr]].push(ptr);
}

MemoryManager::~MemoryManager() {
  for (auto& x : cached_allocations) {
    while (!x.second.empty()) {
      void *ptr = x.second.front();
      x.second.pop();
      LOGD("freeing cached allocation %p with size %d", ptr, size_lookup[ptr]);
      munmap(ptr, size_lookup[ptr]);

      // release fd
      close(handle_lookup[ptr] >> 16);
      release(video0_fd, handle_lookup[ptr]);

      handle_lookup.erase(ptr);
      size_lookup.erase(ptr);
    }
  }
}

// *** SpectraMaster ***

void SpectraMaster::init() {
  LOG("-- Opening devices");
  // video0 is req_mgr, the target of many ioctls
  video0_fd = HANDLE_EINTR(open("/dev/v4l/by-path/platform-soc:qcom_cam-req-mgr-video-index0", O_RDWR | O_NONBLOCK));
  assert(video0_fd >= 0);
  LOGD("opened video0");

  // video1 is cam_sync, the target of some ioctls
  cam_sync_fd = HANDLE_EINTR(open("/dev/v4l/by-path/platform-cam_sync-video-index0", O_RDWR | O_NONBLOCK));
  assert(cam_sync_fd >= 0);
  LOGD("opened video1 (cam_sync)");

  // looks like there's only one of these
  isp_fd = open_v4l_by_name_and_index("cam-isp");
  assert(isp_fd >= 0);
  LOGD("opened isp %d", (int)isp_fd);

  if (getenv("G8_AGNOS") == nullptr) {
    icp_fd = open_v4l_by_name_and_index("cam-icp");
    assert(icp_fd >= 0);
    LOGD("opened icp %d", (int)icp_fd);
  } else {
    LOGD("G8 R1: ICP/BPS is intentionally unused");
  }

  // query ISP for MMU handles
  LOG("-- Query for MMU handles");
  struct cam_isp_query_cap_cmd isp_query_cap_cmd = {0};
  struct cam_query_cap_cmd query_cap_cmd = {0};
  query_cap_cmd.handle_type = 1;
  query_cap_cmd.caps_handle = (uint64_t)&isp_query_cap_cmd;
  query_cap_cmd.size = sizeof(isp_query_cap_cmd);
  int ret = do_cam_control(isp_fd, CAM_QUERY_CAP, &query_cap_cmd, sizeof(query_cap_cmd));
  assert(ret == 0);
  LOGD("using MMU handle: %x", isp_query_cap_cmd.device_iommu.non_secure);
  LOGD("using MMU handle: %x", isp_query_cap_cmd.cdm_iommu.non_secure);
  device_iommu = isp_query_cap_cmd.device_iommu.non_secure;
  cdm_iommu = isp_query_cap_cmd.cdm_iommu.non_secure;

  // query ICP for MMU handles
  if (getenv("G8_AGNOS") == nullptr) {
    struct cam_icp_query_cap_cmd icp_query_cap_cmd = {0};
    query_cap_cmd.caps_handle = (uint64_t)&icp_query_cap_cmd;
    query_cap_cmd.size = sizeof(icp_query_cap_cmd);
    ret = do_cam_control(icp_fd, CAM_QUERY_CAP, &query_cap_cmd, sizeof(query_cap_cmd));
    assert(ret == 0);
    LOGD("using ICP MMU handle: %x", icp_query_cap_cmd.dev_iommu_handle.non_secure);
    icp_device_iommu = icp_query_cap_cmd.dev_iommu_handle.non_secure;
  }

  // subscribe
  LOG("-- Subscribing");
  struct v4l2_event_subscription sub = {0};
  sub.type = V4L_EVENT_CAM_REQ_MGR_EVENT;
  sub.id = V4L_EVENT_CAM_REQ_MGR_SOF_BOOT_TS;
  ret = HANDLE_EINTR(ioctl(video0_fd, VIDIOC_SUBSCRIBE_EVENT, &sub));
  LOGD("req mgr subscribe: %d", ret);

  mem_mgr.init(video0_fd);
}

// *** SpectraCamera ***

SpectraCamera::SpectraCamera(SpectraMaster *master, const CameraConfig &config)
  : m(master),
    enabled(config.enabled),
    cc(config) {
  ife_buf_depth = VIPC_BUFFER_COUNT;
  assert(ife_buf_depth < MAX_IFE_BUFS);
}

SpectraCamera::~SpectraCamera() {
  if (open) {
    camera_close();
  }
}

int SpectraCamera::clear_req_queue() {
  // for "non-realtime" BPS
  if (icp_dev_handle > 0) {
    struct cam_flush_dev_cmd cmd = {
      .session_handle = session_handle,
      .dev_handle = icp_dev_handle,
      .flush_type = CAM_FLUSH_TYPE_ALL,
    };
    int err = do_cam_control(m->icp_fd, CAM_FLUSH_REQ, &cmd, sizeof(cmd));
    assert(err == 0);
    LOGD("flushed bps: %d", err);
  }

  // for "realtime" devices
  struct cam_req_mgr_flush_info req_mgr_flush_request = {0};
  req_mgr_flush_request.session_hdl = session_handle;
  req_mgr_flush_request.link_hdl = link_handle;
  req_mgr_flush_request.flush_type = CAM_REQ_MGR_FLUSH_TYPE_ALL;
  int ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_FLUSH_REQ, &req_mgr_flush_request, sizeof(req_mgr_flush_request));
  LOGD("flushed all req: %d", ret);  // returns a "time until timeout" on clearing the workq

  for (int i = 0; i < MAX_IFE_BUFS; ++i) {
    destroySyncObjectAt(i);
  }

  return ret;
}

void SpectraCamera::camera_open(VisionIpcServer *v) {
  if (!openSensor()) {
    return;
  }

  if (!enabled) return;

  buf.out_img_width = sensor->frame_width / sensor->out_scale;
  buf.out_img_height = (sensor->hdr_offset > 0 ? (sensor->frame_height - sensor->hdr_offset) / 2 : sensor->frame_height) / sensor->out_scale;

  // size is driven by all the HW that handles frames,
  // the video encoder has certain alignment requirements in this case
  std::tie(stride, y_height, uv_height, yuv_size) = get_nv12_info(buf.out_img_width, buf.out_img_height);
  uv_offset = stride * y_height;

  open = true;
  configISP();
  if (cc.output_type == ISP_BPS_PROCESSED) configICP();
  configCSIPHY();

  linkDevices();

  LOGD("camera init %d", cc.camera_num);
  buf.init(this, v, ife_buf_depth, cc.stream_type);
  if (getenv("G8_CAMERA_QUEUE_ONLY") != nullptr) {
    fprintf(stderr, "G8_VIPC_BUF_INIT_DONE sensor=%d\n", cc.camera_num);
    fflush(stderr);
  }

  camera_map_bufs();
  if (getenv("G8_CAMERA_QUEUE_ONLY") != nullptr) {
    fprintf(stderr, "G8_CAMERA_MAP_BUFS_DONE sensor=%d\n", cc.camera_num);
    fflush(stderr);
  }

  clearAndRequeue(1);
  if (getenv("G8_CAMERA_QUEUE_ONLY") != nullptr) {
    const char *target =
      getenv("G8_CAMERA_TARGET_DRIVER") != nullptr ? "IMX520" :
      (getenv("G8_CAMERA_TARGET_WIDE") != nullptr ? "IMX351" : "IMX363");

    fprintf(stderr, "G8_QUEUE_READY sensor=%d depth=%d\n", cc.camera_num, ife_buf_depth);
    fprintf(stderr,
            "G8_IMX520_QUEUE_V1 CAMERA_OPEN_DONE sensor=%d target=%s sensor_started=%d size=%dx%d ife_phy=%u\n",
            cc.camera_num, target,
            sensor_started ? 1 : 0,
            sensor->frame_width, sensor->frame_height, cc.phy);
    fflush(stderr);
  }
}

void SpectraCamera::sensors_start() {
  if (!enabled) return;
  LOGD("starting sensor %d", cc.camera_num);

  if (getenv("G8_AGNOS") != nullptr) {
    // LG SM8150 native sensor state machine:
    // preload stream-off, preload stream-on, then CAM_START_DEV applies stream-on.
    sensors_i2c(sensor->stop_reg_array.data(), sensor->stop_reg_array.size(),
                CAM_SENSOR_PACKET_OPCODE_SENSOR_STREAMOFF, sensor->data_word);
    if (!enabled) return;

    sensors_i2c(sensor->start_reg_array.data(), sensor->start_reg_array.size(),
                CAM_SENSOR_PACKET_OPCODE_SENSOR_STREAMON, sensor->data_word);
    if (!enabled) return;

    int ret = device_control(sensor_fd, CAM_START_DEV, session_handle, sensor_dev_handle);
    if (getenv("G8_CAMERA_FIRST_SOF") != nullptr) {
      fprintf(stderr, "G8_SENSOR_START_RET=%d\n", ret);
      fflush(stderr);
    }
    if (ret != 0) {
      enabled = false;
      return;
    }

    sensor_started = true;
    return;
  }

  sensors_i2c(sensor->start_reg_array.data(), sensor->start_reg_array.size(),
              CAM_SENSOR_PACKET_OPCODE_SENSOR_CONFIG, sensor->data_word);
}

void SpectraCamera::sensors_stop() {
  if (getenv("G8_AGNOS") == nullptr || !sensor_started) return;

  int ret = device_control(sensor_fd, CAM_STOP_DEV, session_handle, sensor_dev_handle);
  if (getenv("G8_CAMERA_FIRST_SOF") != nullptr) {
    fprintf(stderr, "G8_SENSOR_STOP_RET=%d\n", ret);
    fflush(stderr);
  }
  if (ret == 0) sensor_started = false;
}

void SpectraCamera::sensors_poke(int request_id) {
  uint32_t cam_packet_handle = 0;

  // LG SM8150 cam_sensor_i2c_pkt_parse() rejects a backing buffer whose
  // total length is exactly sizeof(cam_packet) when config.offset == 0:
  //   offset >= len_of_buff - sizeof(cam_packet)
  // is true for 0 >= 0. Keep the logical packet size unchanged, but give
  // the kernel one aligned word of backing padding.
  const int packet_size = sizeof(struct cam_packet);
  const int alloc_size = packet_size + sizeof(uint32_t);
  auto pkt = m->mem_mgr.alloc<struct cam_packet>(alloc_size, &cam_packet_handle);

  pkt->num_cmd_buf = 0;
  pkt->kmd_cmd_buf_index = -1;
  pkt->header.size = packet_size;
  pkt->header.op_code = CAM_SENSOR_PACKET_OPCODE_SENSOR_NOP;
  pkt->header.request_id = request_id;

  int ret = device_config(sensor_fd, session_handle, sensor_dev_handle, cam_packet_handle);

  if (getenv("G8_CAMERA_QUEUE_ONLY") != nullptr) {
    fprintf(stderr, "G8_SENSOR_NOP_RET req=%d ret=%d packet=%d alloc=%d\n",
            request_id, ret, packet_size, alloc_size);
    fflush(stderr);
  }

  if (ret != 0) {
    LOGE("** sensor %d FAILED poke, disabling", cc.camera_num);

    // Diagnostic queue-only runs must still execute the full unlink/stop/
    // release path even if the NOP experiment fails.
    if (getenv("G8_CAMERA_QUEUE_ONLY") != nullptr) {
      return;
    }

    enabled = false;
    return;
  }
}

void SpectraCamera::sensors_i2c(const struct i2c_random_wr_payload* dat, int len, int op_code, bool data_word) {
  // LOGD("sensors_i2c: %d", len);
  uint32_t cam_packet_handle = 0;
  int size = sizeof(struct cam_packet)+sizeof(struct cam_cmd_buf_desc)*1;
  auto pkt = m->mem_mgr.alloc<struct cam_packet>(size, &cam_packet_handle);
  pkt->num_cmd_buf = 1;
  pkt->kmd_cmd_buf_index = -1;
  pkt->header.size = size;
  pkt->header.op_code = op_code;
  struct cam_cmd_buf_desc *buf_desc = (struct cam_cmd_buf_desc *)&pkt->payload;

  buf_desc[0].size = buf_desc[0].length = sizeof(struct i2c_rdwr_header) + len*sizeof(struct i2c_random_wr_payload);
  buf_desc[0].type = CAM_CMD_BUF_I2C;

  auto i2c_random_wr = m->mem_mgr.alloc<struct cam_cmd_i2c_random_wr>(buf_desc[0].size, (uint32_t*)&buf_desc[0].mem_handle);
  i2c_random_wr->header.count = len;
  i2c_random_wr->header.op_code = 1;
  i2c_random_wr->header.cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR;
  i2c_random_wr->header.data_type = data_word ? CAMERA_SENSOR_I2C_TYPE_WORD : CAMERA_SENSOR_I2C_TYPE_BYTE;
  i2c_random_wr->header.addr_type = CAMERA_SENSOR_I2C_TYPE_WORD;
  memcpy(i2c_random_wr->random_wr_payload, dat, len*sizeof(struct i2c_random_wr_payload));

  int ret = device_config(sensor_fd, session_handle, sensor_dev_handle, cam_packet_handle);
  if (getenv("G8_CAMERA_CONFIG_ONLY") != nullptr) {
    fprintf(stderr, "G8_SENSOR_CONFIG_RET=%d\n", ret);
    fflush(stderr);
  }
  if (getenv("G8_CAMERA_FIRST_SOF") != nullptr) {
    if (op_code == CAM_SENSOR_PACKET_OPCODE_SENSOR_STREAMON) {
      fprintf(stderr, "G8_STREAMON_PRELOAD_RET=%d\n", ret);
      fflush(stderr);
    } else if (op_code == CAM_SENSOR_PACKET_OPCODE_SENSOR_STREAMOFF) {
      fprintf(stderr, "G8_STREAMOFF_PRELOAD_RET=%d\n", ret);
      fflush(stderr);
    }
  }
  if (ret != 0) {
    LOGE("** sensor %d FAILED i2c, disabling", cc.camera_num);
    enabled = false;
    return;
  }
}

int SpectraCamera::sensors_init() {
  const bool g8_camera = getenv("G8_AGNOS") != nullptr;
  const bool g8_wide = g8_camera && getenv("G8_CAMERA_TARGET_WIDE") != nullptr;
  const bool g8_driver = g8_camera && getenv("G8_CAMERA_TARGET_DRIVER") != nullptr;
  // G8_DUAL_ROAD_DRIVER_V2:
  // - normal logical DRIVER is physical IMX520 slot 1
  // - explicit Stage-7 TARGET_DRIVER harness remains supported
  // This same boolean intentionally selects the IMX520 probe slot, 512-byte
  // power packet, and exact LG IMX520 power sequence below.
  const bool g8_driver_stream =
      g8_camera &&
      cc.stream_type == VISION_STREAM_DRIVER;
  const int g8_sensor_slot = g8_driver_stream ? 1 : cc.camera_num;

  if (g8_wide && g8_driver) {
    fprintf(stderr, "G8_CAMERA_TARGET_CONFLICT wide=1 driver=1\n");
    fflush(stderr);
    return -1;
  }

  uint32_t cam_packet_handle = 0;
  int size = sizeof(struct cam_packet)+sizeof(struct cam_cmd_buf_desc)*2;
  auto pkt = m->mem_mgr.alloc<struct cam_packet>(size, &cam_packet_handle);
  pkt->num_cmd_buf = 2;
  pkt->kmd_cmd_buf_index = -1;
  pkt->header.op_code = CSLDeviceTypeImageSensor | CAM_SENSOR_PACKET_OPCODE_SENSOR_PROBE;
  pkt->header.size = size;
  struct cam_cmd_buf_desc *buf_desc = (struct cam_cmd_buf_desc *)&pkt->payload;

  buf_desc[0].size = buf_desc[0].length = sizeof(struct cam_cmd_i2c_info) + sizeof(struct cam_cmd_probe);
  buf_desc[0].type = CAM_CMD_BUF_LEGACY;
  auto i2c_info = m->mem_mgr.alloc<struct cam_cmd_i2c_info>(buf_desc[0].size, (uint32_t*)&buf_desc[0].mem_handle);
  auto probe = (struct cam_cmd_probe *)(i2c_info.get() + 1);

  probe->camera_id = g8_sensor_slot;
  i2c_info->slave_addr = sensor->getSlaveAddress(g8_sensor_slot);
  // LG CamX asks FAST_PLUS (1 MHz). Stock comma remains FAST (400 kHz).
  i2c_info->i2c_freq_mode = g8_camera ? 3 : I2C_FAST_MODE;
  i2c_info->cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_INFO;

  probe->data_type = CAMERA_SENSOR_I2C_TYPE_WORD;
  probe->addr_type = CAMERA_SENSOR_I2C_TYPE_WORD;
  probe->op_code = 3;
  probe->cmd_type = CAMERA_SENSOR_CMD_TYPE_PROBE;
  probe->reg_addr = sensor->probe_reg_addr;
  probe->expected_data = sensor->probe_expected_data;
  probe->data_mask = 0;

  // IMX363/stock use the existing exact 196-byte packet. IMX351 has more
  // power commands, so give its backing buffer headroom and later set the
  // descriptor to the exact used length.
  const uint32_t power_alloc_size = (g8_wide || g8_driver || g8_driver_stream) ? 512 : 196;
  buf_desc[1].size = buf_desc[1].length = power_alloc_size;
  buf_desc[1].type = CAM_CMD_BUF_I2C;
  auto power_settings = m->mem_mgr.alloc<struct cam_cmd_power>(buf_desc[1].size, (uint32_t*)&buf_desc[1].mem_handle);
  struct cam_cmd_power *power = power_settings.get();

  if (g8_wide) {
    // Exact LG IMX351 CamX power-up:
    // CUSTOM_REG1=1 (1ms)
    // CUSTOM_GPIO1=0, CUSTOM_GPIO2=0, VANA=0, VDIG=0, VIO=0 (1ms)
    // STANDBY=1 (1ms), MCLK=19.2MHz (1ms), RESET=1 (1ms).
    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 6;
    power->power_settings[0].config_val_low = 1;
    power = power_set_wait(power, 1);

    power->count = 5;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 10;
    power->power_settings[0].config_val_low = 0;
    power->power_settings[1].power_seq_type = 11;
    power->power_settings[1].config_val_low = 0;
    power->power_settings[2].power_seq_type = 1;
    power->power_settings[2].config_val_low = 0;
    power->power_settings[3].power_seq_type = 2;
    power->power_settings[3].config_val_low = 0;
    power->power_settings[4].power_seq_type = 3;
    power->power_settings[4].config_val_low = 0;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 9;
    power->power_settings[0].config_val_low = 1;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 0;
    power->power_settings[0].config_val_low = sensor->mclk_frequency;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 8;
    power->power_settings[0].config_val_low = 1;
    power = power_set_wait(power, 1);

    // Probe happens here.

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    power->power_settings[0].power_seq_type = 8;
    power->power_settings[0].config_val_low = 0;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    power->power_settings[0].power_seq_type = 9;
    power->power_settings[0].config_val_low = 0;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    power->power_settings[0].power_seq_type = 0;
    power->power_settings[0].config_val_low = 0;
    power = power_set_wait(power, 1);

    power->count = 6;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    power->power_settings[0].power_seq_type = 3;
    power->power_settings[1].power_seq_type = 2;
    power->power_settings[2].power_seq_type = 1;
    power->power_settings[3].power_seq_type = 11;
    power->power_settings[4].power_seq_type = 10;
    power->power_settings[5].power_seq_type = 6;

    char *power_end = (char *)power + sizeof(struct cam_cmd_power) +
                      (power->count - 1) * sizeof(struct cam_power_settings);
    const uint32_t used = (uint32_t)(power_end - (char *)power_settings.get());
    if (used > power_alloc_size) {
      fprintf(stderr, "G8_IMX351_POWER_OVERFLOW used=%u alloc=%u\n", used, power_alloc_size);
      fflush(stderr);
      return -1;
    }
    buf_desc[1].size = buf_desc[1].length = used;
    fprintf(stderr, "G8_IMX351_POWER_BYTES=%u\n", used);
    fflush(stderr);

  } else if (g8_driver_stream || g8_driver) {
    // G8_IMX520_PROBE_V1
    // Exact LG CamX IMX520 power-up:
    // STANDBY=0; CUSTOM_GPIO1=1; VIO/VANA/VDIG=1 (1ms after VDIG);
    // RESET=1 (1ms); MCLK=19.2MHz (2ms); CUSTOM_GPIO2=1 (1ms).
    //
    // Exact power-down:
    // MCLK=0; RESET=0; VDIG/VANA/VIO=0; CUSTOM_GPIO2=0;
    // CUSTOM_GPIO1=0; STANDBY=1.

    power->count = 5;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 9;
    power->power_settings[0].config_val_low = 0;
    power->power_settings[1].power_seq_type = 10;
    power->power_settings[1].config_val_low = 1;
    power->power_settings[2].power_seq_type = 3;
    power->power_settings[2].config_val_low = 1;
    power->power_settings[3].power_seq_type = 1;
    power->power_settings[3].config_val_low = 1;
    power->power_settings[4].power_seq_type = 2;
    power->power_settings[4].config_val_low = 1;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 8;
    power->power_settings[0].config_val_low = 1;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 0;
    power->power_settings[0].config_val_low = sensor->mclk_frequency;
    power = power_set_wait(power, 2);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 11;
    power->power_settings[0].config_val_low = 1;
    power = power_set_wait(power, 1);

    // Probe happens here.

    power->count = 8;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    power->power_settings[0].power_seq_type = 0;
    power->power_settings[0].config_val_low = 0;
    power->power_settings[1].power_seq_type = 8;
    power->power_settings[1].config_val_low = 0;
    power->power_settings[2].power_seq_type = 2;
    power->power_settings[2].config_val_low = 0;
    power->power_settings[3].power_seq_type = 1;
    power->power_settings[3].config_val_low = 0;
    power->power_settings[4].power_seq_type = 3;
    power->power_settings[4].config_val_low = 0;
    power->power_settings[5].power_seq_type = 11;
    power->power_settings[5].config_val_low = 0;
    power->power_settings[6].power_seq_type = 10;
    power->power_settings[6].config_val_low = 0;
    power->power_settings[7].power_seq_type = 9;
    power->power_settings[7].config_val_low = 1;

    char *power_end = (char *)power + sizeof(struct cam_cmd_power) +
                      (power->count - 1) * sizeof(struct cam_power_settings);
    const uint32_t used = (uint32_t)(power_end - (char *)power_settings.get());
    if (used > power_alloc_size) {
      fprintf(stderr, "G8_IMX520_POWER_OVERFLOW used=%u alloc=%u\n", used, power_alloc_size);
      fflush(stderr);
      return -1;
    }
    buf_desc[1].size = buf_desc[1].length = used;
    fprintf(stderr, "G8_IMX520_POWER_BYTES=%u\n", used);
    fflush(stderr);

  } else if (g8_camera) {
    // LG SM8150 enum:
    // MCLK=0 VANA=1 VDIG=2 VIO=3 CUSTOM_REG1=6 RESET=8.
    //
    // Exact IMX363 CamX power-up:
    // CUSTOM_REG1=1 (1ms), VANA/VDIG/VIO (1ms after VIO),
    // RESET=1 (3ms), MCLK=19.2MHz (1ms).

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 6;
    power->power_settings[0].config_val_low = 1;
    power = power_set_wait(power, 1);

    power->count = 3;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 1;
    power->power_settings[1].power_seq_type = 2;
    power->power_settings[2].power_seq_type = 3;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 8;
    power->power_settings[0].config_val_low = 1;
    power = power_set_wait(power, 3);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 0;
    power->power_settings[0].config_val_low = sensor->mclk_frequency;
    power = power_set_wait(power, 1);

    // Probe happens here.

    // Exact LG power-down: MCLK (1ms), RESET=0 (1ms),
    // VIO, VDIG, VANA, CUSTOM_REG1.
    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    power->power_settings[0].power_seq_type = 0;
    power->power_settings[0].config_val_low = 0;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    power->power_settings[0].power_seq_type = 8;
    power->power_settings[0].config_val_low = 0;
    power = power_set_wait(power, 1);

    power->count = 4;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    power->power_settings[0].power_seq_type = 3;
    power->power_settings[1].power_seq_type = 2;
    power->power_settings[2].power_seq_type = 1;
    power->power_settings[3].power_seq_type = 6;
  } else {
    // Original comma sensor power sequence.
    power->count = 4;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 3;
    power->power_settings[1].power_seq_type = 1;
    power->power_settings[2].power_seq_type = 2;
    power->power_settings[3].power_seq_type = 8;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 0;
    power->power_settings[0].config_val_low = sensor->mclk_frequency;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    power->power_settings[0].power_seq_type = 8;
    power->power_settings[0].config_val_low = 1;
    power = power_set_wait(power, 34);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    power->power_settings[0].power_seq_type = 0;
    power->power_settings[0].config_val_low = 0;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    power->power_settings[0].power_seq_type = 8;
    power->power_settings[0].config_val_low = 1;
    power = power_set_wait(power, 1);

    power->count = 1;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    power->power_settings[0].power_seq_type = 8;
    power->power_settings[0].config_val_low = 0;
    power = power_set_wait(power, 1);

    power->count = 3;
    power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    power->power_settings[0].power_seq_type = 2;
    power->power_settings[1].power_seq_type = 1;
    power->power_settings[2].power_seq_type = 3;
  }

  int ret = do_cam_control(sensor_fd, CAM_SENSOR_PROBE_CMD, (void *)(uintptr_t)cam_packet_handle, 0);
  if (getenv("G8_AGNOS") != nullptr) {
    fprintf(stderr, "G8_SENSOR_PROBE_RET=%d\n", ret);
    fflush(stderr);
  }
  LOGD("probing the sensor: %d", ret);
  return ret;
}

void add_patch(struct cam_packet *pkt, int32_t dst_hdl, uint32_t dst_offset, int32_t src_hdl, uint32_t src_offset) {
  void *ptr = (char*)&pkt->payload + pkt->patch_offset;
  struct cam_patch_desc *p = (struct cam_patch_desc *)((unsigned char*)ptr + sizeof(struct cam_patch_desc)*pkt->num_patches);
  p->dst_buf_hdl = dst_hdl;
  p->src_buf_hdl = src_hdl;
  p->dst_offset = dst_offset;
  p->src_offset = src_offset;
  pkt->num_patches++;
};

void SpectraCamera::config_bps(int idx, int request_id) {
  /*
    Handles per-frame BPS config.
    * BPS = Bayer Processing Segment
  */

  bool needs_downscale = sensor->out_scale > 1;
  int num_io_cfgs = needs_downscale ? 3 : 2;
  int num_patches = needs_downscale ? 14 : 12;
  int size = sizeof(struct cam_packet) + sizeof(struct cam_cmd_buf_desc)*2 + sizeof(struct cam_buf_io_cfg)*num_io_cfgs;
  size += sizeof(struct cam_patch_desc)*num_patches;

  uint32_t cam_packet_handle = 0;
  auto pkt = m->mem_mgr.alloc<struct cam_packet>(size, &cam_packet_handle);

  pkt->header.op_code = CSLDeviceTypeBPS | CAM_ICP_OPCODE_BPS_UPDATE;
  pkt->header.request_id = request_id;
  pkt->header.size = size;

  typedef struct {
    struct {
      uint32_t ptr[2];
      uint32_t unknown[2];
    } frames[9];

    uint32_t unknown1;
    uint32_t unknown2;
    uint32_t unknown3;
    uint32_t unknown4;

    uint32_t cdm_addr;
    uint32_t cdm_size;
    uint32_t settings_addr;
    uint32_t striping_addr;
    uint32_t cdm_addr2;

    uint32_t req_id;
    uint64_t handle;
  } bps_tmp;

  typedef struct {
    uint32_t a;
    uint32_t n;
    unsigned base : 32;
    unsigned unused : 12;
    unsigned length : 20;
    uint32_t p;
    uint32_t u;
    uint32_t h;
    uint32_t b;
  } cdm_tmp;

  // *** cmd buf ***
  std::vector<uint32_t> patches;
  struct cam_cmd_buf_desc *buf_desc = (struct cam_cmd_buf_desc *)&pkt->payload;
  {
    pkt->num_cmd_buf = 2;
    pkt->kmd_cmd_buf_index = -1;
    pkt->kmd_cmd_buf_offset = 0;

    buf_desc[0].meta_data = 0;
    buf_desc[0].mem_handle = bps_cmd.handle;
    buf_desc[0].type = CAM_CMD_BUF_FW;
    buf_desc[0].offset = bps_cmd.aligned_size()*idx;

    buf_desc[0].length = sizeof(bps_tmp) + sizeof(cdm_tmp);
    buf_desc[0].size = buf_desc[0].length;

    // rest gets patched in
    bps_tmp *fp = (bps_tmp *)((unsigned char *)bps_cmd.ptr + buf_desc[0].offset);
    memset(fp, 0, buf_desc[0].length);
    fp->handle = (uint64_t)icp_dev_handle;
    fp->cdm_size = bps_cdm_striping_bl.size;   // this comes from the striping lib create call
    fp->req_id = 0; // why always 0?

    cdm_tmp *pa = (cdm_tmp *)((unsigned char *)fp + sizeof(bps_tmp));
    pa->a = 0;
    pa->n = 1;
    pa->p = 20;  // GENERIC
    pa->u = 0;
    pa->h = 0;
    pa->b = 0;
    pa->unused = 0;
    pa->base = 0; // this gets patched

    int cdm_len = 0;

    if (bps_lin_reg.size() == 0) {
      // set first knee pt to do BLC
      uint32_t new_knee[8];
      new_knee[0] = sensor->black_level << (14 - sensor->bits_per_pixel);
      for (int i = 0; i < 7; i++) {
        uint32_t pts = sensor->linearization_pts[i / 2];
        new_knee[i + 1] = (i % 2 == 0) ? (pts >> 16) : (pts & 0xffff);
      }
      for (int i = 0; i < 4; i++) {
        bps_lin_reg.push_back((new_knee[2*i + 1] << 16) | new_knee[2*i]);
      }
    }

    if (bps_ccm_reg.size() == 0) {
      for (int i = 0; i < 3; i++) {
        bps_ccm_reg.push_back(sensor->color_correct_matrix[i] | (sensor->color_correct_matrix[i+3] << 0x10));
        bps_ccm_reg.push_back(sensor->color_correct_matrix[i+6]);
      }
    }

    // white balance
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x2868, {
      0x04000400,
      0x00000400,
      0x00000000,
      0x00000000,
    });
    // debayer
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x2878, {
      0x00000080,
      0x00800066,
    });
    // linearization
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x1868, bps_lin_reg);
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x1878, bps_lin_reg);
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x1888, bps_lin_reg);
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x1898, bps_lin_reg);
    uint64_t addr;
    cdm_len += write_dmi((unsigned char *)bps_cdm_program_array.ptr + cdm_len, &addr, sensor->linearization_lut.size()*sizeof(uint32_t), 0x1808, 1, CAM_CDM_CMD_DMI);
    patches.push_back(addr - (uint64_t)bps_cdm_program_array.ptr);

    // color correction
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x2e68, bps_ccm_reg);

    // gamma
    for (uint8_t ch = 1; ch <= 3; ch++) {
      cdm_len += write_dmi((unsigned char *)bps_cdm_program_array.ptr + cdm_len, &addr, sensor->gamma_lut_rgb.size()*sizeof(uint32_t), 0x3208, ch, CAM_CDM_CMD_DMI);
      patches.push_back(addr - (uint64_t)bps_cdm_program_array.ptr);
    }

    cdm_len += build_common_ife_bps((unsigned char *)bps_cdm_program_array.ptr + cdm_len, cc, sensor.get(), patches, false);

    pa->length = cdm_len - 1;

    // *** second command ***
    // parsed by cam_icp_packet_generic_blob_handler
    struct isp_packet {
      uint32_t header;
      struct cam_icp_clk_bw_request clk;
    } __attribute__((packed)) tmp;
    tmp.header = CAM_ICP_CMD_GENERIC_BLOB_CLK;
    tmp.header |= (sizeof(cam_icp_clk_bw_request)) << 8;
    tmp.clk.budget_ns = 0x1fca058;
    tmp.clk.frame_cycles = sensor->frame_width * sensor->frame_height; // matches striping lib pixelCount
    tmp.clk.rt_flag = 0x0;
    tmp.clk.uncompressed_bw = 0x38512180;
    tmp.clk.compressed_bw = 0x38512180;

    buf_desc[1].size = sizeof(tmp);
    buf_desc[1].offset = 0;
    buf_desc[1].length = buf_desc[1].size - buf_desc[1].offset;
    buf_desc[1].type = CAM_CMD_BUF_GENERIC;
    buf_desc[1].meta_data = CAM_ICP_CMD_META_GENERIC_BLOB;
    auto buf2 = m->mem_mgr.alloc<uint32_t>(buf_desc[1].size, (uint32_t*)&buf_desc[1].mem_handle);
    memcpy(buf2.get(), &tmp, sizeof(tmp));
  }

  // *** io config ***
  pkt->num_io_configs = num_io_cfgs;
  pkt->io_configs_offset = sizeof(struct cam_cmd_buf_desc)*pkt->num_cmd_buf;
  struct cam_buf_io_cfg *io_cfg = (struct cam_buf_io_cfg *)((char*)&pkt->payload + pkt->io_configs_offset);
  {
    // input frame
    io_cfg[0].offsets[0] = 0;
    io_cfg[0].mem_handle[0] = buf_handle_raw[idx];

    io_cfg[0].planes[0] = (struct cam_plane_cfg){
      .width = sensor->frame_width,
      .height = sensor->frame_height + sensor->extra_height,
      .plane_stride = sensor->frame_stride,
      .slice_height = sensor->frame_height + sensor->extra_height,
    };
    io_cfg[0].format = sensor->mipi_format;
    io_cfg[0].color_space = CAM_COLOR_SPACE_BASE;
    io_cfg[0].color_pattern = 0x5;
    io_cfg[0].bpp = (sensor->mipi_format == CAM_FORMAT_MIPI_RAW_10 ? 0xa : 0xc);
    io_cfg[0].resource_type = CAM_ICP_BPS_INPUT_IMAGE;
    io_cfg[0].fence = sync_objs_ife[idx];
    io_cfg[0].direction = CAM_BUF_INPUT;
    io_cfg[0].subsample_pattern = 0x1;
    io_cfg[0].framedrop_pattern = 0x1;

    // output frame
    io_cfg[1].mem_handle[0] = buf_handle_yuv[idx];
    io_cfg[1].mem_handle[1] = buf_handle_yuv[idx];
    io_cfg[1].planes[0] = (struct cam_plane_cfg){
      .width = buf.out_img_width,
      .height = buf.out_img_height,
      .plane_stride = stride,
      .slice_height = y_height,
    };
    io_cfg[1].planes[1] = (struct cam_plane_cfg){
      .width = buf.out_img_width,
      .height = buf.out_img_height / 2,
      .plane_stride = stride,
      .slice_height = uv_height,
    };
    io_cfg[1].offsets[1] = ALIGNED_SIZE(io_cfg[1].planes[0].plane_stride*io_cfg[1].planes[0].slice_height, 0x1000);
    assert(io_cfg[1].offsets[1] == uv_offset);

    io_cfg[1].format = CAM_FORMAT_NV12;  // TODO: why is this 21 in the dump? should be 12
    io_cfg[1].color_space = CAM_COLOR_SPACE_BT601_FULL;
    io_cfg[1].resource_type = needs_downscale ? CAM_ICP_BPS_OUTPUT_IMAGE_REG1 : CAM_ICP_BPS_OUTPUT_IMAGE_FULL;
    io_cfg[1].fence = sync_objs_bps[idx];
    io_cfg[1].direction = CAM_BUF_OUTPUT;
    io_cfg[1].subsample_pattern = 0x1;
    io_cfg[1].framedrop_pattern = 0x1;

    if (needs_downscale) {
      // downscaling needs a full res placeholder
      uint32_t full_stride, full_y_h, full_uv_h, full_yuv_size;
      std::tie(full_stride, full_y_h, full_uv_h, full_yuv_size) = get_nv12_info(sensor->frame_width, sensor->frame_height);
      io_cfg[2].mem_handle[0] = bps_fullres_dummy.handle;
      io_cfg[2].mem_handle[1] = bps_fullres_dummy.handle;
      io_cfg[2].planes[0] = (struct cam_plane_cfg){
        .width = sensor->frame_width,
        .height = sensor->frame_height,
        .plane_stride = full_stride,
        .slice_height = full_y_h,
      };
      io_cfg[2].planes[1] = (struct cam_plane_cfg){
        .width = sensor->frame_width,
        .height = sensor->frame_height / 2,
        .plane_stride = full_stride,
        .slice_height = full_uv_h,
      };
      io_cfg[2].offsets[1] = ALIGNED_SIZE(full_stride * full_y_h, 0x1000);
      io_cfg[2].format = CAM_FORMAT_NV12;
      io_cfg[2].color_space = CAM_COLOR_SPACE_BT601_FULL;
      io_cfg[2].resource_type = CAM_ICP_BPS_OUTPUT_IMAGE_FULL;
      io_cfg[2].fence = sync_objs_bps[idx];
      io_cfg[2].direction = CAM_BUF_OUTPUT;
      io_cfg[2].subsample_pattern = 0x1;
      io_cfg[2].framedrop_pattern = 0x1;
    }
  }

  // *** patches ***
  // sets up kernel address translation for optional IFE LUTs
  {
    const bool has_linearization =
        !sensor->linearization_pts.empty() &&
        !sensor->linearization_lut.empty();

    const bool has_vignetting =
        cc.vignetting_correction &&
        !sensor->vignetting_lut.empty();

    const size_t expected_patches =
        (has_linearization ? 1U : 0U) +
        (has_vignetting ? 2U : 0U) +
        3U;

    assert(patches.size() == expected_patches || patches.empty());

    pkt->patch_offset =
        sizeof(struct cam_cmd_buf_desc) * pkt->num_cmd_buf +
        sizeof(struct cam_buf_io_cfg) * pkt->num_io_configs;

    if (!patches.empty()) {
      size_t patch_idx = 0;

      if (has_linearization) {
        add_patch(pkt.get(), ife_cmd.handle,
                  patches[patch_idx++],
                  ife_linearization_lut.handle, 0);
      }

      if (has_vignetting) {
        add_patch(pkt.get(), ife_cmd.handle,
                  patches[patch_idx++],
                  ife_vignetting_lut.handle, 0);

        add_patch(pkt.get(), ife_cmd.handle,
                  patches[patch_idx++],
                  ife_vignetting_lut.handle,
                  ife_vignetting_lut.size);
      }

      for (int i = 0; i < 3; i++) {
        add_patch(pkt.get(), ife_cmd.handle,
                  patches[patch_idx++],
                  ife_gamma_lut.handle,
                  ife_gamma_lut.size * i);
      }

      assert(patch_idx == patches.size());
    }
  }

  int ret = device_config(m->icp_fd, session_handle, icp_dev_handle, cam_packet_handle);
  assert(ret == 0);
}

void SpectraCamera::config_ife(int idx, int request_id, bool init) {
  /*
    Handles initial + per-frame IFE config.
    * IFE = Image Front End
  */
  int size = sizeof(struct cam_packet) + sizeof(struct cam_cmd_buf_desc)*2;
  size += sizeof(struct cam_patch_desc)*10;
  if (!init) {
    size += sizeof(struct cam_buf_io_cfg);
  }

  uint32_t cam_packet_handle = 0;
  auto pkt = m->mem_mgr.alloc<struct cam_packet>(size, &cam_packet_handle);

  if (!init) {
    pkt->header.op_code =  CSLDeviceTypeIFE | OpcodesIFEUpdate;  // 0xf000001
    pkt->header.request_id = request_id;
  } else {
    pkt->header.op_code = CSLDeviceTypeIFE | OpcodesIFEInitialConfig; // 0xf000000
    pkt->header.request_id = 1;
  }
  pkt->header.size = size;

  // *** cmd buf ***
  std::vector<uint32_t> patches;
  {
    struct cam_cmd_buf_desc *buf_desc = (struct cam_cmd_buf_desc *)&pkt->payload;
    pkt->num_cmd_buf = 2;

    // *** first command ***
    buf_desc[0].size = ife_cmd.size;
    buf_desc[0].length = 0;
    buf_desc[0].type = CAM_CMD_BUF_DIRECT;
    buf_desc[0].meta_data = CAM_ISP_PACKET_META_COMMON;
    buf_desc[0].mem_handle = ife_cmd.handle;
    buf_desc[0].offset = ife_cmd.aligned_size()*idx;

    // stream of IFE register writes
    bool is_raw = cc.output_type != ISP_IFE_PROCESSED;
    if (!is_raw) {
      if (init) {
        buf_desc[0].length = build_initial_config((unsigned char*)ife_cmd.ptr + buf_desc[0].offset, cc, sensor.get(), patches, buf.out_img_width, buf.out_img_height);
      } else {
        buf_desc[0].length = build_update((unsigned char*)ife_cmd.ptr + buf_desc[0].offset, cc, sensor.get(), patches);
      }
    }

    pkt->kmd_cmd_buf_offset = buf_desc[0].length;
    pkt->kmd_cmd_buf_index = 0;

    // *** second command ***
    // parsed by cam_isp_packet_generic_blob_handler
    struct isp_packet {
      uint32_t type_0;
      cam_isp_resource_hfr_config resource_hfr;

      uint32_t type_1;
      cam_isp_clock_config clock;
      uint64_t extra_rdi_hz[3];

      uint32_t type_2;
      cam_isp_bw_config bw;
      struct cam_isp_bw_vote extra_rdi_vote[6];
    } __attribute__((packed)) tmp;
    memset(&tmp, 0, sizeof(tmp));

    tmp.type_0 = CAM_ISP_GENERIC_BLOB_TYPE_HFR_CONFIG;
    tmp.type_0 |= sizeof(cam_isp_resource_hfr_config) << 8;
    static_assert(sizeof(cam_isp_resource_hfr_config) == 0x20);
    tmp.resource_hfr = {
      .num_ports = 1,
      .port_hfr_config[0] = {
        .resource_type = static_cast<uint32_t>(is_raw ? CAM_ISP_IFE_OUT_RES_RDI_0 : CAM_ISP_IFE_OUT_RES_FULL),
        .subsample_pattern = 1,
        .subsample_period = 0,
        .framedrop_pattern = 1,
        .framedrop_period = 0,
      }
    };

    tmp.type_1 = CAM_ISP_GENERIC_BLOB_TYPE_CLOCK_CONFIG;
    tmp.type_1 |= (sizeof(cam_isp_clock_config) + sizeof(tmp.extra_rdi_hz)) << 8;
    static_assert((sizeof(cam_isp_clock_config) + sizeof(tmp.extra_rdi_hz)) == 0x38);
    tmp.clock = {
      .usage_type = 1, // dual mode
      .num_rdi = 4,
      .left_pix_hz = 404000000,
      .right_pix_hz = 404000000,
      .rdi_hz[0] = 404000000,
    };

    tmp.type_2 = CAM_ISP_GENERIC_BLOB_TYPE_BW_CONFIG;
    tmp.type_2 |= (sizeof(cam_isp_bw_config) + sizeof(tmp.extra_rdi_vote)) << 8;
    static_assert((sizeof(cam_isp_bw_config) + sizeof(tmp.extra_rdi_vote)) == 0xe0);
    tmp.bw = {
      .usage_type = 1, // dual mode
      .num_rdi = 4,
      .left_pix_vote = {
        .resource_id = 0,
        .cam_bw_bps = 450000000,
        .ext_bw_bps = 450000000,
      },
      .rdi_vote[0] = {
        .resource_id = 0,
        .cam_bw_bps = 8706200000,
        .ext_bw_bps = 8706200000,
      },
    };

    static_assert(offsetof(struct isp_packet, type_2) == 0x60);

    buf_desc[1].size = sizeof(tmp);
    buf_desc[1].offset = !init ? 0x60 : 0;
    buf_desc[1].length = buf_desc[1].size - buf_desc[1].offset;
    buf_desc[1].type = CAM_CMD_BUF_GENERIC;
    buf_desc[1].meta_data = CAM_ISP_PACKET_META_GENERIC_BLOB_COMMON;
    auto buf2 = m->mem_mgr.alloc<uint32_t>(buf_desc[1].size, (uint32_t*)&buf_desc[1].mem_handle);
    memcpy(buf2.get(), &tmp, sizeof(tmp));
  }

  // *** io config ***
  if (!init) {
    // configure output frame
    pkt->num_io_configs = 1;
    pkt->io_configs_offset = sizeof(struct cam_cmd_buf_desc)*pkt->num_cmd_buf;

    struct cam_buf_io_cfg *io_cfg = (struct cam_buf_io_cfg *)((char*)&pkt->payload + pkt->io_configs_offset);
    if (cc.output_type != ISP_IFE_PROCESSED) {
      io_cfg[0].mem_handle[0] = buf_handle_raw[idx];
      io_cfg[0].planes[0] = (struct cam_plane_cfg){
        .width = sensor->frame_width,
        .height = sensor->frame_height,
        .plane_stride = sensor->frame_stride,
        .slice_height = sensor->frame_height + sensor->extra_height,
      };
      io_cfg[0].format = sensor->mipi_format;
      io_cfg[0].color_space = CAM_COLOR_SPACE_BASE;
      io_cfg[0].color_pattern = 0x5;
      io_cfg[0].bpp = (sensor->mipi_format == CAM_FORMAT_MIPI_RAW_10 ? 0xa : 0xc);
      io_cfg[0].resource_type = CAM_ISP_IFE_OUT_RES_RDI_0;
      io_cfg[0].fence = sync_objs_ife[idx];
      io_cfg[0].direction = CAM_BUF_OUTPUT;
      io_cfg[0].subsample_pattern = 0x1;
      io_cfg[0].framedrop_pattern = 0x1;
    } else {
      io_cfg[0].mem_handle[0] = buf_handle_yuv[idx];
      io_cfg[0].mem_handle[1] = buf_handle_yuv[idx];
      io_cfg[0].planes[0] = (struct cam_plane_cfg){
        .width = buf.out_img_width,
        .height = buf.out_img_height,
        .plane_stride = stride,
        .slice_height = y_height,
      };
      io_cfg[0].planes[1] = (struct cam_plane_cfg){
        .width = buf.out_img_width,
        .height = buf.out_img_height / 2,
        .plane_stride = stride,
        .slice_height = uv_height,
      };
      io_cfg[0].offsets[1] = uv_offset;
      io_cfg[0].format = CAM_FORMAT_NV12;
      io_cfg[0].color_space = 0;
      io_cfg[0].color_pattern = 0x0;
      io_cfg[0].bpp = 0;
      io_cfg[0].resource_type = CAM_ISP_IFE_OUT_RES_FULL;
      io_cfg[0].fence = sync_objs_ife[idx];
      io_cfg[0].direction = CAM_BUF_OUTPUT;
      io_cfg[0].subsample_pattern = 0x1;
      io_cfg[0].framedrop_pattern = 0x1;
    }
  }

  // *** patches ***
  // IFE DMI patches are optional for sensors without linearization/vignetting LUTs.
  {
    const bool has_linearization =
        !sensor->linearization_pts.empty() &&
        !sensor->linearization_lut.empty();

    const bool has_vignetting =
        cc.vignetting_correction &&
        !sensor->vignetting_lut.empty();

    const size_t expected_patches =
        (has_linearization ? 1U : 0U) +
        (has_vignetting ? 2U : 0U) +
        3U;  // RGB gamma

    assert(patches.size() == expected_patches || patches.empty());

    pkt->patch_offset =
        sizeof(struct cam_cmd_buf_desc) * pkt->num_cmd_buf +
        sizeof(struct cam_buf_io_cfg) * pkt->num_io_configs;

    if (!patches.empty()) {
      size_t patch_idx = 0;

      if (has_linearization) {
        add_patch(pkt.get(),
                  ife_cmd.handle,
                  patches[patch_idx++],
                  ife_linearization_lut.handle,
                  0);
      }

      if (has_vignetting) {
        add_patch(pkt.get(),
                  ife_cmd.handle,
                  patches[patch_idx++],
                  ife_vignetting_lut.handle,
                  0);

        add_patch(pkt.get(),
                  ife_cmd.handle,
                  patches[patch_idx++],
                  ife_vignetting_lut.handle,
                  ife_vignetting_lut.size);
      }

      for (int i = 0; i < 3; i++) {
        add_patch(pkt.get(),
                  ife_cmd.handle,
                  patches[patch_idx++],
                  ife_gamma_lut.handle,
                  ife_gamma_lut.size * i);
      }

      assert(patch_idx == patches.size());
    }
  }
  int ret = device_config(m->isp_fd, session_handle, isp_dev_handle, cam_packet_handle);
  assert(ret == 0);
}

void SpectraCamera::enqueue_frame(uint64_t request_id) {
  int i = request_id % ife_buf_depth;
  assert(sync_objs_ife[i] == 0);

  // create output fences
  struct cam_sync_info sync_create = {0};
  strcpy(sync_create.name, "NodeOutputPortFence");
  int ret = do_sync_control(m->cam_sync_fd, CAM_SYNC_CREATE, &sync_create, sizeof(sync_create));
  if (ret != 0) {
    LOGE("failed to create fence: %d %d", ret, sync_create.sync_obj);
  } else {
    sync_objs_ife[i] = sync_create.sync_obj;
  }

  if (icp_dev_handle > 0) {
    ret = do_cam_control(m->cam_sync_fd, CAM_SYNC_CREATE, &sync_create, sizeof(sync_create));
    if (ret != 0) {
      LOGE("failed to create fence: %d %d", ret, sync_create.sync_obj);
    } else {
      sync_objs_bps[i] = sync_create.sync_obj;
    }
  }

  // schedule request with camera request manager
  struct cam_req_mgr_sched_request req_mgr_sched_request = {0};
  req_mgr_sched_request.session_hdl = session_handle;
  req_mgr_sched_request.link_hdl = link_handle;
  req_mgr_sched_request.req_id = request_id;
  ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_SCHED_REQ, &req_mgr_sched_request, sizeof(req_mgr_sched_request));
  if (ret != 0) {
    LOGE("failed to schedule cam mgr request: %d %lu", ret, request_id);
  }

  // poke sensor, must happen after schedule
  sensors_poke(request_id);

  // submit request to IFE and BPS
  config_ife(i, request_id);
  if (cc.output_type == ISP_BPS_PROCESSED) config_bps(i, request_id);
}

void SpectraCamera::destroySyncObjectAt(int index) {
  auto destroy_sync_obj = [](int cam_sync_fd, int32_t &sync_obj) {
    if (sync_obj == 0) return;

    struct cam_sync_info sync_destroy = {.sync_obj = sync_obj};
    int ret = do_sync_control(cam_sync_fd, CAM_SYNC_DESTROY, &sync_destroy, sizeof(sync_destroy));
    if (ret != 0) {
      LOGE("Failed to destroy sync object: %d, sync_obj: %d", ret, sync_destroy.sync_obj);
    }

    sync_obj = 0;  // Reset the sync object to 0
  };

  destroy_sync_obj(m->cam_sync_fd, sync_objs_ife[index]);
  destroy_sync_obj(m->cam_sync_fd, sync_objs_bps[index]);
}

void SpectraCamera::camera_map_bufs() {
  int ret;
  for (int i = 0; i < ife_buf_depth; i++) {
    // map our VisionIPC bufs into ISP memory
    struct cam_mem_mgr_map_cmd mem_mgr_map_cmd = {0};
    mem_mgr_map_cmd.flags = CAM_MEM_FLAG_HW_READ_WRITE;
    mem_mgr_map_cmd.mmu_hdls[0] = m->device_iommu;
    mem_mgr_map_cmd.num_hdl = 1;
    if (icp_dev_handle > 0) {
      mem_mgr_map_cmd.num_hdl = 2;
      mem_mgr_map_cmd.mmu_hdls[1] = m->icp_device_iommu;
    }

    if (cc.output_type != ISP_IFE_PROCESSED) {
      // RAW bayer images
      mem_mgr_map_cmd.fd = buf.camera_bufs_raw[i].fd;
      ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_MAP_BUF, &mem_mgr_map_cmd, sizeof(mem_mgr_map_cmd));
      assert(ret == 0);
      LOGD("map buf req: (fd: %d) 0x%x %d", buf.camera_bufs_raw[i].fd, mem_mgr_map_cmd.out.buf_handle, ret);
      buf_handle_raw[i] = mem_mgr_map_cmd.out.buf_handle;
    }

    if (cc.output_type != ISP_RAW_OUTPUT) {
      // final processed images
      VisionBuf *vb = buf.vipc_server->get_buffer(buf.stream_type, i);
      mem_mgr_map_cmd.fd = vb->fd;
      ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_MAP_BUF, &mem_mgr_map_cmd, sizeof(mem_mgr_map_cmd));
      LOGD("map buf req: (fd: %d) 0x%x %d", vb->fd, mem_mgr_map_cmd.out.buf_handle, ret);
      buf_handle_yuv[i] = mem_mgr_map_cmd.out.buf_handle;
    }
  }
}

bool SpectraCamera::openSensor() {
  // G8_DUAL_ROAD_DRIVER_V2: normal logical DRIVER maps to physical slot 1.
  const bool g8_driver_stream =
      getenv("G8_AGNOS") != nullptr &&
      cc.stream_type == VISION_STREAM_DRIVER;
  const int physical_sensor_index = g8_driver_stream ? 1 : cc.camera_num;
  sensor_fd = open_v4l_by_name_and_index("cam-sensor-driver", physical_sensor_index);
  assert(sensor_fd >= 0);
  if (g8_driver_stream) {
    fprintf(stderr,
            "G8_IMX520_DRIVER_STREAM_V1 SENSOR_MAP logical_camera=%d physical_sensor=%d phy=%u\n",
            cc.camera_num, physical_sensor_index, cc.phy);
    fflush(stderr);
  }
  LOGD("opened sensor for %d", cc.camera_num);

  LOGD("-- Probing sensor %d", cc.camera_num);

  auto init_sensor_lambda = [this](SensorInfo *s) {
    sensor.reset(s);
    return (sensors_init() == 0);
  };

  // Figure out which sensor we have
  if (getenv("G8_AGNOS") != nullptr) {
    const bool g8_wide = getenv("G8_CAMERA_TARGET_WIDE") != nullptr;
    const bool g8_driver = getenv("G8_CAMERA_TARGET_DRIVER") != nullptr;

    if (g8_wide && g8_driver) {
      fprintf(stderr, "G8_CAMERA_TARGET_CONFLICT wide=1 driver=1\n");
      fflush(stderr);
      enabled = false;
      return false;
    }

    if (g8_driver_stream || g8_driver) {
      fprintf(stderr, "G8_IMX520_PROBE_V1 selecting IMX520 slot=%d\n", cc.camera_num);
      fflush(stderr);
      if (!init_sensor_lambda(new IMX520G8)) {
        LOGE("** G8 IMX520 sensor %d FAILED bringup, disabling", cc.camera_num);
        enabled = false;
        return false;
      }
    } else if (g8_wide) {
      fprintf(stderr, "G8_IMX351_PROBE_PATCH selecting IMX351 slot=%d\n", cc.camera_num);
      fflush(stderr);
      if (!init_sensor_lambda(new IMX351G8)) {
        LOGE("** G8 IMX351 sensor %d FAILED bringup, disabling", cc.camera_num);
        enabled = false;
        return false;
      }
    } else if (!init_sensor_lambda(new IMX363G8)) {
      LOGE("** G8 IMX363 sensor %d FAILED bringup, disabling", cc.camera_num);
      enabled = false;
      return false;
    }
  } else if (!init_sensor_lambda(new OS04C10) &&
             !init_sensor_lambda(new OX03C10)) {
    LOGE("** sensor %d FAILED bringup, disabling", cc.camera_num);
    enabled = false;
    return false;
  }
  LOGD("-- Probing sensor %d success", cc.camera_num);

  if (getenv("G8_CAMERA_PROBE_ONLY") != nullptr) {
    const char *target =
      getenv("G8_CAMERA_TARGET_DRIVER") != nullptr ? "IMX520" :
      (getenv("G8_CAMERA_TARGET_WIDE") != nullptr ? "IMX351" : "IMX363");
    fprintf(stderr, "G8_PROBE_ONLY_SUCCESS sensor=%d target=%s\n", cc.camera_num, target);
    fflush(stderr);
    enabled = false;
    return false;
  }

  // create session
  struct cam_req_mgr_session_info session_info = {};
  int ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_CREATE_SESSION, &session_info, sizeof(session_info));
  LOGD("get session: %d 0x%X", ret, session_info.session_hdl);
  session_handle = session_info.session_hdl;

  // G8_MAIN_FOCUS_ACQUIRE_V1
  // Safely prove that MAIN actuator slot 0 (/dev/v4l-subdev6)
  // can be acquired using the same request-manager session as IMX363.
  // No actuator CONFIG or I2C commands are sent here.
  if (getenv("G8_MAIN_FOCUS_ACQUIRE_TEST") != nullptr &&
      getenv("G8_AGNOS") != nullptr &&
      cc.camera_num == 0 &&
      getenv("G8_CAMERA_TARGET_WIDE") == nullptr &&
      getenv("G8_CAMERA_TARGET_DRIVER") == nullptr) {

    const char *g8_act_path = "/dev/v4l-subdev6";
    int g8_act_fd = open_v4l_by_name_and_index("cam-actuator-driver", 0);

    fprintf(stderr,
            "G8_MAIN_FOCUS_ACQUIRE_V1 OPEN path=%s fd=%d session=0x%X\n",
            g8_act_path, g8_act_fd, session_handle);
    fflush(stderr);

    if (g8_act_fd >= 0) {
      auto g8_act_handle =
          device_acquire(g8_act_fd, session_handle, nullptr);

      if (g8_act_handle) {
        fprintf(stderr,
                "G8_MAIN_FOCUS_ACQUIRE_V1 ACQUIRE_OK handle=0x%X\n",
                *g8_act_handle);
        fflush(stderr);

        int g8_act_release_ret =
            device_control(g8_act_fd, CAM_RELEASE_DEV,
                           session_handle, *g8_act_handle);

        fprintf(stderr,
                "G8_MAIN_FOCUS_ACQUIRE_V1 RELEASE ret=%d\n",
                g8_act_release_ret);
        fflush(stderr);
      } else {
        fprintf(stderr,
                "G8_MAIN_FOCUS_ACQUIRE_V1 ACQUIRE_FAIL\n");
        fflush(stderr);
      }

      close(g8_act_fd);
    }
  }

  // G8_MAIN_FOCUS_INIT_V1
  //
  // MAIN IMX363 actuator:
  //   slot 0 -> /dev/v4l-subdev6
  //   LG actuator = lc898219xi
  //
  // Stage AF-2:
  //   acquire actuator
  //   configure LG slave 0xE4 using CCI FAST_PLUS
  //   write init register 0xE0 = 0x01
  //   DO NOT write focus DAC register 0x84 yet.

  // access the sensor
  LOGD("-- Accessing sensor");
  auto sensor_dev_handle_ = device_acquire(sensor_fd, session_handle, nullptr);
  assert(sensor_dev_handle_);
  sensor_dev_handle = *sensor_dev_handle_;
  LOGD("acquire sensor dev");

  LOG("-- Configuring sensor");
  if (getenv("G8_CAMERA_CONFIG_ONLY") != nullptr) {
    const char *target =
      getenv("G8_CAMERA_TARGET_DRIVER") != nullptr ? "IMX520" :
      (getenv("G8_CAMERA_TARGET_WIDE") != nullptr ? "IMX351" : "IMX363");
    fprintf(stderr,
            "G8_IMX520_CONFIG_V1 BEGIN sensor=%d target=%s writes=%zu size=%ux%u\n",
            cc.camera_num, target, sensor->init_reg_array.size(),
            sensor->frame_width, sensor->frame_height);
    fflush(stderr);
  }

  sensors_i2c(sensor->init_reg_array.data(), sensor->init_reg_array.size(), CAM_SENSOR_PACKET_OPCODE_SENSOR_CONFIG, sensor->data_word);

  // G8_MAIN_FOCUS_AFTER_SENSOR_POWER_V1
  if (getenv("G8_AGNOS") != nullptr &&
      cc.camera_num == 0 &&
      getenv("G8_CAMERA_TARGET_WIDE") == nullptr &&
      getenv("G8_CAMERA_TARGET_DRIVER") == nullptr) {

    const char *g8_act_path = "/dev/v4l-subdev6";
    int g8_act_fd = open_v4l_by_name_and_index("cam-actuator-driver", 0);

    fprintf(stderr,
            "G8_MAIN_FOCUS_INIT_V1 OPEN path=%s fd=%d session=0x%X\n",
            g8_act_path, g8_act_fd, session_handle);
    fflush(stderr);

    if (g8_act_fd >= 0) {
      auto g8_act_handle =
          device_acquire(g8_act_fd, session_handle, nullptr);

      if (!g8_act_handle) {
        fprintf(stderr,
                "G8_MAIN_FOCUS_INIT_V1 ACQUIRE_FAIL\n");
        fflush(stderr);
      } else {
        fprintf(stderr,
                "G8_MAIN_FOCUS_INIT_V1 ACQUIRE_OK handle=0x%X\n",
                *g8_act_handle);
        fflush(stderr);

        // Qualcomm actuator packet opcodes:
        // INIT=0, AUTO_MOVE=1, MANUAL_MOVE=2.
        constexpr uint32_t G8_ACTUATOR_OPCODE_INIT = 0;

        uint32_t g8_act_packet_handle = 0;

        const int g8_act_packet_size =
            sizeof(struct cam_packet) +
            2 * sizeof(struct cam_cmd_buf_desc);

        auto g8_act_pkt =
            m->mem_mgr.alloc<struct cam_packet>(
                g8_act_packet_size,
                &g8_act_packet_handle);

        g8_act_pkt->header.op_code = G8_ACTUATOR_OPCODE_INIT;
        g8_act_pkt->header.size = g8_act_packet_size;
        g8_act_pkt->header.request_id = 0;
        g8_act_pkt->header.flags = 0;
        g8_act_pkt->header.padding = 0;

        g8_act_pkt->cmd_buf_offset = 0;
        g8_act_pkt->num_cmd_buf = 2;
        g8_act_pkt->io_configs_offset = 0;
        g8_act_pkt->num_io_configs = 0;
        g8_act_pkt->patch_offset = 0;
        g8_act_pkt->num_patches = 0;
        g8_act_pkt->kmd_cmd_buf_index = -1;
        g8_act_pkt->kmd_cmd_buf_offset = 0;

        auto *g8_desc =
            reinterpret_cast<struct cam_cmd_buf_desc *>(
                &g8_act_pkt->payload);

        // Command buffer 0: actuator slave/I2C information.
        g8_desc[0].offset = 0;
        g8_desc[0].size =
            g8_desc[0].length =
                sizeof(struct cam_cmd_i2c_info);
        g8_desc[0].type = CAM_CMD_BUF_I2C;
        g8_desc[0].meta_data = 0;

        auto g8_i2c_info =
            m->mem_mgr.alloc<struct cam_cmd_i2c_info>(
                g8_desc[0].size,
                reinterpret_cast<uint32_t *>(&g8_desc[0].mem_handle));

        g8_i2c_info->slave_addr = 0xE4;
        g8_i2c_info->i2c_freq_mode = 3;  // I2C_FAST_PLUS_MODE
        g8_i2c_info->cmd_type =
            CAMERA_SENSOR_CMD_TYPE_I2C_INFO;

        // Command buffer 1: LG LC898219XI initialization.
        // No focus-position/DAC write occurs here.
        g8_desc[1].offset = 0;
        g8_desc[1].size =
            g8_desc[1].length =
                sizeof(struct i2c_rdwr_header) +
                sizeof(struct i2c_random_wr_payload);
        g8_desc[1].type = CAM_CMD_BUF_I2C;
        g8_desc[1].meta_data = 0;

        auto g8_init =
            m->mem_mgr.alloc<struct cam_cmd_i2c_random_wr>(
                g8_desc[1].size,
                reinterpret_cast<uint32_t *>(&g8_desc[1].mem_handle));

        g8_init->header.count = 1;
        g8_init->header.op_code = 1;
        g8_init->header.cmd_type =
            CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR;
        g8_init->header.data_type =
            CAMERA_SENSOR_I2C_TYPE_BYTE;
        g8_init->header.addr_type =
            CAMERA_SENSOR_I2C_TYPE_BYTE;

        g8_init->random_wr_payload[0].reg_addr = 0xE0;
        g8_init->random_wr_payload[0].reg_data = 0x01;

        fprintf(stderr,
                "G8_MAIN_FOCUS_INIT_V1 CONFIG "
                "slave=0xE4 freq=3 reg=0xE0 val=0x01 "
                "packet_handle=0x%X\n",
                g8_act_packet_handle);
        fflush(stderr);

        errno = 0;
        int g8_act_cfg_ret =
            device_config(g8_act_fd,
                          session_handle,
                          *g8_act_handle,
                          g8_act_packet_handle);
        int g8_act_cfg_errno = errno;

        fprintf(stderr,
                "G8_MAIN_FOCUS_INIT_V2 CONFIG_RET=%d ERRNO=%d\n",
                g8_act_cfg_ret, g8_act_cfg_errno);
        fflush(stderr);
        // G8_MAIN_FOCUS_WAKE_8C_V1
        // LC898219XI reference wake sequence: after 0xE0=0x01,
        // allow wake time, then write 0x8C=0xE9. No DAC write yet.
        if (g8_act_cfg_ret == 0) {
          usleep(20000);

          // G8_MAIN_FOCUS_ID_F0_V1
          // Exact-byte identity/communication poll: LC898219XI register 0xF0 == 0xA5.
          constexpr uint8_t G8_WAIT_OP_COND = 1;

          const auto g8_write_mem_handle = g8_desc[1].mem_handle;
          const auto g8_write_offset = g8_desc[1].offset;
          const auto g8_write_size = g8_desc[1].size;
          const auto g8_write_length = g8_desc[1].length;
          const auto g8_write_type = g8_desc[1].type;
          const auto g8_write_meta_data = g8_desc[1].meta_data;

          uint32_t g8_f0_poll_handle = 0;
          auto g8_f0_poll =
              m->mem_mgr.alloc<struct cam_cmd_conditional_wait>(
                  sizeof(struct cam_cmd_conditional_wait),
                  &g8_f0_poll_handle);

          g8_f0_poll->data_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
          g8_f0_poll->addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
          g8_f0_poll->op_code = G8_WAIT_OP_COND;
          g8_f0_poll->cmd_type = CAMERA_SENSOR_CMD_TYPE_WAIT;
          g8_f0_poll->timeout = 10;
          g8_f0_poll->reserved = 0;
          g8_f0_poll->reg_addr = 0xF0;
          g8_f0_poll->reg_data = 0xA5;
          g8_f0_poll->data_mask = 0xFF;

          g8_desc[1].mem_handle = g8_f0_poll_handle;
          g8_desc[1].offset = 0;
          g8_desc[1].size = sizeof(struct cam_cmd_conditional_wait);
          g8_desc[1].length = sizeof(struct cam_cmd_conditional_wait);
          g8_desc[1].type = CAM_CMD_BUF_I2C;
          g8_desc[1].meta_data = 0;

          g8_act_pkt->header.op_code = 1;  // CAM_ACTUATOR_PACKET_AUTO_MOVE_LENS
          g8_act_pkt->header.request_id = 1;
          g8_act_pkt->cmd_buf_offset = sizeof(struct cam_cmd_buf_desc);
          g8_act_pkt->num_cmd_buf = 1;

          errno = 0;
          int g8_f0_poll_ret =
              device_config(g8_act_fd,
                            session_handle,
                            *g8_act_handle,
                            g8_act_packet_handle);
          int g8_f0_poll_errno = errno;

          fprintf(stderr,
                  "G8_MAIN_FOCUS_ID_F0_V1 RET=%d ERRNO=%d reg=0xF0 expected=0xA5 timeout=10\n",
                  g8_f0_poll_ret, g8_f0_poll_errno);
          fflush(stderr);

          // Restore descriptor 1 to the known-good one-byte actuator write buffer.
          g8_desc[1].mem_handle = g8_write_mem_handle;
          g8_desc[1].offset = g8_write_offset;
          g8_desc[1].size = g8_write_size;
          g8_desc[1].length = g8_write_length;
          g8_desc[1].type = g8_write_type;
          g8_desc[1].meta_data = g8_write_meta_data;

          // G8_MAIN_FOCUS_WAKE_B3_EXACT_V1
          // Conservative exact-byte readiness check.
          // The reference driver only requires (B3 & 0xE0) == 0.
          // Our Qualcomm WAIT parser does not reliably expose mask semantics,
          // so B3 == 0x00 is a stronger sufficient condition, not a necessary one.
          uint32_t g8_b3_poll_handle = 0;
          auto g8_b3_poll =
              m->mem_mgr.alloc<struct cam_cmd_conditional_wait>(
                  sizeof(struct cam_cmd_conditional_wait),
                  &g8_b3_poll_handle);

          g8_b3_poll->data_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
          g8_b3_poll->addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
          g8_b3_poll->op_code = G8_WAIT_OP_COND;
          g8_b3_poll->cmd_type = CAMERA_SENSOR_CMD_TYPE_WAIT;
          g8_b3_poll->timeout = 10;
          g8_b3_poll->reserved = 0;
          g8_b3_poll->reg_addr = 0xB3;
          g8_b3_poll->reg_data = 0x00;
          g8_b3_poll->data_mask = 0xFF;

          g8_desc[1].mem_handle = g8_b3_poll_handle;
          g8_desc[1].offset = 0;
          g8_desc[1].size = sizeof(struct cam_cmd_conditional_wait);
          g8_desc[1].length = sizeof(struct cam_cmd_conditional_wait);
          g8_desc[1].type = CAM_CMD_BUF_I2C;
          g8_desc[1].meta_data = 0;

          g8_act_pkt->header.op_code = 1;  // CAM_ACTUATOR_PACKET_AUTO_MOVE_LENS
          g8_act_pkt->header.request_id = 2;
          g8_act_pkt->cmd_buf_offset = sizeof(struct cam_cmd_buf_desc);
          g8_act_pkt->num_cmd_buf = 1;

          errno = 0;
          int g8_b3_poll_ret =
              device_config(g8_act_fd,
                            session_handle,
                            *g8_act_handle,
                            g8_act_packet_handle);
          int g8_b3_poll_errno = errno;

          fprintf(stderr,
                  "G8_MAIN_FOCUS_WAKE_B3_EXACT_V1 RET=%d ERRNO=%d reg=0xB3 expected=0x00 timeout=10\n",
                  g8_b3_poll_ret, g8_b3_poll_errno);
          fflush(stderr);

          // Restore descriptor 1 again for the known-good 0x8C write.
          g8_desc[1].mem_handle = g8_write_mem_handle;
          g8_desc[1].offset = g8_write_offset;
          g8_desc[1].size = g8_write_size;
          g8_desc[1].length = g8_write_length;
          g8_desc[1].type = g8_write_type;
          g8_desc[1].meta_data = g8_write_meta_data;

          // Reuse descriptor 1 as a one-command AUTO_MOVE packet.
          // AUTO_MOVE is applied immediately once actuator state is CONFIG.
          g8_act_pkt->header.op_code = 1;  // CAM_ACTUATOR_PACKET_AUTO_MOVE_LENS
          g8_act_pkt->header.request_id = 3;
          g8_act_pkt->cmd_buf_offset = sizeof(struct cam_cmd_buf_desc);
          g8_act_pkt->num_cmd_buf = 1;

          g8_init->header.count = 1;
          g8_init->header.op_code = 1;
          g8_init->header.cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR;
          g8_init->header.data_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
          g8_init->header.addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
          g8_init->random_wr_payload[0].reg_addr = 0x8C;
          g8_init->random_wr_payload[0].reg_data = 0xE9;

          errno = 0;
          int g8_act_wake_ret =
              device_config(g8_act_fd,
                            session_handle,
                            *g8_act_handle,
                            g8_act_packet_handle);
          int g8_act_wake_errno = errno;

          fprintf(stderr,
                  "G8_MAIN_FOCUS_WAKE_8C_V1 RET=%d ERRNO=%d reg=0x8C val=0xE9\n",
                  g8_act_wake_ret, g8_act_wake_errno);
          fflush(stderr);

          // G8_MAIN_FOCUS_DAC300_V1
          // Permanent fixed MAIN road-camera lens position.
          // Fixed code 100. LC898219XI DAC register 0x84 is WORD data.
          if (g8_f0_poll_ret == 0 &&
              g8_b3_poll_ret == 0 &&
              g8_act_wake_ret == 0) {
            usleep(10000);

            g8_act_pkt->header.op_code = 1;  // CAM_ACTUATOR_PACKET_AUTO_MOVE_LENS
            g8_act_pkt->header.request_id = 4;
            g8_act_pkt->cmd_buf_offset = sizeof(struct cam_cmd_buf_desc);
            g8_act_pkt->num_cmd_buf = 1;

            g8_init->header.count = 1;
            g8_init->header.op_code = 1;
            g8_init->header.cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR;
            g8_init->header.data_type = CAMERA_SENSOR_I2C_TYPE_WORD;
            g8_init->header.addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
            // G8_MAIN_FIXED_FOCUS_V1
            // comma-style fixed road focus: initialize once, then never hunt while driving.
            constexpr uint32_t G8_MAIN_FIXED_FOCUS_CODE = 100;
            g8_init->random_wr_payload[0].reg_addr = 0x84;
            g8_init->random_wr_payload[0].reg_data = G8_MAIN_FIXED_FOCUS_CODE;

            errno = 0;
            int g8_dac100_ret =
                device_config(g8_act_fd,
                              session_handle,
                              *g8_act_handle,
                              g8_act_packet_handle);
            int g8_dac100_errno = errno;

            fprintf(stderr,
                    "G8_MAIN_FIXED_FOCUS_V1 RET=%d ERRNO=%d reg=0x84 data=0x%04X code=%d\n",
                    g8_dac100_ret, g8_dac100_errno, (unsigned)G8_MAIN_FIXED_FOCUS_CODE, (int)G8_MAIN_FIXED_FOCUS_CODE);
            fflush(stderr);

            usleep(20000);
          }
        }

        // G8_MAIN_FOCUS_CONFIG_RETRY_V1
        // The INIT write is idempotent (0xE0=0x01). If Qualcomm CCI returns
        // EAGAIN after the actuator has already reached CONFIG, wait 10 ms
        // and submit the same INIT packet once more.
        if (g8_act_cfg_ret != 0 && g8_act_cfg_errno == EAGAIN) {
          usleep(10000);

          errno = 0;
          int g8_act_cfg_retry_ret =
              device_config(g8_act_fd,
                            session_handle,
                            *g8_act_handle,
                            g8_act_packet_handle);
          int g8_act_cfg_retry_errno = errno;

          fprintf(stderr,
                  "G8_MAIN_FOCUS_CONFIG_RETRY_V1 RET=%d ERRNO=%d\n",
                  g8_act_cfg_retry_ret, g8_act_cfg_retry_errno);
          fflush(stderr);

          if (g8_act_cfg_retry_ret == 0) {
            g8_act_cfg_ret = 0;
            g8_act_cfg_errno = 0;
          }
        }

        // G8_MAIN_FOCUS_STATE_PROBE_V1
        //
        // CONFIG failed. Probe the actuator state without issuing any
        // lens-position command. On the newer Qualcomm actuator state
        // machine, CAM_START_DEV succeeds only after INIT reached CONFIG.
        if (g8_act_cfg_ret != 0) {
          errno = 0;
          int g8_act_start_ret =
              device_control(g8_act_fd,
                             CAM_START_DEV,
                             session_handle,
                             *g8_act_handle);
          int g8_act_start_errno = errno;

          fprintf(stderr,
                  "G8_MAIN_FOCUS_STATE_PROBE_V1 START_RET=%d ERRNO=%d\n",
                  g8_act_start_ret, g8_act_start_errno);
          fflush(stderr);

          // Restore state if START succeeded.
          if (g8_act_start_ret == 0) {
            errno = 0;
            int g8_act_stop_ret =
                device_control(g8_act_fd,
                               CAM_STOP_DEV,
                               session_handle,
                               *g8_act_handle);
            int g8_act_stop_errno = errno;

            fprintf(stderr,
                    "G8_MAIN_FOCUS_STATE_PROBE_V1 STOP_RET=%d ERRNO=%d\n",
                    g8_act_stop_ret, g8_act_stop_errno);
            fflush(stderr);
          }
        }

        // LG donor specifies ~1000 us following this init write.
        usleep(1000);

        int g8_act_release_ret =
            device_control(g8_act_fd,
                           CAM_RELEASE_DEV,
                           session_handle,
                           *g8_act_handle);

        fprintf(stderr,
                "G8_MAIN_FOCUS_INIT_V1 RELEASE ret=%d\n",
                g8_act_release_ret);
        fflush(stderr);
      }

      ::close(g8_act_fd);
    }
  }


  if (getenv("G8_CAMERA_CONFIG_ONLY") != nullptr) {
    const char *target =
      getenv("G8_CAMERA_TARGET_DRIVER") != nullptr ? "IMX520" :
      (getenv("G8_CAMERA_TARGET_WIDE") != nullptr ? "IMX351" : "IMX363");
    fprintf(stderr,
            "G8_IMX520_CONFIG_V1 DONE sensor=%d target=%s enabled=%d writes=%zu size=%ux%u\n",
            cc.camera_num, target, enabled ? 1 : 0, sensor->init_reg_array.size(),
            sensor->frame_width, sensor->frame_height);
    fflush(stderr);

    // Stage 2 boundary: sensor is acquired and init/mode registers are
    // programmed, but camera_open() must not reach ISP/CSIPHY/link/stream.
    enabled = false;
    return false;
  }

  return true;
}

void SpectraCamera::configISP() {
  if (!enabled) return;

  const bool g8_driver_isp =
    getenv("G8_AGNOS") != nullptr &&
    (cc.stream_type == VISION_STREAM_DRIVER ||
     getenv("G8_CAMERA_TARGET_DRIVER") != nullptr);

  struct cam_isp_in_port_info in_port_info = {
    // ISP input to the CSID
    .res_type = cc.phy,
    .lane_type = CAM_ISP_LANE_TYPE_DPHY,
    // G8_IMX520_CSID_2LANE_V1
    // LG IMX520 mode is 2-lane D-PHY. Keep all proven rear cameras at 4 lanes.
    .lane_num = static_cast<uint32_t>(g8_driver_isp ? 2 : 4),
    // Stock LG sensormodule IMX520 laneAssign decodes exactly to 0x3210.
    .lane_cfg = 0x3210,

    .vc = 0x0,
    .dt = sensor->frame_data_type,
    .format = sensor->mipi_format,

    .test_pattern = sensor->bayer_pattern,
    .usage_type = 0x0,

    .left_start = 0,
    .left_stop = sensor->frame_width - 1,
    .left_width = sensor->frame_width,

    .right_start = 0,
    .right_stop = sensor->frame_width - 1,
    .right_width = sensor->frame_width,

    .line_start = sensor->frame_offset,
    .line_stop = sensor->frame_height + sensor->frame_offset - 1,
    .height = sensor->frame_height + sensor->frame_offset,

    .pixel_clk = 0x0,
    .batch_size = 0x0,
    .dsp_mode = CAM_ISP_DSP_MODE_NONE,
    .hbi_cnt = 0x0,

    // ISP outputs
    .num_out_res = 0x1,
    .data[0] = (struct cam_isp_out_port_info){
      .res_type = CAM_ISP_IFE_OUT_RES_FULL,
      .format = CAM_FORMAT_NV12,
      .width = buf.out_img_width,
      .height = buf.out_img_height + sensor->extra_height,
      .comp_grp_id = 0x0, .split_point = 0x0, .secure_mode = 0x0,
    },
  };

  if (g8_driver_isp &&
      (getenv("G8_CAMERA_FIRST_SOF") != nullptr ||
       getenv("G8_CAMERA_FIRST_IFE") != nullptr ||
       getenv("G8_CAMERA_STREAMON_ONLY") != nullptr)) {
    fprintf(stderr,
            "G8_IMX520_CSID_2LANE_V1 ISP_INPUT sensor=%d ife_phy=%u lane_num=%u lane_cfg=0x%X dt=0x%X format=%u size=%ux%u\n",
            cc.camera_num,
            cc.phy,
            in_port_info.lane_num,
            in_port_info.lane_cfg,
            in_port_info.dt,
            in_port_info.format,
            sensor->frame_width,
            sensor->frame_height);
    fflush(stderr);
  }

  if (cc.output_type != ISP_IFE_PROCESSED) {
    in_port_info.line_start = 0;
    in_port_info.line_stop = sensor->frame_height + sensor->extra_height - 1;
    in_port_info.height = sensor->frame_height + sensor->extra_height;

    in_port_info.data[0].res_type = CAM_ISP_IFE_OUT_RES_RDI_0;
    in_port_info.data[0].format = sensor->mipi_format;
  }

  struct cam_isp_resource isp_resource = {
    .resource_id = CAM_ISP_RES_ID_PORT,
    .handle_type = CAM_HANDLE_USER_POINTER,
    .res_hdl = (uint64_t)&in_port_info,
    .length = sizeof(in_port_info),
  };

  auto isp_dev_handle_ = device_acquire(m->isp_fd, session_handle, &isp_resource);
  assert(isp_dev_handle_);
  isp_dev_handle = *isp_dev_handle_;
  LOGD("acquire isp dev");
  if (getenv("G8_CAMERA_PHY_ONLY") != nullptr) {
    fprintf(stderr, "G8_ISP_ACQUIRE_OK handle=0x%X\n", isp_dev_handle);
    fflush(stderr);
  }

  // allocate IFE memory, then configure it
  ife_cmd.init(m, 67984, 0x20, false, m->device_iommu, m->cdm_iommu, ife_buf_depth);
  if (cc.output_type == ISP_IFE_PROCESSED) {
    assert(sensor->gamma_lut_rgb.size() == 64);
    ife_gamma_lut.init(m, sensor->gamma_lut_rgb.size()*sizeof(uint32_t), 0x20, false, m->device_iommu, m->cdm_iommu, 3); // 3 for RGB
    for (int i = 0; i < 3; i++) {
      memcpy(ife_gamma_lut.ptr + ife_gamma_lut.size*i, sensor->gamma_lut_rgb.data(), ife_gamma_lut.size);
    }
    const bool has_linearization =
        !sensor->linearization_pts.empty() &&
        !sensor->linearization_lut.empty();

    const bool has_vignetting =
        cc.vignetting_correction &&
        !sensor->vignetting_lut.empty();

    if (has_linearization) {
      assert(sensor->linearization_pts.size() == 4);
      assert(sensor->linearization_lut.size() == 36);

      ife_linearization_lut.init(
          m,
          sensor->linearization_lut.size() * sizeof(uint32_t),
          0x20, false,
          m->device_iommu, m->cdm_iommu);

      memcpy(ife_linearization_lut.ptr,
             sensor->linearization_lut.data(),
             ife_linearization_lut.size);
    }

    if (has_vignetting) {
      assert(sensor->vignetting_lut.size() == 221);

      ife_vignetting_lut.init(
          m,
          sensor->vignetting_lut.size() * sizeof(uint32_t),
          0x20, false,
          m->device_iommu, m->cdm_iommu, 2);

      for (int i = 0; i < 2; i++) {
        memcpy(ife_vignetting_lut.ptr +
                   ife_vignetting_lut.size * i,
               sensor->vignetting_lut.data(),
               ife_vignetting_lut.size);
      }
    }
  }

  config_ife(0, 1, true);
  if (getenv("G8_CAMERA_PHY_ONLY") != nullptr) {
    fprintf(stderr, "G8_IFE_CONFIG_DONE\n");
    fflush(stderr);
  }
}

void SpectraCamera::configICP() {
  /*
    Configures both the ICP and BPS.
  */

  int cfg_handle;

  uint32_t cfg_size = sizeof(bps_cfg[0]) / sizeof(bps_cfg[0][0]);
  void *cfg = alloc_w_mmu_hdl(m->video0_fd, cfg_size, (uint32_t*)&cfg_handle, 0x1,
                              CAM_MEM_FLAG_HW_READ_WRITE | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_HW_SHARED_ACCESS,
                              m->icp_device_iommu);
  memcpy(cfg, bps_cfg[sensor->num()], cfg_size);

  struct cam_icp_acquire_dev_info icp_info = {
    .scratch_mem_size = 0x0,
    .dev_type = CAM_ICP_RES_TYPE_BPS,
    .io_config_cmd_size = cfg_size,
    .io_config_cmd_handle = cfg_handle,
    .secure_mode = 0,
    .num_out_res = 1,
    .in_res = (struct cam_icp_res_info){
      .format = 0x9,  // RAW MIPI
      .width = sensor->frame_width,
      .height = sensor->frame_height,
      .fps = 20,
    },
    .out_res[0] = (struct cam_icp_res_info){
      .format = 0x3,  // YUV420NV12
      .width = buf.out_img_width,
      .height = buf.out_img_height,
      .fps = 20,
    },
  };
  auto h = device_acquire(m->icp_fd, session_handle, &icp_info);
  assert(h);
  icp_dev_handle = *h;
  LOGD("acquire icp dev");

  release(m->video0_fd, cfg_handle);

  // BPS has a lot of buffers to init
  bps_cmd.init(m, 464, 0x20, true, m->icp_device_iommu, 0, ife_buf_depth);

  // BPSIQSettings struct
  uint32_t settings_size = sizeof(bps_settings[0]) / sizeof(bps_settings[0][0]);
  bps_iq.init(m, settings_size, 0x20, true, m->icp_device_iommu);
  memcpy(bps_iq.ptr, bps_settings[sensor->num()], settings_size);

  // for cdm register writes, just make it bigger than you need
  bps_cdm_program_array.init(m, 0x1000, 0x20, true, m->icp_device_iommu);

  // striping lib output
  uint32_t striping_size = sizeof(bps_striping_output[0]) / sizeof(bps_striping_output[0][0]);
  bps_striping.init(m, striping_size, 0x20, true, m->icp_device_iommu);
  memcpy(bps_striping.ptr, bps_striping_output[sensor->num()], striping_size);

  // used internally by the BPS, we just allocate it.
  // size comes from the BPSStripingLib
  bps_cdm_striping_bl.init(m, 0xcfe0, 0x20, true, m->icp_device_iommu);

  if (sensor->out_scale > 1) {
    uint32_t full_stride, full_y_h, full_uv_h, full_yuv_size;
    std::tie(full_stride, full_y_h, full_uv_h, full_yuv_size) = get_nv12_info(sensor->frame_width, sensor->frame_height);
    bps_fullres_dummy.init(m, full_yuv_size, 0x1000, true, m->icp_device_iommu);
  }

  // LUTs
  assert(sensor->linearization_lut.size() == 36);
  bps_linearization_lut.init(m, sensor->linearization_lut.size()*sizeof(uint32_t), 0x20, true, m->icp_device_iommu);

  // bit shift linearization_lut to bps specs, also compensate for black level here
  uint32_t bl = sensor->black_level << (14 - sensor->bits_per_pixel);
  uint32_t* bps_lut = (uint32_t*)bps_linearization_lut.ptr;
  for (size_t i = 0; i < sensor->linearization_lut.size(); i++) {
    size_t seg = i / 4;
    size_t ch = i % 4;
    if (seg == 0) {
      bps_lut[i] = 0;
      continue;
    }
    uint32_t e = sensor->linearization_lut[(seg - 1) * 4 + ch];
    uint32_t base = e & 0x3fff;
    uint32_t slope_q11 = (e >> 14) & 0x3fff;
    uint32_t slope_q12 = std::min<uint32_t>(slope_q11 << 1, 0x3fff);
    base = (base > bl) ? (base - bl) : 0;
    bps_lut[i] = base | (slope_q12 << 14);
  }

  assert(sensor->gamma_lut_rgb.size() == 64);
  bps_gamma_lut.init(m, sensor->gamma_lut_rgb.size()*sizeof(uint32_t), 0x20, true, m->icp_device_iommu);
  memcpy(bps_gamma_lut.ptr, sensor->gamma_lut_rgb.data(), bps_gamma_lut.size);
}

void SpectraCamera::configCSIPHY() {
  int csiphy_index = cc.camera_num;
  if (getenv("G8_AGNOS") != nullptr) {
    if (getenv("G8_CAMERA_TARGET_WIDE") != nullptr) {
      csiphy_index = 1;
    } else if (getenv("G8_CAMERA_TARGET_DRIVER") != nullptr ||
               cc.stream_type == VISION_STREAM_DRIVER) {
      csiphy_index = 2;
    } else {
      csiphy_index = 0;
    }
  }

  csiphy_fd = open_v4l_by_name_and_index("cam-csiphy-driver", csiphy_index);
  assert(csiphy_fd >= 0);
  LOGD("opened csiphy for camera %d at physical index %d", cc.camera_num, csiphy_index);
  if (getenv("G8_CAMERA_PHY_ONLY") != nullptr || getenv("G8_CAMERA_START_ONLY") != nullptr) {
    const char *target =
      getenv("G8_CAMERA_TARGET_DRIVER") != nullptr ? "IMX520" :
      (getenv("G8_CAMERA_TARGET_WIDE") != nullptr ? "IMX351" : "IMX363");
    fprintf(stderr,
            "G8_IMX520_START_V1 CSIPHY_OPEN target=%s camera_num=%d index=%d ife_phy=%u\n",
            target, cc.camera_num, csiphy_index, cc.phy);
    fflush(stderr);
  }

  struct cam_csiphy_acquire_dev_info csiphy_acquire_dev_info = {.combo_mode = 0};
  auto csiphy_dev_handle_ = device_acquire(csiphy_fd, session_handle, &csiphy_acquire_dev_info);
  assert(csiphy_dev_handle_);
  csiphy_dev_handle = *csiphy_dev_handle_;
  LOGD("acquire csiphy dev");
  if (getenv("G8_CAMERA_PHY_ONLY") != nullptr) {
    fprintf(stderr, "G8_CSIPHY_ACQUIRE_OK handle=0x%X\n", csiphy_dev_handle);
    fflush(stderr);
  }

  // config csiphy
  LOG("-- Config CSI PHY");
  {
    uint32_t cam_packet_handle = 0;
    int size = sizeof(struct cam_packet)+sizeof(struct cam_cmd_buf_desc)*1;
    auto pkt = m->mem_mgr.alloc<struct cam_packet>(size, &cam_packet_handle);
    pkt->num_cmd_buf = 1;
    pkt->kmd_cmd_buf_index = -1;
    pkt->header.size = size;
    struct cam_cmd_buf_desc *buf_desc = (struct cam_cmd_buf_desc *)&pkt->payload;

    buf_desc[0].size = buf_desc[0].length = sizeof(struct cam_csiphy_info);
    buf_desc[0].type = CAM_CMD_BUF_GENERIC;

    auto csiphy_info = m->mem_mgr.alloc<struct cam_csiphy_info>(buf_desc[0].size, (uint32_t*)&buf_desc[0].mem_handle);
    const bool g8_driver_phy =
      getenv("G8_AGNOS") != nullptr &&
      (cc.stream_type == VISION_STREAM_DRIVER ||
       getenv("G8_CAMERA_TARGET_DRIVER") != nullptr);
    const bool g8_wide_phy =
      getenv("G8_AGNOS") != nullptr && getenv("G8_CAMERA_TARGET_WIDE") != nullptr;

    csiphy_info->lane_mask = g8_driver_phy ? 0x7 : 0x1f;
    // Keep the already-proven LG physical mapping. With lane_cnt=2/mask=0x7,
    // only the low two data-lane assignments (0,1) are active for IMX520.
    csiphy_info->lane_assign = 0x3210;
    csiphy_info->csiphy_3phase = 0x0;
    csiphy_info->combo_mode = 0x0;
    csiphy_info->lane_cnt = g8_driver_phy ? 0x2 : 0x4;
    csiphy_info->secure_mode = 0x0;
    csiphy_info->settle_time = MIPI_SETTLE_CNT * 200000000ULL;
    csiphy_info->data_rate =
      g8_driver_phy ? 168800000ULL :
      (g8_wide_phy ? 82500000ULL : 48000000ULL);

    if (getenv("G8_CAMERA_PHY_ONLY") != nullptr || getenv("G8_CAMERA_START_ONLY") != nullptr) {
      const char *target = g8_driver_phy ? "IMX520" : (g8_wide_phy ? "IMX351" : "IMX363");
      fprintf(stderr,
              "G8_IMX520_PHY_V1 CSIPHY_PARAMS target=%s lanes=%u mask=0x%X assign=0x%X data_rate=%llu\n",
              target,
              csiphy_info->lane_cnt,
              csiphy_info->lane_mask,
              csiphy_info->lane_assign,
              (unsigned long long)csiphy_info->data_rate);
      fflush(stderr);
    }

    int ret_ = device_config(csiphy_fd, session_handle, csiphy_dev_handle, cam_packet_handle);
    if (getenv("G8_CAMERA_PHY_ONLY") != nullptr) {
      fprintf(stderr, "G8_CSIPHY_CONFIG_RET=%d\n", ret_);
      fflush(stderr);
    }
    assert(ret_ == 0);
  }
}

void SpectraCamera::linkDevices() {
  LOG("-- Link devices");
  struct cam_req_mgr_link_info req_mgr_link_info = {0};
  req_mgr_link_info.session_hdl = session_handle;
  req_mgr_link_info.num_devices = 2;
  req_mgr_link_info.dev_hdls[0] = isp_dev_handle;
  req_mgr_link_info.dev_hdls[1] = sensor_dev_handle;
  int ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_LINK, &req_mgr_link_info, sizeof(req_mgr_link_info));
  if (getenv("G8_CAMERA_START_ONLY") != nullptr) {
    fprintf(stderr, "G8_LINK_RET=%d link=0x%X\n", ret, req_mgr_link_info.link_hdl);
    fflush(stderr);
  }
  assert(ret == 0);
  link_handle = req_mgr_link_info.link_hdl;
  LOGD("link: %d session: 0x%X isp: 0x%X sensors: 0x%X link: 0x%X", ret, session_handle, isp_dev_handle, sensor_dev_handle, link_handle);

  struct cam_req_mgr_link_control req_mgr_link_control = {0};
  req_mgr_link_control.ops = CAM_REQ_MGR_LINK_ACTIVATE;
  req_mgr_link_control.session_hdl = session_handle;
  req_mgr_link_control.num_links = 1;
  req_mgr_link_control.link_hdls[0] = link_handle;
  ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_LINK_CONTROL, &req_mgr_link_control, sizeof(req_mgr_link_control));
  if (getenv("G8_CAMERA_START_ONLY") != nullptr) {
    fprintf(stderr, "G8_LINK_ACTIVATE_RET=%d\n", ret);
    fflush(stderr);
  }
  LOGD("link control: %d", ret);

  ret = device_control(csiphy_fd, CAM_START_DEV, session_handle, csiphy_dev_handle);
  if (getenv("G8_CAMERA_START_ONLY") != nullptr) {
    fprintf(stderr, "G8_CSIPHY_START_RET=%d\n", ret);
    fflush(stderr);
  }
  LOGD("start csiphy: %d", ret);
  assert(ret == 0);
  ret = device_control(m->isp_fd, CAM_START_DEV, session_handle, isp_dev_handle);
  if (getenv("G8_CAMERA_START_ONLY") != nullptr) {
    fprintf(stderr, "G8_ISP_START_RET=%d\n", ret);
    fflush(stderr);
  }
  LOGD("start isp: %d", ret);
  assert(ret == 0);
  if (cc.output_type == ISP_BPS_PROCESSED) {
    ret = device_control(m->icp_fd, CAM_START_DEV, session_handle, icp_dev_handle);
    LOGD("start icp: %d", ret);
    assert(ret == 0);
  }
}

void SpectraCamera::camera_close() {
  if (getenv("G8_CAMERA_QUEUE_ONLY") != nullptr) {
    fprintf(stderr, "G8_QUEUE_CLEANUP_BEGIN sensor=%d enabled=%d\n", cc.camera_num, enabled ? 1 : 0);
    fflush(stderr);
  }
  LOG("-- Stop devices %d", cc.camera_num);

  if (enabled || getenv("G8_CAMERA_FIRST_SOF") != nullptr) {
    sensors_stop();
    clear_req_queue();

    // ret = device_control(sensor_fd, CAM_STOP_DEV, session_handle, sensor_dev_handle);
    // LOGD("stop sensor: %d", ret);
    int ret = device_control(m->isp_fd, CAM_STOP_DEV, session_handle, isp_dev_handle);
    LOGD("stop isp: %d", ret);
    if (cc.output_type == ISP_BPS_PROCESSED) {
      ret = device_control(m->icp_fd, CAM_STOP_DEV, session_handle, icp_dev_handle);
      LOGD("stop icp: %d", ret);
    }
    ret = device_control(csiphy_fd, CAM_STOP_DEV, session_handle, csiphy_dev_handle);
    LOGD("stop csiphy: %d", ret);

    // link control stop
    LOG("-- Stop link control");
    struct cam_req_mgr_link_control req_mgr_link_control = {0};
    req_mgr_link_control.ops = CAM_REQ_MGR_LINK_DEACTIVATE;
    req_mgr_link_control.session_hdl = session_handle;
    req_mgr_link_control.num_links = 1;
    req_mgr_link_control.link_hdls[0] = link_handle;
    ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_LINK_CONTROL, &req_mgr_link_control, sizeof(req_mgr_link_control));
    LOGD("link control stop: %d", ret);

    // unlink
    LOG("-- Unlink");
    struct cam_req_mgr_unlink_info req_mgr_unlink_info = {0};
    req_mgr_unlink_info.session_hdl = session_handle;
    req_mgr_unlink_info.link_hdl = link_handle;
    ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_UNLINK, &req_mgr_unlink_info, sizeof(req_mgr_unlink_info));
    LOGD("unlink: %d", ret);

    // release devices
    LOGD("-- Release devices");
    ret = device_control(m->isp_fd, CAM_RELEASE_DEV, session_handle, isp_dev_handle);
    LOGD("release isp: %d", ret);
    if (cc.output_type == ISP_BPS_PROCESSED) {
      ret = device_control(m->icp_fd, CAM_RELEASE_DEV, session_handle, icp_dev_handle);
      LOGD("release icp: %d", ret);
    }
    ret = device_control(csiphy_fd, CAM_RELEASE_DEV, session_handle, csiphy_dev_handle);
    LOGD("release csiphy: %d", ret);

    for (int i = 0; i < ife_buf_depth; i++) {
      if (buf_handle_raw[i]) {
        release(m->video0_fd, buf_handle_raw[i]);
      }
      if (buf_handle_yuv[i]) {
        release(m->video0_fd, buf_handle_yuv[i]);
      }
    }
    LOGD("released buffers");
  }

  int ret = device_control(sensor_fd, CAM_RELEASE_DEV, session_handle, sensor_dev_handle);
  LOGD("release sensor: %d", ret);

  // destroyed session
  struct cam_req_mgr_session_info session_info = {.session_hdl = session_handle};
  ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_DESTROY_SESSION, &session_info, sizeof(session_info));
  LOGD("destroyed session %d: %d", cc.camera_num, ret);
  if (getenv("G8_CAMERA_QUEUE_ONLY") != nullptr) {
    fprintf(stderr, "G8_QUEUE_CLEANUP_DONE sensor=%d destroy_session_ret=%d\n", cc.camera_num, ret);
    fflush(stderr);
  }
}

bool SpectraCamera::handle_camera_event(const cam_req_mgr_message *event_data) {
  /*
    Handles camera SOF event. Returns true if the frame is valid for publishing.
  */

  uint64_t request_id = event_data->u.frame_msg.request_id;  // ID from the camera request manager
  uint64_t frame_id_raw = event_data->u.frame_msg.frame_id;  // raw as opposed to our re-indexed frame ID
  uint64_t timestamp = event_data->u.frame_msg.timestamp;    // timestamped in the kernel's SOF IRQ callback
  //LOGD("handle cam %d ts %lu req id %lu frame id %lu", cc.camera_num, timestamp, request_id, frame_id_raw);

  // if there's a lag, some more frames could have already come in before
  // we cleared the queue, so we'll still get them with valid (> 0) request IDs.
  if (timestamp < last_requeue_ts) {
    LOGD("skipping frame: ts before requeue / cam %d ts %lu req id %lu frame id %lu", cc.camera_num, timestamp, request_id, frame_id_raw);
    return false;
  }

  if (stress_test("skipping SOF event")) {
    return false;
  }

  if (!validateEvent(request_id, frame_id_raw)) {
    return false;
  }

  // Update tracking variables
  if (request_id == request_id_last + 1) {
    skip_expected = false;
  }
  frame_id_raw_last = frame_id_raw;
  request_id_last = request_id;

  // G8_FIRST_FRAME_CAMERA_SELECT_V1
  // Optional selector for the existing first-IFE/dump harness. With
  // G8_CAMERA_DUMP_CAMERA_NUM unset, behavior is unchanged. When set, only
  // that logical camera number enters the first-IFE early-return/dump path.
  const char *g8_dump_camera_num_env = getenv("G8_CAMERA_DUMP_CAMERA_NUM");
  const bool g8_first_ife_selected =
      getenv("G8_CAMERA_FIRST_IFE") != nullptr &&
      (g8_dump_camera_num_env == nullptr || atoi(g8_dump_camera_num_env) == cc.camera_num);

  // Wait until frame's fully read out and processed
  if (!waitForFrameReady(request_id)) {
    if (g8_first_ife_selected) {
      fprintf(stderr,
              "G8_FIRST_IFE_FAILED sensor=%d frame=%llu req=%llu\n",
              cc.camera_num,
              (unsigned long long)frame_id_raw,
              (unsigned long long)request_id);
      fflush(stderr);
      g8_first_ife_complete = true;
      return false;
    }

    // Reset queue on sync failure to prevent frame tearing
    LOGE("camera %d sync failure %ld %ld ", cc.camera_num, request_id, frame_id_raw);
    clearAndRequeue(request_id + 1);
    return false;
  }

  int buf_idx = request_id % ife_buf_depth;

  // G8 stage 6: a valid SOF was followed by a signaled IFE output fence.
  // Stop before processFrame()/multi-camera sync or VisionIPC publication.
  if (g8_first_ife_selected) {
    fprintf(stderr,
            "G8_FIRST_IFE_COMPLETE sensor=%d frame=%llu req=%llu buf_idx=%d sync=%d yuv_handle=0x%X raw_handle=0x%X\n",
            cc.camera_num,
            (unsigned long long)frame_id_raw,
            (unsigned long long)request_id,
            buf_idx,
            sync_objs_ife[buf_idx],
            buf_handle_yuv[buf_idx],
            buf_handle_raw[buf_idx]);
    fflush(stderr);

    if (getenv("G8_CAMERA_DUMP_FIRST_FRAME") != nullptr) {
      VisionBuf *vb = buf.vipc_server->get_buffer(buf.stream_type, buf_idx);
      const uint8_t *base = (const uint8_t *)vb->addr;
      const uint8_t *y = vb->y;

      fprintf(stderr,
              "G8_FIRST_FRAME_LAYOUT visible=%ux%u stride=%u y_height=%u uv_height=%u uv_offset=%u yuv_size=%u vb_len=%zu fd=%d\n",
              buf.out_img_width, buf.out_img_height, stride, y_height, uv_height,
              uv_offset, yuv_size, vb->len, vb->fd);
      fflush(stderr);

      uint8_t min_y = 255;
      uint8_t max_y = 0;
      uint64_t sum_y = 0;
      uint64_t nonzero_y = 0;
      uint64_t samples = 0;

      for (uint32_t row = 0; row < buf.out_img_height; ++row) {
        const uint8_t *rowp = y + ((size_t)row * stride);
        for (uint32_t col = 0; col < buf.out_img_width; ++col) {
          uint8_t v = rowp[col];
          if (v < min_y) min_y = v;
          if (v > max_y) max_y = v;
          sum_y += v;
          nonzero_y += (v != 0);
          samples++;
        }
      }

      double mean_y = samples ? ((double)sum_y / (double)samples) : 0.0;
      fprintf(stderr,
              "G8_FIRST_FRAME_STATS samples=%llu min=%u max=%u mean=%.3f nonzero=%llu\n",
              (unsigned long long)samples, (unsigned)min_y, (unsigned)max_y,
              mean_y, (unsigned long long)nonzero_y);

      fprintf(stderr, "G8_FIRST_FRAME_HEAD");
      size_t head_n = yuv_size < 32 ? yuv_size : 32;
      for (size_t i = 0; i < head_n; ++i) {
        fprintf(stderr, " %02X", (unsigned)base[i]);
      }
      fprintf(stderr, "\n");
      fflush(stderr);

      const char *dump_path = "/data/g8-first-frame.nv12";
      FILE *fp = fopen(dump_path, "wb");
      if (fp == nullptr) {
        fprintf(stderr, "G8_FIRST_FRAME_DUMP_OPEN_FAILED path=%s errno=%d\n", dump_path, errno);
        fflush(stderr);
      } else {
        size_t wrote = fwrite(base, 1, yuv_size, fp);
        int close_ret = fclose(fp);
        fprintf(stderr,
                "G8_FIRST_FRAME_DUMP path=%s wrote=%zu expected=%u close_ret=%d\n",
                dump_path, wrote, yuv_size, close_ret);
        fflush(stderr);
      }
    }

    g8_first_ife_complete = true;
    return false;
  }
  // G8_CAMERA_DYNAMIC_AWB_V4
  // G8_COLOR_CAL_CAPTURE_V1
  // One-shot settled-frame NV12 capture.
  {
    FILE *g8_cap_trigger = fopen("/data/G8_CAPTURE_COLOR_FRAME", "rb");
    if (g8_cap_trigger != nullptr) {
      fclose(g8_cap_trigger);

      VisionBuf *g8_cap_vb = buf.vipc_server->get_buffer(buf.stream_type, buf_idx);
      const uint8_t *g8_cap_base = (const uint8_t *)g8_cap_vb->addr;
      const char *g8_cap_path = "/data/g8-color-calibration.nv12";

      fprintf(stderr,
              "G8_COLOR_CAL_LAYOUT visible=%ux%u stride=%u y_height=%u uv_height=%u uv_offset=%u yuv_size=%u\n",
              buf.out_img_width, buf.out_img_height, stride, y_height, uv_height,
              uv_offset, yuv_size);

      FILE *g8_cap_fp = fopen(g8_cap_path, "wb");
      if (g8_cap_fp == nullptr) {
        fprintf(stderr, "G8_COLOR_CAL_CAPTURE_OPEN_FAILED path=%s errno=%d\n",
                g8_cap_path, errno);
      } else {
        size_t g8_cap_wrote = fwrite(g8_cap_base, 1, yuv_size, g8_cap_fp);
        int g8_cap_close_ret = fclose(g8_cap_fp);
        fprintf(stderr,
                "G8_COLOR_CAL_CAPTURE path=%s wrote=%zu expected=%u close_ret=%d\n",
                g8_cap_path, g8_cap_wrote, yuv_size, g8_cap_close_ret);
      }
      fflush(stderr);
      remove("/data/G8_CAPTURE_COLOR_FRAME");
    }
  }

  // Stage 10D: extend the proven gain-normalized V3 controller to all three
  // G8 camera roles with independent state and independent live IFE gains.
  //
  // DRIVER keeps its empirically proven target U=125/V=131.
  // ROAD and WIDE use true neutral U=128/V=128 until sensor-specific road
  // illumination characterization is available. Their actuator remains very
  // conservative: one register count only after three persistent 1 Hz
  // observations, with the same >=800 neutral-evidence gate and HOLD behavior.
  //
  // Enable:
  //   G8_DYNAMIC_AWB_DRIVER=1  -> DRIVER only (backward compatible)
  //   G8_DYNAMIC_AWB_ROAD=1    -> ROAD only
  //   G8_DYNAMIC_AWB_WIDE=1    -> WIDE only
  //   G8_DYNAMIC_AWB_REAR=1    -> ROAD + WIDE
  //   G8_DYNAMIC_AWB_ALL=1     -> ROAD + DRIVER + WIDE
  //
  // No AE, sensor exposure/gain, CSI/CSID/PHY, Bayer, CCM or gamma changes.
  const bool g8_awb_all = getenv("G8_DYNAMIC_AWB_ALL") != nullptr;
  const bool g8_awb_rear = getenv("G8_DYNAMIC_AWB_REAR") != nullptr;
  const bool g8_awb_is_driver = cc.stream_type == VISION_STREAM_DRIVER;
  const bool g8_awb_is_wide =
      cc.stream_type == VISION_STREAM_WIDE_ROAD ||
      getenv("G8_CAMERA_TARGET_WIDE") != nullptr;
  const bool g8_awb_is_road =
      cc.stream_type == VISION_STREAM_ROAD &&
      !g8_awb_is_wide && !g8_awb_is_driver;

  const bool g8_dynamic_awb =
      getenv("G8_AGNOS") != nullptr &&
      ((g8_awb_is_driver &&
        (g8_awb_all || getenv("G8_DYNAMIC_AWB_DRIVER") != nullptr)) ||
       (g8_awb_is_road &&
        (g8_awb_all || g8_awb_rear || getenv("G8_DYNAMIC_AWB_ROAD") != nullptr)) ||
       (g8_awb_is_wide &&
        (g8_awb_all || g8_awb_rear || getenv("G8_DYNAMIC_AWB_WIDE") != nullptr)));

  if (g8_dynamic_awb) {
    struct G8AwbState {
      uint64_t last_ns = 0;
      int dir_b = 0;
      int dir_r = 0;
      int persist_b = 0;
      int persist_r = 0;
      bool ema_valid = false;
      int ema_u_q8 = 0;
      int ema_v_q8 = 0;
    };
    // Key order is ROAD=0, DRIVER=1, WIDE=2; do not rely on enum numeric values.
    static G8AwbState g8_awb_states[3];

    int g8_awb_key = 0;
    const char *g8_awb_stream = "ROAD";
    uint32_t *g8_awb_gain_b = &g8_road_awb_gain_b;
    uint32_t *g8_awb_gain_r = &g8_road_awb_gain_r;
    int g8_target_u = 128;
    int g8_target_v = 128;

    if (g8_awb_is_driver) {
      g8_awb_key = 1;
      g8_awb_stream = "DRIVER";
      g8_awb_gain_b = &g8_driver_awb_gain_b;
      g8_awb_gain_r = &g8_driver_awb_gain_r;
      g8_target_u = 125;
      g8_target_v = 131;

      // G8_IMX520_AWB_MAINLIKE_V3_FIXED
      // DRIVER-only final stability test using the accepted MAIN-style CCM.
      // The visually good point is G=0x80/B=0xCE/R=0xD8.  V1 (116/134) was
      // scene-specific and V2 (128/128) still drifted away from this point.
      // Keep 128/128 only as a diagnostic neutral reference; hold the actual
      // DRIVER IFE gains at CE/D8 so colored/low-neutral scenes cannot push
      // blue upward or red downward.
      if (getenv("G8_IMX520_AWB_MAINLIKE") != nullptr) {
        g8_target_u = 128;
        g8_target_v = 128;
        *g8_awb_gain_b = 0xCE;
        *g8_awb_gain_r = 0xD8;

        static bool g8_imx520_awb_mainlike_logged = false;
        if (!g8_imx520_awb_mainlike_logged) {
          g8_imx520_awb_mainlike_logged = true;
          fprintf(stderr,
                  "G8_IMX520_AWB_MAINLIKE_V3_FIXED enabled=1 "
                  "fixedB=0xCE fixedR=0xD8 monitorTargetU=128 monitorTargetV=128\n");
          fflush(stderr);
        }
      }
    } else if (g8_awb_is_wide) {
      g8_awb_key = 2;
      g8_awb_stream = "WIDE";
      g8_awb_gain_b = &g8_wide_awb_gain_b;
      g8_awb_gain_r = &g8_wide_awb_gain_r;
      g8_target_u = 128;
      g8_target_v = 128;
    }

    G8AwbState &g8s = g8_awb_states[g8_awb_key];
    const uint64_t g8_awb_now_ns = nanos_since_boot();

    if (g8s.last_ns == 0 || g8_awb_now_ns - g8s.last_ns >= 1000000000ULL) {
      g8s.last_ns = g8_awb_now_ns;

      VisionBuf *g8_vb = buf.vipc_server->get_buffer(buf.stream_type, buf_idx);
      const uint8_t *g8_y = g8_vb->y;
      const uint8_t *g8_uv = (const uint8_t *)g8_vb->addr + uv_offset;

      uint32_t g8_hist_u[256] = {};
      uint32_t g8_hist_v[256] = {};
      uint32_t g8_neutral_count = 0;

      const uint32_t g8_uv_rows = buf.out_img_height / 2;
      const uint32_t g8_uv_cols = buf.out_img_width / 2;

      for (uint32_t r = 2; r + 2 < g8_uv_rows; r += 8) {
        const uint8_t *uv_row = g8_uv + ((size_t)r * stride);
        const uint8_t *y_row = g8_y + ((size_t)(r * 2) * stride);
        for (uint32_t c = 2; c + 2 < g8_uv_cols; c += 8) {
          const uint8_t yv = y_row[c * 2];
          if (yv < 56 || yv > 220) continue;

          const uint8_t u = uv_row[c * 2];
          const uint8_t v = uv_row[c * 2 + 1];
          const int du = (u > 128) ? (u - 128) : (128 - u);
          const int dv = (v > 128) ? (v - 128) : (128 - v);
          if (du > 28 || dv > 28 || (du + dv) > 36) continue;

          g8_hist_u[u]++;
          g8_hist_v[v]++;
          g8_neutral_count++;
        }
      }

      if (g8_neutral_count >= 800) {
        auto g8_hist_median = [](const uint32_t *hist, uint32_t total) {
          const uint32_t half = (total + 1) / 2;
          uint32_t acc = 0;
          for (int i = 0; i < 256; ++i) {
            acc += hist[i];
            if (acc >= half) return i;
          }
          return 128;
        };

        const int med_u = g8_hist_median(g8_hist_u, g8_neutral_count);
        const int med_v = g8_hist_median(g8_hist_v, g8_neutral_count);

        if (!g8s.ema_valid) {
          g8s.ema_u_q8 = med_u << 8;
          g8s.ema_v_q8 = med_v << 8;
          g8s.ema_valid = true;
        } else {
          g8s.ema_u_q8 = (3 * g8s.ema_u_q8 + (med_u << 8) + 2) / 4;
          g8s.ema_v_q8 = (3 * g8s.ema_v_q8 + (med_v << 8) + 2) / 4;
        }

        constexpr int G8_AWB_B_COUNTS_PER_U = 5;
        constexpr int G8_AWB_R_COUNTS_PER_V = 3;

        auto g8_div_q8_round = [](int q8_times_counts) {
          if (q8_times_counts >= 0) return (q8_times_counts + 128) / 256;
          return -((-q8_times_counts + 128) / 256);
        };

        int est_delta_b = g8_div_q8_round(
            (((g8_target_u << 8) - g8s.ema_u_q8) * G8_AWB_B_COUNTS_PER_U));
        int est_delta_r = g8_div_q8_round(
            (((g8_target_v << 8) - g8s.ema_v_q8) * G8_AWB_R_COUNTS_PER_V));

        est_delta_b = std::clamp(est_delta_b, -12, 12);
        est_delta_r = std::clamp(est_delta_r, -9, 9);

        const uint32_t old_b = *g8_awb_gain_b;
        const uint32_t old_r = *g8_awb_gain_r;
        const uint32_t target_b = (uint32_t)std::clamp(
            (g8_awb_key == 1 ? 0xD0 : (g8_awb_key == 2 ? 0xD8 : 0xCE)) + est_delta_b, 0xB8, 0xF0);
        const uint32_t target_r = (uint32_t)std::clamp(
            (g8_awb_key == 1 ? 0xD8 : (g8_awb_key == 2 ? 0xB4 : 0xB4)) + est_delta_r, 0xB0, 0x100);

        auto g8_target_direction = [](uint32_t current, uint32_t target) {
          if ((int)target >= (int)current + 2) return 1;
          if ((int)target <= (int)current - 2) return -1;
          return 0;
        };

        const int raw_dir_b = g8_target_direction(old_b, target_b);
        const int raw_dir_r = g8_target_direction(old_r, target_r);

        auto g8_awb_persist = [](int raw_dir, int &last_dir, int &count) {
          if (raw_dir == 0) {
            last_dir = 0;
            count = 0;
            return 0;
          }
          if (raw_dir != last_dir) {
            last_dir = raw_dir;
            count = 1;
            return 0;
          }
          count++;
          if (count >= 3) {
            count = 0;
            return raw_dir;
          }
          return 0;
        };

        const int step_b = g8_awb_persist(raw_dir_b, g8s.dir_b, g8s.persist_b);
        const int step_r = g8_awb_persist(raw_dir_r, g8s.dir_r, g8s.persist_r);

        if (g8_awb_is_driver && getenv("G8_IMX520_AWB_MAINLIKE") != nullptr) {
          *g8_awb_gain_b = 0xCE;
          *g8_awb_gain_r = 0xD8;
        } else {
          *g8_awb_gain_b = (uint32_t)std::clamp((int)old_b + step_b, 0xB8, 0xF0);
          *g8_awb_gain_r = (uint32_t)std::clamp((int)old_r + step_r, 0xB0, 0x100);
        }

        fprintf(stderr,
                "G8_CAMERA_DYNAMIC_AWB_V4 stream=%s frame=%llu neutral=%u "
                "medU=%d medV=%d emaUq8=%d emaVq8=%d targetU=%d targetV=%d "
                "dB=%d dR=%d targetB=0x%X targetR=0x%X "
                "rawB=%d pB=%d stepB=%d rawR=%d pR=%d stepR=%d "
                "B=0x%X->0x%X R=0x%X->0x%X\n",
                g8_awb_stream, (unsigned long long)frame_id_raw,
                g8_neutral_count, med_u, med_v,
                g8s.ema_u_q8, g8s.ema_v_q8, g8_target_u, g8_target_v,
                est_delta_b, est_delta_r, target_b, target_r,
                raw_dir_b, g8s.persist_b, step_b,
                raw_dir_r, g8s.persist_r, step_r,
                old_b, *g8_awb_gain_b, old_r, *g8_awb_gain_r);
        fflush(stderr);
      } else {
        g8s.dir_b = g8s.dir_r = 0;
        g8s.persist_b = g8s.persist_r = 0;
        g8s.ema_valid = false;
        fprintf(stderr,
                "G8_CAMERA_DYNAMIC_AWB_V4 HOLD stream=%s frame=%llu neutral=%u "
                "B=0x%X R=0x%X\n",
                g8_awb_stream, (unsigned long long)frame_id_raw,
                g8_neutral_count, *g8_awb_gain_b, *g8_awb_gain_r);
        fflush(stderr);
      }
    }
  }

  bool ret = processFrame(buf_idx, request_id, frame_id_raw, timestamp);
  destroySyncObjectAt(buf_idx);
  enqueue_frame(request_id + ife_buf_depth);  // request next frame for this slot
  return ret;
}

bool SpectraCamera::validateEvent(uint64_t request_id, uint64_t frame_id_raw) {
  // check if the request ID is even valid. this happens after queued
  // requests are cleared. unclear if it happens any other time.
  if (request_id == 0) {
    if (invalid_request_count++ > ife_buf_depth+2) {
      LOGE("camera %d reset after half second of invalid requests", cc.camera_num);
      clearAndRequeue(request_id_last + 1);
      invalid_request_count = 0;
    }
    return false;
  }
  invalid_request_count = 0;

  // check for skips in frame_id or request_id
  if (!skip_expected) {
    if (frame_id_raw != frame_id_raw_last + 1) {
      LOGE("camera %d frame ID skipped, %lu -> %lu", cc.camera_num, frame_id_raw_last, frame_id_raw);
      clearAndRequeue(request_id + 1);
      return false;
    }

    if (request_id != request_id_last + 1) {
      LOGE("camera %d requests skipped %ld -> %ld", cc.camera_num, request_id_last, request_id);
      clearAndRequeue(request_id + 1);
      return false;
    }
  }
  return true;
}

void SpectraCamera::clearAndRequeue(uint64_t from_request_id) {
  // clear everything, then queue up a fresh set of frames
  LOGW("clearing and requeuing camera %d from %lu", cc.camera_num, from_request_id);
  clear_req_queue();
  last_requeue_ts = nanos_since_boot();
  for (uint64_t id = from_request_id; id < from_request_id + ife_buf_depth; ++id) {
    enqueue_frame(id);
  }
  skip_expected = true;
}

bool SpectraCamera::waitForFrameReady(uint64_t request_id) {
  int buf_idx = request_id % ife_buf_depth;
  assert(sync_objs_ife[buf_idx]);

  if (stress_test("sync sleep time")) {
    util::sleep_for(350);
    return false;
  }

  auto waitForSync = [&](uint32_t sync_obj, int timeout_ms, const char *sync_type) {
    double st = millis_since_boot();
    struct cam_sync_wait sync_wait = {};
    sync_wait.sync_obj = sync_obj;
    sync_wait.timeout_ms = stress_test(sync_type) ? 1 : timeout_ms;
    bool ret = do_sync_control(m->cam_sync_fd, CAM_SYNC_WAIT, &sync_wait, sizeof(sync_wait)) == 0;
    double et = millis_since_boot();
    if (!ret) LOGE("camera %d %s failed after %.2fms", cc.camera_num, sync_type, et-st);
    return ret;
  };

  // wait for frame from IFE
  // - in RAW_OUTPUT mode, this time is just the frame readout from the sensor
  // - in IFE_PROCESSED mode, this time also includes image processing (~1ms)
  bool success = waitForSync(sync_objs_ife[buf_idx], 100, "IFE sync");
  if (success && sync_objs_bps[buf_idx]) {
    // BPS is typically 7ms
    success = waitForSync(sync_objs_bps[buf_idx], 50, "BPS sync");
  }

  return success;
}

bool SpectraCamera::processFrame(int buf_idx, uint64_t request_id, uint64_t frame_id_raw, uint64_t timestamp) {
  if (!syncFirstFrame(cc.camera_num, request_id, frame_id_raw, timestamp, cc.staggered_sof)) {
    return false;
  }

  // in IFE_PROCESSED mode, we can't know the true EOF, so recover it with sensor readout time
  uint64_t timestamp_eof = timestamp + sensor->readout_time_ns;

  // G8_IFE_PROCESSING_TIME_V1
  // On the G8 IFE path this callback can run before the estimated EOF.
  // Avoid unsigned uint64 underflow from (now - future_eof), which otherwise
  // appears in driverCameraState as ~1.844674e10 seconds. Do not alter the
  // SOF/EOF timestamps themselves; only saturate the derived duration at 0.
  const uint64_t processing_now_ns = nanos_since_boot();
  const bool g8_ife_future_eof =
      getenv("G8_AGNOS") != nullptr &&
      cc.output_type == ISP_IFE_PROCESSED &&
      processing_now_ns < timestamp_eof;
  const float frame_processing_time = g8_ife_future_eof ? 0.0f :
      float((processing_now_ns - timestamp_eof) * 1e-9);

  if (g8_ife_future_eof &&
      getenv("G8_IMX520_DRIVER_STREAM_TEST") != nullptr) {
    static bool g8_processing_time_clamp_logged = false;
    if (!g8_processing_time_clamp_logged) {
      fprintf(stderr,
              "G8_IFE_PROCESSING_TIME_V1 CLAMP now=%llu eof=%llu delta_ns=%llu processing=0\n",
              (unsigned long long)processing_now_ns,
              (unsigned long long)timestamp_eof,
              (unsigned long long)(timestamp_eof - processing_now_ns));
      fflush(stderr);
      g8_processing_time_clamp_logged = true;
    }
  }

  // Update buffer and frame data
  buf.cur_buf_idx = buf_idx;
  buf.cur_frame_data = {
    .frame_id = (uint32_t)(frame_id_raw - camera_sync_data[cc.camera_num].frame_id_offset),
    .request_id = (uint32_t)request_id,
    .timestamp_sof = timestamp,
    .timestamp_eof = timestamp_eof,
    .processing_time = frame_processing_time
  };
  return true;
}

bool SpectraCamera::syncFirstFrame(int camera_id, uint64_t request_id, uint64_t raw_id, uint64_t timestamp, bool staggered) {
  if (first_frame_synced) return true;

  // Store the frame data for this camera
  camera_sync_data[camera_id] = SyncData{timestamp, raw_id + 1, staggered};

  // Ensure all cameras are up. The LG G8 port intentionally instantiates
  // only the road camera, while ALL_CAMERA_CONFIGS still contains the comma
  // wide/driver entries. Treat the G8 as a one-camera platform here.
  int enabled_camera_count = getenv("G8_AGNOS") != nullptr ? 1 :
    std::count_if(std::begin(ALL_CAMERA_CONFIGS), std::end(ALL_CAMERA_CONFIGS),
                  [](const auto &config) { return config.enabled; });
  bool all_cams_up = camera_sync_data.size() == enabled_camera_count;

  if (getenv("G8_CAMERA_VIPC_TEST") != nullptr && !first_frame_synced) {
    fprintf(stderr,
            "G8_SINGLE_CAMERA_SYNC camera=%d registered=%zu expected=%d raw_id=%llu request=%llu\n",
            camera_id, camera_sync_data.size(), enabled_camera_count,
            (unsigned long long)raw_id, (unsigned long long)request_id);
    fflush(stderr);
  }

  // Check that camera timestamps are properly aligned:
  // - non-staggered cameras should be within 0.2ms of each other
  // - staggered cameras should be within 0.2ms of a 25ms offset from non-staggered cameras
  const uint64_t half_period_ns = 25 * 1000000ULL;  // 25ms
  const uint64_t tolerance_ns = 200000ULL;           // 0.2ms
  bool all_cams_synced = true;
  for (const auto &[cam, sync_data] : camera_sync_data) {
    if (cam == camera_id) continue;
    uint64_t diff = std::max(timestamp, sync_data.timestamp) -
                    std::min(timestamp, sync_data.timestamp);
    bool pair_staggered = staggered != sync_data.staggered;
    uint64_t expected_offset = pair_staggered ? half_period_ns : 0;
    uint64_t error = (diff > expected_offset) ? diff - expected_offset : expected_offset - diff;
    if (error > tolerance_ns) {
      all_cams_synced = false;
    }
  }

  if (all_cams_up && all_cams_synced) {
    first_frame_synced = true;
    for (const auto&[cam, sync_data] : camera_sync_data) {
      LOGW("camera %d synced on frame_id_offset %ld timestamp %lu", cam, sync_data.frame_id_offset, sync_data.timestamp);
    }
  }

  // Timeout in case the timestamps never line up
  if (raw_id > 40) {
    LOGE("camera first frame sync timed out");
    first_frame_synced = true;
  }

  return false;
}
