/*
    Copyright 2010 Carsten Juttner <carjay@gmx.net>
    Copyright 2012,2013 Stefan Seyfried <seife@tuxboxcvs.slipkontur.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.

    openGL based framebuffer implementation
    based on Carjay's neutrino-hd-dvbapi work, see
        http://gitorious.org/neutrino-hd/neutrino-hd-dvbapi

    TODO: AV-Sync code is "experimental" at best
*/

#include "config.h"
#include <vector>

#include <sys/types.h>
#include <signal.h>

#include <cstdio>
#include <cstring>
#include <errno.h>
#include <inttypes.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <climits>
#include <mpv/render_gl.h>
#include "glfb_priv.h"
#include "video_lib.h"
#include "audio_lib.h"

#include "hal_debug.h"

#define hal_debug_c(args...) _hal_debug(HAL_DEBUG_INIT, NULL, args)
#define hal_info_c(args...) _hal_info(HAL_DEBUG_INIT, NULL, args)
#define hal_debug(args...) _hal_debug(HAL_DEBUG_INIT, this, args)
#define hal_info(args...) _hal_info(HAL_DEBUG_INIT, this, args)


extern cVideo *videoDecoder;
extern cAudio *audioDecoder;
extern int sleep_us;

/* the private class that does stuff only needed inside libstb-hal.
 * is used e.g. by cVideo... */
GLFbPC *glfb_priv = NULL;

GLFramebuffer::GLFramebuffer(int x, int y)
{
	Init();
	glfb_priv = new GLFbPC(x, y, osd_buf);
	si = glfb_priv->getScreenInfo();
	start();
	while (!glfb_priv->mInitDone)
		usleep(1);
}

GLFramebuffer::~GLFramebuffer()
{
	glfb_priv->mShutDown = true;
	glfb_priv->wake();
	join();
	delete glfb_priv;
	glfb_priv = NULL;
}

void GLFramebuffer::blit()
{
	glfb_priv->blit();
}

GLFbPC::GLFbPC(int x, int y, std::vector<unsigned char> &buf): mReInit(true), mShutDown(false), mInitDone(false)
{
	osd_buf = &buf;
	mState.width = x;
	mState.height = y;
	mX = &_mX[0];
	mY = &_mY[0];
	*mX = x;
	*mY = y;
	av_reduce(&mOA.num, &mOA.den, x, y, INT_MAX);
	mVA = mOA; /* initial aspect ratios are from the FB resolution, those */
	_mVA = mVA; /* will be updated by the videoDecoder functions anyway */
	mVAchanged = true;
	mCrop = DISPLAY_AR_MODE_PANSCAN;
	zoom = 1.0;
	xscale = 1.0;
	const char *tmp = getenv("GLFB_FULLSCREEN");
	mFullscreen = !!(tmp);

	mState.blit = true;
	last_apts = 0;
	mUserEvent = 0;
	mViewX = mViewY = 0;
	mRender = NULL;
	mVideoFbo = mVideoTex = 0;
	mVideoW = mVideoH = 0;
	mVideoValid = false;

	/* linux framebuffer compat mode */
	si.bits_per_pixel = 32;
	si.xres = mState.width;
	si.xres_virtual = si.xres;
	si.yres = mState.height;
	si.yres_virtual = si.yres;
	si.blue.length = 8;
	si.blue.offset = 0;
	si.green.length = 8;
	si.green.offset = 8;
	si.red.length = 8;
	si.red.offset = 16;
	si.transp.length = 8;
	si.transp.offset = 24;

	unlink("/tmp/neutrino.input");
	mkfifo("/tmp/neutrino.input", 0600);
	input_fd = open("/tmp/neutrino.input", O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (input_fd < 0)
		hal_info("%s: could not open /tmp/neutrino.input FIFO: %m\n", __func__);
	initKeys();
}

GLFbPC::~GLFbPC()
{
	mShutDown = true;
	if (input_fd >= 0)
		close(input_fd);
	osd_buf->clear();
}

void GLFbPC::initKeys()
{
	mKeyMap[SDLK_UP]    = KEY_UP;
	mKeyMap[SDLK_DOWN]  = KEY_DOWN;
	mKeyMap[SDLK_LEFT]  = KEY_LEFT;
	mKeyMap[SDLK_RIGHT] = KEY_RIGHT;

	mKeyMap[SDLK_F1]  = KEY_RED;
	mKeyMap[SDLK_F2]  = KEY_GREEN;
	mKeyMap[SDLK_F3]  = KEY_YELLOW;
	mKeyMap[SDLK_F4]  = KEY_BLUE;

	mKeyMap[SDLK_F5]  = KEY_RECORD;
	mKeyMap[SDLK_F6]  = KEY_PLAY;
	mKeyMap[SDLK_F7]  = KEY_PAUSE;
	mKeyMap[SDLK_F8]  = KEY_STOP;

	mKeyMap[SDLK_F9]  = KEY_FORWARD;
	mKeyMap[SDLK_F10] = KEY_REWIND;
	mKeyMap[SDLK_F11] = KEY_NEXT;
	mKeyMap[SDLK_F12] = KEY_PREVIOUS;

	mKeyMap[SDLK_PAGEUP]   = KEY_PAGEUP;
	mKeyMap[SDLK_PAGEDOWN] = KEY_PAGEDOWN;

	mKeyMap[SDLK_RETURN]   = KEY_OK;
	mKeyMap[SDLK_KP_ENTER] = KEY_OK;
	mKeyMap[SDLK_ESCAPE]   = KEY_EXIT;

	mKeyMap[SDLK_0] = KEY_0;
	mKeyMap[SDLK_1] = KEY_1;
	mKeyMap[SDLK_2] = KEY_2;
	mKeyMap[SDLK_3] = KEY_3;
	mKeyMap[SDLK_4] = KEY_4;
	mKeyMap[SDLK_5] = KEY_5;
	mKeyMap[SDLK_6] = KEY_6;
	mKeyMap[SDLK_7] = KEY_7;
	mKeyMap[SDLK_8] = KEY_8;
	mKeyMap[SDLK_9] = KEY_9;
	mKeyMap[SDLK_KP_0] = KEY_0;
	mKeyMap[SDLK_KP_1] = KEY_1;
	mKeyMap[SDLK_KP_2] = KEY_2;
	mKeyMap[SDLK_KP_3] = KEY_3;
	mKeyMap[SDLK_KP_4] = KEY_4;
	mKeyMap[SDLK_KP_5] = KEY_5;
	mKeyMap[SDLK_KP_6] = KEY_6;
	mKeyMap[SDLK_KP_7] = KEY_7;
	mKeyMap[SDLK_KP_8] = KEY_8;
	mKeyMap[SDLK_KP_9] = KEY_9;

	mKeyMap[SDLK_PLUS]     = KEY_VOLUMEUP;
	mKeyMap[SDLK_KP_PLUS]  = KEY_VOLUMEUP;
	mKeyMap[SDLK_MINUS]    = KEY_VOLUMEDOWN;
	mKeyMap[SDLK_KP_MINUS] = KEY_VOLUMEDOWN;
	mKeyMap[SDLK_PERIOD]   = KEY_MUTE;
	mKeyMap[SDLK_A] = KEY_AUDIO;
	mKeyMap[SDLK_E] = KEY_EPG;
	//     [SDLK_F]  is reserved to toggle fullscreen;
	mKeyMap[SDLK_G] = KEY_GAMES;
	mKeyMap[SDLK_H] = KEY_HELP;
	mKeyMap[SDLK_I] = KEY_INFO;
	mKeyMap[SDLK_M] = KEY_MENU;
	mKeyMap[SDLK_P] = KEY_POWER;
	mKeyMap[SDLK_R] = KEY_RADIO;
	mKeyMap[SDLK_S] = KEY_SUBTITLE;
	mKeyMap[SDLK_T] = KEY_TV;
	mKeyMap[SDLK_V] = KEY_VIDEO;
	mKeyMap[SDLK_Z] = KEY_SLEEP;

	/* text editing inside input dialogs: raw codes, turned back into
	 * glyphs and edit actions by neutrino (CRCInput::getUnicodeValue()
	 * and the RC_backspace handlers) */
	mKeyMap[SDLK_BACKSPACE] = KEY_BACKSPACE;
	mKeyMap[SDLK_SPACE]     = KEY_SPACE;

	/* shift keys, they arrive as upper case text */
	mKeyMap['F'] = KEY_FAVORITES;
	mKeyMap['M'] = KEY_MODE;
	mKeyMap['S'] = KEY_SAT;
	mKeyMap['T'] = KEY_TEXT;
	mKeyMap['W'] = KEY_WWW;
}

void GLFramebuffer::run()
{
	int x = glfb_priv->mState.width;
	int y = glfb_priv->mState.height;
	hal_info("GLFB: GL thread starting x %d y %d\n", x, y);
	SDL_SetAppMetadata("Neutrino", NULL, "neutrino");
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		hal_info("GLFB: SDL_Init failed: %s\n", SDL_GetError());
		_exit(1); /* Life is hard */
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
	if (glfb_priv->mFullscreen)
		flags |= SDL_WINDOW_FULLSCREEN;
	glfb_priv->mWindow = SDL_CreateWindow("Neutrino", x, y, flags);
	if (!glfb_priv->mWindow)
	{
		hal_info("GLFB: SDL_CreateWindow failed: %s\n", SDL_GetError());
		_exit(1);
	}
	glfb_priv->mContext = SDL_GL_CreateContext(glfb_priv->mWindow);
	if (!glfb_priv->mContext)
	{
		hal_info("GLFB: SDL_GL_CreateContext failed: %s\n", SDL_GetError());
		_exit(1);
	}
	SDL_GL_MakeCurrent(glfb_priv->mWindow, glfb_priv->mContext);
	/* swap in step with the display; on KMS an unthrottled swap queues buffers faster than they are flipped */
	if (!SDL_GL_SetSwapInterval(1))
		hal_info("GLFB: SDL_GL_SetSwapInterval: %s\n", SDL_GetError());
	SDL_HideCursor();
	/* printable keys come in as text, translated with the keyboard layout of
	 * the compositor or, on KMS, of the console */
	SDL_StartTextInput(glfb_priv->mWindow);
	/* 32bit FB depth, *2 because tuxtxt uses a shadow buffer */
	int fbmem = x * y * 4 * 2;
	osd_buf.resize(fbmem);
	hal_info("GLFB: OSD buffer set to %d bytes at 0x%p\n", fbmem, osd_buf.data());
	glfb_priv->mInitDone = true; /* signal that setup is finished */

	if (!glfb_priv->setupGLObjects())
	{
		hal_info("GLFB: could not set up the OpenGL ES 2.0 objects\n");
		_exit(1);
	}
	glfb_priv->mUserEvent = SDL_RegisterEvents(1);
	glfb_priv->setupRender();
	/* the shutdown joins this thread, so its signal must not land here */
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	while (!glfb_priv->mShutDown)
	{
		glfb_priv->pollEvents();
		glfb_priv->render();
	}
	glfb_priv->teardownRender();
	glfb_priv->releaseGLObjects();
	SDL_GL_DestroyContext(glfb_priv->mContext);
	SDL_DestroyWindow(glfb_priv->mWindow);
	SDL_Quit();
	hal_info("GLFB: GL thread stopping\n");
}

static const char *vertex_shader =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_tex;\n"
	"uniform vec2 u_scale;\n"
	"uniform float u_xproj;\n"
	"varying vec2 v_tex;\n"
	"void main()\n"
	"{\n"
	"	gl_Position = vec4(a_pos.x * u_scale.x * u_xproj, a_pos.y * u_scale.y, 0.0, 1.0);\n"
	"	v_tex = a_tex;\n"
	"}\n";

/* the OSD and video buffers are BGRA, the texture is uploaded as RGBA */
static const char *fragment_shader =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_bgra;\n"
	"varying vec2 v_tex;\n"
	"void main()\n"
	"{\n"
	"	vec4 c = texture2D(u_tex, v_tex);\n"
	"	gl_FragColor = mix(c, c.bgra, u_bgra);\n"
	"}\n";

static GLuint compileShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[1024];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		hal_info_c("GLFB::%s: shader compilation failed: %s\n", __func__, log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

bool GLFbPC::setupGLObjects()
{
	GLuint vs = compileShader(GL_VERTEX_SHADER, vertex_shader);
	GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragment_shader);
	if (!vs || !fs)
		return false;
	mState.program = glCreateProgram();
	glAttachShader(mState.program, vs);
	glAttachShader(mState.program, fs);
	glLinkProgram(mState.program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok = GL_FALSE;
	glGetProgramiv(mState.program, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		char log[1024];
		glGetProgramInfoLog(mState.program, sizeof(log), NULL, log);
		hal_info("GLFB::%s: program link failed: %s\n", __func__, log);
		return false;
	}
	mState.a_pos = glGetAttribLocation(mState.program, "a_pos");
	mState.a_tex = glGetAttribLocation(mState.program, "a_tex");
	mState.u_scale = glGetUniformLocation(mState.program, "u_scale");
	mState.u_xproj = glGetUniformLocation(mState.program, "u_xproj");
	mState.u_bgra = glGetUniformLocation(mState.program, "u_bgra");
	mState.xproj = 1.0;
	glUseProgram(mState.program);
	glUniform1f(mState.u_bgra, 1.0);
	glUniform1i(glGetUniformLocation(mState.program, "u_tex"), 0);
	glActiveTexture(GL_TEXTURE0);

	/* non power of two textures need clamping and no mipmaps in GLES2 */
	glGenTextures(1, &mState.osdtex);
	glGenTextures(1, &mState.displaytex);
	glBindTexture(GL_TEXTURE_2D, mState.osdtex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mState.width, mState.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* we do not yet know the size of the video, start with 1 black pixel */
	unsigned char buf[4] = { 0, 0, 0, 0 };
	glBindTexture(GL_TEXTURE_2D, mState.displaytex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return true;
}


void GLFbPC::releaseGLObjects()
{
	glDeleteTextures(1, &mState.osdtex);
	glDeleteTextures(1, &mState.displaytex);
	glDeleteProgram(mState.program);
}


void GLFbPC::pollEvents()
{
	SDL_Event ev;
	/* the software live TV decoder still paces the loop, everything else just waits */
	int timeout = 250;
	if (!mVideoValid && videoDecoder)
		timeout = sleep_us > 1000 ? sleep_us / 1000 : 1;
	if (!SDL_WaitEventTimeout(&ev, timeout))
		return;
	do
	{
		switch (ev.type)
		{
			case SDL_EVENT_KEY_DOWN:
				if (!producesText(ev.key.key))
					handleKey(ev.key.key);
				break;
			case SDL_EVENT_TEXT_INPUT:
				for (const char *c = ev.text.text; c && *c; c++)
					if (*c > 0x20 && *c < 0x7f)
						handleKey((SDL_Keycode)*c);
					else if (*c == ' ')
						handleKey(SDLK_SPACE);
				break;
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				mReInit = true;
				break;
			case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
				mFullscreen = true;
				mReInit = true;
				break;
			case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
				mFullscreen = false;
				mReInit = true;
				break;
			case SDL_EVENT_QUIT:
				/* the window is all there is of neutrino on a desktop, closing
				 * it ends the program. This thread keeps drawing until the
				 * shutdown that follows takes the framebuffer down. */
				hal_info("GLFB::%s: window closed, shutting down\n", __func__);
				kill(getpid(), SIGTERM);
				break;
			default:
				break;
		}
	} while (SDL_PollEvent(&ev));
}

bool GLFbPC::producesText(SDL_Keycode key)
{
	if (key >= 0x20 && key < 0x7f)
		return true;
	switch (key)
	{
		case SDLK_KP_0: case SDLK_KP_1: case SDLK_KP_2: case SDLK_KP_3: case SDLK_KP_4:
		case SDLK_KP_5: case SDLK_KP_6: case SDLK_KP_7: case SDLK_KP_8: case SDLK_KP_9:
		case SDLK_KP_PLUS: case SDLK_KP_MINUS: case SDLK_KP_PERIOD:
			return true;
		default:
			return false;
	}
}

void GLFbPC::handleKey(SDL_Keycode key)
{
	hal_debug("GLFB::%s: 0x%x\n", __func__, (unsigned int)key);
	if (key == SDLK_F)
	{
		hal_info("GLFB::%s: toggle fullscreen %s\n", __func__, mFullscreen ? "off" : "on");
		/* the compositor answers with ENTER/LEAVE_FULLSCREEN and a new size */
		SDL_SetWindowFullscreen(mWindow, !mFullscreen);
		return;
	}
	std::map<SDL_Keycode, int>::const_iterator i = mKeyMap.find(key);
	if (i == mKeyMap.end())
		return;
	struct input_event iev;
	memset(&iev, 0, sizeof(iev));
	iev.code = i->second;
	iev.value = 1; /* key down */
	iev.type = EV_KEY;
	gettimeofday(&iev.time, NULL);
	hal_debug("GLFB::%s: pushing 0x%x\n", __func__, iev.code);
	write(input_fd, &iev, sizeof(iev));
	iev.value = 0; /* neutrino is stupid, so push key up directly after key down */
	write(input_fd, &iev, sizeof(iev));
}

int sleep_us = 30000;

void GLFbPC::render()
{
	mReInitLock.lock();
	if (mReInit)
	{
		int xoff = 0;
		int yoff = 0;
		mVAchanged = true;
		mReInit = false;
		mX = &_mX[mFullscreen];
		mY = &_mY[mFullscreen];
		/* the compositor decides the window size, the picture is fitted into it */
		int x = 0, y = 0;
		SDL_GetWindowSizeInPixels(mWindow, &x, &y);
		if (x <= 0 || y <= 0)
		{
			x = mState.width;
			y = mState.height;
		}
		*mX = x;
		*mY = y;
		AVRational a = { x, y };
		if (av_cmp_q(a, mOA) < 0)
			*mY = x * mOA.den / mOA.num;
		else if (av_cmp_q(a, mOA) > 0)
			*mX = y * mOA.num / mOA.den;
		xoff = (x - *mX) / 2;
		yoff = (y - *mY) / 2;
		hal_info("%s: reinit mX:%d mY:%d xoff:%d yoff:%d fs %d\n", __func__, *mX, *mY, xoff, yoff, mFullscreen);
		mViewX = xoff;
		mViewY = yoff;
		glViewport(xoff, yoff, *mX, *mY);
		float aspect = static_cast<float>(*mX) / *mY;
		float osdaspect = static_cast<float>(mOA.den) / mOA.num;

		/* glOrtho(aspect * -osdaspect, aspect * osdaspect, -1.0, 1.0, -1.0, 1.0) */
		mState.xproj = 1.0 / (aspect * osdaspect);
		glClearColor(0.0, 0.0, 0.0, 1.0);

		glEnable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	mReInitLock.unlock();

	uint64_t flags = mRender ? mpv_render_context_update(mRender) : 0;
	restoreGLState();
	cMpvEngine::VideoParams vp;
	vp.valid = false;
	cMpvEngine *engine = cMpvEngine::getInstance();
	if (engine)
		vp = engine->getVideoParams();
	if (!vp.valid)
		mVideoValid = false;
	if ((flags & MPV_RENDER_UPDATE_FRAME) && vp.valid)
		renderVideo(vp);
	if (mVideoValid)
	{
		AVRational a;
		av_reduce(&a.num, &a.den, vp.w, vp.h, INT_MAX);
		if (av_cmp_q(a, mVA))
		{
			mVA = a;
			mVAchanged = true;
		}
	}
	else
		bltDisplayBuffer(); /* decoded video stream */
	if (mState.blit)
	{
		/* only blit manually after fb->blit(), this helps to find missed blit() calls */
		mState.blit = false;
		hal_debug("GLFB::%s blit!\n", __func__);
		bltOSDBuffer(); /* OSD */
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(mViewX, mViewY, *mX, *mY);
	glClear(GL_COLOR_BUFFER_BIT);

	if (mVAchanged)
	{
		mVAchanged = false;
		zoom = 1.0;
		xscale = 1.0;
		int cmp = (mCrop == DISPLAY_AR_MODE_NONE) ? 0 : av_cmp_q(mVA, mOA);
		const AVRational a149 = { 14, 9 };
		switch (cmp)
		{
			default:
			case INT_MIN: /* invalid */
			case 0: /* identical */
				hal_debug("%s: mVA == mOA (or fullscreen mode :-)\n", __func__);
				break;
			case 1: /* mVA > mOA -- video is wider than display */
				hal_debug("%s: mVA > mOA\n", __func__);
				xscale = av_q2d(mVA) / av_q2d(mOA);
				switch (mCrop)
				{
					case DISPLAY_AR_MODE_PANSCAN:
						break;
					case DISPLAY_AR_MODE_LETTERBOX:
						zoom = av_q2d(mOA) / av_q2d(mVA);
						break;
					case DISPLAY_AR_MODE_PANSCAN2:
						zoom = av_q2d(mOA) / av_q2d(a149);
						break;
					default:
						break;
				}
				break;
			case -1: /* mVA < mOA -- video is taller than display */
				hal_debug("%s: mVA < mOA\n", __func__);
				xscale = av_q2d(mVA) / av_q2d(mOA);
				switch (mCrop)
				{
					case DISPLAY_AR_MODE_LETTERBOX:
						break;
					case DISPLAY_AR_MODE_PANSCAN2:
						if (av_cmp_q(a149, mOA) < 0)
						{
							zoom = av_q2d(mVA) * av_q2d(a149) / av_q2d(mOA);
							break;
						}
					/* fallthrough for output format 14:9 */
					case DISPLAY_AR_MODE_PANSCAN:
						zoom = av_q2d(mOA) / av_q2d(mVA);
						break;
					default:
						break;
				}
				break;
		}
	}
	glUniform1f(mState.u_bgra, mVideoValid ? 0.0 : 1.0);
	glBindTexture(GL_TEXTURE_2D, mVideoValid ? mVideoTex : mState.displaytex);
	drawSquare(zoom, xscale);
	glUniform1f(mState.u_bgra, 1.0);
	glBindTexture(GL_TEXTURE_2D, mState.osdtex);
	drawSquare(1.0, -100);

	SDL_GL_SwapWindow(mWindow);
	if (flags & MPV_RENDER_UPDATE_FRAME)
		mpv_render_context_report_swap(mRender);

	GLuint err = glGetError();
	if (err != 0)
		hal_info("GLFB::%s: GLError:%d 0x%04x\n", __func__, err, err);
}

static void *glGetProc(void *, const char *name)
{
	return (void *)SDL_GL_GetProcAddress(name);
}

/* static */ void GLFbPC::renderUpdateCb(void *ctx)
{
	((GLFbPC *)ctx)->wake();
}

void GLFbPC::wake()
{
	if (!mUserEvent)
		return;
	SDL_Event ev;
	SDL_zero(ev);
	ev.type = mUserEvent;
	SDL_PushEvent(&ev);
}

bool GLFbPC::setupRender()
{
	cMpvEngine *engine = cMpvEngine::getInstance();
	if (!engine)
		return false;
	mpv_opengl_init_params gl = { glGetProc, NULL };
	int advanced = 1;
	/* vaapi needs the native display to create its VADisplay */
	SDL_PropertiesID props = SDL_GetWindowProperties(mWindow);
	void *wl = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
	void *x11 = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
	mpv_opengl_drm_params_v2 drm = { -1, -1, -1, NULL, -1 };
	mpv_render_param params[5];
	int n = 0;
	params[n].type = MPV_RENDER_PARAM_API_TYPE;
	params[n++].data = const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL);
	params[n].type = MPV_RENDER_PARAM_OPENGL_INIT_PARAMS;
	params[n++].data = &gl;
	params[n].type = MPV_RENDER_PARAM_ADVANCED_CONTROL;
	params[n++].data = &advanced;
	if (wl)
	{
		params[n].type = MPV_RENDER_PARAM_WL_DISPLAY;
		params[n++].data = wl;
	}
	else if (x11)
	{
		params[n].type = MPV_RENDER_PARAM_X11_DISPLAY;
		params[n++].data = x11;
	}
	else
	{
		/* KMS without a display server: vaapi opens the render node itself */
		drm.render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
		if (drm.render_fd >= 0)
		{
			params[n].type = MPV_RENDER_PARAM_DRM_DISPLAY_V2;
			params[n++].data = &drm;
		}
	}
	params[n].type = MPV_RENDER_PARAM_INVALID;
	params[n].data = NULL;
	int r = mpv_render_context_create(&mRender, engine->getHandle(), params);
	if (r < 0)
	{
		hal_info("GLFB::%s: mpv_render_context_create failed: %s\n", __func__, mpv_error_string(r));
		mRender = NULL;
		return false;
	}
	mpv_render_context_set_update_callback(mRender, renderUpdateCb, this);
	hal_info("GLFB::%s: libmpv render context ready (%s)\n", __func__, wl ? "wayland display" : x11 ? "x11 display" : drm.render_fd >= 0 ? "drm render node" : "no native display");
	return true;
}

void GLFbPC::teardownRender()
{
	if (mRender)
	{
		mpv_render_context_set_update_callback(mRender, NULL, NULL);
		mpv_render_context_free(mRender);
		mRender = NULL;
	}
	if (mVideoFbo)
		glDeleteFramebuffers(1, &mVideoFbo);
	if (mVideoTex)
		glDeleteTextures(1, &mVideoTex);
	mVideoFbo = mVideoTex = 0;
	mVideoValid = false;
}

void GLFbPC::renderVideo(const cMpvEngine::VideoParams &vp)
{
	if (!mVideoFbo || vp.w != mVideoW || vp.h != mVideoH)
	{
		if (!mVideoFbo)
			glGenFramebuffers(1, &mVideoFbo);
		if (!mVideoTex)
			glGenTextures(1, &mVideoTex);
		glBindTexture(GL_TEXTURE_2D, mVideoTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vp.w, vp.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindFramebuffer(GL_FRAMEBUFFER, mVideoFbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mVideoTex, 0);
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			hal_info("GLFB::%s: video FBO %dx%d incomplete: 0x%04x\n", __func__, vp.w, vp.h, status);
			return;
		}
		mVideoW = vp.w;
		mVideoH = vp.h;
		hal_info("GLFB::%s: video FBO %dx%d\n", __func__, vp.w, vp.h);
	}
	mpv_opengl_fbo fbo = { (int)mVideoFbo, mVideoW, mVideoH, 0 };
	int flip = 0;
	mpv_render_param params[3];
	params[0].type = MPV_RENDER_PARAM_OPENGL_FBO;
	params[0].data = &fbo;
	params[1].type = MPV_RENDER_PARAM_FLIP_Y;
	params[1].data = &flip;
	params[2].type = MPV_RENDER_PARAM_INVALID;
	params[2].data = NULL;
	int r = mpv_render_context_render(mRender, params);
	if (r < 0)
		hal_debug("GLFB::%s: mpv_render_context_render: %s\n", __func__, mpv_error_string(r));
	else
		mVideoValid = true;
	restoreGLState();
}

/* mpv uses our GL context for its own objects and leaves its state behind */
void GLFbPC::restoreGLState()
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glUseProgram(mState.program);
	glActiveTexture(GL_TEXTURE0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquation(GL_FUNC_ADD);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glClearColor(0.0, 0.0, 0.0, 1.0);
}

void GLFbPC::drawSquare(float size, float x_factor)
{
	GLfloat vertices[] =
	{
		 1.0f,  1.0f,
		-1.0f,  1.0f,
		-1.0f, -1.0f,
		 1.0f, -1.0f,
	};

	GLfloat texcoords[] =
	{
		1.0, 0.0,
		0.0, 0.0,
		0.0, 1.0,
		1.0, 1.0,
	};
	if (x_factor > -99.0) /* x_factor == -100 => OSD */
	{
		if (videoDecoder &&
			videoDecoder->pig_x > 0 && videoDecoder->pig_y > 0 &&
			videoDecoder->pig_w > 0 && videoDecoder->pig_h > 0)
		{
			/* these calculations even consider cropping and panscan mode
			 * maybe this could be done with some clever opengl tricks? */
			double w2 = (double)mState.width * 0.5l;
			double h2 = (double)mState.height * 0.5l;
			double x = (double)(videoDecoder->pig_x - w2) / w2 / x_factor / size;
			double y = (double)(h2 - videoDecoder->pig_y) / h2 / size;
			double w = (double)videoDecoder->pig_w / w2;
			double h = (double)videoDecoder->pig_h / h2;
			x += ((1.0l - x_factor * size) / 2.0l) * w / x_factor / size;
			y += ((size - 1.0l) / 2.0l) * h / size;
			vertices[0] = x + w; /* top right x */
			vertices[1] = y; /* top right y */
			vertices[2] = x; /* top left x */
			vertices[3] = y; /* top left y */
			vertices[4] = x; /* bottom left x */
			vertices[5] = y - h; /* bottom left y */
			vertices[6] = vertices[0]; /* bottom right x */
			vertices[7] = vertices[5]; /* bottom right y */
		}
	}
	else
		x_factor = 1.0; /* OSD */

	glUniform2f(mState.u_scale, size * x_factor, size);
	glUniform1f(mState.u_xproj, mState.xproj);
	glEnableVertexAttribArray(mState.a_pos);
	glEnableVertexAttribArray(mState.a_tex);
	glVertexAttribPointer(mState.a_pos, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(mState.a_tex, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glDisableVertexAttribArray(mState.a_pos);
	glDisableVertexAttribArray(mState.a_tex);
}


void GLFbPC::bltOSDBuffer()
{
	/* FIXME: copy each time */
	glBindTexture(GL_TEXTURE_2D, mState.osdtex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mState.width, mState.height, GL_RGBA, GL_UNSIGNED_BYTE, osd_buf->data());
}

void GLFbPC::bltDisplayBuffer()
{
	if (!videoDecoder) /* cannot start yet */
		return;
	static bool warn = true;
	cVideo::SWFramebuffer *buf = videoDecoder->getDecBuf();
	if (!buf)
	{
		if (warn)
			hal_info("GLFB::%s did not get a buffer...\n", __func__);
		warn = false;
		return;
	}
	warn = true;
	int w = buf->width(), h = buf->height();
	if (w == 0 || h == 0)
		return;

	AVRational a = buf->AR();
	if (a.den != 0 && a.num != 0 && av_cmp_q(a, _mVA))
	{
		_mVA = a;
		/* _mVA is the raw buffer's aspect, mVA is the real scaled output aspect */
		av_reduce(&mVA.num, &mVA.den, w * a.num, h * a.den, INT_MAX);
		mVAchanged = true;
	}

	glBindTexture(GL_TEXTURE_2D, mState.displaytex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, &(*buf)[0]);

	/* "rate control" mechanism starts here...
	 * this implementation is pretty naive and not working too well, but
	 * better this than nothing... :-) */
	int64_t apts = 0;
	/* 18000 is the magic value for A/V sync in my libao->pulseaudio->intel_hda setup */
	int64_t vpts = buf->pts();
	if (audioDecoder)
		apts = audioDecoder->getPts();
	/* skip A/V sync if PTS is invalid (AV_NOPTS_VALUE = INT64_MIN or
	   any negative value) or audio hasn't started yet (apts == 0) -
	   use frame rate based timing instead */
	if (vpts < 0 || apts <= 0)
	{
		int rate, dummy1, dummy2;
		videoDecoder->getPictureInfo(dummy1, dummy2, rate);
		if (rate > 0)
			sleep_us = 1000000 / rate;
		else
			sleep_us = 40000; /* ~25fps fallback */
	}
	else
	{
		vpts += 18000;
		if (apts != last_apts)
		{
			int rate, dummy1, dummy2;
			if (apts < vpts)
				sleep_us = (sleep_us * 2 + (vpts - apts) * 10 / 9) / 3;
			else if (sleep_us > 1000)
				sleep_us -= 1000;
			last_apts = apts;
			videoDecoder->getPictureInfo(dummy1, dummy2, rate);
			if (rate > 0)
				rate = 1000000 / rate; /* limit to frame rate */
			else
				rate = 40000; /* ~25fps fallback */
			if (sleep_us > rate)
				sleep_us = rate;
			else if (sleep_us < 1)
				sleep_us = 1;
		}
	}
	/* drain buffer when it's filling up - reduce sleep to prevent
	   overflow and the stuttering/freezing it causes */
	if (videoDecoder->buf_num > VDEC_MAXBUFS / 2)
		sleep_us /= 2;
	const double av_diff = ((double)buf->pts() - (double)apts) / 90000.0;
	hal_debug("vpts: 0x%" PRIx64 " apts: 0x%" PRIx64 " diff: %6.3f sleep_us %d buf %d\n",
		  buf->pts(), apts, av_diff, sleep_us, videoDecoder->buf_num);
}
