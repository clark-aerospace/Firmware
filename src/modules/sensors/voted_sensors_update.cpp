/****************************************************************************
 *
 *   Copyright (c) 2016 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file voted_sensors_update.cpp
 *
 * @author Beat Kueng <beat-kueng@gmx.net>
 */

#include "voted_sensors_update.h"

#include <lib/sensor_calibration/Utilities.hpp>
#include <lib/ecl/geo/geo.h>
#include <lib/systemlib/mavlink_log.h>
#include <uORB/Subscription.hpp>

using namespace sensors;
using namespace matrix;
using namespace time_literals;

VotedSensorsUpdate::VotedSensorsUpdate(bool hil_enabled,
				       uORB::SubscriptionCallbackWorkItem(&vehicle_imu_sub)[MAX_SENSOR_COUNT]) :
	ModuleParams(nullptr),
	_vehicle_imu_sub(vehicle_imu_sub),
	_hil_enabled(hil_enabled)
{
	if (_hil_enabled) { // HIL has less accurate timing so increase the timeouts a bit
		_gyro.voter.set_timeout(500000);
		_accel.voter.set_timeout(500000);
	}
}

int VotedSensorsUpdate::init(sensor_combined_s &raw)
{
	raw.accelerometer_timestamp_relative = sensor_combined_s::RELATIVE_TIMESTAMP_INVALID;
	raw.timestamp = 0;

	initializeSensors();

	_selection_changed = true;

	return 0;
}

void VotedSensorsUpdate::initializeSensors()
{
	initSensorClass(_gyro, MAX_SENSOR_COUNT);
	initSensorClass(_accel, MAX_SENSOR_COUNT);
}

void VotedSensorsUpdate::parametersUpdate()
{
	updateParams();

	// run through all IMUs
	for (uint8_t uorb_index = 0; uorb_index < MAX_SENSOR_COUNT; uorb_index++) {
		uORB::SubscriptionData<vehicle_imu_s> imu{ORB_ID(vehicle_imu), uorb_index};
		imu.update();

		if (imu.get().timestamp > 0 && imu.get().accel_device_id > 0 && imu.get().gyro_device_id > 0) {

			// find corresponding configured accel priority
			int8_t accel_cal_index = calibration::FindCalibrationIndex("ACC", imu.get().accel_device_id);

			if (accel_cal_index >= 0) {
				// found matching CAL_ACCx_PRIO
				int32_t accel_priority_old = _accel.priority_configured[uorb_index];

				_accel.priority_configured[uorb_index] = calibration::GetCalibrationParam("ACC", "PRIO", accel_cal_index);

				if (accel_priority_old != _accel.priority_configured[uorb_index]) {
					if (_accel.priority_configured[uorb_index] == 0) {
						// disabled
						_accel.priority[uorb_index] = 0;

					} else {
						// change relative priority to incorporate any sensor faults
						int priority_change = _accel.priority_configured[uorb_index] - accel_priority_old;
						_accel.priority[uorb_index] = math::constrain(_accel.priority[uorb_index] + priority_change, 1, 100);
					}
				}
			}

			// find corresponding configured gyro priority
			int8_t gyro_cal_index = calibration::FindCalibrationIndex("GYRO", imu.get().gyro_device_id);

			if (gyro_cal_index >= 0) {
				// found matching CAL_GYROx_PRIO
				int32_t gyro_priority_old = _gyro.priority_configured[uorb_index];

				_gyro.priority_configured[uorb_index] = calibration::GetCalibrationParam("GYRO", "PRIO", gyro_cal_index);

				if (gyro_priority_old != _gyro.priority_configured[uorb_index]) {
					if (_gyro.priority_configured[uorb_index] == 0) {
						// disabled
						_gyro.priority[uorb_index] = 0;

					} else {
						// change relative priority to incorporate any sensor faults
						int priority_change = _gyro.priority_configured[uorb_index] - gyro_priority_old;
						_gyro.priority[uorb_index] = math::constrain(_gyro.priority[uorb_index] + priority_change, 1, 100);
					}
				}
			}
		}
	}
}

void VotedSensorsUpdate::imuPoll(struct sensor_combined_s &raw)
{
	for (int uorb_index = 0; uorb_index < MAX_SENSOR_COUNT; uorb_index++) {
		vehicle_imu_s imu_report;

		if ((_accel.priority[uorb_index] > 0) && (_gyro.priority[uorb_index] > 0)
		    && _vehicle_imu_sub[uorb_index].update(&imu_report)) {

			// copy corresponding vehicle_imu_status for accel & gyro error counts
			vehicle_imu_status_s imu_status{};
			_vehicle_imu_status_subs[uorb_index].copy(&imu_status);

			_accel_device_id[uorb_index] = imu_report.accel_device_id;
			_gyro_device_id[uorb_index] = imu_report.gyro_device_id;

			// convert the delta velocities to an equivalent acceleration
			const float accel_dt_inv = 1.e6f / (float)imu_report.delta_velocity_dt;
			Vector3f accel_data = Vector3f{imu_report.delta_velocity} * accel_dt_inv;


			// convert the delta angles to an equivalent angular rate
			const float gyro_dt_inv = 1.e6f / (float)imu_report.delta_angle_dt;
			Vector3f gyro_rate = Vector3f{imu_report.delta_angle} * gyro_dt_inv;

			_last_sensor_data[uorb_index].timestamp = imu_report.timestamp_sample;
			_last_sensor_data[uorb_index].accelerometer_m_s2[0] = accel_data(0);
			_last_sensor_data[uorb_index].accelerometer_m_s2[1] = accel_data(1);
			_last_sensor_data[uorb_index].accelerometer_m_s2[2] = accel_data(2);
			_last_sensor_data[uorb_index].accelerometer_integral_dt = imu_report.delta_velocity_dt;
			_last_sensor_data[uorb_index].accelerometer_clipping = imu_report.delta_velocity_clipping;
			_last_sensor_data[uorb_index].gyro_rad[0] = gyro_rate(0);
			_last_sensor_data[uorb_index].gyro_rad[1] = gyro_rate(1);
			_last_sensor_data[uorb_index].gyro_rad[2] = gyro_rate(2);
			_last_sensor_data[uorb_index].gyro_integral_dt = imu_report.delta_angle_dt;


			_last_accel_timestamp[uorb_index] = imu_report.timestamp_sample;

			_accel.voter.put(uorb_index, imu_report.timestamp, _last_sensor_data[uorb_index].accelerometer_m_s2,
					 imu_status.accel_error_count, _accel.priority[uorb_index]);

			_gyro.voter.put(uorb_index, imu_report.timestamp, _last_sensor_data[uorb_index].gyro_rad,
					imu_status.gyro_error_count, _gyro.priority[uorb_index]);
		}
	}

	// find the best sensor
	int accel_best_index = -1;
	int gyro_best_index = -1;

	if (_param_sens_imu_mode.get()) {
		_accel.voter.get_best(hrt_absolute_time(), &accel_best_index);
		_gyro.voter.get_best(hrt_absolute_time(), &gyro_best_index);

		checkFailover(_accel, "Accel");
		checkFailover(_gyro, "Gyro");

	} else {

		// use sensor_selection to find best
		if (_sensor_selection_sub.update(&_selection)) {
			// reset inconsistency checks against primary
			for (int sensor_index = 0; sensor_index < MAX_SENSOR_COUNT; sensor_index++) {
				_accel_diff[sensor_index].zero();
			}

			for (int sensor_index = 0; sensor_index < MAX_SENSOR_COUNT; sensor_index++) {
				_gyro_diff[sensor_index].zero();
			}
		}

		for (int i = 0; i < MAX_SENSOR_COUNT; i++) {
			if ((_accel_device_id[i] != 0) && (_accel_device_id[i] == _selection.accel_device_id)) {
				accel_best_index = i;
			}

			if ((_gyro_device_id[i] != 0) && (_gyro_device_id[i] == _selection.gyro_device_id)) {
				gyro_best_index = i;
			}
		}
	}

	// write data for the best sensor to output variables
	if ((accel_best_index >= 0) && (gyro_best_index >= 0)) {
		raw.timestamp = _last_sensor_data[gyro_best_index].timestamp;
		memcpy(&raw.accelerometer_m_s2, &_last_sensor_data[accel_best_index].accelerometer_m_s2,
		       sizeof(raw.accelerometer_m_s2));
		memcpy(&raw.gyro_rad, &_last_sensor_data[gyro_best_index].gyro_rad, sizeof(raw.gyro_rad));
		raw.accelerometer_integral_dt = _last_sensor_data[accel_best_index].accelerometer_integral_dt;
		raw.gyro_integral_dt = _last_sensor_data[gyro_best_index].gyro_integral_dt;
		raw.accelerometer_clipping = _last_sensor_data[accel_best_index].accelerometer_clipping;

		if ((accel_best_index != _accel.last_best_vote) || (_selection.accel_device_id != _accel_device_id[accel_best_index])) {
			_accel.last_best_vote = (uint8_t)accel_best_index;
			_selection.accel_device_id = _accel_device_id[accel_best_index];
			_selection_changed = true;
		}

		if ((_gyro.last_best_vote != gyro_best_index) || (_selection.gyro_device_id != _gyro_device_id[gyro_best_index])) {
			_gyro.last_best_vote = (uint8_t)gyro_best_index;
			_selection.gyro_device_id = _gyro_device_id[gyro_best_index];
			_selection_changed = true;

			// clear all registered callbacks
			for (auto &sub : _vehicle_imu_sub) {
				sub.unregisterCallback();
			}

			for (int i = 0; i < MAX_SENSOR_COUNT; i++) {
				vehicle_imu_s report{};

				if (_vehicle_imu_sub[i].copy(&report)) {
					if ((report.gyro_device_id != 0) && (report.gyro_device_id == _gyro_device_id[gyro_best_index])) {
						_vehicle_imu_sub[i].registerCallback();
					}
				}
			}
		}
	}
}

bool VotedSensorsUpdate::checkFailover(SensorData &sensor, const char *sensor_name)
{
	if (sensor.last_failover_count != sensor.voter.failover_count() && !_hil_enabled) {

		uint32_t flags = sensor.voter.failover_state();
		int failover_index = sensor.voter.failover_index();

		if (flags == DataValidator::ERROR_FLAG_NO_ERROR) {
			if (failover_index != -1) {
				// we switched due to a non-critical reason. No need to panic.
				PX4_INFO("%s sensor switch from #%i", sensor_name, failover_index);
			}

		} else {
			if (failover_index != -1) {

				const hrt_abstime now = hrt_absolute_time();

				if (now - _last_error_message > 3_s) {
					mavlink_log_emergency(&_mavlink_log_pub, "%s #%i fail: %s%s%s%s%s!",
							      sensor_name,
							      failover_index,
							      ((flags & DataValidator::ERROR_FLAG_NO_DATA) ? " OFF" : ""),
							      ((flags & DataValidator::ERROR_FLAG_STALE_DATA) ? " STALE" : ""),
							      ((flags & DataValidator::ERROR_FLAG_TIMEOUT) ? " TIMEOUT" : ""),
							      ((flags & DataValidator::ERROR_FLAG_HIGH_ERRCOUNT) ? " ERR CNT" : ""),
							      ((flags & DataValidator::ERROR_FLAG_HIGH_ERRDENSITY) ? " ERR DNST" : ""));
					_last_error_message = now;
				}

				// reduce priority of failed sensor to the minimum
				sensor.priority[failover_index] = 1;
			}
		}

		sensor.last_failover_count = sensor.voter.failover_count();
		return true;
	}

	return false;
}

void VotedSensorsUpdate::initSensorClass(SensorData &sensor_data, uint8_t sensor_count_max)
{
	bool added = false;
	int max_sensor_index = -1;

	for (unsigned i = 0; i < sensor_count_max; i++) {

		max_sensor_index = i;

		if (!sensor_data.advertised[i] && sensor_data.subscription[i].advertised()) {
			sensor_data.advertised[i] = true;
			sensor_data.priority[i] = DEFAULT_PRIORITY;
			sensor_data.priority_configured[i] = DEFAULT_PRIORITY;

			if (i > 0) {
				/* the first always exists, but for each further sensor, add a new validator */
				if (sensor_data.voter.add_new_validator()) {
					added = true;

				} else {
					PX4_ERR("failed to add validator for sensor %s %i", sensor_data.subscription[i].get_topic()->o_name, i);
				}
			}
		}
	}

	// never decrease the sensor count, as we could end up with mismatching validators
	if (max_sensor_index + 1 > sensor_data.subscription_count) {
		sensor_data.subscription_count = max_sensor_index + 1;
	}

	if (added) {
		// force parameter refresh if anything was added
		parametersUpdate();
	}
}

void VotedSensorsUpdate::printStatus()
{
	PX4_INFO("selected gyro: %d (%d)", _selection.gyro_device_id, _gyro.last_best_vote);
	_gyro.voter.print();

	PX4_INFO_RAW("\n");
	PX4_INFO("selected accel: %d (%d)", _selection.accel_device_id, _accel.last_best_vote);
	_accel.voter.print();
}

void VotedSensorsUpdate::sensorsPoll(sensor_combined_s &raw)
{
	imuPoll(raw);

	// publish sensor selection if changed
	if (_param_sens_imu_mode.get()) {
		if (_selection_changed) {
			// don't publish until selected IDs are valid
			if (_selection.accel_device_id > 0 && _selection.gyro_device_id > 0) {
				_selection.timestamp = hrt_absolute_time();
				_sensor_selection_pub.publish(_selection);
				_selection_changed = false;
			}

			for (int sensor_index = 0; sensor_index < MAX_SENSOR_COUNT; sensor_index++) {
				_accel_diff[sensor_index].zero();
				_gyro_diff[sensor_index].zero();
			}
		}
	}

	calcAccelInconsistency();
	calcGyroInconsistency();

	sensors_status_imu_s status{};
	status.accel_device_id_primary = _selection.accel_device_id;
	status.gyro_device_id_primary = _selection.gyro_device_id;

	for (int i = 0; i < MAX_SENSOR_COUNT; i++) {
		if (_accel_device_id[i] != 0) {
			status.accel_device_ids[i] = _accel_device_id[i];
			status.accel_inconsistency_m_s_s[i] = _accel_diff[i].norm();
			status.accel_healthy[i] = (_accel.voter.get_sensor_state(i) == DataValidator::ERROR_FLAG_NO_ERROR);
		}

		if (_gyro_device_id[i] != 0) {
			status.gyro_device_ids[i] = _gyro_device_id[i];
			status.gyro_inconsistency_rad_s[i] = _gyro_diff[i].norm();
			status.gyro_healthy[i] = (_gyro.voter.get_sensor_state(i) == DataValidator::ERROR_FLAG_NO_ERROR);
		}
	}


	status.timestamp = hrt_absolute_time();
	_sensors_status_imu_pub.publish(status);
}

void VotedSensorsUpdate::setRelativeTimestamps(sensor_combined_s &raw)
{
	if (_last_accel_timestamp[_accel.last_best_vote]) {
		raw.accelerometer_timestamp_relative = (int32_t)((int64_t)_last_accel_timestamp[_accel.last_best_vote] -
						       (int64_t)raw.timestamp);
	}
}

void VotedSensorsUpdate::calcAccelInconsistency()
{
	const Vector3f primary_accel{_last_sensor_data[_accel.last_best_vote].accelerometer_m_s2};

	// Check each sensor against the primary
	for (int sensor_index = 0; sensor_index < MAX_SENSOR_COUNT; sensor_index++) {
		// check that the sensor we are checking against is not the same as the primary
		if (_accel.advertised[sensor_index] && (_accel.priority[sensor_index] > 0)
		    && (_accel_device_id[sensor_index] != _selection.accel_device_id)) {

			const Vector3f current_accel{_last_sensor_data[sensor_index].accelerometer_m_s2};
			_accel_diff[sensor_index] = 0.95f * _accel_diff[sensor_index] + 0.05f * (primary_accel - current_accel);
		}
	}
}

void VotedSensorsUpdate::calcGyroInconsistency()
{
	const Vector3f primary_gyro{_last_sensor_data[_gyro.last_best_vote].gyro_rad};

	// Check each sensor against the primary
	for (int sensor_index = 0; sensor_index < MAX_SENSOR_COUNT; sensor_index++) {
		// check that the sensor we are checking against is not the same as the primary
		if (_gyro.advertised[sensor_index] && (_gyro.priority[sensor_index] > 0)
		    && (_gyro_device_id[sensor_index] != _selection.gyro_device_id)) {

			const Vector3f current_gyro{_last_sensor_data[sensor_index].gyro_rad};
			_gyro_diff[sensor_index] = 0.95f * _gyro_diff[sensor_index] + 0.05f * (primary_gyro - current_gyro);
		}
	}
}
