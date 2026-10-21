/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _TETRIS_AUDIO_H
#define _TETRIS_AUDIO_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define TETRIS_AUDIO_ABI_VERSION 1
#define TETRIS_AUDIO_MODE_OFF 0
#define TETRIS_AUDIO_MODE_DUAL_MONO 1
#define TETRIS_AUDIO_MODE_STEREO 2
/* Q16 amplitude: closest to unity in the 16-bit ADDA gain field. */
#define TETRIS_AUDIO_RECEIVER_GAIN 0xffffU
#define TETRIS_AUDIO_RECEIVER_MIN_DB (-10)
/* Keep the optional media gain below the codec's +8 dB hardware limit. */
#define TETRIS_AUDIO_RECEIVER_MAX_DB 6
#define TETRIS_AUDIO_RECEIVER_DEFAULT_DB 0

struct tetris_audio_caps {
	__u32 version;
	__u32 modes;
	__u32 receiver_gain;
	__u32 reserved;
};

#define TETRIS_AUDIO_GET_CAPS _IOR('T', 0, struct tetris_audio_caps)
#define TETRIS_AUDIO_SET_MODE _IOW('T', 1, __u32)
#define TETRIS_AUDIO_GET_STATE _IOR('T', 2, __u32)
/* Optional signed dB control; older ABI-v1 kernels return -ENOTTY. */
#define TETRIS_AUDIO_SET_GAIN _IOW('T', 3, __s32)
#define TETRIS_AUDIO_GET_GAIN _IOR('T', 4, __s32)

/* Optional, descriptor-bound PCM path. Independent of the routing ABI above. */
#define TETRIS_AUDIO_SPLIT_VERSION 1
#define TETRIS_AUDIO_SPLIT_FORMAT_S32_LE 1
struct tetris_audio_split_caps {
	__u32 version;
	__u32 card;
	__u32 device;
	__u32 rate;
	__u32 channels;
	__u32 format;
	__u32 reserved[2];
};
#define TETRIS_AUDIO_GET_SPLIT_CAPS _IOR('T', 5, struct tetris_audio_split_caps)
/* Bind after HW_PARAMS; enable the route before PREPARE. -1 unbinds while off. */
#define TETRIS_AUDIO_BIND_SPLIT_PCM _IOW('T', 6, __s32)

#ifdef __KERNEL__
struct snd_soc_card;
int tetris_audio_lease_register(struct snd_soc_card *card);
#endif

#endif
