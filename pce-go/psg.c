// psg.c - Programmable Sound Generator
//
#include <stdlib.h>
#include <string.h>
#include "pico.h"
#include "pce.h"
#include "psg.h"

// Legacy logarithmic amplitude table. No longer used now that DDA shares the
// wave/noise linear volume path; kept for reference (see plan Phase 4).
static const uint8_t __attribute__((unused)) vol_tbl[32] = {
	100 >> 8, 451 >> 8, 508 >> 8, 573 >> 8, 646 >> 8, 728 >> 8, 821 >> 8, 925 >> 8,
	1043 >> 8, 1175 >> 8, 1325 >> 8, 1493 >> 8, 1683 >> 8, 1898 >> 8, 2139 >> 8, 2411 >> 8,
	2718 >> 8, 3064 >> 8, 3454 >> 8, 3893 >> 8, 4388 >> 8, 4947 >> 8, 5576 >> 8, 6285 >> 8,
	7085 >> 8, 7986 >> 8, 9002 >> 8, 10148 >> 8, 11439 >> 8, 12894 >> 8, 14535 >> 8, 16384 >> 8
};

// The buffer should be signed but it seems to sound better
// unsigned. I am still reviewing the implementation bellow.
// In some games it also sounds better in 8 bit than in 16...
// typedef uint8_t sample_t;
typedef int16_t sample_t;

static int samplerate = 22050;
static int stereo = true;


static void __not_in_flash_func(psg_update_chan)(sample_t *buf, int ch, size_t dwSize)
{
	psg_chan_t *chan = &PCE.PSG.chan[ch];
	int sample = 0;
	uint32_t Tp;
	sample_t *buf_end = buf + dwSize;

	/*
	* This gives us a volume level of (0...15).
	*/
	int lvol = ((chan->balance >> 4) * 9 * (chan->control & 0x1F)) / 256;
	int rvol = ((chan->balance & 0xF) * 9 * (chan->control & 0x1F)) / 256;

	if (!stereo) {
		lvol = (lvol + rvol) / 2;
	}

	/*
	* Channel priority when enabled (Mesen2 PcePsgChannel): DDA > Noise > Wave.
	* When disabled, the channel is silent — the final memset zero-fills buf.
	* Synthesis is driven at the output sample rate; sample accuracy comes from
	* osd_psg_sync segmenting these calls at each register write.
	*/
	if (!(chan->control & PSG_CHAN_ENABLE)) {
		chan->wave_accum = 0;
	}
	/*
	* DDA: sample-and-hold the single value last written to reg 6. Centered
	* like the wave path (value - 16, with the +1 deadband for >=0).
	*/
	else if (chan->control & PSG_DDA_ENABLE) {
		if ((sample = ((int)chan->dda_value - 16)) >= 0)
			sample++;
		while (buf < buf_end) {
			*buf++ = (sample * lvol);
			if (stereo)
				*buf++ = (sample * rvol);
		}
	}
	/*
	* Noise generation (ch 4/5 only). Real PCE 18-bit LFSR (taps 0,1,11,12,17)
	* clocked at master/period, period = (freq==0x1F)?32:((~freq)&0x1F)*64 in
	* PSG clocks. Advanced via a 16.16 fixed accumulator of PSG-clocks-per-
	* output-sample.
	*/
	else if ((ch == 4 || ch == 5) && (chan->noise_ctrl & PSG_NOISE_ENABLE)) {
		uint32_t freq = chan->noise_ctrl & 0x1F;
		uint32_t period = (freq == 0x1F) ? 32u : (((~freq) & 0x1F) * 64u);
		uint32_t clk_per_sample = (uint32_t)(((uint64_t)CLOCK_PSG << 16) / (uint32_t)samplerate);
		uint32_t period_fp = period << 16;

		while (buf < buf_end) {
			chan->noise_accum += (int32_t)clk_per_sample;
			while ((uint32_t)chan->noise_accum >= period_fp) {
				chan->noise_accum -= (int32_t)period_fp;
				uint32_t v = (uint32_t)chan->noise_rand;
				uint32_t bit = ((v >> 0) ^ (v >> 1) ^ (v >> 11)
				              ^ (v >> 12) ^ (v >> 17)) & 0x01;
				chan->noise_level = (v & 0x01) ? 15 : -16;
				chan->noise_rand = (int32_t)((v >> 1) | (bit << 17));
			}

			*buf++ = (chan->noise_level * lvol);

			if (stereo) {
				*buf++ = (chan->noise_level * rvol);
			}
		}
	}
	/*
	* PSG Wave generation.
	*/
	else if ((Tp = chan->freq_lsb + (chan->freq_msb << 8)) > 0) {
		/*
		 * Thank god for well commented code!  The original line of code read:
		 * fixed_inc = ((uint32_t) (3.2 * 1118608 / samplerate) << 16) / Tp;
		 * and had nary a comment to be found.  It took a little head scratching to get
		 * it figured out.  The 3.2 * 1118608 comes out to 3574595.6 which is obviously
		 * meant to represent the 3.58mhz cpu clock speed used in the pc engine to
		 * decrement the sound 'frequency'.  I haven't figured out why the original
		 * author had the two numbers multiplied together to get the odd value instead of
		 * just using 3580000.  I did some checking and the value will compute the same
		 * using either value divided by any standard soundcard samplerate.
		 *
		 * Taken from the PSG doc written by Paul Clifford (paul@plasma.demon.co.uk)
		 * <in reference to the 12 bit frequency value in PSG registers 2 and 3>
		 * "For waveform output, a copy of this value is, in effect, decremented 3,580,000
		 *  times a second until zero is reached.  When this happens the PSG advances an
		 *  internal pointer into the channel's waveform buffer by one."
		 *
		 * So all we need to do to emulate original pc engine behaviour is take our soundcard's
		 * sampling rate into consideration with regard to the 3580000 effective pc engine
		 * samplerate.  We use 16.16 fixed arithmetic for speed.
		 */
		uint32_t fixed_inc = ((CLOCK_PSG / samplerate) << 16) / Tp;

		while (buf < buf_end) {
			if ((sample = (chan->wave_data[chan->wave_index] - 16)) >= 0)
				sample++;

			*buf++ = (sample * lvol);

			if (stereo) {
				*buf++ = (sample * rvol);
			}

			chan->wave_accum += fixed_inc;
			chan->wave_accum &= 0x1FFFFF;	/* (31 << 16) + 0xFFFF */
			chan->wave_index = chan->wave_accum >> 16;
		}
	}

	if (buf < buf_end) {
		memset(buf, 0, (void*)buf_end - (void*)buf);
	}
}


int
psg_init(int _samplerate, bool _stereo)
{
	// Seed the noise LFSRs non-zero (Mesen2 uses 1). The 18-bit LFSR is a
	// stuck-at-zero state if seeded with 0, producing silent noise.
	PCE.PSG.chan[4].noise_rand = 1;
	PCE.PSG.chan[5].noise_rand = 1;

	samplerate = _samplerate;
	stereo = _stereo;

	return 0;
}


void
psg_term(void)
{
	//
}


void __not_in_flash_func(psg_update)(int16_t *output, size_t length, uint32_t channels)
{
	int lvol = (PCE.PSG.volume >> 4);
	int rvol = (PCE.PSG.volume & 0x0F);

	if (stereo) {
		length *= 2;
	}

	memset(output, 0, length * sizeof(int16_t));
#if 1
	static sample_t mix_buffer[((44100 / 60) * 2) + 2];
#else
	static sample_t mix_buffer[256]; // Use this to test buffer overflow
#endif
	for (int i = 0; i < PSG_CHANNELS; i++)
	{
		psg_update_chan(mix_buffer, i, length);

		// We still emulate disabled channel, we just don't mix them with the output
		if (!(channels & (1 << i)))
			continue;

		for (int j = 0; j < length; j += 2) {
			int sl = output[j] + mix_buffer[j] * lvol;
			int sr = output[j + 1] + mix_buffer[j + 1] * rvol;
			if (sl > 32767) sl = 32767; else if (sl < -32768) sl = -32768;
			if (sr > 32767) sr = 32767; else if (sr < -32768) sr = -32768;
			output[j] = sl;
			output[j + 1] = sr;
		}
	}
}
