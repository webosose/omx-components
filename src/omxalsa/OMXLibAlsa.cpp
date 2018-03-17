#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <IL/OMX_Core.h>
#include <IL/OMX_Component.h>
#include <IL/OMX_Broadcom.h>
#include "omxcommon.h"
#include "logging.h"
#include "OMXLibAlsa.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}


#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

template <class X> static inline X max(X a, X b)
{
	return (a > b) ? a : b;
}

template <class X> static OMX_ERRORTYPE omx_cast(X* &toptr, OMX_PTR fromptr)
{
	toptr = (X*) fromptr;
	if (toptr->nSize < sizeof(X)) return OMX_ErrorBadParameter;
	if (toptr->nVersion.nVersion != OMX_VERSION) return OMX_ErrorVersionMismatch;
	return OMX_ErrorNone;
}

template <class X> static void omx_init(X &omx)
{
	omx.nSize = sizeof(X);
	omx.nVersion.nVersion = OMX_VERSION;
}

void __comx_process_mark(COMX_COMPONENT *comp, OMX_BUFFERHEADERTYPE *hdr)
{
	if (hdr->hMarkTargetComponent == (OMX_HANDLETYPE) comp) {
		__comx_event(comp, OMX_EventMark, 0, 0, hdr->pMarkData);
		hdr->hMarkTargetComponent = 0;
		hdr->pMarkData = 0;
	}
}

static void audio_omx_init(COMX_COMPONENT *comp, const char *name, OMX_PTR pAppData, OMX_CALLBACKTYPE* pCallbacks, COMX_PORT *ports, size_t nports)
{
	comp->omx.nSize = sizeof comp->omx;
	comp->omx.nVersion.nVersion = OMX_VERSION;
	comp->omx.pApplicationPrivate = pAppData;
	comp->omx.GetComponentVersion = comx_get_component_version;
	comp->omx.SendCommand = comx_send_command;
	comp->omx.GetParameter = comx_get_parameter;
	comp->omx.GetState = comx_get_state;
	comp->omx.ComponentTunnelRequest = comx_component_tunnel_request;
	comp->omx.UseBuffer = comx_use_buffer;
	comp->omx.AllocateBuffer = comx_allocate_buffer;
	comp->omx.FreeBuffer = comx_free_buffer;
	comp->omx.EmptyThisBuffer = comx_empty_this_buffer;
	comp->omx.FillThisBuffer = comx_fill_this_buffer;
	comp->omx.SetCallbacks = comx_set_callbacks;
	comp->omx.UseEGLImage = comx_use_egl_image;
	comp->omx.ComponentRoleEnum = comx_component_role_enum;

	comp->name = name;
	comp->cb = *pCallbacks;
	comp->state = OMX_StateLoaded;
	comp->nports = nports;
	comp->ports = ports;

	comxq_init(&comp->cmdq, offsetof(COMX_COMMAND, next));
	pthread_cond_init(&comp->cond, 0);
	pthread_mutex_init(&comp->mutex, 0);
	pthread_create(&comp->component_thread, 0, comx_worker, comp);

	for (size_t i = 0; i < comp->nports; i++) {
		COMX_PORT *port = &comp->ports[i];
		pthread_cond_init(&port->cond_no_buffers, 0);
		pthread_cond_init(&port->cond_populated, 0);
		pthread_cond_init(&port->cond_idle, 0);
		comxq_init(&port->tunnel_supplierq,
		           port->def.eDir == OMX_DirInput
		           ? offsetof(OMX_BUFFERHEADERTYPE, pInputPortPrivate)
		           : offsetof(OMX_BUFFERHEADERTYPE, pOutputPortPrivate));
	}
}

/* ALSA Sink OMX Component */

#define OMXALSA_PORT_AUDIO		0
#define OMXALSA_PORT_CLOCK		1

typedef struct _OMX_ALSASINK {
	COMX_COMPONENT gcomp;
	COMX_PORT port_data[2];
	COMX_QUEUE playq;
	pthread_cond_t cond_play;
	size_t frame_size, sample_rate, play_queue_size;
	int64_t starttime;
	int32_t timescale;
	OMX_AUDIO_PARAM_PCMMODETYPE pcm;
	snd_pcm_format_t pcm_format;
	snd_pcm_state_t pcm_state;
	snd_pcm_sframes_t pcm_delay;
	char device_name[16];
} OMX_ALSASINK;

static OMX_ERRORTYPE omxalsasink_set_parameter(OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nParamIndex, OMX_PTR pComponentParameterStructure)
{
	static const struct {
		snd_pcm_format_t fmt;
		unsigned char pcm_mode;
		unsigned char bits_per_sample;
		unsigned char numerical_data;
		unsigned char endianess;
	} fmtmap[] = {
			{ SND_PCM_FORMAT_S8,     OMX_AUDIO_PCMModeLinear,  8, OMX_NumericalDataSigned,   OMX_EndianLittle },
			{ SND_PCM_FORMAT_U8,     OMX_AUDIO_PCMModeLinear,  8, OMX_NumericalDataUnsigned, OMX_EndianLittle },
			{ SND_PCM_FORMAT_S16_LE, OMX_AUDIO_PCMModeLinear, 16, OMX_NumericalDataSigned,   OMX_EndianLittle },
			{ SND_PCM_FORMAT_U16_LE, OMX_AUDIO_PCMModeLinear, 16, OMX_NumericalDataUnsigned, OMX_EndianLittle },
			{ SND_PCM_FORMAT_S16_BE, OMX_AUDIO_PCMModeLinear, 16, OMX_NumericalDataSigned,   OMX_EndianBig },
			{ SND_PCM_FORMAT_U16_BE, OMX_AUDIO_PCMModeLinear, 16, OMX_NumericalDataUnsigned, OMX_EndianBig },
			{ SND_PCM_FORMAT_S24_LE, OMX_AUDIO_PCMModeLinear, 24, OMX_NumericalDataSigned,   OMX_EndianLittle },
			{ SND_PCM_FORMAT_U24_LE, OMX_AUDIO_PCMModeLinear, 24, OMX_NumericalDataUnsigned, OMX_EndianLittle },
			{ SND_PCM_FORMAT_S24_BE, OMX_AUDIO_PCMModeLinear, 24, OMX_NumericalDataSigned,   OMX_EndianBig },
			{ SND_PCM_FORMAT_U24_BE, OMX_AUDIO_PCMModeLinear, 24, OMX_NumericalDataUnsigned, OMX_EndianBig },
			{ SND_PCM_FORMAT_S32_LE, OMX_AUDIO_PCMModeLinear, 32, OMX_NumericalDataSigned,   OMX_EndianLittle },
			{ SND_PCM_FORMAT_U32_LE, OMX_AUDIO_PCMModeLinear, 32, OMX_NumericalDataUnsigned, OMX_EndianLittle },
			{ SND_PCM_FORMAT_S32_BE, OMX_AUDIO_PCMModeLinear, 32, OMX_NumericalDataSigned,   OMX_EndianBig },
			{ SND_PCM_FORMAT_U32_BE, OMX_AUDIO_PCMModeLinear, 32, OMX_NumericalDataUnsigned, OMX_EndianBig },
			{ SND_PCM_FORMAT_A_LAW,  OMX_AUDIO_PCMModeALaw,    8, OMX_NumericalDataUnsigned, OMX_EndianLittle },
			{ SND_PCM_FORMAT_MU_LAW, OMX_AUDIO_PCMModeMULaw,   8, OMX_NumericalDataUnsigned, OMX_EndianLittle },
	};
	OMX_ALSASINK *sink = (OMX_ALSASINK *) hComponent;
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	COMX_PORT *port;
	OMX_AUDIO_PARAM_PCMMODETYPE *pmt;
	OMX_ERRORTYPE r;
	snd_pcm_format_t pcm_format = SND_PCM_FORMAT_UNKNOWN;

	/* Valid in OMX_StateLoaded or on disabled port... so can't check
	 * state without port number which is in index specific struct. */
	if (comp->state == OMX_StateInvalid) return OMX_ErrorInvalidState;

	switch (nParamIndex) {
		case OMX_IndexParamAudioPcm:
			if ((r = omx_cast(pmt, pComponentParameterStructure))) return r;
			if (!(port = comx_get_port(comp, pmt->nPortIndex))) return OMX_ErrorBadPortIndex;
			if (pmt->nPortIndex != OMXALSA_PORT_AUDIO) return OMX_ErrorBadParameter;

			if (comp->state != OMX_StateLoaded && port->def.bEnabled)
				return OMX_ErrorIncorrectStateOperation;

			for (size_t i = 0; i < ARRAY_SIZE(fmtmap); i++) {
				if (fmtmap[i].pcm_mode == pmt->ePCMMode &&
				    fmtmap[i].bits_per_sample == pmt->nBitPerSample &&
				    fmtmap[i].numerical_data == pmt->eNumData &&
				    fmtmap[i].endianess == pmt->eEndian) {
					pcm_format = fmtmap[i].fmt;
					break;
				}
			}
			if (pcm_format == SND_PCM_FORMAT_UNKNOWN)
				return OMX_ErrorBadParameter;

			memcpy(&sink->pcm, pmt, sizeof *pmt);
			sink->pcm_format = pcm_format;
			break;
		default:
			LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "UNSUPPORTED %x, %p", nParamIndex, pComponentParameterStructure);
			return OMX_ErrorNotImplemented;
	}
	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omxalsasink_get_config(OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nIndex, OMX_PTR pComponentConfigStructure)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	OMX_ALSASINK *sink = (OMX_ALSASINK *) hComponent;
	OMX_PARAM_U32TYPE *u32param;
	OMX_ERRORTYPE r;

	if (comp->state == OMX_StateInvalid) return OMX_ErrorInvalidState;
	if (!sink->frame_size) return OMX_ErrorInvalidState;

	switch (nIndex) {
		case OMX_IndexConfigAudioRenderingLatency:
			if ((r = omx_cast(u32param, pComponentConfigStructure))) return r;
			/* Number of samples received but not played */
			pthread_mutex_lock(&comp->mutex);
			u32param->nU32 = sink->play_queue_size / sink->frame_size;
			if (sink->pcm_state == SND_PCM_STATE_RUNNING)
				u32param->nU32 += sink->pcm_delay;
			pthread_mutex_unlock(&comp->mutex);
			LOG_DEBUG("OMX_IndexConfigAudioRenderingLatency %d", u32param->nU32);
			break;
		default:
			LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "UNSUPPORTED %x, %p", nIndex, pComponentConfigStructure);
			return OMX_ErrorNotImplemented;
	}
	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omxalsasink_set_config(OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nIndex, OMX_PTR pComponentConfigStructure)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT*) hComponent;
	OMX_ALSASINK *sink = (OMX_ALSASINK*) hComponent;
	OMX_CONFIG_BOOLEANTYPE *bt;
	OMX_CONFIG_BRCMAUDIODESTINATIONTYPE *adest;
	OMX_ERRORTYPE r;

	if (comp->state == OMX_StateInvalid) return OMX_ErrorInvalidState;

	switch (nIndex) {
		case OMX_IndexConfigBrcmClockReferenceSource:
			if ((r = omx_cast(bt, pComponentConfigStructure))) return r;
			LOG_DEBUG("OMX_IndexConfigBrcmClockReferenceSource %d", bt->bEnabled);
			break;
		case OMX_IndexConfigBrcmAudioDestination:
			if ((r = omx_cast(adest, pComponentConfigStructure))) return r;
			strncpy(sink->device_name, (const char*) adest->sName, sizeof sink->device_name - 1);
			LOG_DEBUG("OMX_IndexConfigBrcmAudioDestination %s", adest->sName);
			break;
		default:
			LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "UNSUPPORTED %x, %p", nIndex, pComponentConfigStructure);
			return OMX_ErrorNotImplemented;
	}
	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omxalsasink_get_extension_index(OMX_HANDLETYPE hComponent, OMX_STRING cParameterName, OMX_INDEXTYPE *pIndexType)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "UNSUPPORTED '%s', %p", cParameterName, pIndexType);
	return OMX_ErrorNotImplemented;
}

static OMX_ERRORTYPE omxalsasink_deinit(OMX_HANDLETYPE hComponent)
{
	OMX_ALSASINK *sink = (OMX_ALSASINK *) hComponent;
	COMX_COMPONENT *comp = &sink->gcomp;
	LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "omxdrmsink_deinit: destroying");
	pthread_mutex_lock(&comp->mutex);
	comp->state = OMX_StateInvalid;
	for (size_t i = 0; i < comp->nports; i++) {
		COMX_PORT *port = &comp->ports[i];
		port->def.bEnabled = OMX_FALSE;
	}
	pthread_cond_broadcast(&comp->cond);
	pthread_mutex_unlock(&comp->mutex);
	comx_fini(&sink->gcomp);
	free(sink);
	LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "omxdrmsink_deinit: destroyed");
	return OMX_ErrorNone;
}

static void *omxalsasink_worker(void *ptr)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) ptr;
	OMX_HANDLETYPE hComponent = (OMX_HANDLETYPE) comp;
	OMX_ALSASINK *sink = (OMX_ALSASINK *) hComponent;
	OMX_BUFFERHEADERTYPE *buf;
	COMX_PORT *audio_port = &comp->ports[OMXALSA_PORT_AUDIO];
	COMX_PORT *clock_port = &comp->ports[OMXALSA_PORT_CLOCK];
	snd_pcm_t *dev = 0;
	snd_pcm_sframes_t n, delay;
	snd_pcm_hw_params_t *hwp;
	snd_pcm_uframes_t buffer_size, period_size, period_size_max;
	SwrContext *resampler = 0;
	uint8_t *resample_buf = 0;
	int32_t timescale;
	uint64_t layout;
	size_t resample_bufsz;
	unsigned int rate;
	struct timespec ts;
	int err;

	LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "worker started");

	err = snd_pcm_open(&dev, sink->device_name, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) goto alsa_error;

	rate = sink->pcm.nSamplingRate;
	buffer_size = rate / 5;
	period_size = buffer_size / 4;
	period_size_max = buffer_size / 3;

	snd_pcm_hw_params_alloca(&hwp);
	snd_pcm_hw_params_any(dev, hwp);
	err = snd_pcm_hw_params_set_channels(dev, hwp, sink->pcm.nChannels);
	if (err) goto alsa_error;
	err = snd_pcm_hw_params_set_access(dev, hwp, sink->pcm.bInterleaved ? SND_PCM_ACCESS_RW_INTERLEAVED : SND_PCM_ACCESS_RW_NONINTERLEAVED);
	if (err) goto alsa_error;
	err = snd_pcm_hw_params_set_rate_near(dev, hwp, &rate, 0);
	if (err) goto alsa_error;
	err = snd_pcm_hw_params_set_format(dev, hwp, sink->pcm_format);
	if (err) goto alsa_error;
	err = snd_pcm_hw_params_set_period_size_max(dev, hwp, &period_size_max, 0);
	if (err) goto alsa_error;
	err = snd_pcm_hw_params_set_buffer_size_near(dev, hwp, &buffer_size);
	if (err) goto alsa_error;
	err = snd_pcm_hw_params_set_period_size_near(dev, hwp, &period_size, 0);
	if (err) goto alsa_error;
	err = snd_pcm_hw_params(dev, hwp);
	if (err) goto alsa_error;

	sink->pcm.nSamplingRate = rate;
	sink->frame_size = (sink->pcm.nChannels * sink->pcm.nBitPerSample) >> 3;
	sink->sample_rate = rate;

	layout = av_get_default_channel_layout(sink->pcm.nChannels);
	resampler = swr_alloc_set_opts(NULL,
	                               layout, AV_SAMPLE_FMT_S16, rate,
	                               layout, AV_SAMPLE_FMT_S16, rate,
	                               0, NULL);
	if (!resampler) goto err;

	av_opt_set_double(resampler, "cutoff", 0.985, 0);
	av_opt_set_int(resampler,"filter_size", 64, 0);
	if (swr_init(resampler) < 0) goto err;

	resample_bufsz = audio_port->def.nBufferSize * 2;
	resample_buf = (uint8_t *) malloc(resample_bufsz);
	if (!resample_buf) goto err;

	LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "sample_rate %d, frame_size %d", rate, sink->frame_size);

	pthread_mutex_lock(&comp->mutex);
	while (comp->wanted_state == OMX_StateExecuting) {
		/* Update hw buffer length, and xrun state */
		sink->pcm_state = snd_pcm_state(dev);
		delay = 0;
		snd_pcm_delay(dev, &delay);
		if (resampler) delay += swr_get_delay(resampler, rate);
		sink->pcm_delay = delay;

		/* Wait for buffer, or timeout to refresh state */
		buf = 0;
		timescale = sink->timescale;
		if (timescale)
			buf = (OMX_BUFFERHEADERTYPE*) comxq_dequeue(&sink->playq);
		if (!buf) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			ts.tv_nsec += 10000000UL; /* 10 ms */
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_nsec -= 1000000000UL;
				ts.tv_sec++;
			}
			pthread_cond_timedwait(&sink->cond_play, &comp->mutex, &ts);
			continue;
		}

		if (clock_port->tunnel_comp && !(buf->nFlags & OMX_BUFFERFLAG_TIME_UNKNOWN)) {
			OMX_TIME_CONFIG_TIMESTAMPTYPE tst;
			int64_t pts = omx_ticks_to_s64(buf->nTimeStamp);

			omx_init(tst);
			tst.nPortIndex = clock_port->tunnel_port;
			tst.nTimestamp = buf->nTimeStamp;
			if (resampler && buf->nFlags & OMX_BUFFERFLAG_STARTTIME)
				swr_init(resampler);
			if (buf->nFlags & (OMX_BUFFERFLAG_STARTTIME|OMX_BUFFERFLAG_DISCONTINUITY)) {
				LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "STARTTIME nTimeStamp=%llx", pts);
				sink->starttime = pts;
			}

			pts -= (int64_t)sink->pcm_delay * OMX_TICKS_PER_SECOND / rate;

			pthread_mutex_unlock(&comp->mutex);
			if (buf->nFlags & (OMX_BUFFERFLAG_STARTTIME|OMX_BUFFERFLAG_DISCONTINUITY))
				OMX_SetConfig(clock_port->tunnel_comp, OMX_IndexConfigTimeClientStartTime, &tst);
			if (pts >= sink->starttime) {
				tst.nTimestamp = omx_ticks_from_s64(pts);
				OMX_SetConfig(clock_port->tunnel_comp, OMX_IndexConfigTimeCurrentAudioReference, &tst);
			}
			pthread_mutex_lock(&comp->mutex);
		}

		if (buf->nFlags & (OMX_BUFFERFLAG_DECODEONLY|OMX_BUFFERFLAG_CODECCONFIG|OMX_BUFFERFLAG_DATACORRUPT)) {
			LOG_DEBUG ("skipping: %d bytes, flags %x", buf->nFilledLen, buf->nFlags);
			sink->play_queue_size -= buf->nFilledLen;
		} else {
			uint8_t *out_ptr, *in_ptr;
			int in_len, out_len;

			pthread_mutex_unlock(&comp->mutex);

			in_ptr = (uint8_t *)(buf->pBuffer + buf->nOffset);
			in_len = buf->nFilledLen / sink->frame_size;

			if (resampler) {
				int delta = 0;

				if (timescale != 0x10000 && timescale >= 0x0100 && timescale <= 0x20000)
					delta = ((int64_t)in_len*(0x10000-timescale))>>16;

				out_len = resample_bufsz / sink->frame_size;
				swr_set_compensation(resampler, delta, in_len);

				out_ptr = resample_buf;
				out_len = swr_convert(resampler, &out_ptr, out_len,
				                      (const uint8_t **) &in_ptr, in_len);

				if (out_len < 0) out_len = 0;
			} else {
				out_ptr = in_ptr;
				out_len = in_len;
			}

			pthread_mutex_lock(&comp->mutex);
			sink->play_queue_size -= buf->nFilledLen;
			sink->pcm_delay += out_len;
			pthread_mutex_unlock(&comp->mutex);

			while (out_len > 0) {
				n = snd_pcm_writei(dev, out_ptr, out_len);
				LOG_TRACE ("-");
				if (n < 0) {
					LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "alsa error: %ld: %s", n, snd_strerror(n));
					snd_pcm_recover(dev, n, 1);
					n = 0;
				}
				out_len -= n;
				n *= sink->frame_size;
				out_ptr += n;
			}
			pthread_mutex_lock(&comp->mutex);
		}

		__comx_process_mark(comp, buf);
		if (buf->nFlags & OMX_BUFFERFLAG_EOS) {
			LOG_DEBUG( "end-of-stream");
			pthread_mutex_unlock(&comp->mutex);
			snd_pcm_drain(dev);
			snd_pcm_prepare(dev);
			pthread_mutex_lock(&comp->mutex);
			sink->pcm_state = SND_PCM_STATE_PREPARED;
			sink->pcm_delay = 0;
			__comx_event(comp, OMX_EventBufferFlag, OMXALSA_PORT_AUDIO, buf->nFlags, 0);
		}
		__comx_empty_buffer_done(comp, buf);
	}
	pthread_mutex_unlock(&comp->mutex);
	cleanup:
	if (dev) snd_pcm_close(dev);
	if (resampler) swr_close(resampler);
	free(resample_buf);
	LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "worker stopped");
	return 0;

	alsa_error:
	LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "ALSA error: %s", snd_strerror(err));
	err:
	pthread_mutex_lock(&comp->mutex);
	/* FIXME: Current we just go to invalid state, but we might go
	 * back to Idle with ErrorResourcesPreempted and let the client
	 * recover. However, omxplayer does not care about this. */
	comp->state = OMX_StateInvalid;
	__comx_event(comp, OMX_EventError, OMX_StateInvalid, 0, 0);
	pthread_mutex_unlock(&comp->mutex);
	goto cleanup;
}

static OMX_ERRORTYPE omxalsasink_audio_do_buffer(COMX_COMPONENT *comp, COMX_PORT *port, OMX_BUFFERHEADERTYPE *buf)
{
	OMX_ALSASINK *sink = (OMX_ALSASINK *) comp;
	sink->play_queue_size += buf->nFilledLen;
	comxq_enqueue(&sink->playq, (void *) buf);
	pthread_cond_signal(&sink->cond_play);
	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omxalsasink_audio_flush(COMX_COMPONENT *comp, COMX_PORT *port)
{
	OMX_ALSASINK *sink = (OMX_ALSASINK *) comp;
	OMX_BUFFERHEADERTYPE *buf;
	while ((buf = (OMX_BUFFERHEADERTYPE *) comxq_dequeue(&sink->playq)) != 0) {
		sink->play_queue_size -= buf->nFilledLen;
		__comx_empty_buffer_done(comp, buf);
	}
	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omxalsasink_clock_do_buffer(COMX_COMPONENT *comp, COMX_PORT *port, OMX_BUFFERHEADERTYPE *buf)
{
	OMX_ALSASINK *sink = (OMX_ALSASINK *) comp;
	OMX_TIME_MEDIATIMETYPE *pMediaTime;
	int wake = 0;

	if (omx_cast(pMediaTime, buf->pBuffer) == OMX_ErrorNone) {
		LOG_DEBUG("port %d %p: clock %u bytes, flags=%x, nTimeStamp=%llx, eState=%d, xScale=%x",
		       port, buf, buf->nFilledLen, buf->nFlags, omx_ticks_to_s64(buf->nTimeStamp),
		       pMediaTime->eState, pMediaTime->xScale);
		wake = (!sink->timescale && pMediaTime->xScale);
		sink->timescale = pMediaTime->xScale;
	} else {
		LOG_DEBUG( "port:%d %p: clock %u bytes, flags=%x, nTimeStamp=%llx", port,
		       buf, buf->nFilledLen, buf->nFlags, omx_ticks_to_s64(buf->nTimeStamp));
	}
	__comx_process_mark(comp, buf);
	__comx_empty_buffer_done(comp, buf);
	if (wake) pthread_cond_signal(&sink->cond_play);

	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omxalsasink_statechange(COMX_COMPONENT *comp)
{
	OMX_ALSASINK *sink = (OMX_ALSASINK *) comp;
	pthread_cond_signal(&sink->cond_play);
	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omxalsasink_create(OMX_HANDLETYPE *pHandle, OMX_PTR pAppData, OMX_CALLBACKTYPE *pCallbacks)
{
	OMX_ALSASINK *sink;
	COMX_PORT *port;
	pthread_condattr_t attr;

	sink = (OMX_ALSASINK *) calloc(1, sizeof *sink);
	if (!sink) return OMX_ErrorInsufficientResources;

	strncpy(sink->device_name, "default", sizeof sink->device_name - 1);
	comxq_init(&sink->playq, offsetof(OMX_BUFFERHEADERTYPE, pInputPortPrivate));

	/* Audio port */
	port = &sink->port_data[OMXALSA_PORT_AUDIO];
	port->def.nSize = sizeof *port;
	port->def.nVersion.nVersion = OMX_VERSION;
	port->def.nPortIndex = OMXALSA_PORT_AUDIO;
	port->def.eDir = OMX_DirInput;
	port->def.nBufferCountMin = 4;
	port->def.nBufferCountActual = 4;
	port->def.nBufferSize = 8 * 1024;
	port->def.bEnabled = OMX_TRUE;
	port->def.eDomain = OMX_PortDomainAudio;
	port->def.format.audio.cMIMEType = (char *) "raw/audio";
	port->def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;
	port->def.nBufferAlignment = 4;
	port->do_buffer = omxalsasink_audio_do_buffer;
	port->flush = omxalsasink_audio_flush;

	/* Clock port */
	port = &sink->port_data[OMXALSA_PORT_CLOCK];
	port->def.nSize = sizeof *port;
	port->def.nVersion.nVersion = OMX_VERSION;
	port->def.nPortIndex = OMXALSA_PORT_CLOCK;
	port->def.eDir = OMX_DirInput;
	port->def.nBufferCountMin = 1;
	port->def.nBufferCountActual = 1;
	port->def.nBufferSize = sizeof(OMX_TIME_MEDIATIMETYPE);
	port->def.bEnabled = OMX_TRUE;
	port->def.eDomain = OMX_PortDomainOther;
	port->def.format.other.eFormat = OMX_OTHER_FormatTime;
	port->def.nBufferAlignment = 4;
	port->do_buffer = omxalsasink_clock_do_buffer;

	audio_omx_init(&sink->gcomp, OMXLIB_ALSA_RENDERER, pAppData, pCallbacks, sink->port_data, ARRAY_SIZE(sink->port_data));
	sink->gcomp.omx.SetParameter = omxalsasink_set_parameter;
	sink->gcomp.omx.GetConfig = omxalsasink_get_config;
	sink->gcomp.omx.SetConfig = omxalsasink_set_config;
	sink->gcomp.omx.GetExtensionIndex = omxalsasink_get_extension_index;
	sink->gcomp.omx.ComponentDeInit = omxalsasink_deinit;
	sink->gcomp.worker = omxalsasink_worker;
	sink->gcomp.statechange = omxalsasink_statechange;

	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&sink->cond_play, &attr);
	pthread_condattr_destroy(&attr);

	*pHandle = (OMX_HANDLETYPE) sink;
	return OMX_ErrorNone;
}

/* OMX Glue to get Alsa handle */

OMX_ERRORTYPE OMXLIB_ALSA_GetHandle(OMX_OUT OMX_HANDLETYPE* pHandle, OMX_IN OMX_STRING cComponentName,
OMX_IN  OMX_PTR pAppData, OMX_IN OMX_CALLBACKTYPE* pCallbacks)
{
if (strcmp(OMXLIB_ALSA_RENDERER,cComponentName) == 0)
return omxalsasink_create(pHandle, pAppData, pCallbacks);

return OMX_ErrorComponentNotFound;
}

OMX_ERRORTYPE  OMXLIB_ALSA_FreeHandle(OMX_IN OMX_HANDLETYPE hComponent)
{
	return ((OMX_COMPONENTTYPE*)hComponent)->ComponentDeInit(hComponent);
}
