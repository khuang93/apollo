/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/drivers/velodyne/parser/velodyne_parser.h"

namespace apollo {
namespace drivers {
namespace velodyne {

Velodyne64Parser::Velodyne64Parser(Config config) : VelodyneParser(config) {
  for (int i = 0; i < 4; i++) {
    gps_base_usec_[i] = 0;
    previous_packet_stamp_[i] = 0;
  }
  need_two_pt_correction_ = true;
  // init unpack function and order function by model.
  if (config_.model() == HDL64E_S2) {
    inner_time_ = &velodyne::INNER_TIME_64;
    is_s2_ = true;
  } else {  // 64E_S3
    inner_time_ = &velodyne::INNER_TIME_64E_S3;
    is_s2_ = false;
  }

  if (config_.mode() == LAST) {
    mode_ = LAST;
  } else if (config_.mode() == DUAL) {
    mode_ = DUAL;
  }
}

void Velodyne64Parser::setup() {
  VelodyneParser::setup();
  if (!config_.calibration_online() && config_.organized()) {
    init_offsets();
  }
}

void Velodyne64Parser::set_base_time_from_packets(const VelodynePacket& pkt) {
  // const RawPacket* raw = (const RawPacket*)&pkt.data[0];
  const RawPacket* raw = (const RawPacket*)pkt.data().c_str();
  StatusType status_type = StatusType(raw->status_type);
  char status_value = raw->status_value;

  static int year = -1, month = -1, day = -1, hour = -1, minute = -1,
             second = -1;
  static int gps_status = 0;
  static tm time;

  switch (status_type) {
    case YEAR:
      year = status_value + 2000;
      break;
    case MONTH:
      month = status_value;
      break;
    case DATE:
      day = status_value;
      break;
    case HOURS:
      hour = status_value;
      break;
    case MINUTES:
      minute = status_value;
      break;
    case SECONDS:
      second = status_value;
      break;
    case GPS_STATUS:
      gps_status = status_value;
      break;
    default:
      break;
  }

  LOG_INFO_FORMAT("Get base time from packets. Obtained (%d.%d.%d %d:%d:%d)",
                  year, month, day, hour, minute, second);

  if (status_type == GPS_STATUS && year > 0 && month > 0 && day > 0 &&
      hour >= 0 && minute >= 0 && second >= 0) {
    if (gps_status != 65) {
      AWARN << "Sync failed because Velodyne-GPS Sync is NOT good! "
                      << "Status: " << (int)gps_status
                      << " (65 = both; 86 = gps only; 80 = PPS only; 0 "
                      << "= GPS not connected)";
    }

    time.tm_year = year - 1900;
    time.tm_mon = month - 1;
    time.tm_mday = day;
    time.tm_hour = hour;
    time.tm_min = 0;
    time.tm_sec = 0;

//    AINFO << "Set base unix time: (%d.%d.%d %d:%d:%d)", time.tm_year,
//        time.tm_mon, time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec;

    uint64_t unix_base = static_cast<uint64_t>(timegm(&time));
    for (int i = 0; i < 4; ++i) {
      gps_base_usec_[i] = unix_base * 1000000;
    }
  }
}

void Velodyne64Parser::check_gps_status(const VelodynePacket& pkt) {
  // const RawPacket* raw = (const RawPacket*)&pkt.data[0];
  const RawPacket* raw = (const RawPacket*)pkt.data().c_str();
  StatusType status_type = StatusType(raw->status_type);
  char status_value = raw->status_value;

  if (status_type == StatusType::GPS_STATUS) {
    if (status_value != 65) {
      AWARN << "Sync failed because Velodyne-GPS Sync is NOT good! "
            << "Status: " << (int)status_value
            << " (65 = both; 86 = gps only; 80 = PPS only; "
            << "0 = GPS not connected)";
    }
  }
}

void Velodyne64Parser::init_offsets() {
  int width = 64;
  // pre compute col offsets
  for (int i = 0; i < width; ++i) {
    int col = velodyne::ORDER_64[i];
    // compute offset, NOTICE: std::map doesn't have const [] since [] may
    // insert new values into map
    const LaserCorrection& corrections = calibration_.laser_corrections_[col];
    int offset = int(corrections.rot_correction / ANGULAR_RESOLUTION + 0.5);
    offsets_[i] = offset;
  }
}

void Velodyne64Parser::generate_pointcloud(
    const std::shared_ptr<VelodyneScan>& scan_msg,
    std::shared_ptr<PointCloud>& pointcloud) {
  if (config_.calibration_online() && !calibration_.initialized_) {
    if (online_calibration_.decode(scan_msg) == -1) {
      return;
    }
    calibration_ = online_calibration_.calibration();
    if (config_.organized()) {
      init_offsets();
    }
  }

  // allocate a point cloud with same time and frame ID as raw data
  pointcloud->mutable_header()->set_frame_id(scan_msg->header().frame_id());
  pointcloud->set_height(1);
  pointcloud->mutable_header()->set_sequence_num(scan_msg->header().sequence_num());

  bool skip = false;
  size_t packets_size = scan_msg->firing_pkts_size();
  for (size_t i = 0; i < packets_size; ++i) {
    if (gps_base_usec_[0] == 0) {
      // only set one time type when call this function, so cannot break
      set_base_time_from_packets(scan_msg->firing_pkts(i));
      // If base time not ready then set empty_unpack true
      skip = true;
    } else {
      check_gps_status(scan_msg->firing_pkts(i));
      unpack(scan_msg->firing_pkts(i), pointcloud);
      last_time_stamp_ = pointcloud->measurement_time();
      ADEBUG << "stamp: " << std::fixed << last_time_stamp_;
    }
  }

  if (skip) {
    pointcloud->Clear();
  } else {
    int size = pointcloud->point_size();
    if (size == 0) {
      // we discard this pointcloud if empty
      AERROR << "All points is NAN! Please check velodyne:" << config_.model();
    } else {
      uint64_t timestamp = pointcloud->point(size - 1).timestamp();
      pointcloud->set_measurement_time(timestamp / 1e9);
      pointcloud->mutable_header()->set_lidar_timestamp(timestamp);
    }
    pointcloud->set_width(pointcloud->point_size());
  }
}

uint64_t Velodyne64Parser::get_timestamp(double base_time, float time_offset,
                                       uint16_t block_id) {
  double t = base_time - time_offset;
  double timestamp = 0;
  int index = 0;

  if (is_s2_) {
    index = block_id & 1;  // % 2
    double& previous_packet_stamp = previous_packet_stamp_[index];
    uint64_t& gps_base_usec = gps_base_usec_[index];
    timestamp = get_gps_stamp(t, previous_packet_stamp, gps_base_usec);
  } else {                 // 64E_S3
    index = block_id & 3;  // % 4
    double& previous_packet_stamp = previous_packet_stamp_[index];
    uint64_t& gps_base_usec = gps_base_usec_[index];
    timestamp = get_gps_stamp(t, previous_packet_stamp, gps_base_usec);
  }
  return timestamp;
}

int Velodyne64Parser::intensity_compensate(const LaserCorrection& corrections,
                                           const uint16_t raw_distance,
                                           int intensity) {
  float tmp = 1 - static_cast<float>(raw_distance) / 65535;
  intensity += corrections.focal_slope *
               (fabs(corrections.focal_offset - 256 * tmp * tmp));

  if (intensity < corrections.min_intensity) {
    intensity = corrections.min_intensity;
  }

  if (intensity > corrections.max_intensity) {
    intensity = corrections.max_intensity;
  }
  return intensity;
}

void Velodyne64Parser::unpack(const VelodynePacket& pkt, std::shared_ptr<PointCloud>& pc) {
  ADEBUG << "Received packet, time: " << pkt.stamp();

  // const RawPacket* raw = (const RawPacket*)&pkt.data[0];
  const RawPacket* raw = (const RawPacket*)pkt.data().c_str();
  double basetime = raw->gps_timestamp;  // usec

  for (int i = 0; i < BLOCKS_PER_PACKET; ++i) {  // 12
    if (mode_ != DUAL && !is_s2_ && ((i & 3) >> 1) > 0) {
      // i%4/2  even-numbered block contain duplicate data
      continue;
    }

    // upper bank lasers are numbered [0..31], lower bank lasers are [32..63]
    // NOTE: this is a change from the old velodyne_common implementation
    int bank_origin = (raw->blocks[i].laser_block_id == LOWER_BANK) ? 32 : 0;

    for (int j = 0, k = 0; j < SCANS_PER_BLOCK;
         ++j, k += RAW_SCAN_SIZE) {  // 32, 3
      // One point
      uint8_t laser_number = j + bank_origin;  // hardware laser number
      LaserCorrection& corrections =
          calibration_.laser_corrections_[laser_number];

      union RawDistance raw_distance;
      raw_distance.bytes[0] = raw->blocks[i].data[k];
      raw_distance.bytes[1] = raw->blocks[i].data[k + 1];

      // compute time
      uint64_t timestamp = get_timestamp(basetime, (*inner_time_)[i][j], i);

      if (j == SCANS_PER_BLOCK - 1) {
        // set header stamp before organize the point cloud
        pc->set_measurement_time(static_cast<double>(timestamp / 1e9));
      }

      float distance = raw_distance.raw_distance * DISTANCE_RESOLUTION +
                       corrections.dist_correction;

      if (raw_distance.raw_distance == 0 ||
          !is_scan_valid(raw->blocks[i].rotation, distance)) {
        // if organized append a nan point to the cloud
        if (config_.organized()) {
          apollo::drivers::PointXYZIT* point_new = pc->add_point();
          point_new->set_x(nan);
          point_new->set_y(nan);
          point_new->set_z(nan);
          point_new->set_timestamp(timestamp);
          point_new->set_intensity(0);
        }
        continue;
      }

      apollo::drivers::PointXYZIT* point = pc->add_point();
      point->set_timestamp(timestamp);
      // Position Calculation, append this point to the cloud
      compute_coords(distance, corrections, raw->blocks[i].rotation, point);
      point->set_intensity(intensity_compensate(
          corrections, raw_distance.raw_distance, raw->blocks[i].data[k + 2]));
      // append this point to the cloud
    }
  }
}

void Velodyne64Parser::order(std::shared_ptr<PointCloud>& cloud) {

  int height = 64;
  cloud->set_height(height);
  int width = cloud->point_size() / cloud->height();
  cloud->set_width(width);

  std::shared_ptr<PointCloud> cloud_origin =
      std::make_shared<PointCloud>();
  cloud_origin->CopyFrom(*cloud);

  for (int i = 0; i < height; ++i) {
      int col = velodyne::ORDER_64[i];

      for (int j = 0; j < width; ++j) {
          // make sure offset is initialized, should be init at setup() just once
          int row = (j + offsets_[i] + width) % width;
          int target_index = j * height + i;
          int origin_index = row * height + col;
          cloud->mutable_point(target_index)->CopyFrom(cloud_origin->point(origin_index));

      }
  }

}

}  // namespace velodyne
}  // namespace drivers
}  // namespace apollo