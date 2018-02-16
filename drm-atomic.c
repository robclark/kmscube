/*
 * Copyright (c) 2017 Rob Clark <rclark@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "drm-common.h"

#define VOID2U64(x) ((uint64_t)(unsigned long)(x))

static struct drm drm = {
	.kms_out_fence_fd = -1,
};

static int add_connector_property(drmModeAtomicReq *req, struct connector *obj,
					const char *name, uint64_t value)
{
	unsigned int i;
	int prop_id = 0;

	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id < 0) {
		printf("no connector property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj->connector->connector_id,
			prop_id, value);
}

static int add_crtc_property(drmModeAtomicReq *req, struct crtc *obj,
				const char *name, uint64_t value)
{
	unsigned int i;
	int prop_id = -1;

	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id < 0) {
		printf("no crtc property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj->crtc->crtc_id,
			prop_id, value);
}

static int add_plane_property(drmModeAtomicReq *req, struct plane *obj,
				const char *name, uint64_t value)
{
	unsigned int i;
	int prop_id = -1;

	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}


	if (prop_id < 0) {
		printf("no plane property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj->plane->plane_id,
			prop_id, value);
}

static int drm_atomic_commit(uint32_t fb_id, uint32_t flags)
{
	drmModeAtomicReq *req;
	uint32_t blob_id;
	int ret;

	req = drmModeAtomicAlloc();

	if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) {
		if (add_connector_property(req, drm.connector, "CRTC_ID",
						drm.crtc_id) < 0)
				return -1;

		if (drmModeCreatePropertyBlob(drm.fd, drm.mode, sizeof(*drm.mode),
					      &blob_id) != 0)
			return -1;

		if (add_crtc_property(req, drm.crtc, "MODE_ID", blob_id) < 0)
			return -1;

		if (add_crtc_property(req, drm.crtc, "ACTIVE", 1) < 0)
			return -1;

		if (drm.wb_connector) {
			uint32_t wb_crtc_id = drm.wb_crtc->crtc->crtc_id;

			if (add_connector_property(req, drm.wb_connector,
					"CRTC_ID", wb_crtc_id) < 0)
				return -1;

			if (add_crtc_property(req, drm.wb_crtc, "MODE_ID", blob_id) < 0)
				return -1;

			if (add_crtc_property(req, drm.wb_crtc, "ACTIVE", 1) < 0)
				return -1;
		}
	}

	add_plane_property(req, drm.plane, "FB_ID", fb_id);
	add_plane_property(req, drm.plane, "CRTC_ID", drm.crtc_id);
	add_plane_property(req, drm.plane, "SRC_X", 0);
	add_plane_property(req, drm.plane, "SRC_Y", 0);
	add_plane_property(req, drm.plane, "SRC_W", drm.mode->hdisplay << 16);
	add_plane_property(req, drm.plane, "SRC_H", drm.mode->vdisplay << 16);
	add_plane_property(req, drm.plane, "CRTC_X", 0);
	add_plane_property(req, drm.plane, "CRTC_Y", 0);
	add_plane_property(req, drm.plane, "CRTC_W", drm.mode->hdisplay);
	add_plane_property(req, drm.plane, "CRTC_H", drm.mode->vdisplay);

	if (drm.kms_in_fence_fd != -1) {
		add_crtc_property(req, drm.crtc, "OUT_FENCE_PTR",
				VOID2U64(&drm.kms_out_fence_fd));
		add_plane_property(req, drm.plane, "IN_FENCE_FD", drm.kms_in_fence_fd);
	}

	if (drm.wb_connector) {
		uint32_t wb_crtc_id = drm.wb_crtc->crtc->crtc_id;

		add_plane_property(req, drm.wb_plane, "FB_ID", fb_id);
		add_plane_property(req, drm.wb_plane, "CRTC_ID", wb_crtc_id);
		add_plane_property(req, drm.wb_plane, "SRC_X", 0);
		add_plane_property(req, drm.wb_plane, "SRC_Y", 0);
		add_plane_property(req, drm.wb_plane, "SRC_W", drm.mode->hdisplay << 16);
		add_plane_property(req, drm.wb_plane, "SRC_H", drm.mode->vdisplay << 16);
		add_plane_property(req, drm.wb_plane, "CRTC_X", 0);
		add_plane_property(req, drm.wb_plane, "CRTC_Y", 0);
		add_plane_property(req, drm.wb_plane, "CRTC_W", drm.mode->hdisplay);
		add_plane_property(req, drm.wb_plane, "CRTC_H", drm.mode->vdisplay);

		// TODO allocate a real writeback buffer
		add_connector_property(req, drm.wb_connector, "WRITEBACK_FB_ID", fb_id);

		if (drm.kms_in_fence_fd) {
			// TODO writeback connector out-fence
			add_plane_property(req, drm.wb_plane, "IN_FENCE_FD",
					drm.kms_in_fence_fd);
		}
	}

	ret = drmModeAtomicCommit(drm.fd, req, flags, NULL);
	if (ret)
		goto out;

	if (drm.kms_in_fence_fd != -1) {
		close(drm.kms_in_fence_fd);
		drm.kms_in_fence_fd = -1;
	}

out:
	drmModeAtomicFree(req);

	return ret;
}

static EGLSyncKHR create_fence(const struct egl *egl, int fd)
{
	EGLint attrib_list[] = {
		EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd,
		EGL_NONE,
	};
	EGLSyncKHR fence = egl->eglCreateSyncKHR(egl->display,
			EGL_SYNC_NATIVE_FENCE_ANDROID, attrib_list);
	assert(fence);
	return fence;
}

static int atomic_run(const struct gbm *gbm, const struct egl *egl)
{
	struct gbm_bo *bo = NULL;
	struct drm_fb *fb;
	uint32_t i = 0;
	uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;
	int ret;

	if (egl_check(egl, eglDupNativeFenceFDANDROID) ||
	    egl_check(egl, eglCreateSyncKHR) ||
	    egl_check(egl, eglDestroySyncKHR) ||
	    egl_check(egl, eglWaitSyncKHR) ||
	    egl_check(egl, eglClientWaitSyncKHR))
		return -1;

	/* Allow a modeset change for the first commit only. */
	flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

	while (1) {
		struct gbm_bo *next_bo;
		EGLSyncKHR gpu_fence = NULL;   /* out-fence from gpu, in-fence to kms */
		EGLSyncKHR kms_fence = NULL;   /* in-fence to gpu, out-fence from kms */

		if (drm.kms_out_fence_fd != -1) {
			kms_fence = create_fence(egl, drm.kms_out_fence_fd);
			assert(kms_fence);

			/* driver now has ownership of the fence fd: */
			drm.kms_out_fence_fd = -1;

			/* wait "on the gpu" (ie. this won't necessarily block, but
			 * will block the rendering until fence is signaled), until
			 * the previous pageflip completes so we don't render into
			 * the buffer that is still on screen.
			 */
			egl->eglWaitSyncKHR(egl->display, kms_fence, 0);
		}

		egl->draw(i++);

		/* insert fence to be singled in cmdstream.. this fence will be
		 * signaled when gpu rendering done
		 */
		gpu_fence = create_fence(egl, EGL_NO_NATIVE_FENCE_FD_ANDROID);
		assert(gpu_fence);

		eglSwapBuffers(egl->display, egl->surface);

		/* after swapbuffers, gpu_fence should be flushed, so safe
		 * to get fd:
		 */
		drm.kms_in_fence_fd = egl->eglDupNativeFenceFDANDROID(egl->display, gpu_fence);
		egl->eglDestroySyncKHR(egl->display, gpu_fence);
		assert(drm.kms_in_fence_fd != -1);

		next_bo = gbm_surface_lock_front_buffer(gbm->surface);
		if (!next_bo) {
			printf("Failed to lock frontbuffer\n");
			return -1;
		}
		fb = drm_fb_get_from_bo(next_bo);
		if (!fb) {
			printf("Failed to get a new framebuffer BO\n");
			return -1;
		}

		if (kms_fence) {
			EGLint status;

			/* Wait on the CPU side for the _previous_ commit to
			 * complete before we post the flip through KMS, as
			 * atomic will reject the commit if we post a new one
			 * whilst the previous one is still pending.
			 */
			do {
				status = egl->eglClientWaitSyncKHR(egl->display,
								   kms_fence,
								   0,
								   EGL_FOREVER_KHR);
			} while (status != EGL_CONDITION_SATISFIED_KHR);

			egl->eglDestroySyncKHR(egl->display, kms_fence);
		}

		/*
		 * Here you could also update drm plane layers if you want
		 * hw composition
		 */
		ret = drm_atomic_commit(fb->fb_id, flags);
		if (ret) {
			printf("failed to commit: %s\n", strerror(errno));
			return -1;
		}

		/* release last buffer to render on again: */
		if (bo)
			gbm_surface_release_buffer(gbm->surface, bo);
		bo = next_bo;

		/* Allow a modeset change for the first commit only. */
		flags &= ~(DRM_MODE_ATOMIC_ALLOW_MODESET);
	}

	return ret;
}

static int get_plane_type(uint32_t plane_id)
{
	int type = -1;
	drmModeObjectPropertiesPtr props =
		drmModeObjectGetProperties(drm.fd, plane_id, DRM_MODE_OBJECT_PLANE);

	for (unsigned i = 0; (i < props->count_props) && (type == -1); i++) {
		drmModePropertyPtr p =
			drmModeGetProperty(drm.fd, props->props[i]);

		if (strcmp(p->name, "type") == 0)
			type = props->prop_values[i];

		drmModeFreeProperty(p);
	}

	drmModeFreeObjectProperties(props);

	return type;
}

/* Pick a plane.. something that at a minimum can be connected to
 * the chosen crtc, but prefer primary plane.
 *
 * Seems like there is some room for a drmModeObjectGetNamedProperty()
 * type helper in libdrm..
 */
static int get_plane_id(void)
{
	drmModePlaneResPtr plane_resources;
	uint32_t i;
	int ret = -EINVAL;
	bool found_primary = false;

	plane_resources = drmModeGetPlaneResources(drm.fd);
	if (!plane_resources) {
		printf("drmModeGetPlaneResources failed: %s\n", strerror(errno));
		return -1;
	}

	for (i = 0; (i < plane_resources->count_planes) && !found_primary; i++) {
		uint32_t id = plane_resources->planes[i];
		drmModePlanePtr plane = drmModeGetPlane(drm.fd, id);
		if (!plane) {
			printf("drmModeGetPlane(%u) failed: %s\n", id, strerror(errno));
			continue;
		}

		if (plane->possible_crtcs & (1 << drm.crtc_index)) {
			/* primary or not, this plane is good enough to use: */
			ret = id;

			if (get_plane_type(id) == DRM_PLANE_TYPE_PRIMARY)
				found_primary = true;
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_resources);

	return ret;
}

const struct drm * init_drm_atomic(const char *device, bool writeback)
{
	uint32_t plane_id;
	int ret;

	ret = init_drm(&drm, device);
	if (ret)
		return NULL;

	ret = drmSetClientCap(drm.fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		printf("no atomic modesetting support: %s\n", strerror(errno));
		return NULL;
	}

	ret = get_plane_id();
	if (!ret) {
		printf("could not find a suitable plane\n");
		return NULL;
	} else {
		plane_id = ret;
	}

	/* We only do single plane to single crtc to single connector, no
	 * fancy multi-monitor or multi-plane stuff.  So just grab the
	 * plane/crtc/connector property info for one of each:
	 */
	drm.plane = calloc(1, sizeof(*drm.plane));
	drm.crtc = calloc(1, sizeof(*drm.crtc));
	drm.connector = calloc(1, sizeof(*drm.connector));

#define get_resource(var, type, Type, id) do { 					\
		(var)->type = drmModeGet##Type(drm.fd, id);			\
		if (!(var)->type) {						\
			printf("could not get %s %i: %s\n",			\
					#type, id, strerror(errno));		\
			return NULL;						\
		}								\
	} while (0)

	get_resource(drm.plane, plane, Plane, plane_id);
	get_resource(drm.crtc, crtc, Crtc, drm.crtc_id);
	get_resource(drm.connector, connector, Connector, drm.connector_id);

#define get_properties(var, type, TYPE, id) do {				\
		uint32_t i;							\
		(var)->props = drmModeObjectGetProperties(drm.fd,		\
				id, DRM_MODE_OBJECT_##TYPE);			\
		if (!(var)->props) {						\
			printf("could not get %s %u properties: %s\n", 		\
					#type, id, strerror(errno));		\
			return NULL;						\
		}								\
		(var)->props_info = calloc((var)->props->count_props,		\
				sizeof((var)->props_info));			\
		for (i = 0; i < (var)->props->count_props; i++) {		\
			(var)->props_info[i] = drmModeGetProperty(drm.fd,	\
					(var)->props->props[i]);		\
		}								\
	} while (0)

	get_properties(drm.plane, plane, PLANE, plane_id);
	get_properties(drm.crtc, crtc, CRTC, drm.crtc_id);
	get_properties(drm.connector, connector, CONNECTOR, drm.connector_id);

	if (writeback) {
		drmModeRes *resources;
		drmModePlaneResPtr plane_resources;
		drmModeConnector *connector = NULL;
		uint32_t wb_crtc_id, wb_plane_id = 0;
		int wb_crtc_index;

		resources = drmModeGetResources(drm.fd);
		if (!resources) {
			printf("drmModeGetResources failed: %s\n", strerror(errno));
			return NULL;
		}

// XXX hack to avoid libdrm patch
#define DRM_MODE_CONNECTOR_WRITEBACK	18

		/* find a writeback connector: */
		for (int i = 0; i < resources->count_connectors; i++) {
			connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
			if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
				/* found one, let's use this! */
				break;
			}
			drmModeFreeConnector(connector);
			connector = NULL;
		}

		if (!connector) {
			printf("no writeback connector found!\n");
			return NULL;
		}

		/* let's find a CRTC that we can use with this connector: */
		wb_crtc_id = find_crtc_for_connector(&drm, resources, connector);
		if (!wb_crtc_id) {
			printf("no crtc for writeback found!\n");
			return NULL;
		}

		wb_crtc_index = find_crtc_index(resources, wb_crtc_id);

		/* and now a plane to use with wb connector/crtc, which isn't
		 * already in use:
		 */
		plane_resources = drmModeGetPlaneResources(drm.fd);
		if (!plane_resources) {
			printf("drmModeGetPlaneResources failed: %s\n", strerror(errno));
			return NULL;
		}

		for (unsigned i = 0; (i < plane_resources->count_planes) && !wb_plane_id; i++) {
			uint32_t id = plane_resources->planes[i];
			drmModePlanePtr plane = drmModeGetPlane(drm.fd, id);
			if (!plane) {
				printf("drmModeGetPlane(%u) failed: %s\n", id, strerror(errno));
				continue;
			}

			/* look for compatible plane, which isn't the one used for display: */
			if ((plane->possible_crtcs & (1 << wb_crtc_index)) &&
			    (id != plane_id)) {
				int type = get_plane_type(id);
				if ((type == DRM_PLANE_TYPE_PRIMARY) ||
				    (type == DRM_PLANE_TYPE_OVERLAY))
					wb_plane_id = id;
			}

			drmModeFreePlane(plane);
		}

		drmModeFreePlaneResources(plane_resources);

		if (!wb_plane_id) {
			printf("could not find plane for writeback!\n");
			return NULL;
		}

		/* ok, good to go! */
		drm.wb_plane = calloc(1, sizeof(*drm.wb_plane));
		drm.wb_crtc = calloc(1, sizeof(*drm.wb_crtc));
		drm.wb_connector = calloc(1, sizeof(*drm.wb_connector));

		get_resource(drm.wb_plane, plane, Plane, wb_plane_id);
		get_resource(drm.wb_crtc, crtc, Crtc, wb_crtc_id);
		get_resource(drm.wb_connector, connector, Connector,
				connector->connector_id);

		get_properties(drm.wb_plane, plane, PLANE, wb_plane_id);
		get_properties(drm.wb_crtc, crtc, CRTC, wb_crtc_id);
		get_properties(drm.wb_connector, connector, CONNECTOR,
				connector->connector_id);
	}

	drm.run = atomic_run;

	return &drm;
}
