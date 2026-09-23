// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Producer half of the cross-process dma-buf zero-copy spike (epic #1699).
 *
 * Mimics a browser GPU process: allocates a gbm_bo with an explicit DRM format
 * modifier, imports it into GLES via EGL_EXT_image_dma_buf_import(_modifiers),
 * renders a frame-dependent pattern into it, exports an acquire sync_file via
 * EGL_ANDROID_native_fence_sync and ships plane fds + metadata + fence to the
 * consumer over SOCK_SEQPACKET/SCM_RIGHTS. Fully offscreen: no window, no surface.
 */
#include "common.h"
#include "vkcommon.h"

#include "third_party/gbm.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <poll.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

static PFNEGLGETPLATFORMDISPLAYEXTPROC p_eglGetPlatformDisplayEXT;
static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC p_glEGLImageTargetRenderbufferStorageOES;
static PFNEGLCREATESYNCKHRPROC p_eglCreateSyncKHR;
static PFNEGLDESTROYSYNCKHRPROC p_eglDestroySyncKHR;
static PFNEGLWAITSYNCKHRPROC p_eglWaitSyncKHR;
static PFNEGLDUPNATIVEFENCEFDANDROIDPROC p_eglDupNativeFenceFDANDROID;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC p_eglQueryDmaBufModifiersEXT;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;

struct opts
{
	const char *render_node, *sock_path, *mods;
	uint32_t w, h, fourcc, usage;
	char mode;
	int frames, fps, no_fence, no_release_wait;
	int alloc_chromium; /* exact browser allocation: v1 _with_modifiers, no usage; else gbm_bo_create */
	int return_path;    /* GL side of the woven-output direction: import + sample what Vulkan exported */
};

struct pbo
{
	struct gbm_bo *bo;
	uint32_t id;
	EGLImageKHR img;
	GLuint rb, fbo;
	uint64_t modifier;
	uint32_t planes, stride[4], offset[4];
};

static EGLDisplay g_dpy;
static struct gbm_device *g_gbm;
static int g_have_native_fence;

static void
die_egl(const char *what)
{
	fprintf(stderr, "EGL error in %s: 0x%x\n", what, eglGetError());
	exit(1);
}

static int
has_ext(const char *list, const char *ext)
{
	size_t l = strlen(ext);
	for (const char *p = list; p && (p = strstr(p, ext)); p += l)
		if ((p == list || p[-1] == ' ') && (p[l] == ' ' || p[l] == 0))
			return 1;
	return 0;
}

static uint32_t
parse_usage(const char *s)
{
	uint32_t u = 0;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", s);
	for (char *t = strtok(buf, ",|"); t; t = strtok(NULL, ",|")) {
		if (!strcmp(t, "rendering")) u |= GBM_BO_USE_RENDERING;
		else if (!strcmp(t, "scanout")) u |= GBM_BO_USE_SCANOUT;
		else if (!strcmp(t, "linear")) u |= GBM_BO_USE_LINEAR;
		else if (!strcmp(t, "none")) u |= 0;
		else { fprintf(stderr, "bad usage flag %s\n", t); exit(2); }
	}
	return u;
}

/* Modifier list selection. Returns count; mods filled (max 64). -1 = no modifier API (plain gbm_bo_create). */
static int
choose_modifiers(const struct opts *o, uint64_t *mods)
{
	if (!strcmp(o->mods, "none"))
		return -1;
	if (!strcmp(o->mods, "linear")) {
		mods[0] = DRM_FORMAT_MOD_LINEAR;
		return 1;
	}
	if (strcmp(o->mods, "auto") != 0) {
		int n = 0;
		char buf[1024];
		snprintf(buf, sizeof(buf), "%s", o->mods);
		for (char *t = strtok(buf, ","); t && n < 64; t = strtok(NULL, ","))
			mods[n++] = strtoull(t, NULL, 16);
		return n;
	}

	/* auto: Vulkan (importer) SAMPLED+dma-buf-importable modifiers, intersected with
	 * the modifiers EGL can render to (non-external-only). gbm picks from the result. */
	VkInstance inst = spike_vk_instance();
	VkPhysicalDevice pd = spike_vk_pick(inst, o->render_node, 0);
	int has_alpha;
	VkFormat vf = spike_fourcc_to_vk(o->fourcc, &has_alpha);
	struct spike_mod_info *vm;
	uint32_t nv = spike_vk_modifiers(pd, vf, &vm);

	EGLint ne = 0;
	p_eglQueryDmaBufModifiersEXT(g_dpy, (EGLint)o->fourcc, 0, NULL, NULL, &ne);
	EGLuint64KHR *em = calloc(ne + 1, sizeof(*em));
	EGLBoolean *eo = calloc(ne + 1, sizeof(*eo));
	p_eglQueryDmaBufModifiersEXT(g_dpy, (EGLint)o->fourcc, ne, em, eo, &ne);
	printf("producer: EGL modifiers for %s:\n", spike_fourcc_name(o->fourcc));
	for (int i = 0; i < ne; i++)
		printf("  %s%s\n", spike_mod_name(em[i]), eo[i] ? "  [external_only]" : "");

	int n = 0;
	for (uint32_t i = 0; i < nv; i++) {
		if (!(vm[i].features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT))
			continue;
		if (spike_vk_mod_image_ok(pd, vf, vm[i].modifier, VK_IMAGE_USAGE_SAMPLED_BIT, NULL) != VK_SUCCESS)
			continue;
		for (int j = 0; j < ne; j++)
			if (em[j] == vm[i].modifier && !eo[j] && n < 64)
				mods[n++] = vm[i].modifier;
	}
	printf("producer: auto modifier list (Vulkan SAMPLED+dma-buf ∩ EGL renderable), %d entries:\n", n);
	for (int i = 0; i < n; i++)
		printf("  %s\n", spike_mod_name(mods[i]));
	free(em), free(eo), free(vm);
	vkDestroyInstance(inst, NULL);
	return n;
}

/* EGL dma-buf import with explicit modifier. Does not take ownership of fds. */
static EGLImageKHR
egl_import_dmabuf(uint32_t w, uint32_t h, uint32_t fourcc, uint32_t planes, const int *fds, const uint32_t *offset,
                  const uint32_t *stride, uint64_t modifier)
{
	static const EGLint fd_a[] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT,
	                              EGL_DMA_BUF_PLANE3_FD_EXT};
	static const EGLint off_a[] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
	                               EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
	static const EGLint pitch_a[] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
	                                 EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
	static const EGLint lo_a[] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
	                              EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
	static const EGLint hi_a[] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
	                              EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
	EGLint a[64];
	int k = 0;
	a[k++] = EGL_WIDTH, a[k++] = (EGLint)w;
	a[k++] = EGL_HEIGHT, a[k++] = (EGLint)h;
	a[k++] = EGL_LINUX_DRM_FOURCC_EXT, a[k++] = (EGLint)fourcc;
	for (uint32_t i = 0; i < planes; i++) {
		a[k++] = fd_a[i], a[k++] = fds[i];
		a[k++] = off_a[i], a[k++] = (EGLint)offset[i];
		a[k++] = pitch_a[i], a[k++] = (EGLint)stride[i];
		a[k++] = lo_a[i], a[k++] = (EGLint)(modifier & 0xffffffff);
		a[k++] = hi_a[i], a[k++] = (EGLint)(modifier >> 32);
	}
	a[k++] = EGL_NONE;
	return p_eglCreateImageKHR(g_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
}

static uint32_t g_next_bo_id = 1;

static void
alloc_bo(const struct opts *o, const uint64_t *mods, int nmods, struct pbo *p, int verbose)
{
	memset(p, 0, sizeof(*p));
	const char *path = NULL;
	if (o->alloc_chromium) {
		/* Exactly what the browser's Wayland GPU path does at its pinned tag:
		 * modifiers advertised -> v1 gbm_bo_create_with_modifiers (no usage flags);
		 * none advertised -> gbm_bo_create(SCANOUT|TEXTURING|RENDERING). Mesa's gbm.h has no
		 * GBM_BO_USE_TEXTURING (a minigbm flag); against Mesa it contributes 0. No fallback. */
		if (nmods > 0) {
			p->bo = gbm_bo_create_with_modifiers(g_gbm, o->w, o->h, o->fourcc, mods, (unsigned)nmods);
			path = "gbm_bo_create_with_modifiers [chromium-exact]";
		} else {
			p->bo = gbm_bo_create(g_gbm, o->w, o->h, o->fourcc, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
			path = "gbm_bo_create(SCANOUT|RENDERING) [chromium-exact]";
		}
		if (!p->bo) {
			fprintf(stderr, "producer: chromium-exact allocation %s FAILED (gbm returned NULL)\n", path);
			exit(1);
		}
	} else if (nmods >= 0) {
		p->bo = gbm_bo_create_with_modifiers2(g_gbm, o->w, o->h, o->fourcc, mods, (unsigned)nmods, o->usage);
		path = "gbm_bo_create_with_modifiers2";
		if (!p->bo) {
			if (verbose)
				fprintf(stderr, "producer: gbm_bo_create_with_modifiers2 failed: %s; trying _with_modifiers\n",
				        strerror(errno));
			p->bo = gbm_bo_create_with_modifiers(g_gbm, o->w, o->h, o->fourcc, mods, (unsigned)nmods);
			path = "gbm_bo_create_with_modifiers";
		}
	}
	if (!p->bo && !o->alloc_chromium) {
		if (verbose && nmods >= 0)
			fprintf(stderr, "producer: modifier allocation failed: %s; falling back to gbm_bo_create\n",
			        strerror(errno));
		p->bo = gbm_bo_create(g_gbm, o->w, o->h, o->fourcc, o->usage);
		path = "gbm_bo_create";
	}
	if (!p->bo) {
		fprintf(stderr, "producer: all gbm allocation paths failed: %s\n", strerror(errno));
		exit(1);
	}
	p->id = g_next_bo_id++;
	p->modifier = gbm_bo_get_modifier(p->bo);
	p->planes = (uint32_t)gbm_bo_get_plane_count(p->bo);
	for (uint32_t i = 0; i < p->planes; i++) {
		p->stride[i] = gbm_bo_get_stride_for_plane(p->bo, (int)i);
		p->offset[i] = gbm_bo_get_offset(p->bo, (int)i);
	}
	if (verbose) {
		printf("producer: bo via %s: %ux%u %s usage=0x%x modifier=%s planes=%u\n", path, o->w, o->h,
		       spike_fourcc_name(o->fourcc), o->alloc_chromium ? 0u : o->usage, spike_mod_name(p->modifier), p->planes);
		for (uint32_t i = 0; i < p->planes; i++)
			printf("  plane %u: stride=%u offset=%u\n", i, p->stride[i], p->offset[i]);
	}

	/* Import into EGL exactly as a dma-buf importer would (explicit modifier). */
	int fds[4];
	for (uint32_t i = 0; i < p->planes; i++)
		fds[i] = gbm_bo_get_fd_for_plane(p->bo, (int)i);
	p->img = egl_import_dmabuf(o->w, o->h, o->fourcc, p->planes, fds, p->offset, p->stride, p->modifier);
	for (uint32_t i = 0; i < p->planes; i++)
		close(fds[i]); /* EGL_EXT_image_dma_buf_import: the app keeps fd ownership */
	if (p->img == EGL_NO_IMAGE_KHR)
		die_egl("eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)");

	glGenRenderbuffers(1, &p->rb);
	glBindRenderbuffer(GL_RENDERBUFFER, p->rb);
	p_glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, p->img);
	glGenFramebuffers(1, &p->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, p->fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, p->rb);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "producer: FBO incomplete 0x%x (glError 0x%x)\n", st, glGetError());
		exit(1);
	}
}

static void
free_bo(struct pbo *p)
{
	glDeleteFramebuffers(1, &p->fbo);
	glDeleteRenderbuffers(1, &p->rb);
	p_eglDestroyImageKHR(g_dpy, p->img);
	gbm_bo_destroy(p->bo);
	memset(p, 0, sizeof(*p));
}

static const char *vs_src = "#version 300 es\n"
                            "void main(){ vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"
                            "  gl_Position = vec4(p*2.0-1.0, 0.0, 1.0); }\n";
/* Must match spike_pattern() in common.h bit for bit. */
static const char *fs_src =
    "#version 300 es\n"
    "precision highp float; precision highp int;\n"
    "uniform int u_frame; uniform ivec2 u_size; out vec4 o;\n"
    "void main(){ int x = int(gl_FragCoord.x); int y = int(gl_FragCoord.y);\n"
    "  int w = u_size.x, h = u_size.y; ivec4 c;\n"
    "  if (x < 4 || y < 4 || x >= w-4 || y >= h-4) c = ivec4(255,0,255,255);\n"
    "  else { int fm = u_frame % 251;\n"
    "    if ((((x>>5)+(y>>5))&1) == 1) c = ivec4(fm, 255-fm, (x^y)&255, 255);\n"
    "    else c = ivec4(x&255, y&255, (u_frame*7)&255, 192); }\n"
    "  o = vec4(c) / 255.0; }\n";

static GLuint
compile(GLenum t, const char *s)
{
	GLuint sh = glCreateShader(t);
	glShaderSource(sh, 1, &s, NULL);
	glCompileShader(sh);
	GLint ok;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(sh, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(1);
	}
	return sh;
}

/* Wait (GPU-side if possible) for a sync_file fd; takes ownership of fd. */
static void
wait_release_fence(int fd)
{
	if (g_have_native_fence) {
		EGLint a[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
		EGLSyncKHR s = p_eglCreateSyncKHR(g_dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, a);
		if (s != EGL_NO_SYNC_KHR) { /* EGL owns fd now */
			p_eglWaitSyncKHR(g_dpy, s, 0);
			p_eglDestroySyncKHR(g_dpy, s);
			return;
		}
		fprintf(stderr, "producer: importing release fence failed 0x%x; CPU-polling\n", eglGetError());
	}
	struct pollfd pf = {.fd = fd, .events = POLLIN};
	poll(&pf, 1, 5000);
	close(fd);
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

static int
connect_sock(const char *path)
{
	int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%.100s", path);
	int tries = 0;
	while (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		if (++tries > 100) {
			perror("connect");
			exit(1);
		}
		usleep(50000);
	}
	return sock;
}

/*
 * Woven-output direction: the Vulkan side (consumer --return-path) allocates +
 * exports a dma-buf, writes frame f's pattern into it and sends fds + acquire fence.
 * Here we import it as an EGLImage-backed texture, SAMPLE it into our own RGBA8
 * FBO, hand back a release fence, glReadPixels and verify bit-exact.
 */
static const char *fs_sample_src = "#version 300 es\n"
                                   "precision highp float; precision highp int;\n"
                                   "uniform highp sampler2D t; uniform ivec2 u_size; out vec4 o;\n"
                                   "void main(){ o = texture(t, gl_FragCoord.xy / vec2(u_size)); }\n";

static int
run_return_path(const struct opts *o, GLuint unused_prog)
{
	(void)unused_prog;
	GLuint prog = glCreateProgram();
	glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_sample_src));
	glLinkProgram(prog);
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "t"), 0);
	GLint u_size = glGetUniformLocation(prog, "u_size");

	int sock = connect_sock(o->sock_path);
	int fd_ready = spike_count_fds(), fd_peak = fd_ready;
	GLuint rb = 0, fbo = 0, tex = 0;
	uint32_t cw = 0, ch = 0;
	uint8_t *px = NULL;
	int frames = 0, bad_frames = 0, import_fail = 0, printed = 0, acq = 0, rel = 0;
	uint64_t bad_pixels = 0, first_mod = 0;
	uint32_t first_planes = 0, first_fourcc = 0;
	uint64_t *t_imp = calloc(100000, 8), *t_tot = calloc(100000, 8);
	glGenTextures(1, &tex);
	for (;;) {
		struct spike_frame_msg m;
		int fds[8], nfd;
		ssize_t n = spike_recv(sock, &m, sizeof(m), fds, 8, &nfd);
		uint64_t t0 = spike_now_ns();
		if (n <= 0 || m.magic != SPIKE_MAGIC || m.type == SPIKE_MSG_QUIT) {
			for (int i = 0; i < nfd; i++) close(fds[i]);
			break;
		}
		int pk = spike_count_fds();
		if (pk > fd_peak) fd_peak = pk;
		if (!frames) {
			first_mod = m.modifier, first_planes = m.num_planes, first_fourcc = m.fourcc;
			printf("producer[return]: first frame %ux%u %s modifier=%s planes=%u acquire_fence=%d\n", m.width,
			       m.height, spike_fourcc_name(m.fourcc), spike_mod_name(m.modifier), m.num_planes, m.has_fence);
			for (uint32_t i = 0; i < m.num_planes; i++)
				printf("  plane %u: stride=%u offset=%u\n", i, m.stride[i], m.offset[i]);
		}
		if (cw != m.width || ch != m.height) {
			if (fbo) glDeleteFramebuffers(1, &fbo), glDeleteRenderbuffers(1, &rb);
			cw = m.width, ch = m.height;
			glGenRenderbuffers(1, &rb);
			glBindRenderbuffer(GL_RENDERBUFFER, rb);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)cw, (GLsizei)ch);
			glGenFramebuffers(1, &fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
			free(px);
			px = malloc((size_t)cw * ch * 4);
		}
		EGLImageKHR img = egl_import_dmabuf(m.width, m.height, m.fourcc, m.num_planes, fds, m.offset, m.stride,
		                                    m.modifier);
		EGLint eerr = eglGetError();
		for (uint32_t i = 0; i < m.num_planes; i++)
			close(fds[i]); /* EGL does not take fd ownership */
		int acq_fd = m.has_fence ? fds[m.num_planes] : -1;
		if (img == EGL_NO_IMAGE_KHR) {
			if (import_fail++ < 3)
				fprintf(stderr, "producer[return]: frame %u eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) FAILED 0x%x\n",
				        m.frame, eerr);
			if (acq_fd >= 0) close(acq_fd);
			struct spike_reply_msg r = {.magic = SPIKE_MAGIC, .frame = m.frame, .status = -1};
			spike_send(sock, &r, sizeof(r), NULL, 0);
			frames++;
			continue;
		}
		if (acq_fd >= 0) {
			wait_release_fence(acq_fd); /* same helper: GPU-side wait on a sync_file */
			acq++;
		}
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex);
		p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		uint64_t t1 = spike_now_ns();
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, (GLsizei)cw, (GLsizei)ch);
		glUniform2i(u_size, (GLint)cw, (GLint)ch);
		glDrawArrays(GL_TRIANGLES, 0, 3);

		/* Release fence: the sampling draw is done with the exported buffer. */
		int rel_fd = -1;
		if (g_have_native_fence) {
			EGLSyncKHR sy = p_eglCreateSyncKHR(g_dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
			glFlush();
			if (sy != EGL_NO_SYNC_KHR) {
				rel_fd = p_eglDupNativeFenceFDANDROID(g_dpy, sy);
				p_eglDestroySyncKHR(g_dpy, sy);
			}
		}
		if (rel_fd < 0)
			glFinish();
		else
			rel++;
		struct spike_reply_msg r = {.magic = SPIKE_MAGIC, .frame = m.frame, .has_fence = rel_fd >= 0};
		spike_send(sock, &r, sizeof(r), rel_fd >= 0 ? &rel_fd : NULL, rel_fd >= 0 ? 1 : 0);
		if (rel_fd >= 0) close(rel_fd);

		glReadPixels(0, 0, (GLsizei)cw, (GLsizei)ch, GL_RGBA, GL_UNSIGNED_BYTE, px);
		uint64_t t2 = spike_now_ns();
		glBindTexture(GL_TEXTURE_2D, 0);
		p_eglDestroyImageKHR(g_dpy, img);

		int ha;
		spike_fourcc_to_vk(m.fourcc, &ha);
		uint64_t bad = 0;
		for (uint32_t y = 0; y < ch; y++)
			for (uint32_t x = 0; x < cw; x++) {
				uint8_t e[4];
				spike_pattern((int)x, (int)y, (int)m.frame, (int)cw, (int)ch, e);
				if (!ha) e[3] = 255;
				const uint8_t *g = px + ((size_t)y * cw + x) * 4;
				if (memcmp(g, e, 4) != 0) {
					bad++;
					if (printed < 5) {
						printed++;
						fprintf(stderr, "producer[return]: MISMATCH frame %u (%u,%u) got %02x%02x%02x%02x want %02x%02x%02x%02x (RGBA)\n",
						        m.frame, x, y, g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3]);
					}
				}
			}
		if (bad) bad_frames++, bad_pixels += bad;
		if (frames < 100000) t_imp[frames] = t1 - t0, t_tot[frames] = t2 - t0;
		frames++;
	}
	int fd_end = spike_count_fds();
	spike_dump_fds("producer[return]");
	int nf = frames < 100000 ? frames : 100000, ok = nf - import_fail;
	qsort(t_imp, (size_t)nf, 8, cmp_u64);
	qsort(t_tot, (size_t)nf, 8, cmp_u64);
	double ai = 0, at = 0;
	for (int i = nf - ok; i < nf; i++) ai += t_imp[i], at += t_tot[i];
	int p99 = nf - ok + (ok * 99) / 100;
	if (p99 >= nf) p99 = nf - 1;
	printf("producer[return]: RESULT format=%s modifier=%s planes=%u frames=%d import_failed=%d mismatched_frames=%d "
	       "mismatched_pixels=%llu verify=full acquire_fences=%d release_fences=%d egl_import+acquire_us avg=%.0f "
	       "p99=%.0f  import+sample+readback_us avg=%.0f p99=%.0f fds ready=%d peak=%d end=%d\n",
	       spike_fourcc_name(first_fourcc), spike_mod_name(first_mod), first_planes, frames, import_fail, bad_frames,
	       (unsigned long long)bad_pixels, acq, rel, ok ? ai / ok / 1e3 : 0, ok ? t_imp[p99] / 1e3 : 0,
	       ok ? at / ok / 1e3 : 0, ok ? t_tot[p99] / 1e3 : 0, fd_ready, fd_peak, fd_end);
	close(sock);
	return (import_fail || bad_frames) ? 3 : 0;
}

int
main(int argc, char **argv)
{
	struct opts o = {.render_node = "/dev/dri/renderD128",
	                 .sock_path = spike_default_socket(),
	                 .mods = "auto",
	                 .w = 1920,
	                 .h = 1080,
	                 .fourcc = DRM_FORMAT_ARGB8888,
	                 .usage = GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT,
	                 .mode = 'A',
	                 .frames = 600,
	                 .fps = 60};
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strncmp(a, "--size=", 7)) sscanf(a + 7, "%ux%u", &o.w, &o.h);
		else if (!strncmp(a, "--format=", 9)) o.fourcc = spike_parse_fourcc(a + 9);
		else if (!strncmp(a, "--usage=", 8)) o.usage = parse_usage(a + 8);
		else if (!strncmp(a, "--modifiers=", 12)) o.mods = a + 12;
		else if (!strncmp(a, "--mode=", 7)) o.mode = a[7];
		else if (!strncmp(a, "--frames=", 9)) o.frames = atoi(a + 9);
		else if (!strncmp(a, "--fps=", 6)) o.fps = atoi(a + 6);
		else if (!strncmp(a, "--socket=", 9)) o.sock_path = a + 9;
		else if (!strncmp(a, "--render-node=", 14)) o.render_node = a + 14;
		else if (!strcmp(a, "--no-fence")) o.no_fence = 1;
		else if (!strcmp(a, "--no-release-wait")) o.no_release_wait = 1; /* negative control */
		else if (!strcmp(a, "--alloc=chromium")) o.alloc_chromium = 1;
		else if (!strcmp(a, "--return-path")) o.return_path = 1;
		else {
			fprintf(stderr,
			        "usage: %s [--size=WxH] [--format=ARGB8888|XRGB8888|ABGR8888|XBGR8888]\n"
			        "  [--usage=rendering,scanout,linear] [--modifiers=auto|linear|none|hex,hex]\n"
			        "  [--mode=A|B] [--frames=N] [--fps=N (0=unpaced)] [--no-fence] [--no-release-wait] [--socket=P]\n"
			        "  [--alloc=chromium] [--return-path]\n",
			        argv[0]);
			return 2;
		}
	}
	int fd_start = spike_count_fds();

	int drm = open(o.render_node, O_RDWR | O_CLOEXEC);
	if (drm < 0) { perror(o.render_node); return 1; }
	g_gbm = gbm_create_device(drm);
	if (!g_gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }
	printf("producer: gbm backend '%s' on %s\n", gbm_device_get_backend_name(g_gbm), o.render_node);

	const char *cext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	p_eglGetPlatformDisplayEXT = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!has_ext(cext, "EGL_MESA_platform_gbm") && !has_ext(cext, "EGL_KHR_platform_gbm")) {
		fprintf(stderr, "no EGL gbm platform\n");
		return 1;
	}
	g_dpy = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, g_gbm, NULL);
	EGLint maj, min;
	if (!eglInitialize(g_dpy, &maj, &min)) die_egl("eglInitialize");
	const char *dext = eglQueryString(g_dpy, EGL_EXTENSIONS);
	printf("producer: EGL %d.%d vendor='%s' version='%s'\n", maj, min, eglQueryString(g_dpy, EGL_VENDOR),
	       eglQueryString(g_dpy, EGL_VERSION));
	const char *need[] = {"EGL_EXT_image_dma_buf_import", "EGL_EXT_image_dma_buf_import_modifiers",
	                      "EGL_KHR_surfaceless_context", "EGL_KHR_no_config_context"};
	for (unsigned i = 0; i < 4; i++)
		if (!has_ext(dext, need[i])) { fprintf(stderr, "missing %s\n", need[i]); return 1; }
	g_have_native_fence = has_ext(dext, "EGL_ANDROID_native_fence_sync") && has_ext(dext, "EGL_KHR_wait_sync") &&
	                      !o.no_fence;
	printf("producer: EGL_ANDROID_native_fence_sync=%d EGL_KHR_wait_sync=%d -> %s\n",
	       has_ext(dext, "EGL_ANDROID_native_fence_sync"), has_ext(dext, "EGL_KHR_wait_sync"),
	       g_have_native_fence ? "explicit sync_file fences" : "FALLBACK glFinish, no fences");

	p_eglCreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
	p_eglDestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
	p_glEGLImageTargetRenderbufferStorageOES = (void *)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	p_eglCreateSyncKHR = (void *)eglGetProcAddress("eglCreateSyncKHR");
	p_eglDestroySyncKHR = (void *)eglGetProcAddress("eglDestroySyncKHR");
	p_eglWaitSyncKHR = (void *)eglGetProcAddress("eglWaitSyncKHR");
	p_eglDupNativeFenceFDANDROID = (void *)eglGetProcAddress("eglDupNativeFenceFDANDROID");
	p_eglQueryDmaBufModifiersEXT = (void *)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
	p_glEGLImageTargetTexture2DOES = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
	EGLContext ctx = eglCreateContext(g_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
	if (ctx == EGL_NO_CONTEXT) die_egl("eglCreateContext");
	if (!eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) die_egl("eglMakeCurrent(surfaceless)");
	printf("producer: GL_RENDERER='%s' GL_VERSION='%s'\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

	GLuint prog = glCreateProgram();
	glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
	glLinkProgram(prog);
	glUseProgram(prog);
	GLint u_frame = glGetUniformLocation(prog, "u_frame"), u_size = glGetUniformLocation(prog, "u_size");
	glUniform2i(u_size, (GLint)o.w, (GLint)o.h);

	if (o.return_path)
		return run_return_path(&o, prog);

	uint64_t mods[64];
	int nmods = choose_modifiers(&o, mods);
	if (nmods == 0) {
		fprintf(stderr, "producer: empty modifier list for %s\n", spike_fourcc_name(o.fourcc));
		return 1;
	}

	int sock = connect_sock(o.sock_path);

	struct pbo cur;
	alloc_bo(&o, mods, nmods, &cur, 1);
	uint64_t first_mod = cur.modifier;
	int fd_after_setup = spike_count_fds(), fd_peak = fd_after_setup;

	uint64_t *rt = calloc((size_t)o.frames, sizeof(uint64_t));
	uint64_t period = o.fps > 0 ? 1000000000ull / (uint64_t)o.fps : 0;
	uint64_t t0 = spike_now_ns(), next = t0;
	int fenced_frames = 0, released_fenced = 0, mod_changes = 0;
	for (int f = 0; f < o.frames; f++) {
		if (o.mode == 'B' && f > 0) {
			alloc_bo(&o, mods, nmods, &cur, 0);
			if (cur.modifier != first_mod)
				mod_changes++;
		}
		uint64_t ts = spike_now_ns();
		glBindFramebuffer(GL_FRAMEBUFFER, cur.fbo);
		glViewport(0, 0, (GLsizei)o.w, (GLsizei)o.h);
		glUniform1i(u_frame, f);
		glDrawArrays(GL_TRIANGLES, 0, 3);

		int fds[5], nfd = 0, fence = -1;
		if (g_have_native_fence) {
			EGLSyncKHR s = p_eglCreateSyncKHR(g_dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
			glFlush(); /* the native fence fd materialises on flush */
			if (s != EGL_NO_SYNC_KHR) {
				fence = p_eglDupNativeFenceFDANDROID(g_dpy, s);
				p_eglDestroySyncKHR(g_dpy, s);
			}
			if (fence < 0) {
				fprintf(stderr, "producer: eglDupNativeFenceFDANDROID failed 0x%x, glFinish fallback\n",
				        eglGetError());
				glFinish();
			} else
				fenced_frames++;
		} else {
			glFinish();
		}

		struct spike_frame_msg m = {.magic = SPIKE_MAGIC,
		                            .type = SPIKE_MSG_FRAME,
		                            .frame = (uint32_t)f,
		                            .bo_id = cur.id,
		                            .width = o.w,
		                            .height = o.h,
		                            .fourcc = o.fourcc,
		                            .num_planes = cur.planes,
		                            .modifier = cur.modifier,
		                            .has_fence = fence >= 0};
		for (uint32_t i = 0; i < cur.planes; i++) {
			m.stride[i] = cur.stride[i];
			m.offset[i] = cur.offset[i];
			fds[nfd++] = gbm_bo_get_fd_for_plane(cur.bo, (int)i); /* fresh fds each frame */
		}
		if (fence >= 0)
			fds[nfd++] = fence;
		int peak = spike_count_fds();
		if (peak > fd_peak) fd_peak = peak;
		if (spike_send(sock, &m, sizeof(m), fds, nfd) != 0) {
			perror("producer: sendmsg");
			return 1;
		}
		for (int i = 0; i < nfd; i++)
			close(fds[i]);

		struct spike_reply_msg r;
		int rfd[2], nr;
		if (spike_recv(sock, &r, sizeof(r), rfd, 2, &nr) <= 0) {
			fprintf(stderr, "producer: consumer went away at frame %d\n", f);
			return 1;
		}
		if (r.status != 0)
			fprintf(stderr, "producer: consumer reported status %d on frame %d\n", r.status, f);
		if (nr > 0 && o.no_release_wait) {
			for (int i = 0; i < nr; i++) close(rfd[i]);
		} else if (nr > 0) {
			released_fenced++;
			wait_release_fence(rfd[0]); /* GPU-side wait before we touch the bo again */
			for (int i = 1; i < nr; i++) close(rfd[i]);
		}
		rt[f] = spike_now_ns() - ts;
		if (o.mode == 'B')
			free_bo(&cur);

		if (period) {
			next += period;
			uint64_t now = spike_now_ns();
			if (next > now) {
				struct timespec d = {.tv_sec = (time_t)(next / 1000000000ull), .tv_nsec = (long)(next % 1000000000ull)};
				clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL);
			} else
				next = now;
		}
	}
	glFinish();
	double secs = (double)(spike_now_ns() - t0) / 1e9;
	struct spike_frame_msg q = {.magic = SPIKE_MAGIC, .type = SPIKE_MSG_QUIT};
	spike_send(sock, &q, sizeof(q), NULL, 0);
	if (o.mode == 'A')
		free_bo(&cur);
	int fd_end = spike_count_fds();
	spike_dump_fds("producer");

	qsort(rt, (size_t)o.frames, sizeof(uint64_t), cmp_u64);
	uint64_t sum = 0;
	for (int i = 0; i < o.frames; i++) sum += rt[i];
	printf("producer: RESULT mode=%c frames=%d fps=%.2f (target %d) render+send+release-roundtrip avg=%.0fus "
	       "p99=%.0fus acquire_fences=%d release_fences=%d modifier=%s modifier_changes_in_modeB=%d "
	       "fds start=%d after_setup=%d peak=%d end=%d\n",
	       o.mode, o.frames, o.frames / secs, o.fps, sum / 1e3 / o.frames, rt[(o.frames * 99) / 100] / 1e3,
	       fenced_frames, released_fenced, spike_mod_name(first_mod), mod_changes, fd_start, fd_after_setup,
	       fd_peak, fd_end);
	close(sock);
	eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(g_dpy, ctx);
	eglTerminate(g_dpy);
	gbm_device_destroy(g_gbm);
	close(drm);
	return 0;
}
