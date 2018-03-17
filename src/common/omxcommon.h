/* Generic OMX helpers */
#pragma once


typedef struct _COMX_COMMAND
{
	void* next;
	OMX_COMMANDTYPE cmd;
	OMX_U32 param;
	OMX_PTR data;
} COMX_COMMAND;

typedef struct _COMX_QUEUE
{
	void* head, * tail;
	ptrdiff_t offset;
	size_t num;
} COMX_QUEUE;


typedef struct _COMX_PORT
{
	OMX_BOOL new_enabled;
	OMX_PARAM_PORTDEFINITIONTYPE def;

	size_t num_buffers, num_buffers_old;
	pthread_cond_t cond_no_buffers;
	pthread_cond_t cond_populated;
	pthread_cond_t cond_idle;

	OMX_HANDLETYPE tunnel_comp;
	OMX_U32 tunnel_port;
	bool tunnel_supplier;
	COMX_QUEUE tunnel_supplierq;

	OMX_ERRORTYPE (* do_buffer)(struct _COMX_COMPONENT*, struct _COMX_PORT*, OMX_BUFFERHEADERTYPE*);

	OMX_ERRORTYPE (* flush)(struct _COMX_COMPONENT*, struct _COMX_PORT*);
} COMX_PORT;

typedef struct _COMX_COMPONENT
{
	OMX_COMPONENTTYPE omx;
	OMX_CALLBACKTYPE cb;
	OMX_STATETYPE state, wanted_state;

	pthread_t component_thread, worker_thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	const char* name;
	size_t nports;
	COMX_PORT* ports;
	COMX_QUEUE cmdq;

	void* (* worker)(void*);

	OMX_ERRORTYPE (* statechange)(struct _COMX_COMPONENT*);
} COMX_COMPONENT;

void comxq_init(COMX_QUEUE* q, ptrdiff_t offset);

OMX_ERRORTYPE comx_fill_this_buffer(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE* pBuffer);
OMX_ERRORTYPE comx_empty_this_buffer(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE* pBuffer);
OMX_ERRORTYPE comx_set_callbacks(OMX_HANDLETYPE hComponent, OMX_CALLBACKTYPE* pCallbacks, OMX_PTR pAppData);
OMX_ERRORTYPE comx_use_egl_image(OMX_HANDLETYPE hComponent,
                                 OMX_BUFFERHEADERTYPE** ppBufferHdr, OMX_U32 nPortIndex,
                                 OMX_PTR pAppPrivate, void* eglImage);
OMX_ERRORTYPE comx_component_role_enum(OMX_HANDLETYPE hComponent, OMX_U8 *cRole, OMX_U32 nIndex);
OMX_ERRORTYPE comx_free_buffer(OMX_HANDLETYPE hComponent, OMX_U32 nPortIndex, OMX_BUFFERHEADERTYPE* pBuffer);
OMX_ERRORTYPE comx_allocate_buffer(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE **ppBufferHdr,
                                   OMX_U32 nPortIndex, OMX_PTR pAppPrivate, OMX_U32 nSizeBytes);

OMX_ERRORTYPE comx_component_tunnel_request(
		OMX_HANDLETYPE hComponent, OMX_U32 nPort,
		OMX_HANDLETYPE hTunneledComp, OMX_U32 nTunneledPort, OMX_TUNNELSETUPTYPE* pTunnelSetup);

OMX_ERRORTYPE comx_use_buffer(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE **ppBufferHdr,
                              OMX_U32 nPortIndex, OMX_PTR pAppPrivate, OMX_U32 nSizeBytes, OMX_U8* pBuffer);


OMX_ERRORTYPE comx_send_command(OMX_HANDLETYPE hComponent, OMX_COMMANDTYPE Cmd, OMX_U32 nParam1, OMX_PTR pCmdData);

OMX_ERRORTYPE comx_get_component_version(
		OMX_HANDLETYPE hComponent, OMX_STRING pComponentName,
		OMX_VERSIONTYPE *pComponentVersion, OMX_VERSIONTYPE *pSpecVersion, OMX_UUIDTYPE *pComponentUUID);

OMX_ERRORTYPE comx_get_parameter(OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nParamIndex, OMX_PTR pComponentParameterStructure);
OMX_ERRORTYPE comx_get_state(OMX_HANDLETYPE hComponent, OMX_STATETYPE *pState);
COMX_PORT* comx_get_port(COMX_COMPONENT* comp, size_t idx);


void __comx_event(COMX_COMPONENT *comp, OMX_EVENTTYPE eEvent, OMX_U32 nData1, OMX_U32 nData2, OMX_PTR pEventData);
OMX_ERRORTYPE __comx_empty_buffer_done(COMX_COMPONENT *comp, OMX_BUFFERHEADERTYPE *hdr);
void comxq_enqueue(COMX_QUEUE* q, void* item);
void* comxq_dequeue(COMX_QUEUE* q);
void *comx_worker(void *ptr);
void comx_fini(COMX_COMPONENT *comp);