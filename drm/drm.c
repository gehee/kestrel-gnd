#include "drm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <pthread.h>
#include <assert.h>
#include <signal.h>

#define OSD_PLANE_FMT DRM_FORMAT_ARGB8888
#define VIDEO_PLANE_FMT DRM_FORMAT_NV12

// kestrel-splash holds DRM master until it gets our SIGUSR1 (sent by
// S70kestrel before it starts us), at which point it drops master but keeps
// its last frame on screen rather than exiting outright - our own modeset
// needs that master, and exiting would free its framebuffers, which is what
// used to blank the screen for however long our own setup took. Once our
// first modeset actually lands, the hand-off is done and it's safe for the
// splash to go; this is what tells it so, right at that moment, so nothing
// is ever on screen but one of the two full frames.
static void notify_splash_done(void)
{
    FILE* f = fopen("/var/run/kestrel-splash.pid", "r");
    if (!f) return;
    int pid = 0;
    int ok = fscanf(f, "%d", &pid);
    fclose(f);
    if (ok == 1 && pid > 0) kill(pid, SIGTERM);
    remove("/var/run/kestrel-splash.pid");
}

int modeset_open(int *out, const char *node)
{
	int fd, ret;
	uint64_t cap;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "cannot open '%s': %m\n", node);
		return ret;
	}

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret) {
		fprintf(stderr, "failed to set universal planes cap, %d\n", ret);
		return ret;
	}

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		fprintf(stderr, "failed to set atomic cap, %d", ret);
		return ret;
	}

	/* Writeback connectors are hidden from clients that do not ask for them.
	   Needed to capture the composited output (video plane + OSD plane) for
	   screen recording. Not fatal if the kernel does not support it. */
#ifdef DRM_CLIENT_CAP_WRITEBACK_CONNECTORS
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1))
		fprintf(stderr, "note: writeback connectors unavailable\n");
#endif

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) < 0 || !cap) {
		fprintf(stderr, "drm device '%s' does not support dumb buffers\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

	if (drmGetCap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) < 0 || !cap) {
		fprintf(stderr, "drm device '%s' does not support atomic KMS\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

    // Attempt to set master
    ret = drmSetMaster(fd);
    if (ret < 0) {
        fprintf(stderr, "Warning: Failed to set DRM master on '%s': %s (Check if another display server is running)\n", node, strerror(errno));
        // We don't return error here because we might already be master (if we are the only opener), 
        // or we might want to continue and fail later at commit time if strictly necessary.
        // But usually for atomic modeset we need to be master.
    }

	*out = fd;
	return 0;
}


int64_t get_property_value(int fd, drmModeObjectPropertiesPtr props,
				  const char *name)
{
	drmModePropertyPtr prop;
	uint64_t value;
	bool found;
	int j;

	found = false;
	for (j = 0; j < props->count_props && !found; j++) {
		prop = drmModeGetProperty(fd, props->props[j]);
		if (!strcmp(prop->name, name)) {
			value = props->prop_values[j];
			found = true;
		}
		drmModeFreeProperty(prop);
	}

	if (!found)
		return -1;
	return value;
}


void modeset_get_object_properties(int fd, struct drm_object *obj,
					  uint32_t type)
{
	const char *type_str;
	unsigned int i;

	obj->props = drmModeObjectGetProperties(fd, obj->id, type);
	if (!obj->props) {
		switch(type) {
			case DRM_MODE_OBJECT_CONNECTOR:
				type_str = "connector";
				break;
			case DRM_MODE_OBJECT_PLANE:
				type_str = "plane";
				break;
			case DRM_MODE_OBJECT_CRTC:
				type_str = "CRTC";
				break;
			default:
				type_str = "unknown type";
				break;
		}
		fprintf(stderr, "cannot get %s %d properties: %s\n",
			type_str, obj->id, strerror(errno));
		return;
	}

	obj->props_info = calloc(obj->props->count_props, sizeof(obj->props_info));
	for (i = 0; i < obj->props->count_props; i++)
		obj->props_info[i] = drmModeGetProperty(fd, obj->props->props[i]);
}

int set_drm_object_property(drmModeAtomicReq *req, struct drm_object *obj,
				   const char *name, uint64_t value)
{
	uint32_t prop_id = 0;
	if (obj->props) {
		for (int i = 0; i < obj->props->count_props; i++) {
			if (!strcmp(obj->props_info[i]->name, name)) {
                // If it's immutable, we can't change it via atomic request.
                if (obj->props_info[i]->flags & DRM_MODE_PROP_IMMUTABLE) {
                    return 0;
                }
				prop_id = obj->props_info[i]->prop_id;
				break;
			}
		}
	}

	if (prop_id == 0) {
		// Only warn for non-critical properties
		if (strcmp(name, "zpos") != 0 && strcmp(name, "alpha") != 0 && strcmp(name, "pixel blend mode") != 0 && strcmp(name, "VRR_ENABLED") != 0) {
			fprintf(stderr, "Property '%s' not found on object %u\n", name, obj->id);
			return -1;
		}
		return 0; // Ignore missing optional properties
	}
	
	int ret = drmModeAtomicAddProperty(req, obj->id, prop_id, value);
    if (ret < 0) {
        fprintf(stderr, "drmModeAtomicAddProperty failed for %s on %u: %d\n", name, obj->id, ret);
    }
    return ret;
}

void dump_obj_props(int fd, struct drm_object *obj, const char* label) {
    fprintf(stderr, "Properties for %s (id %u):\n", label, obj->id);
    for (uint32_t i = 0; i < obj->props->count_props; i++) {
        fprintf(stderr, "  %s: %lu\n", obj->props_info[i]->name, (unsigned long)obj->props->prop_values[i]);
    }
}


int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn, struct modeset_output *out)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	uint32_t crtc;

	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id) {
			crtc = enc->crtc_id;
			if (crtc > 0) {
				drmModeFreeEncoder(enc);
				out->crtc.id = crtc;
				out->saved_crtc = drmModeGetCrtc(fd, crtc);
				for (i = 0; i < res->count_crtcs; ++i) {
					if (res->crtcs[i] == crtc) {
						out->crtc_index = i;
						break;
					}
				}
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n",
				i, conn->encoders[i], errno);
			continue;
		}

		for (j = 0; j < res->count_crtcs; ++j) {
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			crtc = res->crtcs[j];

			if (crtc > 0) {
				out->saved_crtc = drmModeGetCrtc(fd, crtc);
				fprintf(stdout, "crtc %u found for encoder %u, will need full modeset\n",
					crtc, conn->encoders[i]);;
				drmModeFreeEncoder(enc);
				out->crtc.id = crtc;
				out->crtc_index = j;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr, "cannot find suitable crtc for connector %u\n",
		conn->connector_id);
	return -ENOENT;
}

const char* drm_fourcc_to_string(uint32_t fourcc) {
    char* result = malloc(5);
    result[0] = (char)((fourcc >> 0) & 0xFF);
    result[1] = (char)((fourcc >> 8) & 0xFF);
    result[2] = (char)((fourcc >> 16) & 0xFF);
    result[3] = (char)((fourcc >> 24) & 0xFF);
    result[4] = '\0';
    return result;
}

int modeset_find_plane(int fd, struct modeset_output *out, struct drm_object *plane_out, uint32_t plane_format, int exclude_plane_id)
{
	drmModePlaneResPtr plane_res;
	bool found_plane = false;
	int i, ret = -EINVAL;

	plane_res = drmModeGetPlaneResources(fd);
	if (!plane_res) {
		fprintf(stderr, "drmModeGetPlaneResources failed: %s\n", strerror(errno));
		return -ENOENT;
	}

	for (i = 0; (i < plane_res->count_planes) && !found_plane; i++) {
		uint32_t plane_id = plane_res->planes[i];

		if (plane_id == exclude_plane_id) {
			continue;
		}

		drmModePlanePtr plane = drmModeGetPlane(fd, plane_id);
		if (!plane) {
			fprintf(stderr, "drmModeGetPlane(%u) failed: %s\n", plane_id, strerror(errno));
			continue;
		}

		// Get plane properties
		drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
		if (!props) {
			drmModeFreePlane(plane);
			continue;
		}

		bool is_cursor = false;
		for (uint32_t j = 0; j < props->count_props; j++) {
			drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[j]);
			if (!prop)
				continue;

			if (strcmp(prop->name, "type") == 0) {
				uint64_t type_val = props->prop_values[j];
				if (type_val == DRM_PLANE_TYPE_CURSOR) {
					is_cursor = true;
				}
				drmModeFreeProperty(prop);
				break;
			}

			drmModeFreeProperty(prop);
		}

		drmModeFreeObjectProperties(props);

// 		if (is_cursor) {
// 			drmModeFreePlane(plane);
// 			continue;
// 		}

		// CRTC compatibility and format support
		if (plane->possible_crtcs & (1 << out->crtc_index)) {
            // Get plane type to prefer primary/overlay as needed
            uint64_t type_val = 0;
            drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
            if (props) {
                type_val = get_property_value(fd, props, "type");
                drmModeFreeObjectProperties(props);
            }

			for (int j = 0; j < plane->count_formats; j++) {
				if (plane->formats[j] == plane_format) {
                    // Bias: if we are looking for a video plane, and we find a primary, great.
                    // If we find an overlay, also fine.
					plane_out->id = plane_id;
					found_plane = true;
					ret = 0;
                    
                    // If it's a primary plane, we definitely want it (helps with modeset)
                    if (type_val == DRM_PLANE_TYPE_PRIMARY) break; 
				}
			}
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);

	if (ret != 0 && exclude_plane_id == -1) {
		fprintf(stderr, "DEBUG: Failed to find plane for format %c%c%c%c (0x%08x) on CRTC index %d. Dumping available planes:\n",
			(plane_format)&0xff, (plane_format>>8)&0xff, (plane_format>>16)&0xff, (plane_format>>24)&0xff, plane_format, out->crtc_index);
		
		plane_res = drmModeGetPlaneResources(fd);
		if (plane_res) {
			for (i = 0; i < plane_res->count_planes; i++) {
				drmModePlanePtr plane = drmModeGetPlane(fd, plane_res->planes[i]);
				if (!plane) continue;
                
                uint64_t type = 99;
                drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
                if(props) {
                    for(uint32_t k=0; k<props->count_props; k++) {
                        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[k]);
                        if(prop && strcmp(prop->name, "type")==0) type = props->prop_values[k];
                        if(prop) drmModeFreeProperty(prop);
                    }
                    drmModeFreeObjectProperties(props);
                }

				fprintf(stderr, "  Plane %u: Type %lu, CRTCs mask 0x%x (Target Mask 0x%x)\n", plane->plane_id, type, plane->possible_crtcs, (1 << out->crtc_index));
				if (plane->possible_crtcs & (1 << out->crtc_index)) {
					fprintf(stderr, "    (Compatible with CRTC) Formats (%d): ", plane->count_formats);
					for (uint32_t j=0; j<plane->count_formats; j++) {
						uint32_t fmt = plane->formats[j];
						fprintf(stderr, "%c%c%c%c ", (fmt)&0xff, (fmt>>8)&0xff, (fmt>>16)&0xff, (fmt>>24)&0xff);
					}
					fprintf(stderr, "\n");
				}
				drmModeFreePlane(plane);
			}
			drmModeFreePlaneResources(plane_res);
		}
	}

	return ret;
}


void modeset_drm_object_fini(struct drm_object *obj)
{
	for (int i = 0; i < obj->props->count_props; i++)
		drmModeFreeProperty(obj->props_info[i]);
	free(obj->props_info);
	drmModeFreeObjectProperties(obj->props);
}


int modeset_setup_objects(int fd, struct modeset_output *out)
{
	struct drm_object *connector = &out->connector;
	struct drm_object *crtc = &out->crtc;
	struct drm_object *plane_video = &out->video_plane;
	struct drm_object *plane_osd = &out->osd_plane;

	modeset_get_object_properties(fd, connector, DRM_MODE_OBJECT_CONNECTOR);
	if (!connector->props)
		goto out_conn;

    for (uint32_t i = 0; i < connector->props->count_props; i++) {
        if (!strcmp(connector->props_info[i]->name, "vrr_capable")) {
            printf("DRM: Monitor reports vrr_capable = %lu\n", (unsigned long)connector->props->prop_values[i]);
        }
        if (!strcmp(connector->props_info[i]->name, "vrr_range")) {
            printf("DRM: Monitor reports vrr_range property exists\n");
        }
    }

	modeset_get_object_properties(fd, crtc, DRM_MODE_OBJECT_CRTC);
	if (!crtc->props)
		goto out_crtc;
    
    for (uint32_t i = 0; i < crtc->props->count_props; i++) {
        if (!strcmp(crtc->props_info[i]->name, "VRR_ENABLED")) {
            printf("DRM: Variable Refresh Rate (VRR) support detected on CRTC %u\n", crtc->id);
            break;
        }
    }

	modeset_get_object_properties(fd, plane_video, DRM_MODE_OBJECT_PLANE);
	if (!plane_video->props)
		goto out_plane;
	modeset_get_object_properties(fd, plane_osd, DRM_MODE_OBJECT_PLANE);
	if (!plane_osd->props)
		goto out_plane;
	return 0;

out_plane:
	modeset_drm_object_fini(crtc);
out_crtc:
	modeset_drm_object_fini(connector);
out_conn:
	return -ENOMEM;
}


void modeset_destroy_objects(int fd, struct modeset_output *out)
{
	modeset_drm_object_fini(&out->connector);
	modeset_drm_object_fini(&out->crtc);
	modeset_drm_object_fini(&out->video_plane);
	modeset_drm_object_fini(&out->osd_plane);
}


int modeset_create_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_create_dumb creq = {0};;
	struct drm_mode_destroy_dumb dreq = {0};;
	struct drm_mode_map_dumb mreq = {0};;
	int ret;

	creq.width = buf->width;
	creq.height = buf->height;
	creq.bpp = 32; // ARGB8888 is 32 bits per pixel
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		fprintf(stderr, "cannot create buffer (%d): %m\n", errno);
		return -errno;
	}
	buf->stride = creq.pitch;
	buf->size = creq.size;
	buf->handle = creq.handle;

    // Create framebuffer
    uint32_t handles[4] = {buf->handle, 0, 0, 0};
    uint32_t pitches[4] = {buf->stride, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};

	ret = drmModeAddFB2(fd, buf->width, buf->height, OSD_PLANE_FMT, handles, pitches, offsets, &buf->fb, 0);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer (%d): %m\n", errno);
		ret = -errno;
		goto err_destroy;
	}

	mreq.handle = buf->handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		fprintf(stderr, "cannot map buffer (%d): %m\n", errno);
		ret = -errno;
		goto err_fb;
	}

	buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,  fd, mreq.offset);
	if (buf->map == MAP_FAILED) {
		fprintf(stderr, "cannot mmap buffer (%d): %m\n", errno);
		ret = -errno;
		goto err_fb;
	}

	memset(buf->map, 0, buf->size);

	return 0;

err_fb:
	drmModeRmFB(fd, buf->fb);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}


void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_destroy_dumb dreq;

	munmap(buf->map, buf->size);

	drmModeRmFB(fd, buf->fb);

	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

int modeset_create_video_fb(int fd, uint32_t width, uint32_t height, uint32_t format, uint32_t *fb_id) {
    struct drm_mode_create_dumb creq = {0};
    creq.width = width;
    creq.height = height;
    
    if (format == DRM_FORMAT_NV12) {
        creq.height = height * 3 / 2;
        creq.bpp = 8;
    } else if (format == DRM_FORMAT_YUYV) {
        creq.bpp = 16;
    } else {
        fprintf(stderr, "Unsupported video format 0x%x\n", format);
        return -EINVAL;
    }

    int ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret < 0) return -errno;

    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

    if (format == DRM_FORMAT_NV12) {
        handles[0] = creq.handle;
        handles[1] = creq.handle;
        pitches[0] = creq.pitch;
        pitches[1] = creq.pitch;
        offsets[0] = 0;
        offsets[1] = creq.pitch * height;
    } else if (format == DRM_FORMAT_YUYV) {
        handles[0] = creq.handle;
        pitches[0] = creq.pitch;
        offsets[0] = 0;
    }

    ret = drmModeAddFB2(fd, width, height, format, handles, pitches, offsets, fb_id, 0);
    
	// Clear to black
	struct drm_mode_map_dumb mreq = {0};
	mreq.handle = creq.handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) == 0) {
		void *map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
		if (map != MAP_FAILED) {
            if (format == DRM_FORMAT_NV12) {
    			memset(map, 0, creq.pitch * height); // Y plane
	    		memset((uint8_t*)map + creq.pitch * height, 128, creq.pitch * height / 2); // UV plane
            } else if (format == DRM_FORMAT_YUYV) {
                // Y=0 U=128 Y=0 V=128 -> 0x00 0x80 0x00 0x80 -> 0x80008000 (LE)
                uint32_t *p = (uint32_t*)map;
                uint32_t val = 0x80008000;
                for(size_t i=0; i < creq.size/4; i++) p[i] = val;
            }
			munmap(map, creq.size);
		}
	}

    return ret;
}


int modeset_setup_framebuffers(int fd, drmModeConnector *conn, struct modeset_output *out)
{
	for (int i=0; i<OSD_BUF_COUNT; i++) {
		out->osd_bufs[i].width = out->mode.hdisplay;
		out->osd_bufs[i].height = out->mode.vdisplay;
		int ret = modeset_create_fb(fd, &out->osd_bufs[i]);
		if (ret) {
			return ret;
		}
	}
	out->video_crtc_width = out->mode.hdisplay;
	out->video_crtc_height = out->mode.vdisplay;

    // Create a black placeholder FB for the video plane (needed for initial modeset on some drivers)
    // modeset_create_nv12_fb(fd, out->mode.hdisplay, out->mode.vdisplay, (uint32_t*)&out->video_fb_id);
    modeset_create_video_fb(fd, out->mode.hdisplay, out->mode.vdisplay, out->video_format, (uint32_t*)&out->video_fb_id);

	return 0;
}


void modeset_output_destroy(int fd, struct modeset_output *out)
{
	modeset_destroy_objects(fd, out);

	for (int i=0; i<OSD_BUF_COUNT; i++) { 
		modeset_destroy_fb(fd, &out->osd_bufs[i]);
	}
	drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
	free(out);
}

struct modeset_output *modeset_output_create(int fd, drmModeRes *res, drmModeConnector *conn, uint16_t mode_width, uint16_t mode_height, uint32_t mode_vrefresh)
{
	int ret;
	struct modeset_output *out;

	out = malloc(sizeof(*out));
	memset(out, 0, sizeof(*out));
	out->connector.id = conn->connector_id;

	if (conn->connection != DRM_MODE_CONNECTED) {
		fprintf(stderr, "ignoring unused connector %u\n",
			conn->connector_id);
		goto out_error;
	}

	// Writeback is not a display. It reports itself connected and advertises
	// modes like any other connector - this board offers 1920x1080@67 on it -
	// and modeset_prepare takes the last connector that creates successfully,
	// so whether the picture goes to the panel or into a capture buffer came
	// down to enumeration order.
	if (conn->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
		fprintf(stderr, "ignoring writeback connector %u (not a display)\n",
			conn->connector_id);
		goto out_error;
	}

	if (conn->count_modes == 0) {
		fprintf(stderr, "no valid mode for connector %u\n",
			conn->connector_id);
		goto out_error;
	}

	// Name the sink and list what it offers, every time. Without this a black
	// screen tells you nothing: an injected mode the display cannot lock onto
	// looks exactly like a display that was never driven at all.
	{
		const char* t = "connector";
		switch (conn->connector_type) {
			case DRM_MODE_CONNECTOR_HDMIA:       t = "HDMI-A";     break;
			case DRM_MODE_CONNECTOR_HDMIB:       t = "HDMI-B";     break;
			case DRM_MODE_CONNECTOR_DisplayPort: t = "DisplayPort";break;
			case DRM_MODE_CONNECTOR_eDP:         t = "eDP";        break;
			case DRM_MODE_CONNECTOR_DSI:         t = "DSI";        break;
			case DRM_MODE_CONNECTOR_LVDS:        t = "LVDS";       break;
			case DRM_MODE_CONNECTOR_USB:         t = "USB-C";      break;
			case DRM_MODE_CONNECTOR_WRITEBACK:   t = "Writeback";  break;
			default: break;
		}
		printf("display: %s-%u connected, %d modes advertised\n",
		       t, conn->connector_type_id, conn->count_modes);
	}

	int fc = 0;
	if (mode_width>0 && mode_height>0 && mode_vrefresh>0) {
		fc = -1;
		printf( "Available modes:\n");
		for (int i = 0; i < conn->count_modes; i++ ) {
			printf( "%d : %dx%d@%d%s\n",i, conn->modes[i].hdisplay, conn->modes[i].vdisplay , conn->modes[i].vrefresh,
				(conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE) ? " [interlaced - skipped]" : "" );
			if (conn->modes[i].hdisplay == mode_width &&
			conn->modes[i].vdisplay == mode_height &&
			conn->modes[i].vrefresh == mode_vrefresh
			&& !(conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE)
			) {
				fc = i;
			}
		}
		if (fc < 0) {
            // The requested mode is not on offer, so take the nearest thing the
            // sink says it can do.
            //
            // This used to fabricate CVT-RB timings and drive them anyway when
            // 1920x1080 was asked for above 60Hz. That is an overclock: it only
            // worked because the goggle's own panel tolerated being driven
            // outside its EDID, and any display that does not - XREAL glasses,
            // a capture box, a monitor - simply failed to lock and showed
            // black, which looks exactly like an output that was never driven.
            // A sink's mode list is the one statement it makes about what it
            // can take, and there is no reason to disbelieve it.
            //
            // Preference: the highest advertised refresh at the requested
            // resolution that does not exceed what was asked for, then the
            // sink's own preferred mode, then whatever is first.
            int best = -1, best_rate = -1, pref = -1;
            for (int i = 0; i < conn->count_modes; i++) {
                if (conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE) continue;
                if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) pref = i;
                if (conn->modes[i].hdisplay == mode_width &&
                    conn->modes[i].vdisplay == mode_height &&
                    (int)conn->modes[i].vrefresh <= (int)mode_vrefresh &&
                    (int)conn->modes[i].vrefresh > best_rate) {
                    best = i; best_rate = conn->modes[i].vrefresh;
                }
            }
            if (best < 0) best = (pref >= 0) ? pref : 0;
            printf("display: %dx%d@%d not advertised - using %dx%d@%d instead\n",
                   mode_width, mode_height, mode_vrefresh,
                   conn->modes[best].hdisplay, conn->modes[best].vdisplay,
                   conn->modes[best].vrefresh);
            fc = best;
		} 
    } else {
        // No specific mode requested, auto-select highest refresh rate (and resolution)
        uint32_t max_refresh = 0;
        uint32_t max_area = 0;
        
        for (int i = 0; i < conn->count_modes; i++) {
            if (conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE)
                continue;   /* interlaced modes do not composite correctly here */
            uint32_t refresh = conn->modes[i].vrefresh;
            uint32_t area = conn->modes[i].hdisplay * conn->modes[i].vdisplay;
            
            // Priority: Higher Refresh > Higher Resolution
            if (refresh > max_refresh || (refresh == max_refresh && area > max_area)) {
                max_refresh = refresh;
                max_area = area;
                fc = i;
            }
        }
        printf("Auto-selecting highest refresh rate mode: %dx%d@%d\n", 
               conn->modes[fc].hdisplay, conn->modes[fc].vdisplay, conn->modes[fc].vrefresh);
    }
	// Always a mode the sink advertised, so this is the only path now - the
	// label that let the fabricated-timing branch skip this copy is gone with it.
	memcpy(&out->mode, &conn->modes[fc], sizeof(out->mode));

	if (drmModeCreatePropertyBlob(fd, &out->mode, sizeof(out->mode), &out->mode_blob_id) != 0) {

		fprintf(stderr, "couldn't create a blob property\n");
		goto out_error;
	}

	ret = modeset_find_crtc(fd, res, conn, out);
	if (ret) {
		fprintf(stderr, "no valid crtc for connector %u\n", conn->connector_id);
		goto out_blob;
	}

	out->video_format = VIDEO_PLANE_FMT; // Default NV12
	ret = modeset_find_plane(fd, out, &out->video_plane, out->video_format, -1);
	if (ret) {
		fprintf(stderr, "Failed to find NV12 plane. Trying YUYV...\n");
        out->video_format = DRM_FORMAT_YUYV;
        ret = modeset_find_plane(fd, out, &out->video_plane, out->video_format, -1);
        if (ret) {
    		fprintf(stderr, "no valid video plane with format NV12 or YUYV for crtc %u\n", out->crtc.id);
	    	goto out_blob;
        }
	}
	fprintf(stdout, "Using plane %d (%c%c%c%c) for Video\n",  out->video_plane.id,
        (out->video_format)&0xff, (out->video_format>>8)&0xff, (out->video_format>>16)&0xff, (out->video_format>>24)&0xff);

	ret = modeset_find_plane(fd, out, &out->osd_plane, OSD_PLANE_FMT, out->video_plane.id);
	if (ret) {
		fprintf(stderr, "no valid osd plane with format RGB8888 for crtc %u\n", out->crtc.id);
		goto out_blob;
	}
	fprintf(stdout, "Using plane %d (RGB8888) for OSD\n",  out->osd_plane.id);

	ret = modeset_setup_objects(fd, out);
	if (ret) {
		fprintf(stderr, "cannot get plane properties\n");
		goto out_blob;
	}

	ret = modeset_setup_framebuffers(fd, conn, out);
	if (ret) {
		fprintf(stderr, "cannot create framebuffers for connector %u\n",
			conn->connector_id);
		goto out_obj;
	}

	out->video_request = drmModeAtomicAlloc();
	assert(out->video_request);
	out->osd_request = drmModeAtomicAlloc();
	assert(out->video_request);

	return out;

out_obj:
	modeset_destroy_objects(fd, out);
out_blob:
	drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
out_error:
	free(out);
	return NULL;
}


int modeset_prepare(int fd, struct modeset_output *output_list, uint16_t mode_width, uint16_t mode_height, uint32_t mode_vrefresh)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modeset_output *out;

	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
			errno);
		return -errno;
	}

	bool found = false;
	for (i = 0; i < res->count_connectors; ++i) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
				i, res->connectors[i], errno);
			continue;
		}

		out = modeset_output_create(fd, res, conn, mode_width, mode_height, mode_vrefresh);
		drmModeFreeConnector(conn);
		if (!out)
			continue;
		*output_list = *out;
		found = true;
	}
	if (!found) {
		fprintf(stderr, "couldn't create any outputs (no connected displays found)\n");
		drmModeFreeResources(res);
		return -1;
	}

	drmModeFreeResources(res);
	return 0;
}

int modeset_perform_modeset(int fd, struct modeset_output *out, drmModeAtomicReq * req, plane_info* video_info, plane_info* osd_info)
{
	int ret, flags;

	// Reset request to allow reuse
	drmModeAtomicSetCursor(req, 0);

	bool full = out->initialized == false; 
    if (full) {
        printf("Performing initial full modeset setup on connector %u, crtc %u...\n", out->connector.id, out->crtc.id);
        fflush(stdout);
    }

	if(video_info) {
		ret = modeset_atomic_prepare_commit(fd, out, req, video_info->plane, video_info->fb_id, video_info->width, video_info->height, video_info->zpos, full);
        full = false; // Only do full modeset (connector/crtc setup) once per request
		if (ret < 0) return ret;
	} else if (full) {
        // Use placeholder FB for video plane if it's potentially primary
        ret = modeset_atomic_prepare_commit(fd, out, req, &out->video_plane, out->video_fb_id, out->mode.hdisplay, out->mode.vdisplay, 0, true);
        full = false;
        if (ret < 0) return ret;
    }

	if(osd_info) {
		ret = modeset_atomic_prepare_commit(fd, out, req, osd_info->plane, osd_info->fb_id, osd_info->width, osd_info->height, osd_info->zpos, full);
		if (ret < 0) return ret;
	}

	/* Blocking, so it cannot collide with the video thread's flip events.
	 * One commit, nothing else: this used to run a TEST_ONLY check first and,
	 * on failure, retry with the very same flags - two extra ioctls of full
	 * atomic validation on every HUD frame for no change in outcome. */
	flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
    
    if (ret < 0) {
        static time_t last_error_time = 0;
        static time_t last_dump_time = 0;
        time_t now = time(NULL);
        if (now - last_error_time >= 5) {
            fprintf(stderr, "Atomic commit failed (flags 0x%x): %s (%d)\n", flags, strerror(-ret), -ret);
            last_error_time = now;
        }
        if (now - last_dump_time >= 60) {
            dump_obj_props(fd, &out->osd_plane, "OSD Plane");
            dump_obj_props(fd, &out->video_plane, "Video Plane");
            dump_obj_props(fd, &out->crtc, "CRTC");
            dump_obj_props(fd, &out->connector, "Connector");
            last_dump_time = now;
        }
    } else {
        if (!out->initialized) {
            printf("Initial atomic modeset successful.\n");
            notify_splash_done();
        }
        out->initialized = true;
    }

	return ret;
}


int modeset_atomic_prepare_commit(int fd, struct modeset_output *out, drmModeAtomicReq *req, struct drm_object *plane, 
	int fb_id, uint32_t width, uint32_t height, int zpos, bool full_modeset)
{
	if (full_modeset) {
		if (set_drm_object_property(req, &out->connector, "CRTC_ID", out->crtc.id) < 0)
			return -1;
		if (set_drm_object_property(req, &out->crtc, "MODE_ID", out->mode_blob_id) < 0)
			return -1;
		if (set_drm_object_property(req, &out->crtc, "ACTIVE", 1) < 0)
			return -1;
		// Enable VRR if supported and allowed
		set_drm_object_property(req, &out->crtc, "VRR_ENABLED", out->allow_vrr ? 1 : 0);
	}

	if (set_drm_object_property(req, plane, "FB_ID", fb_id) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_ID", (fb_id > 0) ? out->crtc.id : 0) < 0)
		return -1;

	// With nvidia, cannot zoom in.
	// // {
	// set_drm_object_property(req, plane, "SRC_X", 0);
	// set_drm_object_property(req, plane, "SRC_Y", 0);
	// set_drm_object_property(req, plane, "SRC_W", width << 16);
	// set_drm_object_property(req, plane, "SRC_H", height << 16);
	// set_drm_object_property(req, plane, "CRTC_X", 0);
	// set_drm_object_property(req, plane, "CRTC_Y", 0);
	// set_drm_object_property(req, plane, "CRTC_W", width);
	// set_drm_object_property(req, plane, "CRTC_H", height);
	// // }

	// Optional zpos, alpha and blending - don't fail if missing
	set_drm_object_property(req, plane, "zpos", zpos);
    set_drm_object_property(req, plane, "alpha", 65535);
    set_drm_object_property(req, plane, "pixel blend mode", 1); // 1 = Pre-multiplied

	// SRC rect (16.16 fixed point)
	if (set_drm_object_property(req, plane, "SRC_X", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_Y", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_W", width << 16) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_H", height << 16) < 0)
		return -1;

	// CRTC rect
    // If it's the OSD plane, we usually want it full screen or matching its buffer size
    // The current logic tries to maintain aspect ratio based on video_crtc_width/height.
    // For OSD we might just want to match the output mode.
	uint32_t crtcw =  out->mode.hdisplay;
	uint32_t crtch = out->mode.vdisplay;
    
    // If it's a video plane, we might scale. If it's OSD, usually 1:1.
    // However, let's keep it flexible.
    if (width != crtcw || height != crtch) {
        float ratio = (float)width/height;
        if (crtcw / ratio > crtch) {
            crtcw = crtch * ratio;
        } else {
            crtch = crtcw / ratio;
        }
    }

	int crtcx = (out->mode.hdisplay - crtcw) / 2;
	int crtcy = (out->mode.vdisplay - crtch) / 2;

	if (set_drm_object_property(req, plane, "CRTC_X", crtcx) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_Y", crtcy) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_W", crtcw) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_H", crtch) < 0)
		return -1;

	return 0;
}

void restore_planes_zpos(int fd, struct modeset_output *output_list) {
	// restore osd zpos
	int ret, flags;
	struct modeset_buf *buf = &output_list->osd_bufs[0];

	// TODO(geehe) Find a more elegant way to do this.
	int64_t zpos = get_property_value(fd, output_list->osd_plane.props, "zpos");
	ret = modeset_atomic_prepare_commit(fd, output_list, output_list->osd_request, &output_list->osd_plane, buf->fb, buf->width, buf->height, zpos, true);
	if (ret < 0) {
		fprintf(stderr, "prepare atomic commit failed for plane %d, %m\n", output_list->osd_plane.id);
		return;
	}
	ret = drmModeAtomicCommit(fd, output_list->osd_request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret < 0) 
		fprintf(stderr, "modeset atomic commit failed for plane %d, %m\n", output_list->osd_plane.id);

	zpos = get_property_value(fd, output_list->video_plane.props, "zpos");
	ret = modeset_atomic_prepare_commit(fd, output_list, output_list->video_request, &output_list->video_plane, buf->fb, buf->width, buf->height, zpos, true);
	if (ret < 0) {
		fprintf(stderr, "prepare atomic commit failed for plane %d, %m\n", output_list->video_plane.id);
		return;
	}
	ret = drmModeAtomicCommit(fd, output_list->video_request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret < 0) 
		fprintf(stderr, "modeset atomic commit failed for plane %d, %m\n", output_list->video_plane.id);
}

void modeset_cleanup(int fd, struct modeset_output *output_list)
{
	modeset_output_destroy(fd, output_list);
}

/*
 * Draw on back framebuffer before the page-flip is requested.
 */

 static void modeset_paint_framebuffer(struct modeset_output *out)
 {
	 struct modeset_buf *buf;
	 unsigned int j, k, off;
	 unsigned int r,g,b;
	 r =5;
	 g = 5;
	 b = 5;
 
	 buf = &out->osd_bufs[out->osd_buf_switch];
	 for (j = 0; j < buf->height; ++j) {
		 for (k = 0; k < buf->width; ++k) {
			 off = buf->stride * j + k * 4;
			 *(uint32_t*)&buf->map[off] =
					  (r << 16) | (g << 8) | b;
		 }
	 }
 
 }
