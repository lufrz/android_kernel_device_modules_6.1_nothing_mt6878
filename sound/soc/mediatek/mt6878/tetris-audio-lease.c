// SPDX-License-Identifier: GPL-2.0
/* Optional, descriptor-owned receiver routing for Tetris media playback. */
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/compat.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <sound/control.h>
#include <sound/minors.h>
#include <sound/pcm.h>
#include <sound/soc.h>

#include "../common/mtk-base-afe.h"
#include "../../codecs/mt6369.h"
#include "mt6878-afe-common.h"
#include "mt6878-reg.h"
#include "tetris-audio.h"

enum lease_control_id {
	LEASE_RCV, LEASE_HPL, LEASE_HPR, LEASE_LOL, LEASE_DAC,
	LEASE_FEED, LEASE_SPEAKER_L, LEASE_SPEAKER_R, LEASE_GAIN,
	LEASE_SPIN, LEASE_MODEM, LEASE_SCP_CALL, LEASE_DSP_CALL,
	LEASE_DSP_VOIP, LEASE_SPLIT_FEED, LEASE_CONTROL_COUNT,
};

static const char * const lease_control_names[] = {
	"RCV Mux", "HPL Mux", "HPR Mux", "LOL Mux", "DAC In Mux",
	"ADDA_DL_CH1 DL_24CH_CH1", "I2SOUT4_CH1 DL_24CH_CH1",
	"I2SOUT4_CH2 DL_24CH_CH2", "ADDA_DL_GAIN", "aw882xx_spin_switch",
	"Speech_MD_Status", "Speech_SCP_CALL_STATE", "dsp_call_final_runtime_en",
	"dsp_voipdl_runtime_en", "ADDA_DL_CH1 DL8_CH1",
};

struct lease_control {
	struct snd_kcontrol *control;
	snd_kcontrol_put_t *put;
};

struct tetris_audio_lease {
	struct miscdevice misc;
	struct kref ref;
	struct rw_semaphore lifetime;
	struct mutex lock;
	struct snd_soc_card *card;
	struct mtk_base_afe *afe;
	struct mt6369_priv *codec;
	struct lease_control *guarded;
	unsigned int guarded_count;
	struct snd_kcontrol *controls[LEASE_CONTROL_COUNT];
	struct snd_ctl_elem_value value;
	struct snd_pcm *split_pcm;
	struct file *split_file;
	u32 saved_gain;
	u32 mode;
	u32 feed_mode;
	s32 receiver_gain_db;
	bool open;
	bool dead;
	bool preempted;
	bool fault;
	bool prepared;
};

/* One Tetris sound card. Pointer and callbacks change under controls_rwsem. */
static struct tetris_audio_lease *tetris_lease;

static bool lease_guards(const char *name)
{
	/* This volume flag votes for the PMIC supply; it does not change routing. */
	if (!strcmp(name, "ADDA_DL_MAX_VOL"))
		return false;

	return !strncmp(name, "ADDA_DL_", 8) ||
		!strncmp(name, "I2SOUT4_CH", 10) ||
		!strcmp(name, "RCV Mux") || !strcmp(name, "HPL Mux") ||
		!strcmp(name, "HPR Mux") || !strcmp(name, "LOL Mux") ||
		!strcmp(name, "DAC In Mux") || !strcmp(name, "Handset Volume") ||
		!strcmp(name, "Headphone Plugged In") || !strcmp(name, "PMIC_REG_CLEAR") ||
		!strcmp(name, "aw882xx_spin_switch") ||
		!strcmp(name, "Speech_MD_Status") ||
		!strcmp(name, "Speech_SCP_CALL_STATE") ||
		!strcmp(name, "dsp_call_final_runtime_en") ||
		!strcmp(name, "dsp_voipdl_runtime_en");
}

static struct lease_control *lease_find_control(struct tetris_audio_lease *lease,
					       struct snd_kcontrol *control)
{
	unsigned int i;

	for (i = 0; i < lease->guarded_count; ++i)
		if (lease->guarded[i].control == control)
			return &lease->guarded[i];
	return NULL;
}

static int lease_get(struct tetris_audio_lease *lease, struct snd_kcontrol *control,
		     bool is_enum, unsigned int *value)
{
	struct snd_ctl_elem_value *data = &lease->value;
	int ret;

	memset(data, 0, sizeof(*data));
	ret = control->get(control, data);

	if (ret >= 0)
		*value = is_enum ? data->value.enumerated.item[0] :
			data->value.integer.value[0];
	return ret;
}

static int lease_put(struct tetris_audio_lease *lease, enum lease_control_id id,
		     bool is_enum, unsigned int value)
{
	struct snd_kcontrol *control = lease->controls[id];
	struct lease_control *guard = lease_find_control(lease, control);
	struct snd_ctl_elem_value *data = &lease->value;
	unsigned int actual;
	int ret;

	if (!guard)
		return -EINVAL;
	ret = lease_get(lease, control, is_enum, &actual);
	if (ret < 0 || actual == value)
		return ret < 0 ? ret : 0;
	memset(data, 0, sizeof(*data));
	if (is_enum)
		data->value.enumerated.item[0] = value;
	else
		data->value.integer.value[0] = value;
	/* Call the original callback without recursively entering the lease guard. */
	ret = guard->put(control, data);
	if (ret < 0)
		return ret;
	ret = lease_get(lease, control, is_enum, &actual);
	return ret < 0 ? ret : (actual == value ? 0 : -EIO);
}

/* Validate provenance before interpreting private_data, as ALSA PCM linking does. */
static int lease_check_split_file(struct tetris_audio_lease *lease,
				  struct file *file)
{
	struct inode *inode = file_inode(file);
	struct snd_pcm *pcm;
	struct snd_pcm_file *pcm_file;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	int ret = -EINVAL;

	if (!lease->split_pcm || !S_ISCHR(inode->i_mode) || imajor(inode) != snd_major)
		return -EINVAL;
	pcm = snd_lookup_minor_data(iminor(inode), SNDRV_DEVICE_TYPE_PCM_PLAYBACK);
	if (!pcm)
		return -EINVAL;
	if (pcm != lease->split_pcm)
		goto out;
	pcm_file = file->private_data;
	if (!pcm_file || !pcm_file->substream)
		goto out;
	substream = pcm_file->substream;
	if (substream->pcm != pcm || substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		goto out;
	snd_pcm_stream_lock_irqsave(substream, flags);
	runtime = substream->runtime;
	/* DPCM needs the leased route connected before this configured FE can prepare. */
	if (runtime && runtime->rate == 48000 && runtime->channels == 1 &&
	    runtime->format == SNDRV_PCM_FORMAT_S32_LE &&
	    (runtime->state == SNDRV_PCM_STATE_SETUP ||
	     runtime->state == SNDRV_PCM_STATE_PREPARED ||
	     runtime->state == SNDRV_PCM_STATE_RUNNING))
		ret = 0;
	snd_pcm_stream_unlock_irqrestore(substream, flags);
out:
	snd_card_unref(pcm->card);
	return ret;
}

/* controls_rwsem -> lease.lock -> DAPM. Stock put callers already hold the first. */
static int lease_stop(struct tetris_audio_lease *lease)
{
	struct mt6878_afe_private *afe_priv = lease->afe->platform_priv;
	int rcv_ret, feed_ret, spin_ret, gain_ret = 0;

	if (!lease->prepared)
		return 0;
	rcv_ret = lease_put(lease, LEASE_RCV, true, 0);
	feed_ret = lease->feed_mode == TETRIS_AUDIO_MODE_STEREO ?
		lease_put(lease, LEASE_SPLIT_FEED, false, 0) : lease_put(lease, LEASE_FEED, false, 0);
	spin_ret = lease_put(lease, LEASE_SPIN, true, 0);
	lease->mode = TETRIS_AUDIO_MODE_OFF;
	/* Restore stock gain only after the receiver has been disconnected. */
	if (!rcv_ret && !feed_ret) {
		snd_soc_dapm_mutex_lock(lease->card);
		WRITE_ONCE(lease->codec->tetris_media_receiver, false);
		WRITE_ONCE(afe_priv->tetris_media_receiver, false);
		gain_ret = regmap_write(lease->afe->regmap, AFE_ADDA_DL_SRC_CON1,
					lease->saved_gain);
		snd_soc_dapm_mutex_unlock(lease->card);
		lease->prepared = false;
		lease->feed_mode = TETRIS_AUDIO_MODE_OFF;
		pm_runtime_put(lease->afe->dev);
	}
	if (rcv_ret || feed_ret || spin_ret || gain_ret) {
		lease->fault = true;
		dev_err(lease->card->dev, "Receiver restore failed: %d/%d/%d/%d\n",
			rcv_ret, feed_ret, spin_ret, gain_ret);
		return -EIO;
	}
	return 0;
}

/* Called with controls_rwsem and lease.lock held; never updates call volume. */
static int lease_set_gain(struct tetris_audio_lease *lease, s32 gain_db)
{
	unsigned int gain;
	int ret;

	if (gain_db < TETRIS_AUDIO_RECEIVER_MIN_DB ||
	    gain_db > TETRIS_AUDIO_RECEIVER_MAX_DB)
		return -EINVAL;
	if (lease->fault)
		return -EIO;
	if (lease->preempted)
		return -EPIPE;
	/* MT6369 encodes +8 dB as 0, with one step per dB below that. */
	gain = DL_GAIN_0DB - gain_db;
	if (lease->prepared) {
		snd_soc_dapm_mutex_lock(lease->card);
		ret = regmap_update_bits(lease->codec->regmap, MT6369_ZCD_CON5,
					 RG_AUDHSGAIN_MASK_SFT, gain << RG_AUDHSGAIN_SFT);
		if (!ret)
			WRITE_ONCE(lease->codec->tetris_media_receiver_gain, gain);
		snd_soc_dapm_mutex_unlock(lease->card);
		if (ret)
			return ret;
	}
	lease->receiver_gain_db = gain_db;
	return 0;
}

static int lease_stock_put(struct snd_kcontrol *control,
			   struct snd_ctl_elem_value *value)
{
	struct tetris_audio_lease *lease = tetris_lease;
	struct lease_control *guard;
	struct file *split_file = NULL;
	int ret;

	if (!lease)
		return -ENODEV;
	guard = lease_find_control(lease, control);
	if (!guard)
		return -EINVAL;
	mutex_lock(&lease->lock);
	if (lease->prepared) {
		dev_info(lease->card->dev, "Tetris audio lease preempted: control=%s mode=%u\n",
			 control->id.name, lease->mode);
		lease->preempted = true;
		ret = lease_stop(lease);
		if (ret)
			goto out;
		split_file = lease->split_file;
		lease->split_file = NULL;
	}
	ret = guard->put(control, value);
out:
	mutex_unlock(&lease->lock);
	/* fput defers final PCM close until after the ALSA control callback returns. */
	if (split_file)
		fput(split_file);
	return ret;
}

static int lease_preflight(struct tetris_audio_lease *lease)
{
	unsigned int i, value;
	int ret;

	for (i = LEASE_RCV; i <= LEASE_DAC; ++i) {
		ret = lease_get(lease, lease->controls[i], true, &value);
		if (ret < 0 || value)
			return ret < 0 ? ret : -EBUSY;
	}
	ret = lease_get(lease, lease->controls[LEASE_SPIN], true, &value);
	if (ret < 0 || value)
		return ret < 0 ? ret : -EBUSY;
	for (i = LEASE_MODEM; i <= LEASE_DSP_VOIP; ++i) {
		ret = lease_get(lease, lease->controls[i], false, &value);
		if (ret < 0 || value)
			return ret < 0 ? ret : -EBUSY;
	}
	/* Accept only the observed media path; never mix into another ADDA source. */
	for (i = 0; i < lease->guarded_count; ++i) {
		struct snd_kcontrol *control = lease->guarded[i].control;
		const char *name = control->id.name;
		bool speaker = !strncmp(name, "I2SOUT4_CH", 10);
		bool expected = control == lease->controls[LEASE_SPEAKER_L] ||
			control == lease->controls[LEASE_SPEAKER_R];

		if (!speaker && strncmp(name, "ADDA_DL_CH", 10))
			continue;
		ret = lease_get(lease, control, false, &value);
		if (ret < 0 || value != expected)
			return ret < 0 ? ret : -EBUSY;
	}
	return 0;
}

static int lease_start(struct tetris_audio_lease *lease, u32 mode)
{
	struct mt6878_afe_private *afe_priv = lease->afe->platform_priv;
	u32 gain = TETRIS_AUDIO_RECEIVER_GAIN << AFE_DL_GAIN1_CTL_PRE_SFT |
		TETRIS_AUDIO_RECEIVER_GAIN << AFE_DL_GAIN2_CTL_PRE_SFT;
	int ret;

	if (lease->fault)
		return -EIO;
	if (lease->preempted)
		return -EPIPE;
	if (lease->mode == mode)
		return 0;
	if (lease->prepared)
		return -EBUSY;
	if (mode == TETRIS_AUDIO_MODE_STEREO &&
	    (!lease->split_file || lease_check_split_file(lease, lease->split_file)))
		return -ENXIO;
	ret = lease_preflight(lease);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(lease->afe->dev);
	if (ret < 0)
		return ret;
	snd_soc_dapm_mutex_lock(lease->card);
	ret = regmap_read(lease->afe->regmap, AFE_ADDA_DL_SRC_CON1, &lease->saved_gain);
	if (!ret) {
		WRITE_ONCE(lease->codec->tetris_media_receiver_gain,
			   DL_GAIN_0DB - lease->receiver_gain_db);
		WRITE_ONCE(afe_priv->tetris_media_receiver, true);
		WRITE_ONCE(lease->codec->tetris_media_receiver, true);
		ret = regmap_write(lease->afe->regmap, AFE_ADDA_DL_SRC_CON1, gain);
		lease->prepared = true;
	}
	snd_soc_dapm_mutex_unlock(lease->card);
	if (!lease->prepared) {
		pm_runtime_put(lease->afe->dev);
		return ret;
	}
	/* Retain the selected feed across a failed cleanup so a retry cannot miss DL8. */
	lease->feed_mode = mode;
	/* The speaker keeps the stock Awinic channel; HAL sends it only the right input. */
	if (!ret)
		ret = lease_put(lease, LEASE_SPIN, true, 0);
	if (!ret)
		ret = mode == TETRIS_AUDIO_MODE_STEREO ?
			lease_put(lease, LEASE_SPLIT_FEED, false, 1) : lease_put(lease, LEASE_FEED, false, 1);
	if (!ret)
		ret = lease_put(lease, LEASE_RCV, true, 2);
	if (ret) {
		lease_stop(lease);
		return ret;
	}
	lease->mode = mode;
	return 0;
}

static void lease_free(struct kref *ref)
{
	struct tetris_audio_lease *lease = container_of(ref, struct tetris_audio_lease, ref);

	kfree(lease->guarded);
	kfree(lease);
}

static int lease_open(struct inode *inode, struct file *file)
{
	struct tetris_audio_lease *lease = container_of(file->private_data,
						     struct tetris_audio_lease, misc);
	int ret = 0;

	mutex_lock(&lease->lock);
	if (lease->dead)
		ret = -ENODEV;
	else if (lease->open)
		ret = -EBUSY;
	else {
		lease->open = true;
		lease->preempted = false;
		lease->receiver_gain_db = TETRIS_AUDIO_RECEIVER_DEFAULT_DB;
		kref_get(&lease->ref);
		file->private_data = lease;
	}
	mutex_unlock(&lease->lock);
	return ret;
}

static long lease_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct tetris_audio_lease *lease = file->private_data;
	const struct tetris_audio_caps caps = {
		.version = TETRIS_AUDIO_ABI_VERSION,
		.modes = BIT(TETRIS_AUDIO_MODE_DUAL_MONO) | BIT(TETRIS_AUDIO_MODE_STEREO),
		.receiver_gain = TETRIS_AUDIO_RECEIVER_GAIN,
	};
	struct tetris_audio_split_caps split_caps = {
		.version = TETRIS_AUDIO_SPLIT_VERSION,
		.rate = 48000,
		.channels = 1,
		.format = TETRIS_AUDIO_SPLIT_FORMAT_S32_LE,
	};
	struct file *new_split_file = NULL, *old_split_file = NULL;
	u32 mode = 0;
	s32 gain_db = 0, split_fd = -1;
	int ret = 0;

	if (cmd != TETRIS_AUDIO_GET_CAPS && cmd != TETRIS_AUDIO_SET_MODE &&
	    cmd != TETRIS_AUDIO_GET_STATE && cmd != TETRIS_AUDIO_SET_GAIN &&
	    cmd != TETRIS_AUDIO_GET_GAIN && cmd != TETRIS_AUDIO_GET_SPLIT_CAPS &&
	    cmd != TETRIS_AUDIO_BIND_SPLIT_PCM)
		return -ENOTTY;
	if (cmd == TETRIS_AUDIO_SET_MODE) {
		if (copy_from_user(&mode, (void __user *)arg, sizeof(mode)))
			return -EFAULT;
		if (mode > TETRIS_AUDIO_MODE_STEREO)
			return -EINVAL;
	} else if (cmd == TETRIS_AUDIO_SET_GAIN) {
		if (copy_from_user(&gain_db, (void __user *)arg, sizeof(gain_db)))
			return -EFAULT;
	} else if (cmd == TETRIS_AUDIO_BIND_SPLIT_PCM) {
		if (copy_from_user(&split_fd, (void __user *)arg, sizeof(split_fd)))
			return -EFAULT;
		if (split_fd < -1)
			return -EINVAL;
		if (split_fd >= 0) {
			new_split_file = fget(split_fd);
			if (!new_split_file)
				return -EBADF;
		}
	}
	/* A surviving file descriptor must not dereference a removed sound card. */
	down_read(&lease->lifetime);
	if (lease->dead) {
		ret = -ENODEV;
		goto out_lifetime;
	}
	if (cmd == TETRIS_AUDIO_GET_CAPS) {
		ret = copy_to_user((void __user *)arg, &caps, sizeof(caps)) ? -EFAULT : 0;
		goto out_lifetime;
	}
	if (cmd == TETRIS_AUDIO_GET_SPLIT_CAPS) {
		if (!lease->split_pcm) {
			ret = -ENODEV;
			goto out_lifetime;
		}
		split_caps.card = lease->split_pcm->card->number;
		split_caps.device = lease->split_pcm->device;
		ret = copy_to_user((void __user *)arg, &split_caps, sizeof(split_caps)) ?
			-EFAULT : 0;
		goto out_lifetime;
	}
	down_write(&lease->card->snd_card->controls_rwsem);
	mutex_lock(&lease->lock);
	if (lease->dead)
		ret = -ENODEV;
	else if (cmd == TETRIS_AUDIO_GET_STATE)
		mode = lease->mode;
	else if (cmd == TETRIS_AUDIO_GET_GAIN)
		gain_db = lease->receiver_gain_db;
	else if (cmd == TETRIS_AUDIO_SET_GAIN)
		ret = lease_set_gain(lease, gain_db);
	else if (cmd == TETRIS_AUDIO_BIND_SPLIT_PCM) {
		if (lease->prepared)
			ret = -EBUSY;
		else if (new_split_file)
			ret = lease_check_split_file(lease, new_split_file);
		if (!ret) {
			old_split_file = lease->split_file;
			lease->split_file = new_split_file;
			new_split_file = NULL;
		}
	} else if (mode == TETRIS_AUDIO_MODE_OFF) {
		ret = lease_stop(lease);
		if (!ret) {
			lease->preempted = false;
			old_split_file = lease->split_file;
			lease->split_file = NULL;
		}
	} else
		ret = lease_start(lease, mode);
	mutex_unlock(&lease->lock);
	up_write(&lease->card->snd_card->controls_rwsem);
	if (!ret && cmd == TETRIS_AUDIO_GET_STATE &&
	    copy_to_user((void __user *)arg, &mode, sizeof(mode)))
		ret = -EFAULT;
	if (!ret && cmd == TETRIS_AUDIO_GET_GAIN &&
	    copy_to_user((void __user *)arg, &gain_db, sizeof(gain_db)))
		ret = -EFAULT;
out_lifetime:
	up_read(&lease->lifetime);
	if (old_split_file)
		fput(old_split_file);
	if (new_split_file)
		fput(new_split_file);
	return ret;
}

static int lease_release(struct inode *inode, struct file *file)
{
	struct tetris_audio_lease *lease = file->private_data;
	struct file *split_file = NULL;

	down_read(&lease->lifetime);
	if (!lease->dead) {
		down_write(&lease->card->snd_card->controls_rwsem);
		mutex_lock(&lease->lock);
		lease_stop(lease);
		split_file = lease->split_file;
		lease->split_file = NULL;
		lease->open = false;
		mutex_unlock(&lease->lock);
		up_write(&lease->card->snd_card->controls_rwsem);
	}
	up_read(&lease->lifetime);
	if (split_file)
		fput(split_file);
	kref_put(&lease->ref, lease_free);
	return 0;
}

static const struct file_operations lease_fops = {
	.owner = THIS_MODULE,
	.open = lease_open,
	.release = lease_release,
	.unlocked_ioctl = lease_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.llseek = no_llseek,
};

static void lease_remove(void *data)
{
	struct tetris_audio_lease *lease = data;
	struct file *split_file;
	unsigned int i;

	misc_deregister(&lease->misc);
	down_write(&lease->lifetime);
	down_write(&lease->card->snd_card->controls_rwsem);
	mutex_lock(&lease->lock);
	lease_stop(lease);
	split_file = lease->split_file;
	lease->split_file = NULL;
	lease->dead = true;
	for (i = 0; i < lease->guarded_count; ++i)
		lease->guarded[i].control->put = lease->guarded[i].put;
	tetris_lease = NULL;
	mutex_unlock(&lease->lock);
	up_write(&lease->card->snd_card->controls_rwsem);
	up_write(&lease->lifetime);
	if (split_file)
		fput(split_file);
	kref_put(&lease->ref, lease_free);
}

int tetris_audio_lease_register(struct snd_soc_card *card)
{
	struct tetris_audio_lease *lease;
	struct snd_kcontrol *control;
	struct snd_soc_component *component;
	struct snd_soc_pcm_runtime *rtd;
	unsigned int i, count = 0;
	int ret = -ENODEV;

	/* Supplier unbind must remove this consumer before freeing its controls. */
	for_each_card_components(card, component) {
		if (component->dev == card->dev)
			continue;
		if (!device_link_add(card->dev, component->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
			return -ENODEV;
	}
	lease = kzalloc(sizeof(*lease), GFP_KERNEL);
	if (!lease)
		return -ENOMEM;
	kref_init(&lease->ref);
	init_rwsem(&lease->lifetime);
	mutex_init(&lease->lock);
	lease->card = card;
	lease->misc.minor = MISC_DYNAMIC_MINOR;
	lease->misc.name = "tetris_audio";
	lease->misc.fops = &lease_fops;
	lease->misc.parent = card->dev;
	lease->misc.mode = 0600;
	for (i = 0; i < LEASE_CONTROL_COUNT; ++i) {
		lease->controls[i] = snd_soc_card_get_kcontrol(card, lease_control_names[i]);
		if (!lease->controls[i] || !lease->controls[i]->get || !lease->controls[i]->put)
			goto free;
	}
	component = snd_soc_kcontrol_component(lease->controls[LEASE_GAIN]);
	lease->afe = snd_soc_component_get_drvdata(component);
	component = snd_soc_dapm_to_component(snd_soc_dapm_kcontrol_dapm(lease->controls[LEASE_RCV]));
	lease->codec = snd_soc_component_get_drvdata(component);
	if (!lease->afe || !lease->codec)
		goto free;
	for_each_card_rtds(card, rtd) {
		struct snd_soc_dai *cpu = asoc_rtd_to_cpu(rtd, 0);

		if (rtd->pcm && cpu->id == MT6878_MEMIF_DL8 &&
		    !strcmp(cpu->name, "DL8") &&
		    rtd->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream) {
			lease->split_pcm = rtd->pcm;
			break;
		}
	}
	down_write(&card->snd_card->controls_rwsem);
	list_for_each_entry(control, &card->snd_card->controls, list)
		if (control->put && lease_guards(control->id.name))
			++count;
	lease->guarded = kcalloc(count, sizeof(*lease->guarded), GFP_KERNEL);
	if (!lease->guarded) {
		ret = -ENOMEM;
		goto unlock;
	}
	list_for_each_entry(control, &card->snd_card->controls, list) {
		if (!control->put || !lease_guards(control->id.name))
			continue;
		lease->guarded[lease->guarded_count].control = control;
		lease->guarded[lease->guarded_count++].put = control->put;
	}
	if (tetris_lease) {
		ret = -EBUSY;
		goto unlock;
	}
	tetris_lease = lease;
	for (i = 0; i < lease->guarded_count; ++i)
		lease->guarded[i].control->put = lease_stock_put;
	up_write(&card->snd_card->controls_rwsem);
	ret = misc_register(&lease->misc);
	if (ret) {
		down_write(&card->snd_card->controls_rwsem);
		for (i = 0; i < lease->guarded_count; ++i)
			lease->guarded[i].control->put = lease->guarded[i].put;
		tetris_lease = NULL;
		up_write(&card->snd_card->controls_rwsem);
		goto free;
	}
	return devm_add_action_or_reset(card->dev, lease_remove, lease);
unlock:
	up_write(&card->snd_card->controls_rwsem);
free:
	kref_put(&lease->ref, lease_free);
	return ret;
}
