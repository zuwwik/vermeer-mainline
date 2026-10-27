// SPDX-License-Identifier: GPL-2.0-only
/* AudioReach modem voice graphs. The PCM controls a DSP-to-DSP stream;
 * there is no shared-memory PCM endpoint in this graph.
 */
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>
#include <sound/soc-dapm.h>
#include <sound/soc-dpcm.h>
#include "q6apm.h"
#include "q6apm-voice.h"

#define APM_VOICE_REGISTER_CFG	0x01001008
#define APM_VOICE_DEREGISTER_CFG	0x01001009
#define VCPM_VSID		0x080011bc
#define VCPM_TX_CHANNELS		0x08001310
#define VCPM_CAL_KEYS		0x0800116b
#define VCPM_VOLUME_KEY		0x08001166
#define VCPM_BOOST_KEY		0x080011b6
#define VCPM_BWE_KEY		0x08001167
#define VOICE_MAX_SUBGRAPHS	16
#define VOICE_MAX_MFCS		8
#define VOICE_MAX_FIRMWARE	(512 * 1024)

struct voice_fw_header {
	u8 magic[8];
	__le32 version;
	__le32 paths;
} __packed;

struct voice_fw_path {
	__le32 backend;
	__le32 module;
	__le32 instance;
	__le32 num_subgraphs;
	__le32 num_mfcs;
	__le32 open_size;
	__le32 cal_size;
	__le32 persist_size;
	__le32 gain_instance;
} __packed;

struct q6apm_voice_path {
	struct q6apm_voice *voice;
	const struct voice_fw_path *fw;
	const __le32 *subgraphs;
	const __le32 *mfcs;
	u32 backend;
	u32 num_subgraphs;
	u32 num_mfcs;
	u32 open_size;
	u32 cal_size;
	u32 persist_size;
	u32 cal_offset;
	u32 persist_offset;
	void *buffer;
	dma_addr_t dma;
	u64 dsp_address;
	size_t buffer_size;
	u32 map_handle;
	unsigned long registered;
	bool opened;
	bool started;
	unsigned int users;
	gpr_port_t *port;
	struct completion done;
	spinlock_t response_lock; /* Serializes callbacks against timeout/reset. */
	u32 sequence;
	u32 expected_token;
	u32 expected_opcode;
	u32 status;
};

struct q6apm_voice {
	struct q6apm *apm;
	struct device *dma_dev;
	struct mutex lock; /* Serializes session, graph and control operations. */
	const struct firmware *firmware;
	const char *firmware_prefix;
	struct q6apm_voice_path paths[2];
	unsigned int users;
	u32 route;
	u32 volume;
	u32 capture_gain;
	u32 vsid;
	bool mic_muted;
	bool enabled;
	bool media_enabled;
	bool owned;
	bool poisoned;
	long long sid;
	struct snd_kcontrol *controls[7];
};

struct snd_soc_dai_driver q6apm_voice_dai = {
	.name = Q6APM_VOICE_DAI_NAME,
	.id = Q6APM_VOICE_DAI_ID,
	.playback = {
		.stream_name = "Voice Call Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "Voice Call Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
};

static int voice_callback(const struct gpr_resp_pkt *packet, void *priv, int op)
{
	struct q6apm_voice_path *path = priv;
	const struct gpr_ibasic_rsp_result_t *result = packet->payload;
	u32 opcode = packet->hdr.opcode;

	guard(spinlock_irqsave)(&path->response_lock);
	if (packet->payload_size < 0)
		return 0;
	if (!path->expected_token || packet->hdr.token != path->expected_token)
		return 0;

	if (opcode == GPR_BASIC_RSP_RESULT) {
		if (packet->payload_size < sizeof(*result) ||
		    result->opcode != READ_ONCE(path->expected_opcode))
			return 0;
		/* Successful MAP has a separate response containing the handle. */
		if (result->opcode == APM_CMD_SHARED_MEM_MAP_REGIONS && !result->status)
			return 0;
		path->status = result->status;
	} else if (opcode == APM_CMD_RSP_SHARED_MEM_MAP_REGIONS) {
		if (READ_ONCE(path->expected_opcode) != APM_CMD_SHARED_MEM_MAP_REGIONS ||
		    packet->payload_size < sizeof(u32))
			return 0;
		path->map_handle = get_unaligned_le32(packet->payload);
		path->status = path->map_handle ? 0 : 1;
	} else {
		return 0;
	}
	path->expected_token = 0;
	complete(&path->done);
	return 0;
}

/* Caller holds voice->lock. Tokens also reject a response to a timed-out
 * earlier request, including two successive requests with the same opcode.
 */
static int voice_send(struct q6apm_voice_path *path, struct gpr_pkt *packet)
{
	int ret;

	packet->hdr.src_port = path->port->id;
	if (!++path->sequence)
		++path->sequence;
	packet->hdr.token = path->sequence;
	{
		guard(spinlock_irqsave)(&path->response_lock);
		reinit_completion(&path->done);
		path->status = 0;
		path->expected_opcode = packet->hdr.opcode;
		path->expected_token = packet->hdr.token;
	}
	ret = gpr_send_port_pkt(path->port, packet);
	if (ret < 0)
		goto out;
	if (!wait_for_completion_timeout(&path->done, 5 * HZ)) {
		dev_err(path->voice->dma_dev, "voice port %u: command %#x timed out\n",
			path->backend, packet->hdr.opcode);
		ret = -ETIMEDOUT;
	} else if (path->status) {
		dev_err(path->voice->dma_dev, "voice port %u: command %#x DSP error %#x\n",
			path->backend, packet->hdr.opcode, path->status);
		ret = -EREMOTEIO;
	} else {
		ret = 0;
	}
out:
	{
		guard(spinlock_irqsave)(&path->response_lock);
		path->expected_token = 0;
	}
	return ret;
}

static int voice_oob(struct q6apm_voice_path *path, u32 opcode, u32 offset, u32 size)
{
	struct apm_cmd_header *header;
	u64 address = path->dsp_address + offset;
	struct gpr_pkt *packet __free(kfree) =
		audioreach_alloc_cmd_pkt(0, opcode, 0, path->port->id, APM_MODULE_INSTANCE_ID);

	if (IS_ERR(packet))
		return PTR_ERR(packet);
	header = (void *)packet + GPR_HDR_SIZE;
	header->payload_address_lsw = lower_32_bits(address);
	header->payload_address_msw = upper_32_bits(address);
	header->mem_map_handle = path->map_handle;
	header->payload_size = size;
	return voice_send(path, packet);
}

static int voice_module_param(struct q6apm_voice_path *path, u32 module, u32 id,
			      const void *values, u32 count)
{
	struct apm_module_param_data *param;
	u32 size = count * sizeof(u32);
	struct gpr_pkt *packet __free(kfree) =
		audioreach_alloc_cmd_pkt(ALIGN(sizeof(*param) + size, 8), APM_CMD_SET_CFG,
					 0, path->port->id, module);

	if (IS_ERR(packet))
		return PTR_ERR(packet);
	param = (void *)packet + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	param->module_instance_id = module;
	param->param_id = id;
	param->param_size = size;
	memcpy(param + 1, values, size);
	return voice_send(path, packet);
}

static int voice_param(struct q6apm_voice_path *path, u32 id, const u32 *values, u32 count)
{
	return voice_module_param(path, VCPM_MODULE_INSTANCE_ID, id, values, count);
}

static int voice_capture_gain(struct q6apm_voice *voice, u32 gain, bool mute)
{
	struct q6apm_voice_path *path = &voice->paths[1];
	u32 value = mute ? 0 : gain;

	return voice_module_param(path, le32_to_cpu(path->fw->gain_instance),
				  PARAM_ID_VOL_CTRL_MASTER_GAIN, &value, 1);
}

static int voice_volume(struct q6apm_voice *voice, u32 volume)
{
	u32 keys[] = { voice->vsid, 3, VCPM_VOLUME_KEY, 5 - volume,
		       VCPM_BOOST_KEY, 0, VCPM_BWE_KEY, 0 };

	return voice_param(&voice->paths[0], VCPM_CAL_KEYS, keys, ARRAY_SIZE(keys));
}

static int voice_mgmt(struct q6apm_voice_path *path, u32 opcode)
{
	struct apm_module_param_data *param;
	u32 *values;
	u32 size = sizeof(u32) * (path->num_subgraphs + 1);
	struct gpr_pkt *packet __free(kfree) =
		audioreach_alloc_cmd_pkt(ALIGN(sizeof(*param) + size, 8), opcode,
					 0, path->port->id, APM_MODULE_INSTANCE_ID);

	if (IS_ERR(packet))
		return PTR_ERR(packet);
	param = (void *)packet + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	param->module_instance_id = APM_MODULE_INSTANCE_ID;
	param->param_id = APM_PARAM_ID_SUB_GRAPH_LIST;
	param->param_size = size;
	values = (void *)(param + 1);
	values[0] = path->num_subgraphs;
	memcpy(values + 1, path->subgraphs, size - sizeof(u32));
	return voice_send(path, packet);
}

static bool voice_valid_params(const u8 *data, size_t size)
{
	size_t offset = 0, length;

	while (offset < size) {
		if (size - offset < sizeof(struct apm_module_param_data))
			return false;
		length = get_unaligned_le32(data + offset + 8);
		if (get_unaligned_le32(data + offset + 12) || length > size - offset - 16)
			return false;
		offset += 16 + length;
		if (offset != size)
			offset = ALIGN(offset, 8);
	}
	return offset == size;
}

static const void *voice_take(const u8 **cursor, const u8 *end, size_t size)
{
	const u8 *data = *cursor;

	if (data > end || size > end - data)
		return NULL;
	*cursor += size;
	return data;
}

static int voice_parse_path(struct q6apm_voice_path *path, const u8 **cursor, const u8 *end)
{
	const struct voice_fw_path *fw = voice_take(cursor, end, sizeof(*fw));
	const u8 *open, *cal, *persist;
	u32 offset = 0, length, id, i, j;

	if (!fw)
		return -EINVAL;
	path->backend = le32_to_cpu(fw->backend);
	path->num_subgraphs = le32_to_cpu(fw->num_subgraphs);
	path->num_mfcs = le32_to_cpu(fw->num_mfcs);
	path->open_size = le32_to_cpu(fw->open_size);
	path->cal_size = le32_to_cpu(fw->cal_size);
	path->persist_size = le32_to_cpu(fw->persist_size);
	if (path->backend >= APM_PORT_MAX || !path->num_subgraphs ||
	    path->num_subgraphs > VOICE_MAX_SUBGRAPHS || path->num_mfcs > VOICE_MAX_MFCS ||
	    !path->open_size || path->open_size > VOICE_MAX_FIRMWARE ||
	    path->cal_size > VOICE_MAX_FIRMWARE || path->persist_size > VOICE_MAX_FIRMWARE)
		return -EINVAL;
	path->subgraphs = voice_take(cursor, end, path->num_subgraphs * sizeof(__le32));
	path->mfcs = voice_take(cursor, end, path->num_mfcs * sizeof(__le32));
	open = voice_take(cursor, end, path->open_size);
	cal = voice_take(cursor, end, path->cal_size);
	persist = voice_take(cursor, end, path->persist_size);
	if (!path->subgraphs || !path->mfcs || !open || !cal || !persist ||
	    !voice_valid_params(open, path->open_size) ||
	    !voice_valid_params(cal, path->cal_size))
		return -EINVAL;
	for (i = 0; i < path->num_subgraphs; i++) {
		if (!le32_to_cpu(path->subgraphs[i]))
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (path->subgraphs[j] == path->subgraphs[i])
				return -EINVAL;
	}
	i = 0;
	while (offset < path->persist_size) {
		if (path->persist_size - offset < 8 || i++ >= VOICE_MAX_SUBGRAPHS)
			return -EINVAL;
		id = get_unaligned_le32(persist + offset);
		length = get_unaligned_le32(persist + offset + 4);
		if (!length || length > path->persist_size - offset - 8 || length % 8 ||
		    !voice_valid_params(persist + offset + 8, length))
			return -EINVAL;
		for (j = 0; j < path->num_subgraphs; j++)
			if (le32_to_cpu(path->subgraphs[j]) == id)
				break;
		if (j == path->num_subgraphs)
			return -EINVAL;
		offset += 8 + length;
	}
	path->cal_offset = ALIGN(path->open_size, SZ_4K);
	path->persist_offset = ALIGN(path->cal_offset + path->cal_size, SZ_4K);
	path->buffer_size = ALIGN(path->persist_offset + path->persist_size, SZ_4K);
	path->buffer = dma_alloc_coherent(path->voice->dma_dev, path->buffer_size,
					  &path->dma, GFP_KERNEL);
	if (!path->buffer)
		return -ENOMEM;
	memcpy(path->buffer, open, path->open_size);
	memcpy(path->buffer + path->cal_offset, cal, path->cal_size);
	memcpy(path->buffer + path->persist_offset, persist, path->persist_size);
	path->fw = fw;
	return 0;
}

static int voice_map(struct q6apm_voice_path *path)
{
	struct apm_cmd_shared_mem_map_regions *header;
	struct apm_shared_map_region_payload *region;
	struct gpr_pkt *packet __free(kfree) =
		audioreach_alloc_apm_pkt(sizeof(*header) + sizeof(*region),
					 APM_CMD_SHARED_MEM_MAP_REGIONS, 0, path->port->id);

	if (IS_ERR(packet))
		return PTR_ERR(packet);
	header = (void *)packet + GPR_HDR_SIZE;
	header->mem_pool_id = APM_MEMORY_MAP_SHMEM8_4K_POOL;
	header->num_regions = 1;
	region = (void *)(header + 1);
	region->shm_addr_lsw = lower_32_bits(path->dsp_address);
	region->shm_addr_msw = upper_32_bits(path->dsp_address);
	region->mem_size_bytes = path->buffer_size;
	return voice_send(path, packet);
}

static int voice_unmap(struct q6apm_voice_path *path)
{
	struct apm_cmd_shared_mem_unmap_regions *header;
	int ret;
	struct gpr_pkt *packet __free(kfree) =
		audioreach_alloc_apm_pkt(sizeof(*header), APM_CMD_SHARED_MEM_UNMAP_REGIONS,
					 0, path->port->id);

	if (IS_ERR(packet))
		return PTR_ERR(packet);
	header = (void *)packet + GPR_HDR_SIZE;
	header->mem_map_handle = path->map_handle;
	ret = voice_send(path, packet);
	if (!ret)
		path->map_handle = 0;
	return ret;
}

static int voice_persist(struct q6apm_voice_path *path, bool enable)
{
	u32 offset = 0, length, index = 0;
	int ret;

	while (offset < path->persist_size) {
		length = get_unaligned_le32(path->buffer + path->persist_offset + offset + 4);
		if (enable || test_bit(index, &path->registered)) {
			ret = voice_oob(path, enable ? APM_VOICE_REGISTER_CFG :
					APM_VOICE_DEREGISTER_CFG,
					path->persist_offset + offset + 8, length);
			if (ret) {
				/* A timeout cannot establish whether registration completed. */
				if (ret == -ETIMEDOUT)
					path->voice->poisoned = true;
				return ret;
			}
			if (enable)
				set_bit(index, &path->registered);
			else
				clear_bit(index, &path->registered);
		}
		offset += 8 + length;
		index++;
	}
	return 0;
}

static int voice_cleanup(struct q6apm_voice *voice)
{
	struct q6apm_voice_path *path;
	int i, ret, error = 0;

	for (i = 1; i >= 0; i--) {
		path = &voice->paths[i];
		if (path->started) {
			ret = voice_mgmt(path, APM_CMD_GRAPH_STOP);
			if (ret)
				error = ret;
			else
				path->started = false;
		}
		if (path->registered) {
			ret = voice_persist(path, false);
			if (ret)
				error = ret;
		}
		if (path->opened) {
			ret = voice_mgmt(path, APM_CMD_GRAPH_CLOSE);
			if (ret)
				error = ret;
			else
				path->opened = false;
		}
	}
	if (!error && !voice->poisoned) {
		for (i = 0; i < 2; i++) {
			path = &voice->paths[i];
			if (path->map_handle) {
				ret = voice_unmap(path);
				if (ret)
					error = ret;
			}
		}
	}
	if (error || voice->poisoned) {
		voice->poisoned = true;
		dev_err(voice->dma_dev, "voice cleanup incomplete; retaining DSP memory until reboot\n");
		return error ?: -EIO;
	}
	for (i = 0; i < 2; i++) {
		path = &voice->paths[i];
		if (path->buffer) {
			dma_free_coherent(voice->dma_dev, path->buffer_size,
					  path->buffer, path->dma);
			path->buffer = NULL;
		}
		path->fw = NULL;
	}
	release_firmware(voice->firmware);
	voice->firmware = NULL;
	WRITE_ONCE(voice->media_enabled, false);
	WRITE_ONCE(voice->owned, false);
	module_put(THIS_MODULE);
	return 0;
}

int q6apm_voice_open(struct device *dev, long long sid)
{
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct q6apm_voice *voice = apm->voice;
	struct q6apm_voice_path *path;
	const struct voice_fw_header *header;
	const u8 *cursor, *end;
	char name[128];
	u32 channels[] = { 0, 2 };
	int ret, i;

	if (!voice)
		return -ENODEV;
	guard(mutex)(&voice->lock);
	if (voice->poisoned)
		return -EIO;
	if (voice->users) {
		voice->users++;
		return 0;
	}
	mutex_lock(&apm->graph_lock);
	if (!idr_is_empty(&apm->graph_idr)) {
		mutex_unlock(&apm->graph_lock);
		return -EBUSY;
	}
	WRITE_ONCE(voice->owned, true);
	__module_get(THIS_MODULE);
	mutex_unlock(&apm->graph_lock);
	ret = snprintf(name, sizeof(name), "%s-%s.bin", voice->firmware_prefix,
		       voice->route ? "speaker" : "handset");
	if (ret >= sizeof(name)) {
		ret = -ENAMETOOLONG;
		goto cleanup;
	}
	ret = request_firmware(&voice->firmware, name, dev);
	if (ret)
		goto cleanup;
	header = (const void *)voice->firmware->data;
	if (voice->firmware->size < sizeof(*header) ||
	    voice->firmware->size > VOICE_MAX_FIRMWARE ||
	    memcmp(header->magic, "ARVOICE\0", 8) ||
	    le32_to_cpu(header->version) != 3 || le32_to_cpu(header->paths) != 2) {
		ret = -EINVAL;
		goto cleanup;
	}
	cursor = (const void *)(header + 1);
	end = voice->firmware->data + voice->firmware->size;
	for (i = 0; i < 2; i++) {
		path = &voice->paths[i];
		ret = voice_parse_path(path, &cursor, end);
		if (ret)
			goto cleanup;
		path->dsp_address = path->dma;
		if (sid >= 0)
			path->dsp_address |= (u64)sid << 32;
	}
	if (cursor != end || voice->paths[0].backend == voice->paths[1].backend) {
		ret = -EINVAL;
		goto cleanup;
	}
	for (i = 0; i < 2; i++) {
		path = &voice->paths[i];
		ret = voice_map(path);
		if (ret) {
			if (ret == -ETIMEDOUT)
				voice->poisoned = true;
			goto cleanup;
		}
		/* Close on failure too: GRAPH_OPEN can create some subgraphs. */
		path->opened = true;
		ret = voice_oob(path, APM_CMD_GRAPH_OPEN, 0, path->open_size);
		if (ret)
			goto cleanup;
		if (path->cal_size) {
			ret = voice_oob(path, APM_CMD_SET_CFG, path->cal_offset, path->cal_size);
			if (ret)
				goto cleanup;
		}
		ret = voice_persist(path, true);
		if (ret)
			goto cleanup;
	}
	/* Android binds the newly opened RX/TX voice set through the RX
	 * client's VSID request, then supplies TX device channel information.
	 */
	ret = voice_param(&voice->paths[0], VCPM_VSID, &voice->vsid, 1);
	if (ret)
		goto cleanup;
	channels[0] = voice->vsid;
	ret = voice_param(&voice->paths[1], VCPM_TX_CHANNELS, channels, ARRAY_SIZE(channels));
	if (ret)
		goto cleanup;
	ret = voice_volume(voice, voice->volume);
	if (ret)
		goto cleanup;
	ret = voice_capture_gain(voice, voice->capture_gain, voice->mic_muted);
	if (ret)
		goto cleanup;
	voice->users = 1;
	dev_info(dev, "voice graphs loaded, VSID %#x, %s route\n",
		 voice->vsid, voice->route ? "speaker" : "handset");
	return 0;
cleanup:
	voice_cleanup(voice);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_voice_open);

void q6apm_voice_release(struct device *dev)
{
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct q6apm_voice *voice = apm->voice;

	guard(mutex)(&voice->lock);
	if (WARN_ON(!voice->users))
		return;
	if (!--voice->users && !voice->paths[0].users && !voice->paths[1].users)
		voice_cleanup(voice);
}
EXPORT_SYMBOL_GPL(q6apm_voice_release);

bool q6apm_voice_owns_audio(struct q6apm *apm)
{
	return apm->voice && READ_ONCE(apm->voice->owned);
}

bool q6apm_voice_media_allowed(struct q6apm *apm, u32 id)
{
	return (id == 0 || id == 2) && apm->voice && READ_ONCE(apm->voice->media_enabled);
}

/* Called with graph_lock held. ASoC keeps the existing TDM backend alive
 * for both frontends; only the multimedia stream graph is newly opened.
 * Connect it to a free input of the ACDB device mixer, ahead of the common
 * postprocessing/echo reference. Never open a second hardware endpoint.
 */
int q6apm_voice_media_connection(struct q6apm *apm, struct audioreach_graph_info *info)
{
	struct audioreach_graph_info *backend;
	struct q6apm_voice *voice = apm->voice;

	if (!q6apm_voice_owns_audio(apm))
		return 0;
	if (!q6apm_voice_media_allowed(apm, info->id))
		return -EBUSY;
	if (info->id == 2) {
		/* Raw microphone tap before voice processing/gain. PipeWire applies
		 * its source volume to AP capture; the same user gain is sent to
		 * the modem's downstream gain module, avoiding double attenuation.
		 */
		if (!info->dst_mod_inst_id)
			return -ENOLINK;
		info->src_mod_inst_id = voice->route ? 0x4168 : 0x4167;
		info->src_mod_op_port_id = 3;
		return 0;
	}
	backend = idr_find(&apm->graph_info_idr, voice->paths[0].backend);
	if (!backend || !backend->src_mod_inst_id)
		return -ENOLINK;
	info->src_mod_inst_id = backend->src_mod_inst_id;
	info->src_mod_op_port_id = backend->src_mod_op_port_id;
	info->dst_mod_inst_id = voice->route ? 0x40cc : 0x41d6;
	info->dst_mod_ip_port_id = 2;
	return 0;
}

bool q6apm_voice_ready(struct device *dev)
{
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct q6apm_voice *voice = apm->voice;

	return voice && READ_ONCE(voice->owned) && !READ_ONCE(voice->poisoned);
}
EXPORT_SYMBOL_GPL(q6apm_voice_ready);

int q6apm_voice_check_backend(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct q6apm *apm = dev_get_drvdata(dai->dev->parent);
	struct q6apm_voice *voice = apm->voice;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dpcm *dpcm;
	bool call = false, multimedia = false;
	int ret;

	if (!voice)
		return 0;
	for_each_dpcm_fe(rtd, substream->stream, dpcm) {
		if (!strcmp(snd_soc_rtd_to_cpu(dpcm->fe, 0)->name, Q6APM_VOICE_DAI_NAME))
			call = true;
		else
			multimedia = true;
	}
	if (!call)
		return q6apm_voice_owns_audio(apm) ? -EBUSY : 0;
	if (multimedia)
		return -EBUSY;
	if (!READ_ONCE(voice->enabled))
		return -EACCES;
	/* DPCM opens the backend before the frontend. Claim the voice session
	 * here, before a capture backend can create a multimedia DSP graph.
	 */
	ret = q6apm_voice_open(voice->dma_dev, voice->sid);
	return ret ?: 1;
}
EXPORT_SYMBOL_GPL(q6apm_voice_check_backend);

struct q6apm_graph *q6apm_voice_graph_open(struct q6apm *apm, struct device *dev, u32 id)
{
	struct q6apm_voice *voice = apm->voice;
	struct q6apm_graph *graph;
	int i;

	if (!voice)
		return NULL;
	guard(mutex)(&voice->lock);
	if (!voice->owned)
		return NULL;
	if (voice->poisoned)
		return ERR_PTR(-EIO);
	if (q6apm_voice_media_allowed(apm, id))
		return NULL;
	for (i = 0; i < 2; i++) {
		if (voice->paths[i].backend != id)
			continue;
		graph = kzalloc_obj(*graph);
		if (!graph)
			return ERR_PTR(-ENOMEM);
		graph->apm = apm;
		graph->dev = dev;
		graph->id = id;
		graph->voice_path = &voice->paths[i];
		voice->paths[i].users++;
		return graph;
	}
	return ERR_PTR(-EBUSY);
}

int q6apm_voice_graph_close(struct q6apm_graph *graph)
{
	struct q6apm_voice_path *path = graph->voice_path;
	struct q6apm_voice *voice = path->voice;

	guard(mutex)(&voice->lock);
	path->users--;
	kfree(graph);
	if (!voice->users && !voice->paths[0].users && !voice->paths[1].users)
		return voice_cleanup(voice);
	return 0;
}

int q6apm_voice_graph_command(struct q6apm_graph *graph, u32 opcode)
{
	struct q6apm_voice_path *path = graph->voice_path;
	int ret;

	guard(mutex)(&path->voice->lock);
	if (path->voice->poisoned)
		return -EIO;
	if ((opcode == APM_CMD_GRAPH_START && path->started) ||
	    (opcode == APM_CMD_GRAPH_STOP && !path->started))
		return 0;
	ret = voice_mgmt(path, opcode);
	if (!ret && opcode == APM_CMD_GRAPH_START)
		path->started = true;
	if (!ret && opcode == APM_CMD_GRAPH_STOP)
		path->started = false;
	if (ret == -ETIMEDOUT)
		path->voice->poisoned = true;
	return ret;
}

static int voice_codec_media_format(struct q6apm_voice_path *path,
				    const struct audioreach_module *endpoint,
				    const struct audioreach_module_config *cfg)
{
	struct param_id_hw_ep_mf media = {
		.sample_rate = cfg->sample_rate,
		.bit_width = cfg->bit_width,
		.num_channels = cfg->num_channels,
		.data_format = endpoint->data_format,
	};
	struct param_id_codec_dma_intf_cfg interface = {
		.lpaif_type = endpoint->hw_interface_type,
		.intf_index = endpoint->hw_interface_idx,
		.active_channels_mask = (1U << cfg->num_channels) - 1,
	};
	u32 iid = endpoint->instance_id;
	int ret;

	/* ACDB already selected frame size, channel remapping and XO policy.
	 * The multimedia helper would replace the voice XO policy with zero.
	 * Match PAL: update only the negotiated format and DMA interface.
	 */
	ret = voice_module_param(path, iid, PARAM_ID_HW_EP_MF_CFG,
				 &media, sizeof(media) / sizeof(u32));
	if (ret)
		return ret;
	return voice_module_param(path, iid, PARAM_ID_CODEC_DMA_INTF_CFG,
				  &interface, sizeof(interface) / sizeof(u32));
}

static void voice_tdm_config(bool speaker, struct audioreach_module_config *cfg)
{
	/* Match the receiver format in PAL's MFC and TDM configuration. The
	 * physical serializer still uses four 32-bit slots in both routes.
	 */
	if (!speaker) {
		cfg->bit_width = 24;
		cfg->num_channels = 1;
		cfg->channel_map[0] = PCM_CHANNEL_FC;
		cfg->slot_mask = 1;
	}
}

int q6apm_voice_media_format(struct q6apm_graph *graph, struct audioreach_module_config *cfg)
{
	struct q6apm_voice_path *path = graph->voice_path;
	struct audioreach_module_config output = *cfg;
	struct audioreach_graph_info *info;
	struct audioreach_sub_graph *sg;
	struct audioreach_container *container;
	struct audioreach_module *module, endpoint = {}, mfc = {};
	u32 mid = le32_to_cpu(path->fw->module);
	int ret, i;

	guard(mutex)(&path->voice->lock);
	info = idr_find(&graph->apm->graph_info_idr, graph->id);
	if (!info)
		return -ENODEV;
	list_for_each_entry(sg, &info->sg_list, node) {
		list_for_each_entry(container, &sg->container_list, node) {
			list_for_each_entry(module, &container->modules_list, node) {
				if (module->module_id == mid)
					endpoint = *module;
			}
		}
	}
	if (!endpoint.module_id)
		return -ENODEV;
	/* Retain the board's verified slot/lane/clock parameters. Only the
	 * endpoint instance changes; never instantiate a second hardware port.
	 */
	endpoint.instance_id = le32_to_cpu(path->fw->instance);
	endpoint.data = NULL;
	if (mid == MODULE_ID_TDM_SINK)
		voice_tdm_config(path->voice->route, &output);
	if (mid == MODULE_ID_CODEC_DMA_SOURCE)
		ret = voice_codec_media_format(path, &endpoint, cfg);
	else
		ret = audioreach_set_media_format(graph, &endpoint, &output);
	if (ret)
		return ret;
	mfc.module_id = MODULE_ID_MFC;
	for (i = 0; i < path->num_mfcs; i++) {
		mfc.instance_id = le32_to_cpu(path->mfcs[i]);
		ret = audioreach_set_media_format(graph, &mfc, &output);
		if (ret)
			return ret;
	}
	return 0;
}

static int voice_control_info(struct snd_kcontrol *control, struct snd_ctl_elem_info *info)
{
	static const char * const routes[] = { "Handset", "Speaker" };
	static const char * const sims[] = { "SIM1", "SIM2" };

	if (control->private_value == 1)
		return snd_ctl_enum_info(info, 1, ARRAY_SIZE(routes), routes);
	if (control->private_value == 2)
		return snd_ctl_enum_info(info, 1, ARRAY_SIZE(sims), sims);
	if (control->private_value >= 3 && control->private_value <= 5)
		return snd_ctl_boolean_mono_info(control, info);
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = control->private_value == 6 ? VOL_CTRL_DEFAULT_GAIN : 5;
	return 0;
}

static int voice_control_get(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
	struct q6apm_voice *voice = snd_kcontrol_chip(control);

	guard(mutex)(&voice->lock);
	switch (control->private_value) {
	case 1:
		value->value.enumerated.item[0] = voice->route;
		break;
	case 2:
		value->value.enumerated.item[0] = voice->vsid == 0x11dc5000;
		break;
	case 3:
		value->value.integer.value[0] = !voice->mic_muted;
		break;
	case 4:
		value->value.integer.value[0] = voice->enabled;
		break;
	case 5:
		value->value.integer.value[0] = voice->media_enabled;
		break;
	case 6:
		value->value.integer.value[0] = voice->capture_gain;
		break;
	default:
		value->value.integer.value[0] = voice->volume;
	}
	return 0;
}

static int voice_control_put(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
	struct q6apm_voice *voice = snd_kcontrol_chip(control);
	u32 new = control->private_value == 1 || control->private_value == 2 ?
		value->value.enumerated.item[0] : value->value.integer.value[0];
	int ret;

	guard(mutex)(&voice->lock);
	if (new > (control->private_value == 6 ? VOL_CTRL_DEFAULT_GAIN :
		   control->private_value ? 1 : 5))
		return -EINVAL;
	if (control->private_value == 6) {
		if (new == voice->capture_gain)
			return 0;
		if (voice->poisoned)
			return -EIO;
		if (voice->owned) {
			ret = voice_capture_gain(voice, new, voice->mic_muted);
			if (ret)
				return ret;
		}
		voice->capture_gain = new;
		return 1;
	}
	if (control->private_value == 5) {
		if (new == voice->media_enabled)
			return 0;
		if (new && (!voice->owned || !voice->paths[0].started || voice->poisoned))
			return -EACCES;
		guard(mutex)(&voice->apm->graph_lock);
		if (idr_find(&voice->apm->graph_idr, 0) || idr_find(&voice->apm->graph_idr, 2))
			return -EBUSY;
		WRITE_ONCE(voice->media_enabled, new);
		return 1;
	}
	if (control->private_value == 4) {
		if (new == voice->enabled)
			return 0;
		if (voice->owned)
			return -EBUSY;
		voice->enabled = new;
		return 1;
	}
	if (control->private_value == 3) {
		if (voice->mic_muted == !new)
			return 0;
		if (voice->poisoned)
			return -EIO;
		if (voice->owned) {
			ret = voice_capture_gain(voice, voice->capture_gain, !new);
			if (ret)
				return ret;
		}
		voice->mic_muted = !new;
		return 1;
	}
	if (control->private_value) {
		if (voice->owned)
			return -EBUSY;
		if (control->private_value == 1) {
			if (new == voice->route)
				return 0;
			voice->route = new;
		} else {
			new = new ? 0x11dc5000 : 0x11c05000;
			if (new == voice->vsid)
				return 0;
			voice->vsid = new;
		}
		return 1;
	}
	if (new == voice->volume)
		return 0;
	if (voice->poisoned)
		return -EIO;
	if (voice->owned) {
		ret = voice_volume(voice, new);
		if (ret)
			return ret;
	}
	voice->volume = new;
	return 1;
}

static const struct snd_kcontrol_new voice_controls[] = {
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Voice Call Volume",
	  .info = voice_control_info, .get = voice_control_get, .put = voice_control_put },
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Voice Call Route", .private_value = 1,
	  .info = voice_control_info, .get = voice_control_get, .put = voice_control_put },
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Voice Call SIM", .private_value = 2,
	  .info = voice_control_info, .get = voice_control_get, .put = voice_control_put },
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Voice Call Mic Switch", .private_value = 3,
	  .info = voice_control_info, .get = voice_control_get, .put = voice_control_put },
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Voice Call Switch", .private_value = 4,
	  .info = voice_control_info, .get = voice_control_get, .put = voice_control_put },
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Voice Call Media Switch", .private_value = 5,
	  .info = voice_control_info, .get = voice_control_get, .put = voice_control_put },
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Voice Call Capture Volume", .private_value = 6,
	  .info = voice_control_info, .get = voice_control_get, .put = voice_control_put },
};

int q6apm_voice_link_init(struct snd_soc_pcm_runtime *rtd, const char *firmware_prefix)
{
	struct snd_soc_component *component = snd_soc_rtdcom_lookup(rtd, "q6apm-dai");
	struct q6apm *apm;
	struct q6apm_voice *voice;
	struct of_phandle_args args;
	int i, ret;

	if (!component)
		return -ENODEV;
	apm = dev_get_drvdata(component->dev->parent);
	if (apm->voice)
		return -EBUSY;
	voice = kzalloc_obj(*voice);
	if (!voice)
		return -ENOMEM;
	voice->apm = apm;
	voice->dma_dev = get_device(component->dev);
	voice->vsid = 0x11c05000;
	voice->volume = 3;
	voice->capture_gain = VOL_CTRL_DEFAULT_GAIN;
	voice->firmware_prefix = firmware_prefix;
	if (of_parse_phandle_with_fixed_args(component->dev->of_node, "iommus", 1, 0, &args)) {
		voice->sid = -1;
	} else {
		voice->sid = args.args[0] & 0xf;
		of_node_put(args.np);
	}
	mutex_init(&voice->lock);
	for (i = 0; i < 2; i++) {
		voice->paths[i].voice = voice;
		init_completion(&voice->paths[i].done);
		spin_lock_init(&voice->paths[i].response_lock);
		voice->paths[i].port = gpr_alloc_port(apm->gdev, component->dev,
						      voice_callback, &voice->paths[i]);
		if (IS_ERR(voice->paths[i].port)) {
			ret = PTR_ERR(voice->paths[i].port);
			voice->paths[i].port = NULL;
			goto error;
		}
	}
	for (i = 0; i < ARRAY_SIZE(voice_controls); i++) {
		voice->controls[i] = snd_ctl_new1(&voice_controls[i], voice);
		ret = snd_ctl_add(rtd->card->snd_card, voice->controls[i]);
		if (ret) {
			/* snd_ctl_add() frees the rejected control. */
			while (i--)
				snd_ctl_remove(rtd->card->snd_card, voice->controls[i]);
			goto error;
		}
	}
	apm->voice = voice;
	return 0;
error:
	for (i = 0; i < 2; i++)
		if (voice->paths[i].port)
			gpr_free_port(voice->paths[i].port);
	put_device(voice->dma_dev);
	kfree(voice);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_voice_link_init);

void q6apm_voice_link_exit(struct snd_soc_pcm_runtime *rtd)
{
	struct q6apm *apm = dev_get_drvdata(snd_soc_rtd_to_cpu(rtd, 0)->dev);
	int i;

	if (!apm->voice)
		return;
	for (i = 0; i < ARRAY_SIZE(apm->voice->controls); i++)
		snd_ctl_remove(rtd->card->snd_card, apm->voice->controls[i]);
	q6apm_voice_destroy(apm);
}
EXPORT_SYMBOL_GPL(q6apm_voice_link_exit);

void q6apm_voice_destroy(struct q6apm *apm)
{
	struct q6apm_voice *voice = apm->voice;
	int i;

	if (!voice)
		return;
	/* A failed DSP unmap must never become a use-after-free. A successful
	 * session drops its module reference only after both unmaps complete.
	 */
	if (WARN_ON(voice->owned))
		return;
	for (i = 0; i < 2; i++)
		gpr_free_port(voice->paths[i].port);
	put_device(voice->dma_dev);
	kfree(voice);
	apm->voice = NULL;
}
