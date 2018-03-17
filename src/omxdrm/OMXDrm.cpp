#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <IL/OMX_Core.h>
#include <IL/OMX_Component.h>
#include <MemUtils.h>
#include <unistd.h>
#include <utility>
#include "logging.h"
#include "omxcommon.h"


#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

#define DRM_FRAMEBUFFER_WIDTH 1920
#define DRM_FRAMEBUFFER_HEIGHT 1088

extern std::pair<void**, void**> createFrameBuffer(unsigned int w, unsigned int h, unsigned int format, unsigned int planeId);
extern bool initializeDrm();
extern bool deinititalizeDrm();
extern bool connectFBtoPlane(unsigned int planeId);
extern int connectFBtoPlaneWithSrcRect(uint32_t fb_index, uint32_t src_x, uint32_t src_y, uint32_t src_w, uint32_t src_h);

static void drm_omx_init(COMX_COMPONENT *comp, const char *name, OMX_PTR pAppData, OMX_CALLBACKTYPE* pCallbacks, COMX_PORT *ports, size_t nports)
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

/* DRM Sink OMX Component */

#define OMXDRM_PORT_VIDEO		0

typedef struct _OMX_DRMSINK {
	COMX_COMPONENT gcomp;
	COMX_PORT port_data[1];
	COMX_QUEUE playq;
	pthread_cond_t cond_play;
	size_t frame_size, sample_rate, play_queue_size;
	int64_t starttime;
	int32_t timescale;
	char device_name[16];
	void** fbptr[2];
	uint32_t planeId;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	OMX_COLOR_FORMATTYPE colorFormat;
	uint32_t srcWidth;
	uint32_t srcHeight;
	bool srcScale;
} OMX_DRMSINK;

static OMX_ERRORTYPE omxdrmsink_set_parameter(OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nParamIndex, OMX_PTR pComponentParameterStructure)
{
	OMX_DRMSINK *sink = (OMX_DRMSINK *) hComponent;
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	COMX_PORT *port;
	OMX_ERRORTYPE r;

	/* Valid in OMX_StateLoaded or on disabled port... so can't check
	 * state without port number which is in index specific struct. */
	if (comp->state == OMX_StateInvalid) return OMX_ErrorInvalidState;

	switch (nParamIndex) {
	default:

		LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO, 0, "omxdrmsink_set_parameter: UNSUPPORTED %x, %p", nParamIndex, pComponentParameterStructure);
		return OMX_ErrorNotImplemented;
	}
	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omxdrmsink_get_config(OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nIndex, OMX_PTR pComponentConfigStructure)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	OMX_DRMSINK *sink = (OMX_DRMSINK *) hComponent;
	OMX_PARAM_U32TYPE *u32param;
	OMX_ERRORTYPE r;

	if (comp->state == OMX_StateInvalid) return OMX_ErrorInvalidState;
	if (!sink->frame_size) return OMX_ErrorInvalidState;

	switch (nIndex) {
	default:
		LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO, 0, "omxdrmsink_get_config: UNSUPPORTED %x, %p", nIndex, pComponentConfigStructure);
		return OMX_ErrorNotImplemented;
	}
	return OMX_ErrorNone;
}

#define ALIGN(value, alignment) (((value)+(alignment-1))&~(alignment-1))

static OMX_ERRORTYPE omxdrmsink_set_config(OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nIndex, OMX_PTR pComponentConfigStructure)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT*) hComponent;
	OMX_DRMSINK *sink = (OMX_DRMSINK*) hComponent;
	OMX_VIDEO_PORTDEFINITIONTYPE *configDisplay;
	OMX_CONFIG_PLANEBLENDTYPE *configPlane;
	OMX_ERRORTYPE r;

	if (comp->state == OMX_StateInvalid) return OMX_ErrorInvalidState;

	switch (nIndex) {
	case OMX_IndexConfigDisplayRegion:
		// if ((r = omx_cast(configDisplay, pComponentConfigStructure))) return r;
		pthread_mutex_lock(&comp->mutex);
		sink->srcScale = true;
		configDisplay = reinterpret_cast<OMX_VIDEO_PORTDEFINITIONTYPE *>(pComponentConfigStructure);
		sink->width = ALIGN(configDisplay->nFrameWidth, 16);
		sink->height = ALIGN(configDisplay->nFrameHeight, 16);
		sink->stride = ALIGN(configDisplay->nStride, 16);
		sink->colorFormat = configDisplay->eColorFormat;
		sink->srcWidth = configDisplay->nFrameWidth;
		sink->srcHeight = configDisplay->nFrameHeight;
		LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO, 0, "omxdrmsink_set_config:OMX_IndexConfigDisplayRgn w h f s %d %d %d %d",
				 sink->width, sink->height, sink->colorFormat, configDisplay->nStride);
		pthread_mutex_unlock(&comp->mutex);
		break;

	case OMX_IndexConfigCommonPlaneBlend:
		configPlane = reinterpret_cast<OMX_CONFIG_PLANEBLENDTYPE *>(pComponentConfigStructure);
		sink->planeId = configPlane->nPortIndex;
		/* Now kernel module can handle plane id change during execution as well */
		//  Connect now happens when FB is created, no need of another connect
		//  connectFBtoPlane(sink->planeId);
		break;

	default:
		LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO, 0, "UNSUPPORTED %x, %p", nIndex, pComponentConfigStructure);
		return OMX_ErrorNotImplemented;
	}
	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omxdrmsink_get_extension_index(OMX_HANDLETYPE hComponent, OMX_STRING cParameterName, OMX_INDEXTYPE *pIndexType)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO,0, "UNSUPPORTED '%s', %p", cParameterName, pIndexType);
	return OMX_ErrorNotImplemented;
}

static OMX_ERRORTYPE omxdrmsink_deinit(OMX_HANDLETYPE hComponent)
{
	OMX_DRMSINK *sink = (OMX_DRMSINK *) hComponent;
	COMX_COMPONENT *comp = &sink->gcomp;
	/* Handle case when deinit is called w/o calling flush and port disable */
	pthread_mutex_lock(&comp->mutex);
	comp->state = OMX_StateInvalid;
	for (size_t i = 0; i < comp->nports; i++) {
		COMX_PORT *port = &comp->ports[i];
		port->def.bEnabled = OMX_FALSE;
	}
	pthread_cond_broadcast(&comp->cond);
	pthread_mutex_unlock(&comp->mutex);
	deinititalizeDrm();
	comx_fini(&sink->gcomp);
	LOG_INFO(MSGID_ALSAOMXCOMPONENT_INFO, 0, "omxdrmsink_deinit: destroyed");
	return OMX_ErrorNone;
}

static void *omxdrmsink_worker(void *ptr)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) ptr;
	OMX_HANDLETYPE hComponent = (OMX_HANDLETYPE) comp;
	OMX_DRMSINK *sink = (OMX_DRMSINK *) hComponent;
	OMX_BUFFERHEADERTYPE *buf = 0;
	struct timespec ts;
	OMX_U32 bufSize = 0;
	int err;
	int i = 0;
	void *src = NULL;
	void *srcCb = NULL;
	void *srcCr = NULL;
	void *dst = NULL;
	void *dstCb = NULL;
	void *dstCr = NULL;
	size_t chroma_width = (DRM_FRAMEBUFFER_WIDTH)>>1;
	size_t chroma_stride;
	size_t chroma_height;
	size_t luma_size = DRM_FRAMEBUFFER_WIDTH*DRM_FRAMEBUFFER_HEIGHT;
	size_t fullHD_size = (DRM_FRAMEBUFFER_WIDTH * DRM_FRAMEBUFFER_HEIGHT*3/2);
	uint8_t cur_fb = 1;

	LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO, 0, "worker started");

	pthread_mutex_lock(&comp->mutex);
	while (comp->wanted_state == OMX_StateExecuting) {
		buf = (OMX_BUFFERHEADERTYPE*) comxq_dequeue(&sink->playq);
		if (!buf) {
			pthread_cond_timedwait(&sink->cond_play, &comp->mutex, &ts);
			continue;
		}

		if (buf->nFlags & (OMX_BUFFERFLAG_DECODEONLY|OMX_BUFFERFLAG_CODECCONFIG|OMX_BUFFERFLAG_DATACORRUPT)) {
			LOG_ERROR(MSGID_DRMOMXCOMPONENT_INFO, 0, "Data corrupt");
		} else if (buf->nFilledLen) {
			if (sink->srcScale) {
				cur_fb ^= 1;
				LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO, 0, "flipFB: FB = %d", cur_fb);
			}
			if (sink->height * sink->width == luma_size) {
				/* Case of input frame dimension same as frame buffer size, can copy Y,Cb,Cr togeather */
				memcpy(sink->fbptr[cur_fb][0], buf->pBuffer, fullHD_size);
			} else  {
				dst = (sink->fbptr[cur_fb][0]);
				dstCb = sink->fbptr[cur_fb][1];
				dstCr = sink->fbptr[cur_fb][2];
				chroma_stride = sink->stride>>1;
				chroma_height = sink->height>>1;
				src = buf->pBuffer;
				srcCb = src + sink->stride * sink->height;
				srcCr = srcCb + chroma_height * chroma_stride;
				for (i = sink->height-1; i >=0; --i) {
					memcpy(dst, src, sink->stride);
					dst += DRM_FRAMEBUFFER_WIDTH;
					src += sink->stride;
					if (i & 0x1) {
						memcpy(dstCb, srcCb, chroma_stride);
						srcCb += chroma_stride;
						dstCb += chroma_width;
						memcpy(dstCr, srcCr, chroma_stride);
						srcCr += chroma_stride;
						dstCr += chroma_width;
					}
				}
			}
			if (sink->srcScale) {
				LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO, 0, "scale+flip: Alignedheight = %d fb = %d", sink->height, cur_fb);
				LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO, 0, "srcWidth = %d, srcHeight = %d", sink->srcWidth, sink->srcHeight);
				connectFBtoPlaneWithSrcRect(cur_fb, 0, 0, sink->srcWidth, sink->srcHeight);
				sink->srcScale = false;
			} /* else {
				Needed when page flipping is done for every frame.
				connectFBtoPlane(cur_fb);
				}*/
		} else {
			printf("\n ERROR: No data in Buffer");
			LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO, 0, "No data in buffer");
		}

		if (buf->nFlags & OMX_BUFFERFLAG_EOS) {
			LOG_DEBUG ("end-of-stream");
			printf("\n End of stream");
			__comx_event(comp, OMX_EventBufferFlag, OMXDRM_PORT_VIDEO, buf->nFlags, 0);
		}
		__comx_empty_buffer_done(comp, buf);
	}
	pthread_mutex_unlock(&comp->mutex);

cleanup:
	LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO, 0, "omxdrmsink_worker worker stopped");
	return 0;

drm_error:
	LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO, 0, "omxdrmsink_worker  DRM error: %d", err);
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

static OMX_ERRORTYPE omxdrmsink_video_do_buffer(COMX_COMPONENT *comp, COMX_PORT *port, OMX_BUFFERHEADERTYPE *buf)
{
	OMX_DRMSINK *sink = (OMX_DRMSINK *) comp;
	sink->play_queue_size += buf->nFilledLen;
 	comxq_enqueue(&sink->playq, (void *) buf);
	pthread_cond_signal(&sink->cond_play);
	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omxdrmsink_video_flush(COMX_COMPONENT *comp, COMX_PORT *port)
{
	OMX_DRMSINK *sink = (OMX_DRMSINK *) comp;
	OMX_BUFFERHEADERTYPE *buf;
	while ((buf = (OMX_BUFFERHEADERTYPE *) comxq_dequeue(&sink->playq)) != 0) {
		sink->play_queue_size -= buf->nFilledLen;
		__comx_empty_buffer_done(comp, buf);
	}
	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omxdrmsink_statechange(COMX_COMPONENT *comp)
{
	OMX_DRMSINK *sink = (OMX_DRMSINK *) comp;
	pthread_cond_signal(&sink->cond_play);
 	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omxdrmsink_create(OMX_HANDLETYPE *pHandle, OMX_PTR pAppData, OMX_CALLBACKTYPE *pCallbacks)
{
	OMX_DRMSINK *sink;
	COMX_PORT *port;
	pthread_condattr_t attr;

	sink = (OMX_DRMSINK *) calloc(1, sizeof *sink);
	if (!sink) return OMX_ErrorInsufficientResources;
    LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "Component Buf malloc ********* pFull = %x", sink);


	strncpy(sink->device_name, "default", sizeof sink->device_name - 1);
	comxq_init(&sink->playq, offsetof(OMX_BUFFERHEADERTYPE, pInputPortPrivate));

	/* Videoo port */
	port = &sink->port_data[OMXDRM_PORT_VIDEO];
	port->def.nSize = sizeof *port;
	port->def.nVersion.nVersion = OMX_VERSION;
	port->def.nPortIndex = OMXDRM_PORT_VIDEO;
	port->def.eDir = OMX_DirInput;
	port->def.nBufferCountMin = 1;
	port->def.nBufferCountActual = 1;
	port->def.nBufferSize = 3133440;
	port->def.bEnabled = OMX_TRUE;
	port->def.eDomain = OMX_PortDomainVideo;
	port->def.nBufferAlignment = 4;
	port->do_buffer = omxdrmsink_video_do_buffer;
	port->flush = omxdrmsink_video_flush;

	/* Create full size FB at ONE place during init */
	initializeDrm();
	sink->fbptr[0] = nullptr;
	sink->fbptr[1] = nullptr;
	sink->srcScale = false;
	sink->planeId = 0;  // Do not assume any specific plane id
	sink->width = DRM_FRAMEBUFFER_WIDTH;
	sink->height = DRM_FRAMEBUFFER_HEIGHT;
	sink->colorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
	LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO, 0,"Creating framebuffer in OMX Create %d %d %d %d", sink->width, sink->height, sink->colorFormat, sink->planeId);
	std::pair<void**, void**> p(nullptr, nullptr);
	p = createFrameBuffer(DRM_FRAMEBUFFER_WIDTH, DRM_FRAMEBUFFER_HEIGHT, sink->colorFormat, sink->planeId);
	sink->fbptr[0] = p.first;
	sink->fbptr[1] = p.second;
	if (!sink->fbptr[0] || !sink->fbptr[1]) {
		LOG_ERROR(MSGID_DRMOMXCOMPONENT_INFO, 0, "ERROR creating framebuffer");
	}
	drm_omx_init(&sink->gcomp, "OMX.drm.video_render", pAppData, pCallbacks, sink->port_data, ARRAY_SIZE(sink->port_data));
	sink->gcomp.omx.SetParameter = omxdrmsink_set_parameter;
	sink->gcomp.omx.GetConfig = omxdrmsink_get_config;
	sink->gcomp.omx.SetConfig = omxdrmsink_set_config;
	sink->gcomp.omx.GetExtensionIndex = omxdrmsink_get_extension_index;
	sink->gcomp.omx.ComponentDeInit = omxdrmsink_deinit;
	sink->gcomp.worker = omxdrmsink_worker;
	sink->gcomp.statechange = omxdrmsink_statechange;

	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&sink->cond_play, &attr);
	pthread_condattr_destroy(&attr);

	*pHandle = (OMX_HANDLETYPE) sink;
	return OMX_ErrorNone;
}

/* OMX Glue to get the handle */

#include <OMXDrm.h>

OMX_ERRORTYPE OMXDRM_GetHandle(OMX_OUT OMX_HANDLETYPE* pHandle, OMX_IN OMX_STRING cComponentName,
				OMX_IN  OMX_PTR pAppData, OMX_IN OMX_CALLBACKTYPE* pCallbacks)
{
	if (strcmp(cComponentName, "OMX.drm.video_render") == 0) {
		OMX_ERRORTYPE ret = omxdrmsink_create(pHandle, pAppData, pCallbacks);
		LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "OMXDRM_GetHandle Drmhandle = %x", *pHandle);
		return ret;
	}

	return OMX_ErrorComponentNotFound;
}

OMX_ERRORTYPE OMXDRM_FreeHandle(OMX_IN OMX_HANDLETYPE hComponent)
{
	LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "OMXDRM_FreeHandle Drmhandle = %x", hComponent);
	OMX_ERRORTYPE ret = ((OMX_COMPONENTTYPE*)hComponent)->ComponentDeInit(hComponent);
	OMX_DRMSINK *sink = (OMX_DRMSINK *) hComponent;
	LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "Component Buf free ********* pFull = %x", sink);
	free(sink);
	LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "omxdrmsink_deinit: Freed");
	return ret;
}
