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
#include "logging.h"
#include "omxcommon.h"
const char* const logContextName = "omx-components";
const char* const logPrefix = "[omx-components]";
PmLogContext omxComponentsLogContext;

template<class X>
inline X max(X a, X b)
{
	return (a > b) ? a : b;
}

template<class X>
OMX_ERRORTYPE omx_cast(X*&toptr, OMX_PTR fromptr)
{
	toptr = (X*) fromptr;
	if (toptr->nSize < sizeof(X)) return OMX_ErrorBadParameter;
	if (toptr->nVersion.nVersion != OMX_VERSION) return OMX_ErrorVersionMismatch;
	return OMX_ErrorNone;
}

template<class X>
void omx_init(X &omx)
{
	omx.nSize = sizeof(X);
	omx.nVersion.nVersion = OMX_VERSION;
	PmLogErr error = PmLogGetContext(logContextName, &omxComponentsLogContext);
}


void comxq_init(COMX_QUEUE* q, ptrdiff_t offset)
{
	q->head = q->tail = 0;
	q->offset = offset;
	q->num = 0;
}

void comx_fini(COMX_COMPONENT *comp)
{
	pthread_join(comp->component_thread, 0);

	for (size_t i = 0; i < comp->nports; i++) {
		COMX_PORT *port = &comp->ports[i];
		pthread_cond_destroy(&port->cond_no_buffers);
		pthread_cond_destroy(&port->cond_populated);
		pthread_cond_destroy(&port->cond_idle);
	}
	pthread_mutex_destroy(&comp->mutex);
	pthread_cond_destroy(&comp->cond);
}


void** comxq_nextptr(COMX_QUEUE* q, void* item)
{
	return (void**) ((uint8_t*) item + q->offset);
}

void comxq_enqueue(COMX_QUEUE* q, void* item)
{
	*comxq_nextptr(q, item) = 0;
	if (q->tail)
	{
		*comxq_nextptr(q, q->tail) = item;
		q->tail = item;
	} else
	{
		q->head = q->tail = item;
	}
	q->num++;
}

void* comxq_dequeue(COMX_QUEUE* q)
{
	void* item = q->head;
	if (item)
	{
		q->head = *comxq_nextptr(q, item);
		if (!q->head) q->tail = 0;
		q->num--;
	}
	return item;
}

COMX_PORT* comx_get_port(COMX_COMPONENT* comp, size_t idx)
{
	if (idx < 0 || idx >= comp->nports) return 0;
	return &comp->ports[idx];
}

OMX_ERRORTYPE comx_get_component_version(
		OMX_HANDLETYPE hComponent, OMX_STRING pComponentName,
		OMX_VERSIONTYPE* pComponentVersion, OMX_VERSIONTYPE* pSpecVersion, OMX_UUIDTYPE* pComponentUUID)
{
	COMX_COMPONENT* comp = (COMX_COMPONENT*) hComponent;
	LOG_DEBUG("enter");
	strcpy(pComponentName, comp->name);
	pComponentVersion->nVersion = OMX_VERSION;
	pSpecVersion->nVersion = OMX_VERSION;
	memcpy(pComponentUUID, &hComponent, sizeof hComponent);
	return OMX_ErrorNone;
}


OMX_ERRORTYPE comx_get_state(OMX_HANDLETYPE hComponent, OMX_STATETYPE *pState)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	*pState = comp->state;
	return OMX_ErrorNone;
}

OMX_ERRORTYPE comx_component_tunnel_request(
		OMX_HANDLETYPE hComponent, OMX_U32 nPort,
		OMX_HANDLETYPE hTunneledComp, OMX_U32 nTunneledPort, OMX_TUNNELSETUPTYPE* pTunnelSetup)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	COMX_PORT *port = 0;

	if (comp->state == OMX_StateInvalid) return OMX_ErrorInvalidState;
	if (!(port = comx_get_port(comp, nPort))) return OMX_ErrorBadPortIndex;
	if (comp->state != OMX_StateLoaded && port->def.bEnabled)
		return OMX_ErrorIncorrectStateOperation;

	if (hTunneledComp == 0 || pTunnelSetup == 0) {
		port->tunnel_comp = 0;
		return OMX_ErrorNone;
	}

	if (port->def.eDir == OMX_DirInput) {
		/* Negotiate parameters */
		OMX_PARAM_PORTDEFINITIONTYPE param;
		omx_init(param);
		param.nPortIndex = nTunneledPort;
		if (OMX_GetParameter(hTunneledComp, OMX_IndexParamPortDefinition, &param))
			goto not_compatible;
		if (param.eDomain != port->def.eDomain)
			goto not_compatible;

		param.nBufferCountActual = max(param.nBufferCountMin, port->def.nBufferCountMin);
		param.nBufferSize = max(port->def.nBufferSize, param.nBufferSize);
		param.nBufferAlignment = max(port->def.nBufferAlignment, param.nBufferAlignment);
		port->def.nBufferCountActual = param.nBufferCountActual;
		port->def.nBufferSize = param.nBufferSize;
		port->def.nBufferAlignment = param.nBufferAlignment;
		if (OMX_SetParameter(hTunneledComp, OMX_IndexParamPortDefinition, &param))
			goto not_compatible;

		/* Negotiate buffer supplier */
		OMX_PARAM_BUFFERSUPPLIERTYPE suppl;
		omx_init(suppl);
		suppl.nPortIndex = nTunneledPort;
		if (OMX_GetParameter(hTunneledComp, OMX_IndexParamCompBufferSupplier, &suppl))
			goto not_compatible;

		/* Being supplier is not supported so ask the other side to be it */
		suppl.eBufferSupplier =
				(pTunnelSetup->eSupplier == OMX_BufferSupplyOutput)
				? OMX_BufferSupplyOutput : OMX_BufferSupplyInput;
		if (OMX_SetParameter(hTunneledComp, OMX_IndexParamCompBufferSupplier, &suppl))
			goto not_compatible;

		port->tunnel_comp = hTunneledComp;
		port->tunnel_port = nTunneledPort;
		port->tunnel_supplier = (suppl.eBufferSupplier == OMX_BufferSupplyInput);
		pTunnelSetup->eSupplier = suppl.eBufferSupplier;
		LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "%s: comx_component_tunnel_request: port = %d ComponentTunnnelRequest: %p %d", comp->name, port->def.nPortIndex, hTunneledComp, nTunneledPort);
	} else {
		LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "%s: comx_component_tunnel_request: port = %d OUTPUT TUNNEL UNSUPPORTED: %p, %d, %p",comp->name,port->def.nPortIndex, hTunneledComp, nTunneledPort, pTunnelSetup);
		return OMX_ErrorNotImplemented;
	}
	return OMX_ErrorNone;

	not_compatible:
	LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "%s: comx_component_tunnel_request: port =%d ComponentTunnnelRequest: %p %d - NOT COMPATIBLE", comp->name, port->def.nPortIndex, hTunneledComp, nTunneledPort);
	return OMX_ErrorPortsNotCompatible;
}



OMX_ERRORTYPE comx_get_parameter(OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nParamIndex, OMX_PTR pComponentParameterStructure)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	COMX_PORT *port;
	OMX_PORT_PARAM_TYPE *ppt;
	OMX_PARAM_PORTDEFINITIONTYPE *pdt;
	OMX_ERRORTYPE r;
	OMX_PORTDOMAINTYPE domain;

	if (comp->state == OMX_StateInvalid) return OMX_ErrorInvalidState;

	LOG_DEBUG( "%s called %x, %p", comp->name, nParamIndex, pComponentParameterStructure);
	switch (nParamIndex) {
		case OMX_IndexParamAudioInit:
			domain = OMX_PortDomainAudio;
			goto param_init;
		case OMX_IndexParamVideoInit:
			domain = OMX_PortDomainVideo;
			goto param_init;
		case OMX_IndexParamImageInit:
			domain = OMX_PortDomainImage;
			goto param_init;
		case OMX_IndexParamOtherInit:
			domain = OMX_PortDomainOther;
			goto param_init;
		param_init:
			if ((r = omx_cast(ppt, pComponentParameterStructure))) return r;
			ppt->nPorts = 0;
			ppt->nStartPortNumber = 0;
			for (size_t i = 0; i < comp->nports; i++) {
				if (comp->ports[i].def.eDomain != domain)
					continue;
				if (!ppt->nPorts)
					ppt->nStartPortNumber = i;
				ppt->nPorts++;
			}
			break;
		case OMX_IndexParamPortDefinition:
			if ((r = omx_cast(pdt, pComponentParameterStructure))) return r;
			if (!(port = comx_get_port(comp, pdt->nPortIndex))) return OMX_ErrorBadPortIndex;
			memcpy(pComponentParameterStructure, &port->def, sizeof *pdt);
			break;
		default:
			LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "comx_get_parameter: UNSUPPORTED %x, %p", nParamIndex, pComponentParameterStructure);
			return OMX_ErrorUnsupportedIndex;
	}
	return OMX_ErrorNone;
}


void __comx_event(COMX_COMPONENT *comp, OMX_EVENTTYPE eEvent, OMX_U32 nData1, OMX_U32 nData2, OMX_PTR pEventData)
{
	if (!comp->cb.EventHandler) return;
	pthread_mutex_unlock(&comp->mutex);
	comp->cb.EventHandler((OMX_HANDLETYPE) comp, comp->omx.pApplicationPrivate, eEvent, nData1, nData2, pEventData);
	pthread_mutex_lock(&comp->mutex);
}

void __comx_port_update_buffer_state(COMX_COMPONENT *comp, COMX_PORT *port)
{
	if (port->num_buffers_old == port->num_buffers)
		return;

	port->def.bPopulated = (port->num_buffers >= port->def.nBufferCountActual) ? OMX_TRUE : OMX_FALSE;
	if (port->num_buffers == 0)
		pthread_cond_signal(&port->cond_no_buffers);
	else if (port->num_buffers == port->def.nBufferCountActual)
		pthread_cond_signal(&port->cond_populated);
}

OMX_ERRORTYPE comx_use_buffer(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE **ppBufferHdr,
                                     OMX_U32 nPortIndex, OMX_PTR pAppPrivate, OMX_U32 nSizeBytes, OMX_U8* pBuffer)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	OMX_BUFFERHEADERTYPE *hdr;
	COMX_PORT *port;
	void *buf;

	if (comp->state == OMX_StateInvalid) return OMX_ErrorInvalidState;
	if (!(port = comx_get_port(comp, nPortIndex))) return OMX_ErrorBadPortIndex;

	if (!((comp->state == OMX_StateLoaded && comp->wanted_state == OMX_StateIdle) ||
	      (port->def.bEnabled == OMX_FALSE &&
	       (comp->state == OMX_StateExecuting ||
	        comp->state == OMX_StatePause ||
	        comp->state == OMX_StateIdle))))
		return OMX_ErrorIncorrectStateOperation;

	buf = malloc(sizeof(OMX_BUFFERHEADERTYPE) + (pBuffer ? 0 : nSizeBytes));
	if (!buf) return OMX_ErrorInsufficientResources;

	hdr = (OMX_BUFFERHEADERTYPE *) buf;
	memset(hdr, 0, sizeof *hdr);
	omx_init(*hdr);
	hdr->pBuffer = pBuffer ? (OMX_U8*)pBuffer : (OMX_U8*)((char*)buf + nSizeBytes);
	hdr->nAllocLen = nSizeBytes;
	hdr->pAppPrivate = pAppPrivate;
	if (port->def.eDir == OMX_DirInput) {
		hdr->nInputPortIndex = nPortIndex;
		hdr->pOutputPortPrivate = pAppPrivate;
	} else {
		hdr->nOutputPortIndex = nPortIndex;
		hdr->pInputPortPrivate = pAppPrivate;
	}
	pthread_mutex_lock(&comp->mutex);
	port->num_buffers++;
	__comx_port_update_buffer_state(comp, port);
	pthread_mutex_unlock(&comp->mutex);

	LOG_INFO(MSGID_COMXCOMPONENT_INFO,0 , "comx_use_buffer allocated: %d, %p, %u, %p", nPortIndex, pAppPrivate, nSizeBytes, pBuffer);
	*ppBufferHdr = hdr;

	return OMX_ErrorNone;
}

OMX_ERRORTYPE comx_allocate_buffer(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE **ppBufferHdr,
                                          OMX_U32 nPortIndex, OMX_PTR pAppPrivate, OMX_U32 nSizeBytes)
{
	return comx_use_buffer(hComponent, ppBufferHdr, nPortIndex, pAppPrivate, nSizeBytes, 0);
}

OMX_ERRORTYPE comx_free_buffer(OMX_HANDLETYPE hComponent, OMX_U32 nPortIndex, OMX_BUFFERHEADERTYPE* pBuffer)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	COMX_PORT *port;

	if (!(port = comx_get_port(comp, nPortIndex))) return OMX_ErrorBadPortIndex;

	/* Freeing buffer is allowed in all states, so destructor can
	 * synchronize successfully. */

	pthread_mutex_lock(&comp->mutex);

	if (!((comp->state == OMX_StateIdle && comp->wanted_state == OMX_StateLoaded) ||
	      (port->def.bEnabled == OMX_FALSE &&
	       (comp->state == OMX_StateExecuting ||
	        comp->state == OMX_StatePause ||
	        comp->state == OMX_StateIdle)))) {
		/* In unexpected states the port unpopulated error is sent. */
		if (port->num_buffers == port->def.nBufferCountActual)
			__comx_event(comp, OMX_EventError, OMX_ErrorPortUnpopulated, nPortIndex, 0);
		/* FIXME? should we mark the port also down */
	}

	port->num_buffers--;
	__comx_port_update_buffer_state(comp, port);

	pthread_mutex_unlock(&comp->mutex);

	free(pBuffer);

	return OMX_ErrorNone;
}

void __comx_port_queue_supplier_buffer(COMX_PORT *port, OMX_BUFFERHEADERTYPE *hdr)
{
	comxq_enqueue(&port->tunnel_supplierq, (void *) hdr);
	if (port->tunnel_supplierq.num == port->num_buffers)
		pthread_cond_broadcast(&port->cond_idle);
}

OMX_ERRORTYPE __comx_empty_buffer_done(COMX_COMPONENT *comp, OMX_BUFFERHEADERTYPE *hdr)
{
	COMX_PORT *port = comx_get_port(comp, hdr->nInputPortIndex);
	OMX_ERRORTYPE r;

	if (port->tunnel_comp) {
		/* Buffers are sent to the tunneled port once emptied as long as
		 * the component is in the OMX_StateExecuting state */
		if ((comp->state == OMX_StateExecuting && port->def.bEnabled) ||
		    !port->tunnel_supplier) {
			pthread_mutex_unlock(&comp->mutex);
			r = OMX_FillThisBuffer(port->tunnel_comp, hdr);
			pthread_mutex_lock(&comp->mutex);
		} else {
			r = OMX_ErrorIncorrectStateOperation;
		}
	} else {
		pthread_mutex_unlock(&comp->mutex);
		r = comp->cb.EmptyBufferDone((OMX_HANDLETYPE) comp, hdr->pAppPrivate, hdr);
		pthread_mutex_lock(&comp->mutex);
	}

	if (r != OMX_ErrorNone && port->tunnel_supplier) {
		__comx_port_queue_supplier_buffer(port, hdr);
		r = OMX_ErrorNone;
	}

	return r;
}

OMX_ERRORTYPE comx_empty_this_buffer(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE* pBuffer)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	COMX_PORT *port;
	OMX_ERRORTYPE r;

	if (comp->state == OMX_StateInvalid) return OMX_ErrorInvalidState;
	if (comp->state != OMX_StatePause && comp->state != OMX_StateExecuting &&
	    comp->wanted_state != OMX_StateExecuting)
		return OMX_ErrorIncorrectStateOperation;

	if (!(port = comx_get_port(comp, pBuffer->nInputPortIndex)))
		return OMX_ErrorBadPortIndex;

	pthread_mutex_lock(&comp->mutex);
	if (port->def.bEnabled) {
		if (port->do_buffer)
			r = port->do_buffer(comp, port, pBuffer);
		else
			r = __comx_empty_buffer_done(comp, pBuffer);
	} else {
		if (port->tunnel_supplier) {
			__comx_port_queue_supplier_buffer(port, pBuffer);
			r = OMX_ErrorNone;
		} else {
			r = OMX_ErrorIncorrectStateOperation;
		}
	}
	pthread_mutex_unlock(&comp->mutex);
	return r;
}

OMX_ERRORTYPE comx_fill_this_buffer(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE* pBuffer)
{
	LOG_DEBUG ("stub");
	return OMX_ErrorNotImplemented;
}


OMX_ERRORTYPE __comx_port_unpopulate(COMX_COMPONENT *comp, COMX_PORT *port)
{
	OMX_BUFFERHEADERTYPE *hdr;
	void *buf;

	if (port->tunnel_supplier) {
		LOG_DEBUG("__comx_port_unpopulate: port = %d waiting for supplier buffers (%d / %d)",
		         (int)port->tunnel_supplierq.num, (int)port->num_buffers);
		while (port->tunnel_supplierq.num != port->num_buffers)
			pthread_cond_wait(&port->cond_idle, &comp->mutex);

		LOG_DEBUG("__comx_port_unpopulate: free tunnel buffers");
		while ((hdr = (OMX_BUFFERHEADERTYPE*)comxq_dequeue(&port->tunnel_supplierq)) != 0) {
			buf = hdr->pBuffer;
			OMX_FreeBuffer(port->tunnel_comp, port->tunnel_port, hdr);
			LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "__comx_port_unpopulate: free tunnel buffers ********* buf = %x", buf);
			_aligned_free(buf);
			port->num_buffers--;
			__comx_port_update_buffer_state(comp, port);
		}
	} else {
		/* Wait client / tunnel supplier to allocate buffers */
		LOG_DEBUG( "__comx_port_unpopulate: waiting %d buffers to be freed", (int)port->num_buffers);
		while (port->num_buffers > 0)
			pthread_cond_wait(&port->cond_no_buffers, &comp->mutex);
	}

	LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "__comx_port_unpopulate: UNPOPULATED");
	return OMX_ErrorNone;
}

OMX_ERRORTYPE __comx_port_populate(COMX_COMPONENT *comp, COMX_PORT *port)
{
	OMX_ERRORTYPE r;
	OMX_BUFFERHEADERTYPE *hdr;
	OMX_U8 *buf;

	if (port->tunnel_supplier) {
		LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "__comx_port_populate: Allocating tunnel buffers");
		while (port->num_buffers < port->def.nBufferCountActual) {
			pthread_mutex_unlock(&comp->mutex);
			r = OMX_ErrorInsufficientResources;
			buf = (OMX_U8*)_aligned_malloc(port->def.nBufferSize, port->def.nBufferAlignment);
			LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "__comx_port_populate: Allocated tunnel buffers ********* buf = %x", buf);
			//buf = malloc(port->def.nBufferSize);
			if (buf) {
				r = OMX_UseBuffer(port->tunnel_comp, &hdr,
				                  port->tunnel_port, 0,
				                  port->def.nBufferSize, (OMX_U8*) buf);
				if (r != OMX_ErrorNone) free(buf);
			}

			if (r == OMX_ErrorInvalidState ||
			    r == OMX_ErrorIncorrectStateOperation) {
				/* Non-supplier is not transitioned yet.
				 * Wait for a bit and retry */
				usleep(1000);
				pthread_mutex_lock(&comp->mutex);
				continue;
			}
			pthread_mutex_lock(&comp->mutex);

			if (r != OMX_ErrorNone) {
				/* Hard error. Cancel and bail out */
				__comx_port_unpopulate(comp, port);
				return r;
			}

			if (port->def.eDir == OMX_DirInput)
				hdr->nInputPortIndex = port->def.nPortIndex;
			else
				hdr->nOutputPortIndex = port->def.nPortIndex;
			comxq_enqueue(&port->tunnel_supplierq, (void*) hdr);
			port->num_buffers++;
			__comx_port_update_buffer_state(comp, port);
		}
	} else {
		/* Wait client / tunnel supplier to allocate buffers */
		LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "__comx_port_populate: waiting buffers");
		while (!port->def.bPopulated)
			pthread_cond_wait(&port->cond_populated, &comp->mutex);
	}

	LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "__comx_port_populate: POPULATED");
	return OMX_ErrorNone;
}

OMX_ERRORTYPE comx_send_command(OMX_HANDLETYPE hComponent, OMX_COMMANDTYPE Cmd, OMX_U32 nParam1, OMX_PTR pCmdData)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	COMX_COMMAND *c;

	/* OMX IL Specification is unclear which errors can be returned
	 * inline and which need to be reported with a callback.
	 * This just does minimal state checking, and queues everything
	 * to worker and reports any real errors via the callback. */
	if (!hComponent) return OMX_ErrorInvalidComponent;
	if (comp->state == OMX_StateInvalid) return OMX_ErrorInvalidState;
	if (!comp->cb.EventHandler) return OMX_ErrorNotReady;

	c = (COMX_COMMAND*) malloc(sizeof(COMX_COMMAND));
	if (!c) return OMX_ErrorInsufficientResources;

	LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "comx_send_command: SendCommand %x, %x, %p", Cmd, nParam1, pCmdData);
	c->cmd = Cmd;
	c->param = nParam1;
	c->data = pCmdData;

	pthread_mutex_lock(&comp->mutex);
	comxq_enqueue(&comp->cmdq, (void*) c);
	pthread_cond_signal(&comp->cond);
	pthread_mutex_unlock(&comp->mutex);

	return OMX_ErrorNone;
}

#define COMX_TRANS(a,b) ((((uint32_t)a) << 16) | (uint32_t)b)

OMX_ERRORTYPE comx_do_set_state(COMX_COMPONENT *comp, COMX_COMMAND *cmd)
{
	OMX_STATETYPE new_state = (OMX_STATETYPE) cmd->param;
	OMX_ERRORTYPE r;
	COMX_PORT *port;
	size_t i;

	LOG_INFO(MSGID_COMXCOMPONENT_INFO,0,"%s: comx_do_set_state: current state(comp->state) = %d new_state=%d  ", comp->name,comp->state, new_state);
	if (comp->state == new_state) {
		return OMX_ErrorSameState;
	}

	/* This happens when component is dequeing set state command meanwhile
	 FreeHandle comes from application thread */
	if (comp->state == OMX_StateInvalid) {
		return OMX_ErrorNone;
	}

	if (new_state == OMX_StateInvalid) {
		/* Transition to invalid state is always valid and immediate */
		comp->state = new_state;
		return OMX_ErrorNone;
	}

	comp->wanted_state = new_state;

	if (comp->statechange) {
		r = comp->statechange(comp);
		if (r != OMX_ErrorNone) goto err;
	}

	switch (COMX_TRANS(comp->state, new_state)) {
		case COMX_TRANS(OMX_StateLoaded, OMX_StateIdle):
			/* populate or wait for all enabled ports to be populated */
			for (i = 0; i < comp->nports; i++) {
				if (!comp->ports[i].def.bEnabled) continue;
				r = __comx_port_populate(comp, &comp->ports[i]);
				if (r) goto err;
			}
			break;
		case COMX_TRANS(OMX_StateIdle, OMX_StateLoaded):
			/* free or wait all ports to be unpopulated */
			for (i = 0; i < comp->nports; i++) {
				r = __comx_port_unpopulate(comp, &comp->ports[i]);
				if (r) goto err;
			}
			break;
		case COMX_TRANS(OMX_StateIdle, OMX_StateExecuting):
			/* start threads */
			r = OMX_ErrorInsufficientResources;
			if (comp->worker &&
			    pthread_create(&comp->worker_thread, 0, comp->worker, comp) != 0)
				goto err;
			break;
		case COMX_TRANS(OMX_StateExecuting, OMX_StateIdle):
			/* stop/join threads & wait buffers to be returned to suppliers */
			if (comp->worker_thread) {
				pthread_mutex_unlock(&comp->mutex);
				pthread_join(comp->worker_thread, 0);
				pthread_mutex_lock(&comp->mutex);
				comp->worker_thread = 0;
			}
			for (i = 0; i < comp->nports; i++) {
				port = &comp->ports[i];
				if (!port->tunnel_supplier || !port->def.bEnabled) continue;
				while (port->tunnel_supplierq.num != port->num_buffers)
					pthread_cond_wait(&port->cond_idle, &comp->mutex);
			}
			break;
		default:
			/* FIXME: Pause and WaitForResources states not supported */
			r = OMX_ErrorIncorrectStateTransition;
			goto err;
	}
	if (comp->state != OMX_StateInvalid)
		comp->state = new_state;
	LOG_DEBUG ("transition to state %d: success", new_state);
	return OMX_ErrorNone;
	err:
	comp->wanted_state = comp->state;
	LOG_DEBUG ("transition to state %d: result %x", new_state, r);
	return r;
}

OMX_ERRORTYPE comx_do_port_command(COMX_COMPONENT *comp, COMX_PORT *port, COMX_COMMAND *cmd)
{
	OMX_ERRORTYPE r = OMX_ErrorNone;

	switch (cmd->cmd) {
		case OMX_CommandFlush:
			if (port->flush) r = port->flush(comp, port);
			break;
		case OMX_CommandPortEnable:
			port->def.bEnabled = OMX_TRUE;
			r = __comx_port_populate(comp, port);
			if (r != OMX_ErrorNone)
				port->def.bEnabled = OMX_FALSE;
			break;
		case OMX_CommandPortDisable:
			port->def.bEnabled = OMX_FALSE;
			if (port->flush) port->flush(comp, port);
			r = __comx_port_unpopulate(comp, port);
			break;
		default:
			r = OMX_ErrorNotImplemented;
			break;
	}
	return r;
}

OMX_ERRORTYPE comx_do_command(COMX_COMPONENT *comp, COMX_COMMAND *cmd)
{
	COMX_PORT *port;

	LOG_INFO(MSGID_COMXCOMPONENT_INFO,0,"%s: comx_do_command: cmd %x comp state %x", comp->name, cmd->cmd, cmd->param);
	switch (cmd->cmd) {
		case OMX_CommandStateSet:
			LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0,"%s: state %x", comp->name, cmd->param);
			return comx_do_set_state(comp, cmd);
		case OMX_CommandFlush:
		case OMX_CommandPortEnable:
		case OMX_CommandPortDisable:
			/* FIXME: OMX_ALL is not supported (but not used in omxplayer) */
			if (!(port = comx_get_port(comp, cmd->param)))
				return OMX_ErrorBadPortIndex;
			LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "%s: comx_do_command: command %x", comp->name, cmd->cmd);
			return comx_do_port_command(comp, port, cmd);
		case OMX_CommandMarkBuffer:
			/* FIXME: Not implemented (but not used in omxplayer) */
		default:
			LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "%s: comx_do_command: UNSUPPORTED %x, %x, %p", comp->name, cmd->cmd, cmd->param, cmd->data);
			return OMX_ErrorNotImplemented;
	}
}

void *comx_worker(void *ptr)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) ptr;
	COMX_PORT *port;
	COMX_COMMAND *cmd;
	OMX_BUFFERHEADERTYPE *hdr;
	OMX_ERRORTYPE r;

	LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "%s: comx_worker: start",comp->name );
	pthread_mutex_lock(&comp->mutex);
	while (comp->state != OMX_StateInvalid) {
		cmd = (COMX_COMMAND *) comxq_dequeue(&comp->cmdq);
		if (cmd) {
			r = comx_do_command(comp, cmd);
			if (r == OMX_ErrorNone) {
				__comx_event(comp, OMX_EventCmdComplete,
				             cmd->cmd, cmd->param, cmd->data);
			}
			else {
				__comx_event(comp, OMX_EventError, r, 0, 0);
			}
		} else {
			pthread_cond_wait(&comp->cond, &comp->mutex);
		}

		if (comp->state != OMX_StateExecuting)
			continue;

		/* FIXME: Rate limit and retry if needed suppplier buffer enqueuing */
		for (size_t i = 0; i < comp->nports; i++) {
			port = &comp->ports[i];
			while ((hdr = (OMX_BUFFERHEADERTYPE*)comxq_dequeue(&port->tunnel_supplierq)) != 0) {
				pthread_mutex_unlock(&comp->mutex);
				r = OMX_FillThisBuffer(port->tunnel_comp, hdr);
				pthread_mutex_lock(&comp->mutex);
				if (r != OMX_ErrorNone) {
					__comx_port_queue_supplier_buffer(port, hdr);
					break;
				}
			}
		}
	}
	pthread_mutex_unlock(&comp->mutex);
	/* FIXME: make sure all buffers are returned and worker threads stopped */
	LOG_INFO(MSGID_COMXCOMPONENT_INFO, 0, "comx_worker: stop");
	return 0;
}

OMX_ERRORTYPE comx_set_callbacks(OMX_HANDLETYPE hComponent, OMX_CALLBACKTYPE* pCallbacks, OMX_PTR pAppData)
{
	COMX_COMPONENT *comp = (COMX_COMPONENT *) hComponent;
	if (comp->state == OMX_StateInvalid) return OMX_ErrorInvalidState;
	if (comp->state != OMX_StateLoaded) return OMX_ErrorIncorrectStateOperation;
	pthread_mutex_lock(&comp->mutex);
	comp->omx.pApplicationPrivate = pAppData;
	comp->cb = *pCallbacks;
	pthread_mutex_unlock(&comp->mutex);
	return OMX_ErrorNone;
}

OMX_ERRORTYPE comx_use_egl_image(OMX_HANDLETYPE hComponent,
                                        OMX_BUFFERHEADERTYPE** ppBufferHdr, OMX_U32 nPortIndex,
                                        OMX_PTR pAppPrivate, void* eglImage)
{
	return OMX_ErrorNotImplemented;
}

OMX_ERRORTYPE comx_component_role_enum(OMX_HANDLETYPE hComponent, OMX_U8 *cRole, OMX_U32 nIndex)
{
	return OMX_ErrorNotImplemented;
}
