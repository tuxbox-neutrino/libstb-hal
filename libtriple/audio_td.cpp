#include <cstdio>
#include <cstdlib>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <hardware/aud/aud_inf.h>
#include <hardware/tddevices.h>
#include <avs/avs_inf.h>
#define AUDIO_DEVICE "/dev/" DEVICE_NAME_AUDIO
#include "audio_td.h"
#include "lt_debug.h"
#define lt_debug(args...) _lt_debug(TRIPLE_DEBUG_AUDIO, this, args)
#define lt_info(args...) _lt_info(TRIPLE_DEBUG_AUDIO, this, args)

#include <linux/soundcard.h>

cAudio * audioDecoder = NULL;

typedef struct audio_pdata
{
	int fd;
	int clipfd;
	int mixer_fd;
	int mixer_num;
} audio_pdata;
#define P ((audio_pdata *)pdata)

cAudio::cAudio(void *, void *, void *)
{
	pdata = calloc(1, sizeof(audio_pdata));
	P->clipfd = -1;
	P->mixer_fd = -1;
	int fd = open(AUDIO_DEVICE, O_RDWR);
	if (fd < 0)
		lt_info("%s: open failed (%m)\n", __func__);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	P->fd = fd;
	SetMute(true);
	muted = false;
}

cAudio::~cAudio(void)
{
	if (P->fd >= 0)
		close(P->fd);
	if (P->clipfd >= 0)
		close(P->clipfd);
	if (P->mixer_fd >= 0)
		close(P->mixer_fd);
	free(pdata);
}

int cAudio::mute(void)
{
	return SetMute(true);
}

int cAudio::unmute(void)
{
	return SetMute(false);
}

int cAudio::SetMute(bool enable)
{
	lt_debug("%s(%d)\n", __func__, enable);
	int avsfd;
	int ret;
	muted = enable;
	ret = ioctl(P->fd, MPEG_AUD_SET_MUTE, enable);
	if (ret < 0)
		lt_info("%s(%d) failed (%m)\n", __FUNCTION__, (int)enable);

	/* are we using alternative DSP / mixer? */
	if (P->clipfd != -1 || P->mixer_fd != -1)
		setVolume(volume,volume); /* considers "muted" variable, "remember"
					     is basically always true in this context */
	avsfd = open("/dev/stb/tdsystem", O_RDONLY);
	if (avsfd >= 0)
	{
		if (enable)
			ioctl(avsfd, IOC_AVS_SET_VOLUME, 31);
		else
			ioctl(avsfd, IOC_AVS_SET_VOLUME, 0);
		close(avsfd);
	}
	return ret;
}

int map_volume(const int volume)
{
	unsigned char vol = volume;
	if (vol > 100)
		vol = 100;

//	vol = (invlog63[volume] + 1) / 2;
	vol = 31 - vol * 31 / 100;
	return vol;
}

int cAudio::setVolume(unsigned int left, unsigned int right)
{
//	int avsfd;
	int ret;
	int vl = map_volume(left);
	int vr = map_volume(right);
	volume = (left + right) / 2;
	int v = map_volume(volume);
	if (P->clipfd != -1 && P->mixer_fd != -1) {
		int tmp = 0;
		/* not sure if left / right is correct here, but it is always the same anyways ;-) */
		if (! muted)
			tmp = left << 8 | right;
		ret = ioctl(P->mixer_fd, MIXER_WRITE(P->mixer_num), &tmp);
		if (ret == -1)
			lt_info("%s: MIXER_WRITE(%d),%04x: %m\n", __func__, P->mixer_num, tmp);
		return ret;
	}
//	if (settings.volume_type == CControld::TYPE_OST || forcetype == (int)CControld::TYPE_OST)
	{
		AUDVOL vol;
		vol.frontleft  = vl;
		vol.frontright = vr;
		vol.rearleft   = vl;
		vol.rearright  = vr;
		vol.center     = v;
		vol.lfe        = v;
		ret = ioctl(P->fd, MPEG_AUD_SET_VOL, &vol);
		if (ret < 0)
			lt_info("setVolume MPEG_AUD_SET_VOL failed (%m)\n");
		return ret;
	}
#if 0
	else if (settings.volume_type == CControld::TYPE_AVS || forcetype == (int)CControld::TYPE_AVS)
	{
		if ((avsfd = open(AVS_DEVICE, O_RDWR)) < 0)
			perror("[controld] " AVS_DEVICE);
		else {
			if (ioctl(avsfd, IOC_AVS_SET_VOLUME, v))
				perror("[controld] IOC_AVS_SET_VOLUME");
			close(avsfd);
			return 0;
		}
	}
	fprintf(stderr, "CAudio::setVolume: invalid settings.volume_type = %d\n", settings.volume_type);
	return -1;
#endif
}

int cAudio::Start(void)
{
	int fd = P->fd;
	int ret = ioctl(fd, MPEG_AUD_PLAY);
	/* this seems to be not strictly necessary since neutrino
	   re-mutes all the time, but is certainly more correct */
	ioctl(fd, MPEG_AUD_SET_MUTE, muted);
	return ret;
}

int cAudio::Stop(void)
{
	return ioctl(P->fd, MPEG_AUD_STOP);
}

bool cAudio::Pause(bool /*Pcm*/)
{
	return true;
};

void cAudio::SetSyncMode(AVSYNC_TYPE Mode)
{
	lt_debug("%s %d\n", __FUNCTION__, Mode);
	int fd = P->fd;
	switch (Mode)
	{
		case 0:
			ioctl(fd, MPEG_AUD_SYNC_OFF);
			break;
		default:
			ioctl(fd, MPEG_AUD_SYNC_ON);
			break;
	}
};

void cAudio::SetStreamType(AUDIO_FORMAT type)
{
	lt_debug("%s %d\n", __FUNCTION__, type);
	int fd = P->fd;
	if (type != AUDIO_FMT_DOLBY_DIGITAL && type != AUDIO_FMT_MPEG && type != AUDIO_FMT_MPG1)
		lt_info("%s unhandled AUDIO_FORMAT %d\n", __func__, type);

	bool bypass_disable = (type != AUDIO_FMT_DOLBY_DIGITAL);
	if (bypass_disable)
		ioctl(fd, MPEG_AUD_SET_MODE, AUD_MODE_MPEG);
	else {
		/* dvb2001 does always set AUD_MODE_DTS before setting AUD_MODE_AC3,
		   this might be some workaround, so we do the same... */
		ioctl(fd, MPEG_AUD_SET_MODE, AUD_MODE_DTS);
		ioctl(fd, MPEG_AUD_SET_MODE, AUD_MODE_AC3);
	}
	//setBypassMode(bypass_disable);

	if (type == AUDIO_FMT_MPEG)
		ioctl(fd, MPEG_AUD_SET_STREAM_TYPE, AUD_STREAM_TYPE_PES);
	if (type == AUDIO_FMT_MPG1)
		ioctl(fd, MPEG_AUD_SET_STREAM_TYPE, AUD_STREAM_TYPE_MPEG1);
};

int cAudio::setChannel(int channel)
{
	lt_debug("%s %d\n", __FUNCTION__, channel);
	return 0;
};

int cAudio::PrepareClipPlay(int ch, int srate, int bits, int little_endian)
{
	int fmt;
	unsigned int devmask, stereo, usable;
	const char *dsp_dev = getenv("DSP_DEVICE");
	const char *mix_dev = getenv("MIX_DEVICE");
	lt_debug("%s ch %d srate %d bits %d le %d\n", __FUNCTION__, ch, srate, bits, little_endian);
	if (P->clipfd >= 0) {
		lt_info("%s: clipfd already opened (%d)\n", __func__, P->clipfd);
		return -1;
	}
	P->mixer_num = -1;
	P->mixer_fd = -1;
	/* a different DSP device can be given with DSP_DEVICE and MIX_DEVICE
	 * if this device cannot be opened, we fall back to the internal TD OSS device
	 * Example:
	 *   modprobe ohci-hcd
	 *   modprobe audio
	 *   export DSP_DEVICE=/dev/sound/dsp1
	 *   export MIX_DEVICE=/dev/sound/mixer1
	 *   neutrino
	 */
	if ((!dsp_dev) || (access(dsp_dev, W_OK))) {
		if (dsp_dev)
			lt_info("%s: DSP_DEVICE is set (%s) but cannot be opened,"
				" fall back to /dev/sound/dsp\n", __func__, dsp_dev);
		dsp_dev = "/dev/sound/dsp";
	}
	lt_info("%s: dsp_dev %s mix_dev %s\n", __func__, dsp_dev, mix_dev); /* NULL mix_dev is ok */
	/* the tdoss dsp driver seems to work only on the second open(). really. */
	P->clipfd = open(dsp_dev, O_WRONLY);
	close(P->clipfd);
	P->clipfd = open(dsp_dev, O_WRONLY);
	if (P->clipfd < 0) {
		lt_info("%s open %s: %m\n", dsp_dev, __FUNCTION__);
		return -1;
	}
	fcntl(P->clipfd, F_SETFD, FD_CLOEXEC);
	/* no idea if we ever get little_endian == 0 */
	if (little_endian)
		fmt = AFMT_S16_BE;
	else
		fmt = AFMT_S16_LE;
	if (ioctl(P->clipfd, SNDCTL_DSP_SETFMT, &fmt))
		perror("SNDCTL_DSP_SETFMT");
	if (ioctl(P->clipfd, SNDCTL_DSP_CHANNELS, &ch))
		perror("SNDCTL_DSP_CHANNELS");
	if (ioctl(P->clipfd, SNDCTL_DSP_SPEED, &srate))
		perror("SNDCTL_DSP_SPEED");
	if (ioctl(P->clipfd, SNDCTL_DSP_RESET))
		perror("SNDCTL_DSP_RESET");

	if (!mix_dev)
		return 0;

	P->mixer_fd = open(mix_dev, O_RDWR);
	if (P->mixer_fd < 0) {
		lt_info("%s: open mixer %s failed (%m)\n", __func__, mix_dev);
		/* not a real error */
		return 0;
	}
	if (ioctl(P->mixer_fd, SOUND_MIXER_READ_DEVMASK, &devmask) == -1) {
		lt_info("%s: SOUND_MIXER_READ_DEVMASK %m\n", __func__);
		devmask = 0;
	}
	if (ioctl(P->mixer_fd, SOUND_MIXER_READ_STEREODEVS, &stereo) == -1) {
		lt_info("%s: SOUND_MIXER_READ_STEREODEVS %m\n", __func__);
		stereo = 0;
	}
	usable = devmask & stereo;
	if (usable == 0) {
		lt_info("%s: devmask: %08x stereo: %08x, no usable dev :-(\n",
			__func__, devmask, stereo);
		close(P->mixer_fd);
		P->mixer_fd = -1;
		return 0; /* TODO: should we treat this as error? */
	}
	/* __builtin_popcount needs GCC, it counts the set bits... */
	if (__builtin_popcount (usable) != 1) {
		/* TODO: this code is not yet tested as I have only single-mixer devices... */
		lt_info("%s: more than one mixer control: devmask %08x stereo %08x\n"
			"%s: querying MIX_NUMBER environment variable...\n",
			__func__, devmask, stereo, __func__);
		const char *tmp = getenv("MIX_NUMBER");
		if (tmp)
			P->mixer_num = atoi(tmp);
		lt_info("%s: mixer_num is %d -> device %08x\n",
			__func__, P->mixer_num, (P->mixer_num >= 0) ? (1 << P->mixer_num) : 0);
		/* no error checking, you'd better know what you are doing... */
	} else {
		P->mixer_num = 0;
		while (!(usable & 0x01)) {
			P->mixer_num++;
			usable >>= 1;
		}
	}
	setVolume(volume, volume);

	return 0;
};

int cAudio::WriteClip(unsigned char *buffer, int size)
{
	int ret;
	// lt_debug("cAudio::%s\n", __FUNCTION__);
	if (P->clipfd <= 0) {
		lt_info("%s: clipfd not yet opened\n", __FUNCTION__);
		return -1;
	}
	ret = write(P->clipfd, buffer, size);
	if (ret < 0)
		lt_info("%s: write error (%m)\n", __FUNCTION__);
	return ret;
};

int cAudio::StopClip()
{
	lt_debug("%s\n", __FUNCTION__);
	if (P->clipfd <= 0) {
		lt_info("%s: clipfd not yet opened\n", __FUNCTION__);
		return -1;
	}
	close(P->clipfd);
	P->clipfd = -1;
	if (P->mixer_fd >= 0)
		close(P->mixer_fd);
	P->mixer_fd = -1;
	setVolume(volume, volume);
	return 0;
};

void cAudio::getAudioInfo(int &type, int &layer, int &freq, int &bitrate, int &mode)
{
	lt_debug("%s\n", __FUNCTION__);
	unsigned int atype;
	static const int freq_mpg[] = {44100, 48000, 32000, 0};
	static const int freq_ac3[] = {48000, 44100, 32000, 0};
	scratchl2 i;
	int fd = P->fd;
	if (ioctl(fd, MPEG_AUD_GET_DECTYP, &atype) < 0)
		perror("cAudio::getAudioInfo MPEG_AUD_GET_DECTYP");
	if (ioctl(fd, MPEG_AUD_GET_STATUS, &i) < 0)
		perror("cAudio::getAudioInfo MPEG_AUD_GET_STATUS");

	type = atype;
#if 0
/* this does not work, some of the values are negative?? */
	AMPEGStatus A;
	memcpy(&A, &i.word00, sizeof(i.word00));
	layer   = A.audio_mpeg_layer;
	mode    = A.audio_mpeg_mode;
	bitrate = A.audio_mpeg_bitrate;
	switch(A.audio_mpeg_frequency)
#endif
	/* layer and bitrate are not used anyway... */
	layer   = 0; //(i.word00 >> 17) & 3;
	bitrate = 0; //(i.word00 >> 12) & 3;
	switch (type)
	{
		case 0:	/* MPEG */
			mode = (i.word00 >> 6) & 3;
			freq = freq_mpg[(i.word00 >> 10) & 3];
			break;
		case 1:	/* AC3 */
			mode = (i.word00 >> 28) & 7;
			freq = freq_ac3[(i.word00 >> 16) & 3];
			break;
		default:
			mode = 0;
			freq = 0;
	}
	//fprintf(stderr, "type: %d layer: %d freq: %d bitrate: %d mode: %d\n", type, layer, freq, bitrate, mode);
};

void cAudio::SetSRS(int /*iq_enable*/, int /*nmgr_enable*/, int /*iq_mode*/, int /*iq_level*/)
{
	lt_debug("%s\n", __FUNCTION__);
};

void cAudio::SetSpdifDD(bool enable)
{
	lt_debug("%s %d\n", __FUNCTION__, enable);
};

void cAudio::SetHdmiDD(bool enable)
{
	lt_debug("%s %d\n", __func__, enable);
};

void cAudio::ScheduleMute(bool On)
{
	lt_debug("%s %d\n", __FUNCTION__, On);
};

void cAudio::EnableAnalogOut(bool enable)
{
	lt_debug("%s %d\n", __FUNCTION__, enable);
};

#if 0
void cAudio::setBypassMode(bool disable)
{
	lt_debug("%s %d\n", __FUNCTION__, disable);
	/* disable = true: audio is MPEG, disable = false: audio is AC3 */
	if (disable)
	{
		ioctl(fd, MPEG_AUD_SET_MODE, AUD_MODE_MPEG);
		return;
	}
	/* dvb2001 does always set AUD_MODE_DTS before setting AUD_MODE_AC3,
	   this might be some workaround, so we do the same... */
	ioctl(fd, MPEG_AUD_SET_MODE, AUD_MODE_DTS);
	ioctl(fd, MPEG_AUD_SET_MODE, AUD_MODE_AC3);
	return;
	/* all those ioctl aways return "invalid argument", but they seem to
	   work anyway, so there's no use in checking the return value */
}
#endif
