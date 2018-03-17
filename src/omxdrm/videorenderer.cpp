#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#include <utility>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "xf86drm.h"
#include "xf86drmMode.h"
#include "drm_fourcc.h"

#include "kms.h"

#include "buffers.h"
#include "output.h"
#include "logging.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static inline int64_t U642I64(uint64_t val)
{
	return (int64_t)*((int64_t *)&val);
}

#define bit_name_fn(res)					\
const char * res##_str(int type) {				\
	unsigned int i;						\
	const char *sep = "";					\
	for (i = 0; i < ARRAY_SIZE(res##_names); i++) {		\
		if (type & (1 << i)) {				\
			printf("%s%s", sep, res##_names[i]);	\
			sep = ", ";				\
		}						\
	}							\
	return NULL;						\
}

static const char *mode_type_names[] = {
	"builtin",
	"clock_c",
	"crtc_c",
	"preferred",
	"default",
    	"userdef",
	"driver",
};

static bit_name_fn(mode_type)

static const char *mode_flag_names[] = {
	"phsync",
	"nhsync",
	"pvsync",
	"nvsync",
	"interlace",
	"dblscan",
	"csync",
	"pcsync",
	"ncsync",
	"hskew",
	"bcast",
	"pixmux",
	"dblclk",
	"clkdiv2"
};

static bit_name_fn(mode_flag)


static void free_resources(struct resources *res)
{
	int i;

	if (!res)
		return;

#define free_resource(_res, __res, type, Type)					\
	do {									\
		if (!(_res)->type##s)						\
			break;							\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			if (!(_res)->type##s[i].type)				\
				break;						\
			drmModeFree##Type((_res)->type##s[i].type);		\
		}								\
		free((_res)->type##s);						\
	} while (0)

#define free_properties(_res, __res, type)					\
	do {									\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			drmModeFreeObjectProperties(res->type##s[i].props);	\
			free(res->type##s[i].props_info);			\
		}								\
	} while (0)

	if (res->res) {
		free_properties(res, res, crtc);

		free_resource(res, res, crtc, Crtc);
		free_resource(res, res, encoder, Encoder);

		for (i = 0; i < res->res->count_connectors; i++)
			free(res->connectors[i].name);

		free_resource(res, res, connector, Connector);
		free_resource(res, res, fb, FB);

		drmModeFreeResources(res->res);
	}

	if (res->plane_res) {
		free_properties(res, plane_res, plane);

		free_resource(res, plane_res, plane, Plane);

		drmModeFreePlaneResources(res->plane_res);
	}

	free(res);
}

static struct resources *get_resources(struct device *dev)
{
	struct resources *res;
	int i;

	res = (resources *) calloc(1, sizeof(*res));
	if (res == 0)
		return NULL;

	drmSetClientCap(dev->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	res->res = drmModeGetResources(dev->fd);
	if (!res->res) {
		fprintf(stderr, "drmModeGetResources failed: %s\n",
			strerror(errno));
		goto error;
	}

	res->crtcs = (crtc *) calloc(res->res->count_crtcs, sizeof(*res->crtcs));
	res->encoders = (encoder *) calloc(res->res->count_encoders, sizeof(*res->encoders));
	res->connectors = (connector *) calloc(res->res->count_connectors, sizeof(*res->connectors));
	res->fbs = (fb *) calloc(res->res->count_fbs, sizeof(*res->fbs));

	if (!res->crtcs || !res->encoders || !res->connectors || !res->fbs)
		goto error;

#define get_resource(_res, __res, type, Type)					\
	do {									\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			(_res)->type##s[i].type =				\
				drmModeGet##Type(dev->fd, (_res)->__res->type##s[i]); \
			if (!(_res)->type##s[i].type)				\
				fprintf(stderr, "could not get %s %i: %s\n",	\
					#type, (_res)->__res->type##s[i],	\
					strerror(errno));			\
		}								\
	} while (0)

	get_resource(res, res, crtc, Crtc);
	get_resource(res, res, encoder, Encoder);
	get_resource(res, res, connector, Connector);
	get_resource(res, res, fb, FB);

	/* Set the name of all connectors based on the type name and the per-type ID. */
	for (i = 0; i < res->res->count_connectors; i++) {
		struct connector *connector = &res->connectors[i];
		drmModeConnector *conn = connector->connector;

		asprintf(&connector->name, "%s-%u",
			 util_lookup_connector_type_name(conn->connector_type),
			 conn->connector_type_id);
	}

#define get_properties(_res, __res, type, Type)					\
	do {									\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			struct type *obj = &res->type##s[i];			\
			unsigned int j;						\
			obj->props =						\
				drmModeObjectGetProperties(dev->fd, obj->type->type##_id, \
							   DRM_MODE_OBJECT_##Type); \
			if (!obj->props) {					\
				fprintf(stderr,					\
					"could not get %s %i properties: %s\n", \
					#type, obj->type->type##_id,		\
					strerror(errno));			\
				continue;					\
			}							\
			obj->props_info = ( drmModePropertyRes **) calloc(obj->props->count_props, \
						 sizeof(*obj->props_info));	\
			if (!obj->props_info)					\
				continue;					\
			for (j = 0; j < obj->props->count_props; ++j)		\
				obj->props_info[j] =				\
					drmModeGetProperty(dev->fd, obj->props->props[j]); \
		}								\
	} while (0)

	get_properties(res, res, crtc, CRTC);
	get_properties(res, res, connector, CONNECTOR);

	for (i = 0; i < res->res->count_crtcs; ++i)
		res->crtcs[i].mode = &res->crtcs[i].crtc->mode;

	res->plane_res = drmModeGetPlaneResources(dev->fd);
	if (!res->plane_res) {
		fprintf(stderr, "drmModeGetPlaneResources failed: %s\n",
			strerror(errno));
		return res;
	}

	res->planes = (plane *) calloc(res->plane_res->count_planes, sizeof(*res->planes));
	if (!res->planes)
		goto error;

	get_resource(res, plane_res, plane, Plane);
	get_properties(res, plane_res, plane, PLANE);
        //printf("NUMBER OF PLANES = %d \n",res->plane_res->count_planes);
	return res;

error:
	free_resources(res);
	return NULL;
}

static int get_crtc_index(struct device *dev, uint32_t id)
{
	int i;

	for (i = 0; i < dev->resources->res->count_crtcs; ++i) {
		drmModeCrtc *crtc = dev->resources->crtcs[i].crtc;
		if (crtc && crtc->crtc_id == id)
			return i;
	}

	return -1;
}

static drmModeConnector *get_connector_by_name(struct device *dev, const char *name)
{
	struct connector *connector;
	int i;

	for (i = 0; i < dev->resources->res->count_connectors; i++) {
		connector = &dev->resources->connectors[i];

		if (strcmp(connector->name, name) == 0)
			return connector->connector;
	}

	return NULL;
}

static drmModeConnector *get_connector_by_id(struct device *dev, uint32_t id)
{
	drmModeConnector *connector;
	int i;

	for (i = 0; i < dev->resources->res->count_connectors; i++) {
		connector = dev->resources->connectors[i].connector;
		if (connector && connector->connector_id == id)
			return connector;
	}

	return NULL;
}

static drmModeEncoder *get_encoder_by_id(struct device *dev, uint32_t id)
{
	drmModeEncoder *encoder;
	int i;

	for (i = 0; i < dev->resources->res->count_encoders; i++) {
		encoder = dev->resources->encoders[i].encoder;
		if (encoder && encoder->encoder_id == id)
			return encoder;
	}

	return NULL;
}

/* -----------------------------------------------------------------------------
 * Pipes and planes
 */



static drmModeModeInfo *
connector_find_mode(struct device *dev, uint32_t con_id, const char *mode_str,
        const unsigned int vrefresh)
{
	drmModeConnector *connector;
	drmModeModeInfo *mode;
	int i;

	connector = get_connector_by_id(dev, con_id);
	if (!connector || !connector->count_modes)
		return NULL;

	for (i = 0; i < connector->count_modes; i++) {
		mode = &connector->modes[i];
		if (!strcmp(mode->name, mode_str)) {
			/* If the vertical refresh frequency is not specified then return the
			 * first mode that match with the name. Else, return the mode that match
			 * the name and the specified vertical refresh frequency.
			 */
			if (vrefresh == 0)
				return mode;
			else if (mode->vrefresh == vrefresh)
				return mode;
		}
	}

	return NULL;
}

static struct crtc *pipe_find_crtc(struct device *dev, struct pipe_arg *pipe)
{
	uint32_t possible_crtcs = ~0;
	uint32_t active_crtcs = 0;
	unsigned int crtc_idx;
	unsigned int i;
	int j;

	for (i = 0; i < pipe->num_cons; ++i) {
		uint32_t crtcs_for_connector = 0;
		drmModeConnector *connector;
		drmModeEncoder *encoder;
		int idx;

		connector = get_connector_by_id(dev, pipe->con_ids[i]);
		if (!connector)
			return NULL;

		for (j = 0; j < connector->count_encoders; ++j) {
			encoder = get_encoder_by_id(dev, connector->encoders[j]);
			if (!encoder)
				continue;

			crtcs_for_connector |= encoder->possible_crtcs;

			idx = get_crtc_index(dev, encoder->crtc_id);
			if (idx >= 0)
				active_crtcs |= 1 << idx;
		}

		possible_crtcs &= crtcs_for_connector;
	}

	if (!possible_crtcs)
		return NULL;

	/* Return the first possible and active CRTC if one exists, or the first
	 * possible CRTC otherwise.
	 */
	if (possible_crtcs & active_crtcs)
		crtc_idx = ffs(possible_crtcs & active_crtcs);
	else
		crtc_idx = ffs(possible_crtcs);

	return &dev->resources->crtcs[crtc_idx - 1];
}

static int pipe_find_crtc_and_mode(struct device *dev, struct pipe_arg *pipe)
{
	drmModeModeInfo *mode = NULL;
	int i;

	pipe->mode = NULL;

	for (i = 0; i < (int)pipe->num_cons; i++) {
		mode = connector_find_mode(dev, pipe->con_ids[i],
					   pipe->mode_str, pipe->vrefresh);
		if (mode == NULL) {
			fprintf(stderr,
				"failed to find mode \"%s\" for connector %s\n",
				pipe->mode_str, pipe->cons[i]);
			return -EINVAL;
		}
	}

	/* If the CRTC ID was specified, get the corresponding CRTC. Otherwise
	 * locate a CRTC that can be attached to all the connectors.
	 */
	if (pipe->crtc_id != (uint32_t)-1) {
		for (i = 0; i < dev->resources->res->count_crtcs; i++) {
			struct crtc *crtc = &dev->resources->crtcs[i];

			if (pipe->crtc_id == crtc->crtc->crtc_id) {
				pipe->crtc = crtc;
				break;
			}
		}
	} else {
		pipe->crtc = pipe_find_crtc(dev, pipe);
	}

	if (!pipe->crtc) {
		fprintf(stderr, "failed to find CRTC for pipe\n");
		return -EINVAL;
	}

	pipe->mode = mode;
	pipe->crtc->mode = mode;

	return 0;
}

static bool format_support(const drmModePlanePtr ovr, uint32_t fmt)
{
	unsigned int i;

	for (i = 0; i < ovr->count_formats; ++i) {
		if (ovr->formats[i] == fmt)
			return true;
	}

	return false;
}


static int set_plane(struct device *dev, struct plane_arg *p)
{
	drmModePlane *ovr;
	uint32_t plane_id;
	struct bo *plane_bo;
	uint32_t plane_flags = 0;
	struct crtc *crtc = NULL;
	unsigned int pipe;
	unsigned int i;

	/* Find an unused plane which can be connected to our CRTC. Find the
	 * CRTC index first, then iterate over available planes.
	 */
	for (i = 0; i < (unsigned int)dev->resources->res->count_crtcs; i++) {
		if (p->crtc_id == dev->resources->res->crtcs[i]) {
			crtc = &dev->resources->crtcs[i];
			pipe = i;
			break;
		}
	}

	if (!crtc) {
		fprintf(stderr, "CRTC %u not found\n", p->crtc_id);
		return -1;
	}

	plane_id = p->plane_id;

	for (i = 0; i < dev->resources->plane_res->count_planes; i++) {
		ovr = dev->resources->planes[i].plane;
		if (!ovr)
			continue;

		if (plane_id && plane_id != ovr->plane_id)
			continue;

		if (!format_support(ovr, p->fourcc))
			continue;

		if ((ovr->possible_crtcs & (1 << pipe)) && !ovr->crtc_id) {
			plane_id = ovr->plane_id;
			break;
		}
	}

	if (i == dev->resources->plane_res->count_planes) {
		fprintf(stderr, "no unused plane available for CRTC %u\n",
			crtc->crtc->crtc_id);
		return -1;
	}

	LOG_DEBUG("\nCalling drmModeSetPlane %d %d %d %d %d %d %d %d %d %d %d %d %d",
	         plane_id, crtc->crtc->crtc_id, p->fb_id,
	         plane_flags, p->x, p->y, p->w, p->h,
	         0, 0, p->w << 16, p->h << 16);

	/* note src coords (last 4 args) are in Q16 format */
	if (drmModeSetPlane(dev->fd, plane_id, crtc->crtc->crtc_id, p->fb_id,
			    plane_flags, p->x, p->y, p->w, p->h,
			    0, 0, p->w << 16, p->h << 16)) {
		fprintf(stderr, "failed to enable plane: %s\n",
			strerror(errno));
		LOG_ERROR(MSGID_DRMOMXCOMPONENT_INFO, 0, "failed to enable plane: %s\n",
		strerror(errno));
		return -1;
	}
	ovr->crtc_id = crtc->crtc->crtc_id;
	return plane_id;
}

static void clear_planes(struct device *dev, struct plane_arg *p, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO,0,"\n Destroy FB %d", p[i].fb_id);
		if (p[i].fb_id)
			drmModeRmFB(dev->fd, p[i].fb_id);
		if (p[i].bo) {
			bo_unmap(p[i].bo);
			bo_destroy(p[i].bo);
		}
	}
}

#define min(a, b)	((a) < (b) ? (a) : (b))

struct plane_arg p_plane_args[2];
static struct device dev;

bool initializeDrm() {
	int connectors = 0, crtcs = 0, planes = 0, framebuffers = 0;
	char *device = NULL;
	char *module = NULL;
	unsigned int plane_count = 2;

	memset(&dev, 0, sizeof dev);
	connectors = crtcs = planes = framebuffers = 1;

	dev.fd = util_open(device, module);
	if (dev.fd < 0)
		return false;

	dev.resources = get_resources(&dev);
	if (!dev.resources) {
		drmClose(dev.fd);
		return false;
	}
	memset(p_plane_args, 0, plane_count * sizeof(*p_plane_args));
	return true;
}

bool deinititalizeDrm() {
	LOG_INFO(MSGID_DRMOMXCOMPONENT_INFO,0,"\n Deinitialize DRM");
	clear_planes(&dev, p_plane_args, 2);
	free_resources(dev.resources);
	drmClose(dev.fd);
	return true;
}

std::pair<void**, void**> createFrameBuffer(unsigned int w, unsigned int h, unsigned int format, unsigned int planeId) {
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
	uint32_t plane_flags = 0;
	unsigned int plane_count = 2;
	memset(p_plane_args, 0, plane_count * sizeof(*p_plane_args));
	for (int i = 0; i < plane_count; i++) {
		p_plane_args[i].crtc_id = 43;
		p_plane_args[i].w = w;
		p_plane_args[i].h = h;
		p_plane_args[i].plane_id = planeId;
		if (format == 20) {
			strcpy(p_plane_args[i].format_str, "YU12");
			p_plane_args[i].fourcc = DRM_FORMAT_YUV420;
		} else {
			LOG_ERROR(MSGID_DRMOMXCOMPONENT_INFO, 0, "\n Unsupported format %d", format);
		}
		p_plane_args[i].bo = bo_create(dev.fd, p_plane_args[i].fourcc, p_plane_args[i].w, p_plane_args[i].h, handles,
							pitches, offsets);
		if (p_plane_args[i].bo == NULL)
			return std::make_pair(nullptr, nullptr);

		/* just use single plane format for now.. */
		if (drmModeAddFB2(dev.fd, p_plane_args[i].w, p_plane_args[i].h, p_plane_args[i].fourcc,
						  handles, pitches, offsets, &p_plane_args[i].fb_id, plane_flags)) {
			LOG_ERROR(MSGID_DRMOMXCOMPONENT_INFO, 0, "failed to add fb: %s\n", strerror(errno));
			return std::make_pair(nullptr, nullptr);
		}
		LOG_ERROR(MSGID_DRMOMXCOMPONENT_INFO, 0,
				  "\n plane data = %d %p %p %d \n", p_plane_args[i].fb_id, p_plane_args[i].bo->planes[0], p_plane_args[i].bo, p_plane_args[i].bo->pitch);
	}

	p_plane_args[0].plane_id = p_plane_args[1].plane_id = set_plane(&dev, &p_plane_args[0]);
	if (p_plane_args[0].plane_id == -1) {
		LOG_ERROR(MSGID_DRMOMXCOMPONENT_INFO, 0, "\n ERROR: Set Plane failed %d");
		return std::make_pair(nullptr, nullptr);
	}

	return std::make_pair( p_plane_args[0].bo->planes, p_plane_args[1].bo->planes );
}

int connectFBtoPlane(unsigned int fb_index) {
	return drmModeObjectSetProperty(dev.fd, p_plane_args[fb_index].plane_id, DRM_MODE_OBJECT_PLANE, 0xff01, p_plane_args[fb_index].fb_id);
}

bool setScale(uint32_t planeId, uint32_t src_x, uint32_t src_y, uint32_t src_w, uint32_t src_h,
			  uint32_t crtc_x, uint32_t crtc_y, uint32_t crtc_w, uint32_t crtc_h) {
	LOG_ERROR(MSGID_DRMOMXCOMPONENT_INFO,0,"connectFBtoPlane planeId %d p_plane_args[0].plane_id %d p_plane_args[0].fb_id %d",planeId, p_plane_args[0].plane_id,p_plane_args[0].fb_id);
	if (drmModeSetPlane(dev.fd, p_plane_args[0].plane_id, 43, p_plane_args[0].fb_id,
			    0, crtc_x, crtc_y, crtc_w, crtc_h,
			    0, 0, src_w << 16, src_h << 16)) {
		fprintf(stderr, "failed to set scale: %s\n",
			strerror(errno));
		LOG_ERROR (MSGID_DRMOMXCOMPONENT_INFO, 0,"failed to set scale for plane: %s\n",
		strerror(errno));
		return -1;
	}
	return false;
}

typedef struct {
	/* Signed dest location allows it to be partially off screen */
	int32_t crtc_x, crtc_y;
	uint32_t crtc_w, crtc_h;

	/* Source values are 16.16 fixed point */
	uint32_t src_x, src_y;
	uint32_t src_h, src_w;
} scale_param_t;

scale_param_t scale_param;

bool setSourceWindow(uint32_t planeId, uint32_t src_x, uint32_t src_y, uint32_t src_w, uint32_t src_h) {
	//  LOG_ERROR(MSGID_DRMOMXCOMPONENT_INFO,0,"setSourceWindow planeId %d p_plane_args[0].plane_id %d p_plane_args[0].fb_id %d",planeId, p_plane_args[0].plane_id,p_plane_args[0].fb_id);
		scale_param.src_x = src_x;
		scale_param.src_y = src_y;
		scale_param.src_w = src_w;
		scale_param.src_h = src_h;
		/* This property ignores destination rectangle values */
		if (drmModeObjectSetProperty(dev.fd, p_plane_args[0].plane_id, DRM_MODE_OBJECT_PLANE, 0xff04, (uint64_t)&scale_param)) {
			LOG_INFO(MSGID_COMXCOMPONENT_INFO,0,"\n Unable to set plane %d", planeId);
			return false;
		}
	return true;
}

typedef struct ifb_rectangle {
	/* Source values are 16.16 fixed point */
	uint32_t src_x, src_y;
	uint32_t src_w, src_h;
	uint32_t fb_id;  // Optional needed onl for 0xff05
} ifb_rect_t;

ifb_rect_t ifb_rect;

int connectFBtoPlaneWithSrcRect(uint32_t fb_index, uint32_t src_x, uint32_t src_y, uint32_t src_w, uint32_t src_h) {
	ifb_rect.src_x = src_x;
	ifb_rect.src_y = src_y;
	ifb_rect.src_w = src_w;
	ifb_rect.src_h = src_h;
	ifb_rect.fb_id = p_plane_args[fb_index].fb_id;
	return drmModeObjectSetProperty(dev.fd, p_plane_args[fb_index].plane_id, DRM_MODE_OBJECT_PLANE, 0xff05, (uint64_t)&ifb_rect);
}
