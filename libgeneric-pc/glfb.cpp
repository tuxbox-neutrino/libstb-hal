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
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "glfb_priv.h"
#include "video_lib.h"
#include "audio_lib.h"

#include "hal_debug.h"

#define hal_debug_c(args...) _hal_debug(HAL_DEBUG_INIT, NULL, args)
#define hal_info_c(args...) _hal_info(HAL_DEBUG_INIT, NULL, args)
#define hal_debug(args...) _hal_debug(HAL_DEBUG_INIT, this, args)
#define hal_info(args...) _hal_info(HAL_DEBUG_INIT, this, args)

/* mpv 0.35 renamed the interop that puts DRM PRIME frames on a plane */
#if MPV_CLIENT_API_VERSION >= MPV_MAKE_VERSION(2, 0)
#define DRMPRIME_OVERLAY_INTEROP "drmprime-overlay"
#else
#define DRMPRIME_OVERLAY_INTEROP "drmprime-drm"
#endif


extern cVideo *videoDecoder;
extern cAudio *audioDecoder;
extern int sleep_us;
extern bool HAL_live_mpv;

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

bool GLFramebuffer::setOSDResolution(int x, int y)
{
	if (!glfb_priv->setOSDResolution(x, y))
		return false;
	si = glfb_priv->getScreenInfo();
	return true;
}

/* the OSD buffer was made for the largest size, so it stays where it is and
 * whoever draws into it keeps its pointer */
bool GLFbPC::setOSDResolution(int x, int y)
{
	if (x <= 0 || y <= 0 || (size_t)x * y * 4 * 2 > osd_buf->size())
		return false;

	mReInitLock.lock();
	mState.width = x;
	mState.height = y;
	si.xres = si.xres_virtual = x;
	si.yres = si.yres_virtual = y;
	mOsdResize = true;
	mReInit = true;
	mReInitLock.unlock();
	hal_info("GLFB::%s: %dx%d\n", __func__, x, y);
	wake();
	return true;
}

void GLFbPC::setDisplayMode(int w, int h, float rate)
{
	mReInitLock.lock();
	mWantW = w;
	mWantH = h;
	mWantRate = rate;
	mModeChange = true;
	mReInitLock.unlock();
	wake();
}

/* only on the GL thread */
void GLFbPC::applyDisplayMode(int w, int h, float rate, const char *who)
{
	SDL_DisplayMode mode;
	if (SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(mWindow), w, h, rate, false, &mode) &&
	    SDL_SetWindowFullscreenMode(mWindow, &mode))
	{
		SDL_SyncWindow(mWindow);
		hal_info("GLFB: %s asked for %dx%d at %.2f Hz, using %dx%d at %.2f Hz\n", who, w, h, rate,
			 mode.w, mode.h, mode.refresh_rate);
	}
	else
		hal_info("GLFB: no display mode for %dx%d at %.2f Hz (%s): %s\n", w, h, rate, who, SDL_GetError());
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
	mFramePending = false;
	mPadKey = 0;
	mPadNext = 0;
	memset(mOsdBox, 0, sizeof(mOsdBox));
	mTexStale = false;
	mDirect = false;
	mWinW = mWinH = 0;
	mPlaneOK = false;
	mOnPlane = false;
	mDrmFd = -1;
	mPlaneReq = NULL;
	mPlaneFbo = mPlaneTex = 0;
	mPlaneFboW = mPlaneFboH = 0;
	mOsdSum = 0;
	mPlaneRedraw = true;
	mMargins[0] = mMargins[1] = mMargins[2] = mMargins[3] = 0;
	mFit[0] = mFit[1] = 0;
	mOsdW = x;
	mOsdH = y;
	mOsdResize = false;
	mWantW = mWantH = 0;
	mWantRate = 0;
	mModeChange = false;
	memset(mLastPig, 0, sizeof(mLastPig));
	mLastDirect = false;
	memset(mPadAxis, 0, sizeof(mPadAxis));

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
#ifdef SDL_HINT_KMSDRM_DISPLAY_PLANE
	/* on KMS the window goes to an overlay plane, so that the primary one
	 * below it is free for the video; GLFB_VIDEO_PLANE=0 keeps it all in GL.
	 * An SDL without the hint keeps the window on the primary plane, and
	 * the video stays in GL. */
	const char *plane = getenv("GLFB_VIDEO_PLANE");
	if (!plane || strcmp(plane, "0"))
		SDL_SetHint(SDL_HINT_KMSDRM_DISPLAY_PLANE, "overlay");
#endif
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		hal_info("GLFB: SDL_Init failed: %s\n", SDL_GetError());
		_exit(1); /* Life is hard */
	}
	/* gamepads are opened as they show up, see pollEvents() */
	if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
		hal_info("GLFB: no gamepad support: %s\n", SDL_GetError());
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
	/* GLFB_MODE=<width>x<height>[@<rate>] picks the mode of the display for
	 * the fullscreen window; without it the display stays as it is. On KMS
	 * that is what the television prefers, 4K at whatever rate the board can
	 * drive, while broadcasts want 50 Hz far more than they want pixels. */
	const char *want = getenv("GLFB_MODE");
	if (want && glfb_priv->mFullscreen)
	{
		int w = 0, h = 0;
		float rate = 0;
		if (sscanf(want, "%dx%d@%f", &w, &h, &rate) >= 2)
			glfb_priv->applyDisplayMode(w, h, rate, "GLFB_MODE");
		else
			hal_info("GLFB: GLFB_MODE=%s is not <width>x<height>[@<rate>]\n", want);
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
	/* room for the largest OSD, see setOSDResolution() */
	int fbmem = (x > 1920 ? x : 1920) * (y > 1080 ? y : 1080) * 4 * 2;
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
	if (!HAL_live_mpv && !mVideoValid && videoDecoder)
		timeout = sleep_us > 1000 ? sleep_us / 1000 : 1;
	if (mFramePending)
		timeout = 5;
	if (mPadKey)
	{
		uint64_t now = SDL_GetTicks();
		int wait = mPadNext > now ? (int)(mPadNext - now) : 0;
		if (wait < timeout)
			timeout = wait;
	}
	bool got = SDL_WaitEventTimeout(&ev, timeout);
	padRepeat();
	if (!got)
		return;
	do
	{
		switch (ev.type)
		{
			case SDL_EVENT_GAMEPAD_ADDED:
				if (SDL_OpenGamepad(ev.gdevice.which))
					hal_info("GLFB::%s: gamepad '%s'\n", __func__, SDL_GetGamepadNameForID(ev.gdevice.which));
				break;
			case SDL_EVENT_GAMEPAD_REMOVED:
				SDL_CloseGamepad(SDL_GetGamepadFromID(ev.gdevice.which));
				mPadKey = 0;
				memset(mPadAxis, 0, sizeof(mPadAxis));
				break;
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
			case SDL_EVENT_GAMEPAD_BUTTON_UP:
				padButton(ev.gbutton.button, ev.gbutton.down);
				break;
			case SDL_EVENT_GAMEPAD_AXIS_MOTION:
				padAxis(ev.gaxis.axis, ev.gaxis.value);
				break;
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
	/* on KMS there is no desktop to leave the fullscreen window for */
	const char *driver = SDL_GetCurrentVideoDriver();
	if (key == SDLK_F && !(driver && !strcmp(driver, "kmsdrm")))
	{
		hal_info("GLFB::%s: toggle fullscreen %s\n", __func__, mFullscreen ? "off" : "on");
		/* the compositor answers with ENTER/LEAVE_FULLSCREEN and a new size */
		SDL_SetWindowFullscreen(mWindow, !mFullscreen);
		return;
	}
	std::map<SDL_Keycode, int>::const_iterator i = mKeyMap.find(key);
	if (i == mKeyMap.end())
		return;
	pushKey(i->second);
}

void GLFbPC::pushKey(int code)
{
	struct input_event iev;
	memset(&iev, 0, sizeof(iev));
	iev.code = code;
	iev.value = 1; /* key down */
	iev.type = EV_KEY;
	gettimeofday(&iev.time, NULL);
	hal_debug("GLFB::%s: pushing 0x%x\n", __func__, iev.code);
	write(input_fd, &iev, sizeof(iev));
	iev.value = 0; /* neutrino is stupid, so push key up directly after key down */
	write(input_fd, &iev, sizeof(iev));
}

/* the first repeat of a held gamepad key comes late, the others quickly */
#define PAD_REPEAT_DELAY	400
#define PAD_REPEAT_RATE		120
/* how far the stick or a trigger has to go, and how far back to let go */
#define PAD_AXIS_ON		20000
#define PAD_AXIS_OFF		12000

static bool pad_repeats(int code)
{
	switch (code)
	{
		case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
		case KEY_PAGEUP: case KEY_PAGEDOWN:
		case KEY_VOLUMEUP: case KEY_VOLUMEDOWN:
			return true;
		default:
			return false;
	}
}

void GLFbPC::padPress(int code)
{
	pushKey(code);
	if (pad_repeats(code))
	{
		mPadKey = code;
		mPadNext = SDL_GetTicks() + PAD_REPEAT_DELAY;
	}
	else
		mPadKey = 0;
}

void GLFbPC::padRelease(int code)
{
	if (mPadKey == code)
		mPadKey = 0;
}

void GLFbPC::padRepeat()
{
	if (!mPadKey || SDL_GetTicks() < mPadNext)
		return;
	pushKey(mPadKey);
	mPadNext = SDL_GetTicks() + PAD_REPEAT_RATE;
}

/* SDL names the buttons by where they are, so this fits every gamepad it
 * knows. The names here are the ones on a PlayStation pad. */
void GLFbPC::padButton(int button, bool down)
{
	int code;
	switch (button)
	{
		case SDL_GAMEPAD_BUTTON_DPAD_UP:	code = KEY_UP; break;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN:	code = KEY_DOWN; break;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT:	code = KEY_LEFT; break;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:	code = KEY_RIGHT; break;
		case SDL_GAMEPAD_BUTTON_SOUTH:		code = KEY_OK; break;	/* cross */
		case SDL_GAMEPAD_BUTTON_WEST:		code = KEY_EXIT; break;	/* square */
		case SDL_GAMEPAD_BUTTON_EAST:		code = KEY_HOME; break;	/* circle */
		case SDL_GAMEPAD_BUTTON_NORTH:		code = KEY_WWW; break;	/* triangle */
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:	code = KEY_RED; break;
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:	code = KEY_GREEN; break;
		case SDL_GAMEPAD_BUTTON_START:		code = KEY_MENU; break;
		case SDL_GAMEPAD_BUTTON_GUIDE:		code = KEY_MENU; break;
		case SDL_GAMEPAD_BUTTON_BACK:		code = KEY_HELP; break;	/* create */
		case SDL_GAMEPAD_BUTTON_RIGHT_STICK:	code = KEY_INFO; break;
		case SDL_GAMEPAD_BUTTON_TOUCHPAD:	code = KEY_MUTE; break;
		default:
			hal_debug("GLFB::%s: button %d is not used\n", __func__, button);
			return;
	}
	hal_debug("GLFB::%s: button %d %s -> 0x%x\n", __func__, button, down ? "down" : "up", code);
	if (down)
		padPress(code);
	else
		padRelease(code);
}

/* the left stick works like the direction pad, the right one sets the volume
 * and pages, and the triggers are the two colours the shoulder buttons lack */
void GLFbPC::padAxis(int axis, int value)
{
	int code = 0;
	switch (axis)
	{
		case SDL_GAMEPAD_AXIS_LEFTX:
			code = value < 0 ? KEY_LEFT : KEY_RIGHT;
			break;
		case SDL_GAMEPAD_AXIS_LEFTY:
			code = value < 0 ? KEY_UP : KEY_DOWN;
			break;
		case SDL_GAMEPAD_AXIS_RIGHTX:
			code = value < 0 ? KEY_PAGEUP : KEY_PAGEDOWN;
			break;
		case SDL_GAMEPAD_AXIS_RIGHTY:
			code = value < 0 ? KEY_VOLUMEUP : KEY_VOLUMEDOWN;
			break;
		case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
			code = KEY_YELLOW;
			break;
		case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
			code = KEY_BLUE;
			break;
		default:
			return;
	}
	if (value < 0)
		value = -value;

	int &held = mPadAxis[axis];
	if (value >= PAD_AXIS_ON)
	{
		if (held != code)
		{
			hal_debug("GLFB::%s: axis %d -> 0x%x\n", __func__, axis, code);
			held = code;
			padPress(code);
		}
	}
	else if (value <= PAD_AXIS_OFF && held)
	{
		padRelease(held);
		held = 0;
	}
}

int sleep_us = 30000;

void GLFbPC::render()
{
	/* where the time of a frame goes, for the debug output */
	static uint64_t stat_since, stat_video, stat_draw, stat_swap, stat_max;
	static int stat_frames;
	uint64_t t_start = SDL_GetTicksNS();

	mReInitLock.lock();
	if (mModeChange)
	{
		mModeChange = false;
		/* a desktop keeps its mode, the window is scaled there */
		const char *driver = SDL_GetCurrentVideoDriver();
		if (mFullscreen && driver && !strcmp(driver, "kmsdrm"))
			applyDisplayMode(mWantW, mWantH, mWantRate, "the video system");
	}
	if (mOsdResize)
	{
		mOsdResize = false;
		mOsdW = mState.width;
		mOsdH = mState.height;
		glBindTexture(GL_TEXTURE_2D, mState.osdtex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mOsdW, mOsdH, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
		memset(mOsdBox, 0, sizeof(mOsdBox));
		mState.blit = true;
	}
	const bool reinit = mReInit;
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
		mWinW = x;
		mWinH = y;
		AVRational a = { x, y };
		if (av_cmp_q(a, mOA) < 0)
			*mY = x * mOA.den / mOA.num;
		else if (av_cmp_q(a, mOA) > 0)
			*mX = y * mOA.num / mOA.den;
		xoff = (x - *mX) / 2;
		yoff = (y - *mY) / 2;
		hal_info("%s: reinit mX:%d mY:%d xoff:%d yoff:%d fs %d\n", __func__, *mX, *mY, xoff, yoff, mFullscreen);
		const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(mWindow));
		if (mode)
			hal_info("%s: display mode %dx%d at %.2f Hz\n", __func__, mode->w, mode->h, mode->refresh_rate);
		/* mpv picks the frames to show by the display's rate; without it,
		 * a 50 Hz programme on a 25 Hz display loses more than every other one */
		cMpvEngine *e = cMpvEngine::getInstance();
		if (e && mode && mode->refresh_rate > 0)
			e->setDisplayFps(mode->refresh_rate);
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

	/* The video on the plane below the window: the GPU only draws the OSD,
	 * and only when it has changed. */
	const bool on_plane = mPlaneOK && vp.valid && vp.drmprime;
	if (on_plane != mOnPlane)
	{
		mOnPlane = on_plane;
		mState.blit = true; /* the window is drawn anew in either case */
		mPlaneRedraw = true;
		hal_info("GLFB::%s: the video is %s\n", __func__, on_plane ? "on the plane below the window" : "drawn in the window");
	}
	if (on_plane)
	{
		static uint64_t plane_since, plane_time, plane_max;
		static int plane_frames;
		if (flags & MPV_RENDER_UPDATE_FRAME)
		{
			bool draw = engine && engine->renderBegin();
			mFramePending = false;
			if (draw)
			{
				uint64_t t0 = SDL_GetTicksNS();
				renderPlane();
				uint64_t t = SDL_GetTicksNS() - t0;
				mpv_render_context_report_swap(mRender);
				mVideoValid = true;
				mTexStale = true;
				plane_frames++;
				plane_time += t;
				if (t > plane_max)
					plane_max = t;
			}
			else if (mRender)
			{
				int skip = 1;
				mpv_render_param params[2];
				params[0].type = MPV_RENDER_PARAM_SKIP_RENDERING;
				params[0].data = &skip;
				params[1].type = MPV_RENDER_PARAM_INVALID;
				params[1].data = NULL;
				mpv_render_context_render(mRender, params);
			}
			if (engine)
				engine->renderEnd();
		}
		uint64_t now = SDL_GetTicksNS();
		if (now - plane_since >= 10 * SDL_NS_PER_SECOND)
		{
			if (plane_frames)
				hal_debug("GLFB::%s: %d video frames on the plane in %.1f s, %.1f ms per frame until it is up, longest %.1f ms\n",
					  __func__, plane_frames, (now - plane_since) / 1e9,
					  plane_time / 1e6 / plane_frames, plane_max / 1e6);
			plane_since = now;
			plane_frames = 0;
			plane_time = plane_max = 0;
		}
		if (!mState.blit && !reinit)
			return;
		mState.blit = false;

		/* neutrino blits four times a second whether anything has changed
		 * or not; each swap is a flip of the window's plane, and the video
		 * flip that follows has to wait a frame for it */
		uint64_t sum = osdChecksum();
		if (sum == mOsdSum && !reinit && !mPlaneRedraw)
			return;
		mOsdSum = sum;
		mPlaneRedraw = false;

		/* the window: transparent where nothing but the video is */
		bltOSDBuffer();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(mViewX, mViewY, *mX, *mY);
		glClearColor(0.0, 0.0, 0.0, 0.0);
		glClear(GL_COLOR_BUFFER_BIT);
		glClearColor(0.0, 0.0, 0.0, 1.0);
		drawOSD();
		SDL_GL_SwapWindow(mWindow);
		return;
	}

	/* Drawing the video into a texture first and that onto the window costs
	 * a pass over every pixel that small GPUs do not have to spare at 50
	 * frames per second. Where the picture fills the window anyway, mpv
	 * draws straight into it, further down. */
	const bool direct = vp.valid && directVideo(vp.w, vp.h);
	bool direct_held = false;
	bool mpv_drew = false;

	/* Nothing has changed since the last swap: leave what is on the screen
	 * alone. A swap waits for the display, and the next video frame would
	 * be late by the time it returns. */
	int pig[4] = { 0, 0, 0, 0 };
	if (videoDecoder)
	{
		pig[0] = videoDecoder->pig_x;
		pig[1] = videoDecoder->pig_y;
		pig[2] = videoDecoder->pig_w;
		pig[3] = videoDecoder->pig_h;
	}
	const bool same = !memcmp(pig, mLastPig, sizeof(pig)) && direct == mLastDirect;
	memcpy(mLastPig, pig, sizeof(pig));
	mLastDirect = direct;
	if (mVideoValid && same && !(flags & MPV_RENDER_UPDATE_FRAME) && !mState.blit && !reinit &&
	    !mVAchanged && !mFramePending)
		return;
	if (flags & MPV_RENDER_UPDATE_FRAME)
	{
		bool draw = engine && engine->renderBegin();
		mFramePending = false;
		if (draw && vp.valid && direct)
			direct_held = true;
		else if (draw && vp.valid)
		{
			renderVideo(vp);
			mpv_drew = true;
		}
		else if (draw)
		{
			/* the frame is here before its size is: leave it where it is
			 * and look again in a moment. It may be the only one, of a
			 * file that was loaded paused. */
			mFramePending = true;
		}
		else if (mRender)
		{
			/* a frame of a stream that is being replaced: tell mpv it is
			 * dealt with, it would wait for it otherwise */
			int skip = 1;
			mpv_render_param params[2];
			params[0].type = MPV_RENDER_PARAM_SKIP_RENDERING;
			params[0].data = &skip;
			params[1].type = MPV_RENDER_PARAM_INVALID;
			params[1].data = NULL;
			mpv_render_context_render(mRender, params);
		}
		if (engine && !direct_held)
			engine->renderEnd();
	}
	else if (mVideoValid && !direct && mTexStale && vp.valid)
	{
		/* back from drawing directly, without a new frame: the texture
		 * still has an old one */
		if (engine && engine->renderBegin())
		{
			renderVideo(vp);
			mpv_drew = true;
		}
		if (engine)
			engine->renderEnd();
	}
	uint64_t t_video = SDL_GetTicksNS();
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
	bool drawn = false;
	if (direct && (direct_held || mVideoValid) && vp.valid)
	{
		/* without a new frame mpv draws the one it has again */
		if (!direct_held && engine)
			direct_held = engine->renderBegin() ? true : (engine->renderEnd(), false);
		if (direct_held)
		{
			drawn = renderDirect();
			mpv_drew = true;
		}
	}
	if (direct_held && engine)
		engine->renderEnd();
	if (mVideoValid && drawn != mDirect)
	{
		mDirect = drawn;
		hal_info("GLFB::%s: the video is drawn %s\n", __func__, drawn ? "straight into the window" : "through a texture");
	}
	if (!drawn)
	{
		const bool video = mVideoValid && !mTexStale;
		/* with mpv decoding everything the other texture only ever holds a
		 * still picture; when none is up, the screen stays black between
		 * two channels instead of showing the last one again */
		const bool still = !HAL_live_mpv || (videoDecoder && videoDecoder->stillpicture);
		if (video || still)
		{
			glDisable(GL_BLEND);
			glUniform1f(mState.u_bgra, video ? 0.0 : 1.0);
			glBindTexture(GL_TEXTURE_2D, video ? mVideoTex : mState.displaytex);
			drawSquare(zoom, xscale);
			glEnable(GL_BLEND);
		}
	}
	drawOSD();

	uint64_t t_draw = SDL_GetTicksNS();
	SDL_GL_SwapWindow(mWindow);
	uint64_t t_swap = SDL_GetTicksNS();
	/* a redraw takes a frame along that arrived in the meantime, so mpv is
	 * told about every swap of something it drew, or it would wait for one */
	if (mpv_drew && mRender)
		mpv_render_context_report_swap(mRender);
	if (flags & MPV_RENDER_UPDATE_FRAME)
	{
		stat_frames++;
		stat_video += t_video - t_start;
		stat_draw += t_draw - t_video;
		stat_swap += t_swap - t_draw;
		if (t_swap - t_start > stat_max)
			stat_max = t_swap - t_start;
	}
	if (t_swap - stat_since >= 10 * SDL_NS_PER_SECOND)
	{
		if (stat_frames)
			hal_debug("GLFB::%s: %d video frames in %.1f s, per frame %.1f ms video, %.1f ms OSD and drawing, %.1f ms swap, longest %.1f ms\n",
				  __func__, stat_frames, (t_swap - stat_since) / 1e9,
				  stat_video / 1e6 / stat_frames, stat_draw / 1e6 / stat_frames,
				  stat_swap / 1e6 / stat_frames, stat_max / 1e6);
		stat_since = t_swap;
		stat_frames = 0;
		stat_video = stat_draw = stat_swap = stat_max = 0;
	}

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
#ifdef SDL_HINT_KMSDRM_DISPLAY_PLANE
		const char *driver = SDL_GetCurrentVideoDriver();
		const char *plane = getenv("GLFB_VIDEO_PLANE");
		if (driver && !strcmp(driver, "kmsdrm") && (!plane || strcmp(plane, "0")))
			mPlaneOK = setupPlane(&drm);
#endif
		if (drm.render_fd >= 0 || mPlaneOK)
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
	if (mPlaneOK)
		hal_info("GLFB::%s: DRM PRIME video goes to the primary plane of CRTC %d, the window is above it\n", __func__, drm.crtc_id);
	return true;
}

/* The CRTC and connector of the display, which SDL does not tell: the
 * connected connector and the CRTC its encoder drives. */
bool GLFbPC::setupPlane(void *params)
{
	mpv_opengl_drm_params_v2 *drm = (mpv_opengl_drm_params_v2 *)params;
	SDL_PropertiesID props = SDL_GetWindowProperties(mWindow);
	int fd = (int)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_KMSDRM_DRM_FD_NUMBER, -1);
	if (fd < 0)
		return false;

	int crtc = -1, connector = -1;
	drmModeRes *res = drmModeGetResources(fd);
	for (int i = 0; res && i < res->count_connectors && crtc < 0; i++)
	{
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c)
			continue;
		if (c->connection == DRM_MODE_CONNECTED && c->encoder_id)
		{
			drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id);
			if (e && e->crtc_id)
			{
				crtc = e->crtc_id;
				connector = c->connector_id;
			}
			drmModeFreeEncoder(e);
		}
		drmModeFreeConnector(c);
	}
	drmModeFreeResources(res);
	if (crtc < 0)
	{
		hal_info("GLFB::%s: no active CRTC, the video stays in GL\n", __func__);
		return false;
	}

	cMpvEngine *engine = cMpvEngine::getInstance();
	/* only the overlay interop: the frames are never drawn by the GPU */
	engine->setString("gpu-hwdec-interop", DRMPRIME_OVERLAY_INTEROP);
	engine->setString("drm-drmprime-video-plane", "primary");
	engine->setString("drm-draw-plane", "overlay");

	mDrmFd = fd;
	drm->fd = fd;
	drm->crtc_id = crtc;
	drm->connector_id = connector;
	drm->atomic_request_ptr = &mPlaneReq;
	return true;
}

/* mpv puts the frame on the plane by adding to our atomic request, which
 * is committed here; the GPU does no more than clear the target. The
 * target has the size of the window: mpv places the picture on the plane
 * where it would draw it there. */
bool GLFbPC::renderPlane()
{
	int w = mWinW > 0 ? mWinW : 1, h = mWinH > 0 ? mWinH : 1;
	if (!mPlaneFbo || w != mPlaneFboW || h != mPlaneFboH)
	{
		if (!mPlaneFbo)
			glGenFramebuffers(1, &mPlaneFbo);
		if (!mPlaneTex)
			glGenTextures(1, &mPlaneTex);
		glBindTexture(GL_TEXTURE_2D, mPlaneTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glBindFramebuffer(GL_FRAMEBUFFER, mPlaneFbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mPlaneTex, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		mPlaneFboW = w;
		mPlaneFboH = h;
	}
	planeMargins();

	mPlaneReq = drmModeAtomicAlloc();
	mpv_opengl_fbo fbo = { (int)mPlaneFbo, w, h, 0 };
	/* mpv would wait for the frame's time and the commit for the vblank
	 * after it, a frame late; the vblank alone keeps the time */
	int block = 0;
	mpv_render_param params[3];
	params[0].type = MPV_RENDER_PARAM_OPENGL_FBO;
	params[0].data = &fbo;
	params[1].type = MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME;
	params[1].data = &block;
	params[2].type = MPV_RENDER_PARAM_INVALID;
	params[2].data = NULL;
	glDisable(GL_BLEND);
	int r = mpv_render_context_render(mRender, params);
	restoreGLState();
	/* The commit waits for the flip, which paces the frames to the display
	 * and lets the swap that is reported next to mpv be the real one. */
	int c = (r >= 0 && mPlaneReq) ? drmModeAtomicCommit(mDrmFd, mPlaneReq, 0, NULL) : -1;
	if (mPlaneReq)
		drmModeAtomicFree(mPlaneReq);
	mPlaneReq = NULL;
	static bool warned = false;
	if (c < 0 && !warned)
	{
		warned = true;
		hal_info("GLFB::%s: render %d, commit %d: %m\n", __func__, r, c);
	}
	return r >= 0;
}

/* the picture in the menus: what the GL quad got as position, mpv gets as
 * the room it leaves around the video */
void GLFbPC::planeMargins()
{
	double m[4] = { 0, 0, 0, 0 };
	if (videoDecoder && videoDecoder->pig_x > 0 && videoDecoder->pig_y > 0 &&
	    videoDecoder->pig_w > 0 && videoDecoder->pig_h > 0 && mOsdW > 0 && mOsdH > 0)
	{
		m[0] = (double)videoDecoder->pig_x / mOsdW;
		m[1] = (double)videoDecoder->pig_y / mOsdH;
		m[2] = 1.0 - (double)(videoDecoder->pig_x + videoDecoder->pig_w) / mOsdW;
		m[3] = 1.0 - (double)(videoDecoder->pig_y + videoDecoder->pig_h) / mOsdH;
		for (int i = 0; i < 4; i++)
			m[i] = m[i] < 0 ? 0 : (m[i] > 1 ? 1 : m[i]);
	}
	cMpvEngine *engine = cMpvEngine::getInstance();
	if (engine && memcmp(m, mMargins, sizeof(m)))
	{
		memcpy(mMargins, m, sizeof(m));
		engine->setVideoMargins(m[0], m[1], m[2], m[3]);
	}

	/* the 4:3 modes, as the GL quad does them: letterbox shows all of the
	 * picture, panscan fills the window, 14:9 goes half the way, and
	 * "none" stretches it to the window */
	double fit[2] = { 0, 0 };
	if (m[0] > 0 || m[1] > 0 || m[2] > 0 || m[3] > 0)
	{
		/* the picture in the menus fills its box, as a box with a video
		 * scaler does it */
		double w = (1.0 - m[0] - m[2]) * mWinW, h = (1.0 - m[1] - m[3]) * mWinH;
		if (w > 0 && h > 0)
			fit[1] = w / h;
	}
	else switch (mCrop)
	{
		case DISPLAY_AR_MODE_PANSCAN:
			fit[0] = 1.0;
			break;
		case DISPLAY_AR_MODE_PANSCAN2:
			fit[0] = 0.5;
			break;
		case DISPLAY_AR_MODE_NONE:
			fit[1] = av_q2d(mOA);
			break;
		default:
			break;
	}
	if (engine && memcmp(fit, mFit, sizeof(fit)))
	{
		memcpy(mFit, fit, sizeof(fit));
		engine->setVideoFit(fit[0], fit[1]);
	}
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
	/* mpv expects the state a fresh context has, and would blend its
	 * picture with what is there already */
	glDisable(GL_BLEND);
	int r = mpv_render_context_render(mRender, params);
	if (r < 0)
		hal_debug("GLFB::%s: mpv_render_context_render: %s\n", __func__, mpv_error_string(r));
	else
	{
		mVideoValid = true;
		mTexStale = false;
	}
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
	/* the alpha that ends up in the window counts where the video plane
	 * below it shows; this keeps it right, premultiplied */
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
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
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mOsdW, mOsdH, GL_RGBA, GL_UNSIGNED_BYTE, osd_buf->data());

	/* where the OSD shows anything at all: nothing else of it has to be
	 * drawn, and while a programme is watched that is mostly nothing */
	const uint32_t *p = (const uint32_t *)osd_buf->data();
	const int w = mOsdW, h = mOsdH;
	int x0 = w, x1 = -1, y0 = h, y1 = -1;
	for (int y = 0; y < h; y++, p += w)
	{
		int l = 0;
		while (l < w && !(p[l] & 0xff000000))
			l++;
		if (l == w)
			continue;
		int r = w - 1;
		while (!(p[r] & 0xff000000))
			r--;
		if (l < x0)
			x0 = l;
		if (r > x1)
			x1 = r;
		if (y < y0)
			y0 = y;
		y1 = y;
	}
	mOsdBox[0] = x0;
	mOsdBox[1] = y0;
	mOsdBox[2] = x1 + 1;
	mOsdBox[3] = y1 + 1;
}

/* cheap enough for a few times a second, and an unchanged OSD has the
 * same sum */
uint64_t GLFbPC::osdChecksum()
{
	const uint64_t *p = (const uint64_t *)osd_buf->data();
	size_t n = (size_t)mOsdW * mOsdH * 4 / sizeof(uint64_t);
	uint64_t a = 0, b = 0;
	for (size_t i = 0; i < n; i++)
	{
		a += p[i];
		b ^= p[i] + i;
	}
	return a ^ (b << 1) ^ n;
}

void GLFbPC::drawOSD()
{
	if (mOsdBox[2] <= mOsdBox[0] || mOsdBox[3] <= mOsdBox[1])
		return;

	/* the quad covers the viewport, the scissor keeps the work to the part
	 * of it that shows something */
	const float half = *mX / 2.0f;
	const float u0 = (float)mOsdBox[0] / mOsdW, u1 = (float)mOsdBox[2] / mOsdW;
	const float v0 = (float)mOsdBox[1] / mOsdH, v1 = (float)mOsdBox[3] / mOsdH;
	int sx0 = mViewX + (int)(half * (1.0f + mState.xproj * (2.0f * u0 - 1.0f))) - 2;
	int sx1 = mViewX + (int)(half * (1.0f + mState.xproj * (2.0f * u1 - 1.0f))) + 3;
	int sy0 = mViewY + (int)(*mY * (1.0f - v1)) - 2;
	int sy1 = mViewY + (int)(*mY * (1.0f - v0)) + 3;
	if (sx0 < 0)
		sx0 = 0;
	if (sy0 < 0)
		sy0 = 0;
	glEnable(GL_SCISSOR_TEST);
	glScissor(sx0, sy0, sx1 - sx0, sy1 - sy0);
	glUniform1f(mState.u_bgra, 1.0);
	glBindTexture(GL_TEXTURE_2D, mState.osdtex);
	drawSquare(1.0, -100);
	glDisable(GL_SCISSOR_TEST);
}

/* the video fills the viewport as it is: mpv can draw it there itself. It
 * is given the whole window and centres the picture like the viewport is. */
bool GLFbPC::directVideo(int w, int h)
{
	/* a picture smaller than the window is cheaper the other way round:
	 * mpv's conversion costs per pixel it writes, a plain copy of its
	 * result to the window much less */
	if ((int64_t)w * h < (int64_t)mWinW * mWinH)
		return false;
	if (!mRender || mVAchanged || zoom != 1.0 || xscale != 1.0 || mWinW <= 0 || mWinH <= 0)
		return false;
	if (mState.xproj < 0.999 || mState.xproj > 1.001)
		return false;
	if (av_cmp_q(mVA, mOA))
		return false;
	if (videoDecoder && videoDecoder->pig_x > 0 && videoDecoder->pig_y > 0 &&
	    videoDecoder->pig_w > 0 && videoDecoder->pig_h > 0)
		return false;
	return true;
}

bool GLFbPC::renderDirect()
{
	mpv_opengl_fbo fbo = { 0, mWinW, mWinH, 0 };
	int flip = 1;
	mpv_render_param params[3];
	params[0].type = MPV_RENDER_PARAM_OPENGL_FBO;
	params[0].data = &fbo;
	params[1].type = MPV_RENDER_PARAM_FLIP_Y;
	params[1].data = &flip;
	params[2].type = MPV_RENDER_PARAM_INVALID;
	params[2].data = NULL;
	/* mpv expects the state a fresh context has, and would blend its
	 * picture with what is there already */
	glDisable(GL_BLEND);
	int r = mpv_render_context_render(mRender, params);
	restoreGLState();
	glViewport(mViewX, mViewY, *mX, *mY);
	if (r < 0)
	{
		hal_debug("GLFB::%s: mpv_render_context_render: %s\n", __func__, mpv_error_string(r));
		return false;
	}
	mVideoValid = true;
	mTexStale = true;
	return true;
}

void GLFbPC::bltDisplayBuffer()
{
	if (!videoDecoder) /* cannot start yet */
		return;
	static bool warn = true;
	static bool still_shown = false;
	if (HAL_live_mpv && !videoDecoder->stillpicture)
	{
		/* this texture only ever holds a still picture then; once that is
		 * taken down, nothing of it may show up between two channels */
		if (still_shown)
		{
			static const uint32_t black = 0xff000000;
			glBindTexture(GL_TEXTURE_2D, mState.displaytex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &black);
			still_shown = false;
		}
		return;
	}
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
	still_shown = true;
	if (HAL_live_mpv)
		return; /* nothing to pace, mpv does its own A/V sync */

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
