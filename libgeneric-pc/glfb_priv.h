/*
    Copyright 2010 Carsten Juttner <carjay@gmx.net>
    Copyright 2012,2013,2016 Stefan Seyfried <seife@tuxboxcvs.slipkontur.de>

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

    ********************************************************************
    private stuff of the GLFB thread that is only used inside libstb-hal
    and not exposed to the application.
*/

#ifndef __glfb_priv__
#define __glfb_priv__
#include <stdint.h>
#include <OpenThreads/Mutex>
#include <vector>
#include <map>
#include <SDL3/SDL.h>
#include <GLES2/gl2.h>
#include <linux/fb.h> /* for screeninfo etc. */
#include "glfb.h"
#include "mpv_player.h"
struct mpv_render_context;
extern "C" {
#include <libavutil/rational.h>
}

class GLFbPC
{
	public:
		GLFbPC(int x, int y, std::vector<unsigned char> &buf);
		~GLFbPC();
		std::vector<unsigned char> *getOSDBuffer()
		{
			return osd_buf; /* pointer to OSD bounce buffer */
		}
		int getOSDWidth()
		{
			return mState.width;
		}
		int getOSDHeight()
		{
			return mState.height;
		}
		void blit()
		{
			mState.blit = true;
			wake();
		};
		fb_var_screeninfo getScreenInfo()
		{
			return si;
		}
		bool setOSDResolution(int x, int y);
		/* the mode of the display, where the window owns it */
		void setDisplayMode(int w, int h, float rate);
		void setOutputFormat(AVRational a, int h, int c)
		{
			mOA = a;
			*mY = h;
			mCrop = c;
			mReInit = true;
		}
		/* just make everything public for simplicity - this is only used inside libstb-hal anyway
		private:
		*/
		fb_var_screeninfo si;
		int *mX;
		int *mY;
		int _mX[2]; /* output window size */
		int _mY[2]; /* [0] = normal, [1] = fullscreen */
		AVRational mOA; /* output window aspect ratio */
		AVRational mVA; /* video aspect ratio */
		AVRational _mVA; /* for detecting changes in mVA */
		bool mVAchanged;
		float zoom; /* for cropping */
		float xscale; /* and aspect ratio */
		int mCrop; /* DISPLAY_AR_MODE */

		bool mFullscreen; /* fullscreen? */
		bool mReInit; /* setup things for GL */
		OpenThreads::Mutex mReInitLock;
		bool mShutDown; /* if set main loop is left */
		bool mInitDone; /* condition predicate */
		// OpenThreads::Condition mInitCond; /* condition variable for init */
		// mutable OpenThreads::Mutex mMutex; /* lock our data */

		std::vector<unsigned char> *osd_buf; /* silly bounce buffer */

		std::map<SDL_Keycode, int> mKeyMap;
		SDL_Window *mWindow;
		SDL_GLContext mContext;
		uint32_t mUserEvent; /* wakes the GL thread: blit() and mpv frames */
		int mViewX; /* viewport offset in fullscreen mode */
		int mViewY;
		mpv_render_context *mRender;
		GLuint mVideoFbo; /* mpv renders the video into this FBO ... */
		GLuint mVideoTex; /* ... whose texture is drawn below the OSD */
		int mVideoW;
		int mVideoH;
		bool mVideoValid; /* mpv has drawn a frame since the last video-params change */
		bool mFramePending;
		int input_fd;
		int64_t last_apts;
		void run();

		void render(); /* actual render function */
		void pollEvents(); /* waits for SDL window, keyboard and wakeup events */
		void handleKey(SDL_Keycode key);
		void pushKey(int code);
		int mOsdBox[4]; /* x0, y0, x1, y1 of what the OSD shows; empty when x1 <= x0 */
		bool mDirect; /* what the log said last about how the video is drawn */
		int mLastPig[4]; /* what the last picture on the screen was drawn with */
		bool mLastDirect;
		int mOsdW; /* the size the OSD texture has, the GL thread's copy */
		int mOsdH;
		bool mOsdResize;
		int mWantW; /* display mode asked for with setDisplayMode() */
		int mWantH;
		float mWantRate;
		bool mModeChange;
		void applyDisplayMode(int w, int h, float rate, const char *who);
		/* the video on a plane of the display controller, below the window
		 * that then only carries the OSD (Raspberry Pi and other KMS
		 * hardware that scans DRM PRIME frames itself) */
		bool mPlaneOK; /* set up: mpv hands its frames to the primary plane */
		bool mOnPlane; /* the video that is playing is on the plane */
		int mDrmFd;
		struct _drmModeAtomicReq *mPlaneReq; /* mpv adds the plane's properties here */
		GLuint mPlaneFbo; /* mpv wants a target, only its size matters */
		GLuint mPlaneTex;
		int mPlaneFboW;
		int mPlaneFboH;
		double mMargins[4];
		double mFit[2]; /* panscan and stretched aspect sent to mpv */
		bool setupPlane(void *drm_params);
		bool renderPlane();
		void planeMargins();
		int mWinW; /* the window in pixels, the viewport may be smaller */
		int mWinH;
		bool mTexStale; /* the video was drawn directly, mVideoTex has an old frame */
		void drawOSD();
		uint64_t osdChecksum();
		uint64_t mOsdSum; /* of the OSD that is on the window's plane */
		bool mPlaneRedraw;
		bool directVideo(int w, int h);
		bool renderDirect();
		/* a gamepad as remote control: what is held down repeats */
		int mPadKey; /* the key that repeats, 0 for none */
		uint64_t mPadNext; /* SDL ticks of its next repeat */
		int mPadAxis[SDL_GAMEPAD_AXIS_COUNT]; /* the key each stick axis and trigger holds */
		void padPress(int code);
		void padRelease(int code);
		void padButton(int button, bool down);
		void padAxis(int axis, int value);
		void padRepeat();
		static bool producesText(SDL_Keycode key);
		void wake();
		bool setupRender(); /* the libmpv render context, needs the GL context */
		void teardownRender();
		void renderVideo(const cMpvEngine::VideoParams &vp);
		void restoreGLState();
		static void renderUpdateCb(void *ctx);
		bool setupGLObjects(); /* shaders, textures and stuff */
		void releaseGLObjects();
		void drawSquare(float size, float x_factor = 1); /* do not be square */

		void initKeys(); /* setup key bindings for window */

		struct
		{
			int width; /* width and height, fixed for a framebuffer instance */
			int height;
			bool blit;
			GLuint osdtex; /* holds the OSD texture */
			GLuint displaytex; /* holds the display texture */
			GLuint program; /* the GLES2 shader program */
			GLint a_pos; /* vertex attribute: position */
			GLint a_tex; /* vertex attribute: texture coordinate */
			GLint u_scale; /* uniform: zoom / aspect scaling */
			GLint u_xproj; /* uniform: orthographic x scaling */
			GLint u_bgra; /* uniform: 1 swizzles the BGRA CPU buffers, 0 for the mpv texture */
			float xproj;
		} mState;

		void bltOSDBuffer();
		void bltDisplayBuffer();
};
#endif
