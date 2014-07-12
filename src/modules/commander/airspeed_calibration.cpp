/****************************************************************************
 *
 *   Copyright (c) 2013, 2014 PX4 Development Team. All rights reserved.
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
 * @file airspeed_calibration.cpp
 * Airspeed sensor calibration routine
 */

#include "airspeed_calibration.h"
#include "calibration_messages.h"
#include "commander_helper.h"

#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <math.h>
#include <drivers/drv_hrt.h>
#include <drivers/drv_airspeed.h>
#include <uORB/topics/sensor_combined.h>
#include <uORB/topics/differential_pressure.h>
#include <mavlink/mavlink_log.h>
#include <systemlib/param/param.h>
#include <systemlib/err.h>
#include <systemlib/airspeed.h>

/* oddly, ERROR is not defined for c++ */
#ifdef ERROR
# undef ERROR
#endif
static const int ERROR = -1;

static const char *sensor_name = "dpress";

int do_airspeed_calibration(int mavlink_fd)
{
	/* give directions */
	mavlink_log_info(mavlink_fd, CAL_STARTED_MSG, sensor_name);

	const unsigned calibration_count = 2000;

	int diff_pres_sub = orb_subscribe(ORB_ID(differential_pressure));
	struct differential_pressure_s diff_pres;

	float diff_pres_offset = 0.0f;

	/* Reset sensor parameters */
	struct airspeed_scale airscale = {
		diff_pres_offset,
		1.0f,
	};

	bool paramreset_successful = false;
	int  fd = open(AIRSPEED_DEVICE_PATH, 0);

	if (fd > 0) {
		if (OK == ioctl(fd, AIRSPEEDIOCSSCALE, (long unsigned int)&airscale)) {
			paramreset_successful = true;

		} else {
			mavlink_log_critical(mavlink_fd, "airspeed offset zero failed");
		}

		close(fd);
	}

	if (!paramreset_successful) {
		warn("FAILED to reset - assuming analog");
		mavlink_log_critical(mavlink_fd, "If analog sens, retry with [SENS_DPRES_ANSC=1000]");

		if (param_set(param_find("SENS_DPRES_OFF"), &(diff_pres_offset))) {
			mavlink_log_critical(mavlink_fd, CAL_FAILED_SET_PARAMS_MSG);
			close(diff_pres_sub);
			return ERROR;
		}
	}

	unsigned calibration_counter = 0;

	mavlink_log_critical(mavlink_fd, "Ensure sensor is not measuring wind");
	usleep(500 * 1000);

	while (calibration_counter < calibration_count) {

		/* wait blocking for new data */
		struct pollfd fds[1];
		fds[0].fd = diff_pres_sub;
		fds[0].events = POLLIN;

		int poll_ret = poll(fds, 1, 1000);

		if (poll_ret) {
			orb_copy(ORB_ID(differential_pressure), diff_pres_sub, &diff_pres);

			if (calc_indicated_airspeed(fabsf(diff_pres.differential_pressure_raw_pa)) < 10.0f) {
				mavlink_log_critical(mavlink_fd, "Put a finger on front hole of pitot");
				usleep(5000 * 1000);
				continue;
			}

			/* do not log negative values in the second go */
			if ((diff_pres_offset / calibration_counter) < calc_indicated_airspeed(-5.0f)) {
				mavlink_log_critical(mavlink_fd, "Negative val: swap static<->dynamic ports,restart");
				close(diff_pres_sub);
				return ERROR;
			}

			diff_pres_offset += diff_pres.differential_pressure_raw_pa;
			calibration_counter++;

			if (calibration_counter % (calibration_count / 20) == 0) {
				mavlink_log_info(mavlink_fd, CAL_PROGRESS_MSG, sensor_name, (calibration_counter * 100) / calibration_count / 2);
			}

		} else if (poll_ret == 0) {
			/* any poll failure for 1s is a reason to abort */
			mavlink_log_critical(mavlink_fd, CAL_FAILED_MSG, sensor_name);
			close(diff_pres_sub);
			return ERROR;
		}
	}

	diff_pres_offset = diff_pres_offset / calibration_count;

	if (isfinite(diff_pres_offset)) {

		if (param_set(param_find("SENS_DPRES_OFF"), &(diff_pres_offset))) {
			mavlink_log_critical(mavlink_fd, CAL_FAILED_SET_PARAMS_MSG);
			close(diff_pres_sub);
			return ERROR;
		}

		/* auto-save to EEPROM */
		int save_ret = param_save_default();

		if (save_ret != 0) {
			warn("WARNING: auto-save of params to storage failed");
			mavlink_log_critical(mavlink_fd, CAL_FAILED_SAVE_PARAMS_MSG);
			close(diff_pres_sub);
			return ERROR;
		}

	} else {
		mavlink_log_info(mavlink_fd, CAL_FAILED_MSG, sensor_name);
		close(diff_pres_sub);
		return ERROR;
	}

	/* wait 500 ms to ensure parameter propagated through the system */
	usleep(500 * 1000);

	calibration_counter = 0;
	diff_pres_offset = 0.0f;

	/* just take a few samples and make sure pitot tubes are not reversed */
	while (calibration_counter < 50) {

		/* wait blocking for new data */
		struct pollfd fds[1];
		fds[0].fd = diff_pres_sub;
		fds[0].events = POLLIN;

		int poll_ret = poll(fds, 1, 1000);

		if (poll_ret) {
			orb_copy(ORB_ID(differential_pressure), diff_pres_sub, &diff_pres);

			diff_pres_offset += diff_pres.differential_pressure_raw_pa;
			calibration_counter++;

			float curr_avg = (diff_pres_offset / calibration_counter);

			if (calc_indicated_airspeed(fabsf(curr_avg)) < 10.0f) {
				mavlink_log_critical(mavlink_fd, "Put a finger on front hole of pitot (%.1f m/s)", (double)curr_avg);
				usleep(5000 * 1000);
				continue;
			}

			/* do not log negative values in the second go */
			if (curr_avg < -calc_indicated_airspeed(5.0f)) {
				mavlink_log_critical(mavlink_fd, "Negative val: swap static<->dynamic ports,restart");
				close(diff_pres_sub);

				/* the user setup is wrong, wipe the calibration to force a proper re-calibration */

				diff_pres_offset = 0.0f;
				if (param_set(param_find("SENS_DPRES_OFF"), &(diff_pres_offset))) {
					mavlink_log_critical(mavlink_fd, CAL_FAILED_SET_PARAMS_MSG);
					close(diff_pres_sub);
					return ERROR;
				}

				/* save */
				(void)param_save_default();

				return ERROR;
			}

		} else if (poll_ret == 0) {
			/* any poll failure for 1s is a reason to abort */
			mavlink_log_critical(mavlink_fd, CAL_FAILED_MSG, sensor_name);
			close(diff_pres_sub);
			return ERROR;
		}
	}

	mavlink_log_info(mavlink_fd, CAL_DONE_MSG, sensor_name);
	tune_neutral(true);
	close(diff_pres_sub);
	return OK;
}
