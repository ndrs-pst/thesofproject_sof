// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2016 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//         Keyon Jie <yang.jie@linux.intel.com>

#include <sof/audio/buffer.h>
#include <sof/audio/component.h>
#include <sof/audio/audio_buffer.h>
#include <sof/audio/sink_api.h>
#include <sof/audio/source_api.h>
#include <sof/audio/sink_source_utils.h>
#include <sof/common.h>
#include <rtos/interrupt.h>
#include <rtos/alloc.h>
#include <rtos/cache.h>
#include <sof/lib/memory.h>
#include <sof/lib/notifier.h>
#include <sof/list.h>
#include <rtos/spinlock.h>
#include <rtos/symbol.h>
#include <ipc/topology.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(buffer, CONFIG_SOF_LOG_LEVEL);

SOF_DEFINE_REG_UUID(buffer);
DECLARE_TR_CTX(buffer_tr, SOF_UUID(buffer_uuid), LOG_LEVEL_INFO);

static size_t comp_buffer_get_data_available(struct sof_source *source)
{
	struct comp_buffer *buffer = comp_buffer_get_from_source(source);

	return audio_stream_get_avail_bytes(&buffer->stream);
}

static int comp_buffer_get_data(struct sof_source *source, size_t req_size,
				void const **data_ptr, void const **buffer_start,
				size_t *buffer_size)
{
	struct comp_buffer *buffer = comp_buffer_get_from_source(source);

	if (req_size > audio_stream_get_avail_bytes(&buffer->stream))
		return -ENODATA;

	buffer_stream_invalidate(buffer, req_size);

	/* get circular buffer parameters */
	*data_ptr = buffer->stream.r_ptr;
	*buffer_start = buffer->stream.addr;
	*buffer_size = buffer->stream.size;
	return 0;
}

static int comp_buffer_release_data(struct sof_source *source, size_t free_size)
{
	struct comp_buffer *buffer = comp_buffer_get_from_source(source);

	if (free_size)
		audio_stream_consume(&buffer->stream, free_size);

	return 0;
}

static int comp_buffer_set_ipc_params_source(struct sof_source *source,
					     struct sof_ipc_stream_params *params,
					     bool force_update)
{
	struct comp_buffer *buffer = comp_buffer_get_from_source(source);

	return buffer_set_params(buffer, params, force_update);
}

static int comp_buffer_source_format_set(struct sof_source *source)
{
	struct comp_buffer *buffer = comp_buffer_get_from_source(source);

	audio_stream_recalc_align(&buffer->stream);
	return 0;
}

static int comp_buffer_source_set_alignment_constants(struct sof_source *source,
						      const uint32_t byte_align,
						      const uint32_t frame_align_req)
{
	struct comp_buffer *buffer = comp_buffer_get_from_source(source);

	audio_stream_set_align(byte_align, frame_align_req, &buffer->stream);
	return 0;
}

static size_t comp_buffer_get_free_size(struct sof_sink *sink)
{
	struct comp_buffer *buffer = comp_buffer_get_from_sink(sink);

	return audio_stream_get_free_bytes(&buffer->stream);
}

static int comp_buffer_get_buffer(struct sof_sink *sink, size_t req_size,
				  void **data_ptr, void **buffer_start, size_t *buffer_size)
{
	struct comp_buffer *buffer = comp_buffer_get_from_sink(sink);

	if (req_size >  audio_stream_get_free_bytes(&buffer->stream))
		return -ENODATA;

	/* get circular buffer parameters */
	*data_ptr = buffer->stream.w_ptr;
	*buffer_start = buffer->stream.addr;
	*buffer_size = buffer->stream.size;
	return 0;
}

static int comp_buffer_commit_buffer(struct sof_sink *sink, size_t commit_size)
{
	struct comp_buffer *buffer = comp_buffer_get_from_sink(sink);

	if (commit_size) {
		buffer_stream_writeback(buffer, commit_size);
		audio_stream_produce(&buffer->stream, commit_size);
	}

	return 0;
}

static int comp_buffer_set_ipc_params_sink(struct sof_sink *sink,
					   struct sof_ipc_stream_params *params,
					   bool force_update)
{
	struct comp_buffer *buffer = comp_buffer_get_from_sink(sink);

	return buffer_set_params(buffer, params, force_update);
}

static int comp_buffer_sink_format_set(struct sof_sink *sink)
{
	struct comp_buffer *buffer = comp_buffer_get_from_sink(sink);

	audio_stream_recalc_align(&buffer->stream);
	return 0;
}

static int comp_buffer_sink_set_alignment_constants(struct sof_sink *sink,
						    const uint32_t byte_align,
						    const uint32_t frame_align_req)
{
	struct comp_buffer *buffer = comp_buffer_get_from_sink(sink);

	audio_stream_set_align(byte_align, frame_align_req, &buffer->stream);
	return 0;
}

/* free component in the pipeline */
static void comp_buffer_free(struct sof_audio_buffer *audio_buffer)
{
	if (!audio_buffer)
		return;

	CORE_CHECK_STRUCT(audio_buffer);

	struct comp_buffer *buffer = container_of(audio_buffer, struct comp_buffer, audio_buffer);

	struct buffer_cb_free cb_data = {
		.buffer = buffer,
	};

	buf_dbg(buffer, "buffer_free()");

	notifier_event(buffer, NOTIFIER_ID_BUFFER_FREE,
		       NOTIFIER_TARGET_CORE_LOCAL, &cb_data, sizeof(cb_data));

	/* In case some listeners didn't unregister from buffer's callbacks */
	notifier_unregister_all(NULL, buffer);

	rfree(buffer->stream.addr);
}

static const struct source_ops comp_buffer_source_ops = {
	.get_data_available = comp_buffer_get_data_available,
	.get_data = comp_buffer_get_data,
	.release_data = comp_buffer_release_data,
	.audio_set_ipc_params = comp_buffer_set_ipc_params_source,
	.on_audio_format_set = comp_buffer_source_format_set,
	.set_alignment_constants = comp_buffer_source_set_alignment_constants
};

static const struct sink_ops comp_buffer_sink_ops = {
	.get_free_size = comp_buffer_get_free_size,
	.get_buffer = comp_buffer_get_buffer,
	.commit_buffer = comp_buffer_commit_buffer,
	.audio_set_ipc_params = comp_buffer_set_ipc_params_sink,
	.on_audio_format_set = comp_buffer_sink_format_set,
	.set_alignment_constants = comp_buffer_sink_set_alignment_constants
};

static const struct audio_buffer_ops audio_buffer_ops = {
	.free = comp_buffer_free,
};

static struct comp_buffer *buffer_alloc_struct(void *stream_addr, size_t size, uint32_t caps,
					       uint32_t flags, bool is_shared)
{
	struct comp_buffer *buffer;

	tr_dbg(&buffer_tr, "buffer_alloc_struct()");

	/* allocate new buffer	 */
	enum mem_zone zone = is_shared ? SOF_MEM_ZONE_RUNTIME_SHARED : SOF_MEM_ZONE_RUNTIME;

	buffer = rzalloc(zone, 0, SOF_MEM_CAPS_RAM, sizeof(*buffer));

	if (!buffer) {
		tr_err(&buffer_tr, "buffer_alloc_struct(): could not alloc structure");
		return NULL;
	}

	buffer->caps = caps;
	/* Force channels to 2 for init to prevent bad call to clz in buffer_init_stream */
	buffer->stream.runtime_stream_params.channels = 2;

	audio_buffer_init(&buffer->audio_buffer, BUFFER_TYPE_LEGACY_BUFFER, is_shared,
			  &comp_buffer_source_ops, &comp_buffer_sink_ops, &audio_buffer_ops,
			  &buffer->stream.runtime_stream_params);

	/* From here no more uncached access to the buffer object, except its list headers */
	audio_stream_set_addr(&buffer->stream, stream_addr);
	buffer_init_stream(buffer, size);

	audio_stream_set_underrun(&buffer->stream, !!(flags & SOF_BUF_UNDERRUN_PERMITTED));
	audio_stream_set_overrun(&buffer->stream, !!(flags & SOF_BUF_OVERRUN_PERMITTED));

	list_init(&buffer->source_list);
	list_init(&buffer->sink_list);

	return buffer;
}

struct comp_buffer *buffer_alloc(size_t size, uint32_t caps, uint32_t flags, uint32_t align,
				 bool is_shared)
{
	struct comp_buffer *buffer;
	void *stream_addr;

	tr_dbg(&buffer_tr, "buffer_alloc()");

	/* validate request */
	if (size == 0) {
		tr_err(&buffer_tr, "buffer_alloc(): new size = %zu is invalid", size);
		return NULL;
	}

	stream_addr = rballoc_align(0, caps, size, align);
	if (!stream_addr) {
		tr_err(&buffer_tr, "buffer_alloc(): could not alloc size = %zu bytes of type = %u",
		       size, caps);
		return NULL;
	}

	buffer = buffer_alloc_struct(stream_addr, size, caps, flags, is_shared);
	if (!buffer) {
		tr_err(&buffer_tr, "buffer_alloc(): could not alloc buffer structure");
		rfree(stream_addr);
	}

	return buffer;
}

struct comp_buffer *buffer_alloc_range(size_t preferred_size, size_t minimum_size, uint32_t caps,
				       uint32_t flags, uint32_t align, bool is_shared)
{
	struct comp_buffer *buffer;
	size_t size;
	void *stream_addr = NULL;

	tr_dbg(&buffer_tr, "buffer_alloc_range(): %zu -- %zu bytes", minimum_size, preferred_size);

	/* validate request */
	if (minimum_size == 0 || preferred_size < minimum_size) {
		tr_err(&buffer_tr, "buffer_alloc_range(): new size range %zu -- %zu is invalid",
		       minimum_size, preferred_size);
		return NULL;
	}

	/* Align preferred size to a multiple of the minimum size */
	if (preferred_size % minimum_size)
		preferred_size += minimum_size - preferred_size % minimum_size;

	for (size = preferred_size; size >= minimum_size; size -= minimum_size) {
		stream_addr = rballoc_align(0, caps, size, align);
		if (stream_addr)
			break;
	}

	tr_dbg(&buffer_tr, "buffer_alloc_range(): allocated %zu bytes", size);

	if (!stream_addr) {
		tr_err(&buffer_tr, "buffer_alloc_range(): could not alloc size = %zu bytes of type = %u",
		       minimum_size, caps);
		return NULL;
	}

	buffer = buffer_alloc_struct(stream_addr, size, caps, flags, is_shared);
	if (!buffer) {
		tr_err(&buffer_tr, "buffer_alloc_range(): could not alloc buffer structure");
		rfree(stream_addr);
	}

	return buffer;
}

void buffer_zero(struct comp_buffer *buffer)
{
	buf_dbg(buffer, "stream_zero()");
	CORE_CHECK_STRUCT(&buffer->audio_buffer);

	bzero(audio_stream_get_addr(&buffer->stream), audio_stream_get_size(&buffer->stream));
	if (buffer->caps & SOF_MEM_CAPS_DMA)
		dcache_writeback_region((__sparse_force void __sparse_cache *)
					audio_stream_get_addr(&buffer->stream),
					audio_stream_get_size(&buffer->stream));
}

int buffer_set_size(struct comp_buffer *buffer, uint32_t size, uint32_t alignment)
{
	void *new_ptr = NULL;

	CORE_CHECK_STRUCT(&buffer->audio_buffer);

	/* validate request */
	if (size == 0) {
		buf_err(buffer, "resize size = %u is invalid", size);
		return -EINVAL;
	}

	if (size == audio_stream_get_size(&buffer->stream))
		return 0;

	if (!alignment)
		new_ptr = rbrealloc(audio_stream_get_addr(&buffer->stream), SOF_MEM_FLAG_NO_COPY,
				    buffer->caps, size, audio_stream_get_size(&buffer->stream));
	else
		new_ptr = rbrealloc_align(audio_stream_get_addr(&buffer->stream),
					  SOF_MEM_FLAG_NO_COPY, buffer->caps, size,
					  audio_stream_get_size(&buffer->stream), alignment);
	/* we couldn't allocate bigger chunk */
	if (!new_ptr && size > audio_stream_get_size(&buffer->stream)) {
		buf_err(buffer, "resize can't alloc %u bytes type %u",
			audio_stream_get_size(&buffer->stream), buffer->caps);
		return -ENOMEM;
	}

	/* use bigger chunk, else just use the old chunk but set smaller */
	if (new_ptr)
		buffer->stream.addr = new_ptr;

	buffer_init_stream(buffer, size);

	return 0;
}

int buffer_set_size_range(struct comp_buffer *buffer, size_t preferred_size, size_t minimum_size,
			  uint32_t alignment)
{
	void *ptr = audio_stream_get_addr(&buffer->stream);
	const size_t actual_size = audio_stream_get_size(&buffer->stream);
	void *new_ptr = NULL;
	size_t new_size;

	CORE_CHECK_STRUCT(&buffer->audio_buffer);

	/* validate request */
	if (minimum_size == 0 || preferred_size < minimum_size) {
		buf_err(buffer, "resize size range %zu -- %zu is invalid", minimum_size,
			preferred_size);
		return -EINVAL;
	}

	/* Align preferred size to a multiple of the minimum size */
	if (preferred_size % minimum_size)
		preferred_size += minimum_size - preferred_size % minimum_size;

	if (preferred_size == actual_size)
		return 0;

	if (!alignment) {
		for (new_size = preferred_size; new_size >= minimum_size;
		     new_size -= minimum_size) {
			new_ptr = rbrealloc(ptr, SOF_MEM_FLAG_NO_COPY, buffer->caps, new_size,
					    actual_size);
			if (new_ptr)
				break;
		}
	} else {
		for (new_size = preferred_size; new_size >= minimum_size;
		     new_size -= minimum_size) {
			new_ptr = rbrealloc_align(ptr, SOF_MEM_FLAG_NO_COPY, buffer->caps, new_size,
						  actual_size, alignment);
			if (new_ptr)
				break;
		}
	}

	/* we couldn't allocate bigger chunk */
	if (!new_ptr && new_size > actual_size) {
		buf_err(buffer, "resize can't alloc %zu bytes type %u", new_size, buffer->caps);
		return -ENOMEM;
	}

	/* use bigger chunk, else just use the old chunk but set smaller */
	if (new_ptr)
		buffer->stream.addr = new_ptr;

	buffer_init_stream(buffer, new_size);

	return 0;
}

int buffer_set_params(struct comp_buffer *buffer,
		      struct sof_ipc_stream_params *params, bool force_update)
{
	int ret;
	int i;

	CORE_CHECK_STRUCT(&buffer->audio_buffer);

	if (!params) {
		buf_err(buffer, "buffer_set_params(): !params");
		return -EINVAL;
	}

	if (audio_buffer_hw_params_configured(&buffer->audio_buffer) && !force_update)
		return 0;

	ret = audio_stream_set_params(&buffer->stream, params);
	if (ret < 0) {
		buf_err(buffer, "buffer_set_params(): audio_stream_set_params failed");
		return -EINVAL;
	}

	audio_stream_set_buffer_fmt(&buffer->stream, params->buffer_fmt);
	for (i = 0; i < SOF_IPC_MAX_CHANNELS; i++)
		audio_buffer_set_chmap(&buffer->audio_buffer, i, params->chmap[i]);

	audio_buffer_set_hw_params_configured(&buffer->audio_buffer);

	return 0;
}
EXPORT_SYMBOL(buffer_set_params);

bool buffer_params_match(struct comp_buffer *buffer,
			 struct sof_ipc_stream_params *params, uint32_t flag)
{
	assert(params);
	CORE_CHECK_STRUCT(&buffer->audio_buffer);

	if ((flag & BUFF_PARAMS_FRAME_FMT) &&
	     audio_stream_get_frm_fmt(&buffer->stream) != params->frame_fmt)
		return false;

	if ((flag & BUFF_PARAMS_RATE) &&
	     audio_stream_get_rate(&buffer->stream) != params->rate)
		return false;

	if ((flag & BUFF_PARAMS_CHANNELS) &&
	     audio_stream_get_channels(&buffer->stream) != params->channels)
		return false;

	return true;
}

void comp_update_buffer_produce(struct comp_buffer *buffer, uint32_t bytes)
{
	struct buffer_cb_transact cb_data = {
		.buffer = buffer,
		.transaction_amount = bytes,
		.transaction_begin_address = audio_stream_get_wptr(&buffer->stream),
	};

	/* return if no bytes */
	if (!bytes) {
#if CONFIG_SOF_LOG_DBG_BUFFER
		buf_dbg(buffer, "comp_update_buffer_produce(), no bytes to produce, source->comp.id = %u, source->comp.type = %u, sink->comp.id = %u, sink->comp.type = %u",
			buffer->source ? dev_comp_id(buffer->source) : (unsigned int)UINT32_MAX,
			buffer->source ? dev_comp_type(buffer->source) : (unsigned int)UINT32_MAX,
			buffer->sink ? dev_comp_id(buffer->sink) : (unsigned int)UINT32_MAX,
			buffer->sink ? dev_comp_type(buffer->sink) : (unsigned int)UINT32_MAX);
#endif
		return;
	}

	audio_stream_produce(&buffer->stream, bytes);

	notifier_event(buffer, NOTIFIER_ID_BUFFER_PRODUCE,
		       NOTIFIER_TARGET_CORE_LOCAL, &cb_data, sizeof(cb_data));

#if CONFIG_SOF_LOG_DBG_BUFFER
	buf_dbg(buffer, "comp_update_buffer_produce(), ((buffer->avail << 16) | buffer->free) = %08x, ((buffer->id << 16) | buffer->size) = %08x",
		(audio_stream_get_avail_bytes(&buffer->stream) << 16) |
		 audio_stream_get_free_bytes(&buffer->stream),
		(buffer->id << 16) | audio_stream_get_size(&buffer->stream));
	buf_dbg(buffer, "comp_update_buffer_produce(), ((buffer->r_ptr - buffer->addr) << 16 | (buffer->w_ptr - buffer->addr)) = %08x",
		((char *)audio_stream_get_rptr(&buffer->stream) -
		 (char *)audio_stream_get_addr(&buffer->stream)) << 16 |
		((char *)audio_stream_get_wptr(&buffer->stream) -
		 (char *)audio_stream_get_addr(&buffer->stream)));
#endif
}

void comp_update_buffer_consume(struct comp_buffer *buffer, uint32_t bytes)
{
	struct buffer_cb_transact cb_data = {
		.buffer = buffer,
		.transaction_amount = bytes,
		.transaction_begin_address = audio_stream_get_rptr(&buffer->stream),
	};

	CORE_CHECK_STRUCT(&buffer->audio_buffer);

	/* return if no bytes */
	if (!bytes) {
#if CONFIG_SOF_LOG_DBG_BUFFER
		buf_dbg(buffer, "comp_update_buffer_consume(), no bytes to consume, source->comp.id = %u, source->comp.type = %u, sink->comp.id = %u, sink->comp.type = %u",
			buffer->source ? dev_comp_id(buffer->source) : (unsigned int)UINT32_MAX,
			buffer->source ? dev_comp_type(buffer->source) : (unsigned int)UINT32_MAX,
			buffer->sink ? dev_comp_id(buffer->sink) : (unsigned int)UINT32_MAX,
			buffer->sink ? dev_comp_type(buffer->sink) : (unsigned int)UINT32_MAX);
#endif
		return;
	}

	audio_stream_consume(&buffer->stream, bytes);

	notifier_event(buffer, NOTIFIER_ID_BUFFER_CONSUME,
		       NOTIFIER_TARGET_CORE_LOCAL, &cb_data, sizeof(cb_data));

#if CONFIG_SOF_LOG_DBG_BUFFER
	buf_dbg(buffer, "comp_update_buffer_consume(), (buffer->avail << 16) | buffer->free = %08x, (buffer->id << 16) | buffer->size = %08x, (buffer->r_ptr - buffer->addr) << 16 | (buffer->w_ptr - buffer->addr)) = %08x",
		(audio_stream_get_avail_bytes(&buffer->stream) << 16) |
		 audio_stream_get_free_bytes(&buffer->stream),
		(buffer->id << 16) | audio_stream_get_size(&buffer->stream),
		((char *)audio_stream_get_rptr(&buffer->stream) -
		 (char *)audio_stream_get_addr(&buffer->stream)) << 16 |
		((char *)audio_stream_get_wptr(&buffer->stream) -
		 (char *)audio_stream_get_addr(&buffer->stream)));
#endif
}

/*
 * Locking: must be called with interrupts disabled! Serialized IPCs protect us
 * from racing attach / detach calls, but the scheduler can interrupt the IPC
 * thread and begin using the buffer for streaming. FIXME: this is still a
 * problem with different cores.
 */
void buffer_attach(struct comp_buffer *buffer, struct list_item *head, int dir)
{
	struct list_item *list = buffer_comp_list(buffer, dir);
	CORE_CHECK_STRUCT(&buffer->audio_buffer);
	list_item_prepend(list, head);
}

/*
 * Locking: must be called with interrupts disabled! See buffer_attach() above
 * for details
 */
void buffer_detach(struct comp_buffer *buffer, struct list_item *head, int dir)
{
	struct list_item *buf_list = buffer_comp_list(buffer, dir);
	CORE_CHECK_STRUCT(&buffer->audio_buffer);
	list_item_del(buf_list);
}