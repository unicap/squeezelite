/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 * JackDMP output for squeezelite
 * (c) Arne Caspari 2015, arne@unicap-imaging.org
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// JackDMP Output


#include "squeezelite.h"

#if JACK

#include <jack/jack.h>

jack_port_t *output_port1, *output_port2;
jack_client_t *client;

static log_level loglevel;

static bool running = true;

extern struct outputstate output;
extern struct buffer *outputbuf;

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

#define CLIENT_NAME "squeezelite"

extern u8_t *silencebuf;
#if DSD
extern u8_t *silencebuf_dop;
#endif

void list_devices(void) {
}

void set_volume(unsigned left, unsigned right) {
	LOG_DEBUG("setting internal gain left: %u right: %u", left, right);
	LOCK;
	output.gainL = left;
	output.gainR = right;
	UNLOCK;
}

bool test_open(const char *device, unsigned rates[])
{
	/* client = jack_client_open (CLIENT_NAME, JackNullOption, &status, NULL); */
	/* if (client == NULL) { */
	/* 	fprintf (stderr, "jack_client_open() failed, " */
	/* 		 "status = 0x%2.0x\n", status); */
	/* 	if (status & JackServerFailed) { */
	/* 		fprintf (stderr, "Unable to connect to JACK server\n"); */
	/* 	} */
	/* 	return false; */
	/* } */

	/* if (status & JackServerStarted){ */
	/* 	fprintf (stderr, "JACK server started\n"); */
	/* } */
	/* if (status & JackNameNotUnique) { */
	/* 	const char *client_name; */
	/* 	client_name = jack_get_client_name(client); */
	/* 	fprintf (stderr, "unique name `%s' assigned\n", client_name); */
	/* } */

	/* jack_client_close (client); */

	return true;
}


static u8_t *optr;

static int _write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
						 s32_t cross_gain_in, s32_t cross_gain_out, s32_t **cross_ptr) {
	if (!silence) {
		if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr) {
			_apply_cross(outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
		}

		if (gainL != FIXED_ONE || gainR!= FIXED_ONE) {
			_apply_gain(outputbuf, out_frames, gainL, gainR);
		}

		IF_DSD(
			if (output.dop) {
				update_dop((u32_t *) outputbuf->readp, out_frames, output.invert);
			}
		)

			memcpy(optr, outputbuf->readp, out_frames * BYTES_PER_FRAME);

	} else {

		u8_t *buf = silencebuf;

		IF_DSD(
			if (output.dop) {
				buf = silencebuf_dop;
				update_dop((u32_t *) buf, out_frames, false); // don't invert silence
			}
		)

		memcpy(optr, buf, out_frames * BYTES_PER_FRAME);
	}

	optr += out_frames * BYTES_PER_FRAME;

	return (int)out_frames;
}

/**
 * The process callback for this JACK application is called in a
 * special realtime thread once for each audio cycle.
 *
 * This client follows a simple rule: when the JACK transport is
 * running, copy the input port to the output.  When it stops, exit.
 */

int
output_jack_process (jack_nframes_t nframes, void *arg)
{
	jack_default_audio_sample_t *out1, *out2;
	int i;
	frames_t frames_remaining = nframes;
	frames_t frames;
	s32_t *ptr;

	ptr = (s32_t *)malloc (nframes * BYTES_PER_FRAME);
	optr = (u8_t*)ptr;

	LOCK;

	out1 = (jack_default_audio_sample_t*)jack_port_get_buffer (output_port1, nframes);
	out2 = (jack_default_audio_sample_t*)jack_port_get_buffer (output_port2, nframes);

	output.updated = gettime_ms();
	output.frames_played_dmp = output.frames_played;

	do {
		frames = _output_frames(frames_remaining);
		frames_remaining -= frames;
	} while (frames_remaining > 0 && frames != 0);

	if (frames_remaining > 0) {
		LOG_DEBUG("pad with silence");
		memset(optr, 0, frames_remaining * BYTES_PER_FRAME);
	}

	for( i=0; i<nframes; i++ )
	{
		out1[i] = (float)((float)ptr[i*2]/(float)INT_MAX);  /* left */
		out2[i] = (float)((float)ptr[i*2+1]/(float)INT_MAX);  /* right */
	}

	UNLOCK;

	free (ptr);

	return 0;
}

void output_init_jack(log_level level,
		      const char *device, unsigned output_buf_size,
		      char *params, unsigned rates[], unsigned rate_delay,
		      unsigned idle)
{
	const char **ports;
	jack_status_t status;
	int i;

	char *portspec = next_param(params, ':');
	char *p = next_param(NULL, ':');

	LOG_INFO ("Enter init_jack");
	loglevel = level;

	LOG_INFO ("init output");
	LOG_INFO ("params: %s, portspec: %s", params, portspec);

	memset(&output, 0, sizeof(output));
	/* output.latency = latency; */
	output.format = 0;
	output.start_frames = 0;
	output.write_cb = &_write_frames;
	output.rate_delay = rate_delay;

	//LOG_INFO("requested latency: %u", output.latency);
	client = jack_client_open (CLIENT_NAME, JackNullOption, &status, NULL);
	if (client == NULL) {
		fprintf (stderr, "jack_client_open() failed, "
			 "status = 0x%2.0x\n", status);
		if (status & JackServerFailed) {
			fprintf (stderr, "Unable to connect to JACK server\n");
		}
		return;
	}

	if (status & JackServerStarted){
		fprintf (stderr, "JACK server started\n");
	}
	if (status & JackNameNotUnique) {
		const char *client_name;
		client_name = jack_get_client_name(client);
		fprintf (stderr, "unique name `%s' assigned\n", client_name);
	}

	jack_set_process_callback (client, output_jack_process, NULL);

	/* create two ports */
	output_port1 = jack_port_register (client, "output1",
					  JACK_DEFAULT_AUDIO_TYPE,
					  JackPortIsOutput, 0);

	output_port2 = jack_port_register (client, "output2",
					  JACK_DEFAULT_AUDIO_TYPE,
					  JackPortIsOutput, 0);

	if ((output_port1 == NULL) || (output_port2 == NULL)) {
		fprintf(stderr, "no more JACK ports available\n");
		exit (1);
	}

	/* Tell the JACK server that we are ready to roll.  Our
	 * process() callback will start running now. */

	if (jack_activate (client)) {
		fprintf (stderr, "cannot activate client");
		exit (1);
	}

	/* Connect the ports.  You can't do this before the client is
	 * activated, because we can't make connections to clients
	 * that aren't running.  Note the confusing (but necessary)
	 * orientation of the driver backend ports: playback ports are
	 * "input" to the backend, and capture ports are "output" from
	 * it.
	 */
	ports = jack_get_ports (client, portspec, NULL,
				JackPortIsInput);
	if (ports == NULL) {
		fprintf(stderr, "no playback ports match portspec \"%s\"\n",
			portspec);
		exit (1);
	}

	if (jack_connect (client, jack_port_name (output_port1), ports[0])) {
		fprintf (stderr, "cannot connect output ports\n");
	}

	if (jack_connect (client, jack_port_name (output_port2), ports[1])) {
		fprintf (stderr, "cannot connect output ports\n");
	}

	jack_free (ports);

	for (i=0; i < MAX_SUPPORTED_SAMPLERATES; i++){
		rates[i] = 0;
	}

	rates[0] = jack_get_sample_rate (client);
	LOG_INFO ("sample rate: %d", rates[0]);



	output_init_common(level, device, output_buf_size, rates, idle);
}

void output_close_jack(void) {
	LOG_INFO("close output");

	LOCK;

	running = false;
	jack_client_close (client);

	UNLOCK;

	output_close_common();
}

#endif // JACK
