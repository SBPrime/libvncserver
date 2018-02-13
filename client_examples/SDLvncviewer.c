/**
 * @example SDLvncviewer.c
 */

#include <SDL.h>
#include <signal.h>
#include <rfb/rfbclient.h>

//#include "common.c"

#define WINDOW_WIDTH    1280
#define WINDOW_HEIGHT   720

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "SDL_test_common.h"

#define HAVE_OPENGLES2

#include "SDL_opengles2.h"

typedef struct GLES2_Context
{
#define SDL_PROC(ret,func,params) ret (APIENTRY *func) params;
#include "SDL_gles2funcs.h"
#undef SDL_PROC
} GLES2_Context;

struct { int sdl; int rfb; } buttonMapping[]={
	{1, rfbButton1Mask},
	{2, rfbButton2Mask},
	{3, rfbButton3Mask},
	{4, rfbButton4Mask},
	{5, rfbButton5Mask},
	{0,0}
};

static int buttonMask;

static SDLTest_CommonState *state;
static SDL_GLContext context;
static GLES2_Context ctx;

static int realWidth, realHeight;

static int rightAltKeyDown, leftAltKeyDown;
static int rightCtrlKeyDown, leftCtrlKeyDown;
static int qKeyDown;
static int isRunning = TRUE;
static char* cachedPass = NULL;
static SDL_Rect tmpRectFrom, tmpRectTo;

static SDL_PixelFormat *format;
static unsigned char *texture = NULL;
static Uint32 eventRedraw;

rfbClient* cl = NULL;

/* Call this instead of exit(), so we can clean up SDL: atexit() is evil. */
static void quit(int rc)
{   
    rfbClientLog("QUIT: %d\n", rc); 
    if (cl) {
        if (cl->frameBuffer) {
            free(cl->frameBuffer);
            cl->frameBuffer = NULL;
        }
        rfbClientCleanup(cl);
        cl = NULL;
    }
    
    if (texture) {
        SDL_free(texture);
        texture = NULL;
    }

    SDL_GL_DeleteContext(context);
    SDLTest_CommonQuit(state);
    exit(rc);
}

static int LoadContext(GLES2_Context * data)
{
#if SDL_VIDEO_DRIVER_UIKIT
#define __SDL_NOGETPROCADDR__
#elif SDL_VIDEO_DRIVER_ANDROID
#define __SDL_NOGETPROCADDR__
#elif SDL_VIDEO_DRIVER_PANDORA
#define __SDL_NOGETPROCADDR__
#endif

#if defined __SDL_NOGETPROCADDR__
#define SDL_PROC(ret,func,params) data->func=func;
#else
#define SDL_PROC(ret,func,params) \
    do { \
        data->func = SDL_GL_GetProcAddress(#func); \
        if ( ! data->func ) { \
            return SDL_SetError("Couldn't load GLES2 function %s: %s\n", #func, SDL_GetError()); \
        } \
    } while ( 0 );
#endif /* _SDL_NOGETPROCADDR_ */

#include "SDL_gles2funcs.h"
#undef SDL_PROC
    return 0;
}

#define GL_CHECK(x) \
        x; \
        { \
          GLenum glError = ctx.glGetError(); \
          if(glError != GL_NO_ERROR) { \
            rfbClientLog("glGetError() = %i (0x%.8x) at line %i\n", glError, glError, __LINE__); \
            quit(1); \
          } \
        }

/* 
 * Create shader, load in source, compile, dump debug as necessary.
 *
 * shader: Pointer to return created shader ID.
 * source: Passed-in shader source code.
 * shader_type: Passed to GL, e.g. GL_VERTEX_SHADER.
 */
void 
process_shader(GLuint *shader, const char * source, GLint shader_type)
{
    GLint status = GL_FALSE;
    const char *shaders[1] = { NULL };
    char buffer[1024];
    GLsizei length;

    /* Create shader and load into GL. */
    *shader = GL_CHECK(ctx.glCreateShader(shader_type));

    shaders[0] = source;

    GL_CHECK(ctx.glShaderSource(*shader, 1, shaders, NULL));

    /* Clean up shader source. */
    shaders[0] = NULL;

    /* Try compiling the shader. */
    GL_CHECK(ctx.glCompileShader(*shader));
    GL_CHECK(ctx.glGetShaderiv(*shader, GL_COMPILE_STATUS, &status));

    /* Dump debug info (source and log) if compilation failed. */
    if(status != GL_TRUE) {
        ctx.glGetProgramInfoLog(*shader, sizeof(buffer), &length, &buffer[0]);
        buffer[length] = '\0';
        rfbClientLog("Shader compilation failed: %s", buffer);fflush(stderr);
        quit(-1);        
    }
}

/* 3D data. Vertex range -0.5..0.5 in all axes.
* Z -0.5 is near, 0.5 is far. */
const float _vertices[] =
{
    /* Bottom left */
     0, 1, 0,
     1, 0, 0,
     0, 0, 0,

    /* Top right */
     0, 1, 0,
     1, 1, 0,
     1, 0, 0
};

const char* _shader_vert_src = 
" attribute vec4 av4position;                           "
"                                                       "
" uniform mat4 mvp;                                     "
" uniform mat4 tex;                                     "
"                                                       "
" varying vec2 vv2tex;                                  "
"                                                       "
" void main() {                                         "
"    vec4 tmp = tex * av4position;                      "
"    vv2tex = vec2(tmp.x, tmp.y);                       "
"    gl_Position = mvp * av4position;                   "
" }                                                     ";

const char* _shader_frag_src = 
" precision lowp float;                                 "
"                                                       "
" uniform sampler2D texture;                            "
"                                                       "
" varying vec2 vv2tex;                                  "
"                                                       "
" void main() {                                         "
"    gl_FragColor = texture2D(texture, vv2tex);         "
" }                                                     ";

typedef struct shader_data
{
    GLuint shader_program, shader_frag, shader_vert;
    GLuint texture;

    GLint attr_position;
    GLint attr_mvp, attr_tex;
    GLint attr_texture;

} shader_data;

static void calc_matrix(
    float x, float y, float w, float h,
    unsigned int screenW, unsigned int screenH, 
    float *r)
{
    float sx = 2.0 / screenW;
    float sy = 2.0 / screenH;

    r[ 0] = w * sx;		r[ 1] = 0.0f;		r[ 2] = 0.0f;	r[ 3] = 0.0f;
    r[ 4] = 0.0f;		r[ 5] =-h * sy;		r[ 6] = 0.0f;	r[ 7] = 0.0f;
    r[ 8] = 0.0f;		r[ 9] = 0.0f;		r[10] = 1.0f;	r[11] = 0.0f;
    r[12] =-1.0f + x * sx;	r[13] = 1.0f - y * sy;	r[14] = 0.0f;	r[15] = 1.0f;

}

static void calc_matrix_texture(
    float w, float h, float tw, float th,
    float *r)
{    
    r[0] = (w - 1) / tw;
    r[5] = (h - 1) / th; 
    r[10] = 1; r[15] = 1;
}

shader_data datas;

static int findPower2(int i) {
    int result = 1;
    while (result < i) {
        result *= 2;
    }
    
    return result;
}

static void onUpdate(rfbClient* cl,int x,int y,int w,int h);

static rfbBool onResize(rfbClient* client) {
    int width   = client->width,
        height  = client->height,
        depth   = client->format.bitsPerPixel,
        bpp     = depth / 8
    ;
    
    client->updateRect.x = client->updateRect.y = 0;
    client->updateRect.w = width; 
    client->updateRect.h = height;
    
    if (client->frameBuffer) {
        SDL_free(client->frameBuffer);
        client->frameBuffer = NULL;
    }

    if (texture) {
        SDL_free(texture);
        texture = NULL;
    }
    
    if (bpp == 3 || bpp == 4) {
        texture = SDL_malloc(findPower2(width) * findPower2(height) * bpp);
    }
    client->frameBuffer = SDL_malloc(width * height * bpp);
    //client->format.bitsPerPixel = depth;
    //client->format.redShift = format->Rshift;
    //client->format.greenShift = format->Gshift;
    //client->format.blueShift = format->Bshift;

    //client->format.redMax = format->Rmask>>client->format.redShift;
    //client->format.greenMax = format->Gmask>>client->format.greenShift;
    //client->format.blueMax = format->Bmask>>client->format.blueShift;
    
    SetFormatAndEncodings(client);

    onUpdate(client, 0,0, width, height);
    
    return TRUE;
}

static rfbKeySym SDL_key2rfbKeySym(SDL_KeyboardEvent* e) {
    rfbKeySym k = 0;
    SDL_Keycode sym = e->keysym.sym;
    
    switch (sym) {
	case SDLK_BACKSPACE: k = XK_BackSpace; break;
	case SDLK_TAB: k = XK_Tab; break;
	case SDLK_CLEAR: k = XK_Clear; break;
	case SDLK_RETURN: k = XK_Return; break;
	case SDLK_PAUSE: k = XK_Pause; break;
	case SDLK_ESCAPE: k = XK_Escape; break;
	case SDLK_SPACE: k = XK_space; break;
	case SDLK_DELETE: k = XK_Delete; break;
	case SDLK_KP_0: k = XK_KP_0; break;
	case SDLK_KP_1: k = XK_KP_1; break;
	case SDLK_KP_2: k = XK_KP_2; break;
	case SDLK_KP_3: k = XK_KP_3; break;
	case SDLK_KP_4: k = XK_KP_4; break;
	case SDLK_KP_5: k = XK_KP_5; break;
	case SDLK_KP_6: k = XK_KP_6; break;
	case SDLK_KP_7: k = XK_KP_7; break;
	case SDLK_KP_8: k = XK_KP_8; break;
	case SDLK_KP_9: k = XK_KP_9; break;
	case SDLK_KP_PERIOD: k = XK_KP_Decimal; break;
	case SDLK_KP_DIVIDE: k = XK_KP_Divide; break;
	case SDLK_KP_MULTIPLY: k = XK_KP_Multiply; break;
	case SDLK_KP_MINUS: k = XK_KP_Subtract; break;
	case SDLK_KP_PLUS: k = XK_KP_Add; break;
	case SDLK_KP_ENTER: k = XK_KP_Enter; break;
	case SDLK_KP_EQUALS: k = XK_KP_Equal; break;
	case SDLK_UP: k = XK_Up; break;
	case SDLK_DOWN: k = XK_Down; break;
	case SDLK_RIGHT: k = XK_Right; break;
	case SDLK_LEFT: k = XK_Left; break;
	case SDLK_INSERT: k = XK_Insert; break;
	case SDLK_HOME: k = XK_Home; break;
	case SDLK_END: k = XK_End; break;
	case SDLK_PAGEUP: k = XK_Page_Up; break;
	case SDLK_PAGEDOWN: k = XK_Page_Down; break;
	case SDLK_F1: k = XK_F1; break;
	case SDLK_F2: k = XK_F2; break;
	case SDLK_F3: k = XK_F3; break;
	case SDLK_F4: k = XK_F4; break;
	case SDLK_F5: k = XK_F5; break;
	case SDLK_F6: k = XK_F6; break;
	case SDLK_F7: k = XK_F7; break;
	case SDLK_F8: k = XK_F8; break;
	case SDLK_F9: k = XK_F9; break;
	case SDLK_F10: k = XK_F10; break;
	case SDLK_F11: k = XK_F11; break;
	case SDLK_F12: k = XK_F12; break;
	case SDLK_F13: k = XK_F13; break;
	case SDLK_F14: k = XK_F14; break;
	case SDLK_F15: k = XK_F15; break;
	case SDLK_NUMLOCKCLEAR: k = XK_Num_Lock; break;
	case SDLK_CAPSLOCK: k = XK_Caps_Lock; break;
	case SDLK_SCROLLLOCK: k = XK_Scroll_Lock; break;
	case SDLK_RSHIFT: k = XK_Shift_R; break;
	case SDLK_LSHIFT: k = XK_Shift_L; break;
	case SDLK_RCTRL: k = XK_Control_R; break;
	case SDLK_LCTRL: k = XK_Control_L; break;
	case SDLK_RALT: k = XK_Alt_R; break;
	case SDLK_LALT: k = XK_Alt_L; break;
	/*case SDLK_RMETA: k = XK_Meta_R; break;
	case SDLK_LMETA: k = XK_Meta_L; break;*/
	case SDLK_LGUI: k = XK_Super_L; break;
	case SDLK_RGUI: k = XK_Super_R; break;
#if 0
	case SDLK_COMPOSE: k = XK_Compose; break;
#endif
	case SDLK_MODE: k = XK_Mode_switch; break;
	case SDLK_HELP: k = XK_Help; break;
/*	case SDLK_PRINTSCREEN: k = XK_Print; break;
	case SDLK_SYSREQ: k = XK_Sys_Req; break;
	case SDLK_BREAK: k = XK_Break; break;*/
	default: break;
	}
	/* both SDL and X11 keysyms match ASCII in the range 0x01-0x7f */
	if (k == 0 && sym > 0x0 && sym < 0x100) {
		k = sym;
		if (e->keysym.mod & (KMOD_LSHIFT | KMOD_RSHIFT)) {
			if (k >= '1' && k <= '9')
				k &= ~0x10;
			else if (k >= 'a' && k <= 'f')
				k &= ~0x20;
		}
	}
	if (k == 0) {
#ifndef SDL2            
		if (e->keysym.unicode < 0x100)
			k = e->keysym.unicode;
		else
#endif                    
			rfbClientLog("Unknown keysym: %d\n", sym);
	}

	return k;
}

static void onUpdate(rfbClient* cl,int x,int y,int w,int h) {
    int *eventData = SDL_malloc(sizeof(int) * 4);
    if (eventData == NULL) {
        return;
    }
    
    eventData[0] = x; eventData [1] = y;
    eventData[2] = w; eventData[3] = h;
    
    SDL_Event event;
    SDL_memset(&event, 0, sizeof(event)); 
    event.type = eventRedraw;
    event.user.data1 = eventData;
    SDL_PushEvent(&event);
}

static void redraw(rfbClient* cl, int x, int y, int w, int h) {        
    int status = SDL_GL_MakeCurrent(state->windows[0], context);
    if (status) {
        rfbClientLog("SDL_GL_MakeCurrent(): %s\n", SDL_GetError());
        return;
    }

    if (!cl->frameBuffer || !texture) {
        return;
    }
    
    int bpp = cl->format.bitsPerPixel / 8;            
    float matrix[16], matrix_texture[16];
    int texW = findPower2(w);
    int texH = findPower2(h);

    calc_matrix_texture(w, h, texW, texH, matrix_texture);
    calc_matrix(x, y,   w, h,
        cl->width, cl->height,
	matrix);

    int stridFrom = cl->width * bpp;
    int stridTo = texW * bpp;
    GLenum format = bpp == 3 ? GL_RGB : GL_RGBA;
    
    char *to = texture;
    char *from = cl->frameBuffer + y * stridFrom + x * bpp;
    
    int i;
    for (i = 0; i < h; i++) {
        memcpy(to, from, texW * bpp);
        to += stridTo;
        from += stridFrom;
    }

    GL_CHECK(ctx.glActiveTexture(GL_TEXTURE0));
    GL_CHECK(ctx.glBindTexture(GL_TEXTURE_2D, datas.texture));
    
    GL_CHECK(ctx.glTexImage2D(GL_TEXTURE_2D, 0, format, 
        texW, texH, 0,             
        format, GL_UNSIGNED_BYTE, texture));

    GL_CHECK(ctx.glUniform1i(datas.attr_texture, 0));
    GL_CHECK(ctx.glUniformMatrix4fv(datas.attr_mvp, 1, GL_FALSE, matrix));
    GL_CHECK(ctx.glUniformMatrix4fv(datas.attr_tex, 1, GL_FALSE, matrix_texture));
    GL_CHECK(ctx.glDrawArrays(GL_TRIANGLES, 0, 6));
    
    SDL_GL_SwapWindow(state->windows[0]);
}

static rfbBool handleSDLEvent(rfbClient *cl, SDL_Event *e)
{
    switch(e->type) {
        case SDL_QUIT:
            isRunning = FALSE;
            break;
        case SDL_MOUSEBUTTONUP:
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEMOTION:
	{
		int x, y, state, i;

		if (e->type == SDL_MOUSEMOTION) {
			x = e->motion.x;
			y = e->motion.y;
			state = e->motion.state;
		}
		else {
			x = e->button.x;
			y = e->button.y;
			state = e->button.button;
			for (i = 0; buttonMapping[i].sdl; i++)
				if (state == buttonMapping[i].sdl) {
					state = buttonMapping[i].rfb;
					if (e->type == SDL_MOUSEBUTTONDOWN)
						buttonMask |= state;
					else
						buttonMask &= ~state;
					break;
				}
		}
                
                x = x * cl->width / realWidth;
		y = y * cl->height / realHeight;
		SendPointerEvent(cl, x, y, buttonMask);
		buttonMask &= ~(rfbButton4Mask | rfbButton5Mask);
		break;
	}
	case SDL_KEYUP:
	case SDL_KEYDOWN:
        {
            int down = e->type == SDL_KEYDOWN;
            SendKeyEvent(cl, SDL_key2rfbKeySym(&e->key),
                    e->type == SDL_KEYDOWN ? TRUE : FALSE);
            
            switch (e->key.keysym.sym)
            {
                case SDLK_RCTRL:    rightCtrlKeyDown = down;    break;
                case SDLK_LCTRL:    leftCtrlKeyDown = down;     break;
                case SDLK_RALT:     rightAltKeyDown = down;     break;
                case SDLK_LALT:     leftAltKeyDown = down;      break;
                case SDLK_q:        qKeyDown = down;            break;                
            }
            
            if (qKeyDown && 
                (rightCtrlKeyDown || leftCtrlKeyDown) && 
                (rightAltKeyDown || leftAltKeyDown)) {
                isRunning = FALSE;
                
                SendKeyEvent(cl, XK_Alt_R, FALSE);                
                SendKeyEvent(cl, XK_Alt_L, FALSE);
                SendKeyEvent(cl, XK_Control_R, FALSE);                
                SendKeyEvent(cl, XK_Control_L, FALSE);
                SendKeyEvent(cl, XK_q, FALSE);
            }
            break;
        }
        case SDL_WINDOWEVENT_MINIMIZED:
        case SDL_WINDOWEVENT_LEAVE:
        case SDL_WINDOWEVENT_FOCUS_LOST:
            if (rightAltKeyDown) {
                    SendKeyEvent(cl, XK_Alt_R, FALSE);
                    rightAltKeyDown = FALSE;
                    rfbClientLog("released right Alt key\n");
            }
            if (leftAltKeyDown) {
                    SendKeyEvent(cl, XK_Alt_L, FALSE);
                    leftAltKeyDown = FALSE;
                    rfbClientLog("released left Alt key\n");
            }
            break;        
	default:
        {
            if (e->type == eventRedraw) {
                int *data = e->user.data1;
                redraw(cl, data[0], data[1], data[2], data[3]);
            
                SDL_free(data);            
            } else {
                rfbClientLog("ignore SDL event: 0x%x\n", e->type);
            }
        }
    }
    
    int done;
    SDLTest_CommonEvent(state, e, &done);
    if (done) {
        //isRunning = FALSE;
    }
    return TRUE;
}

/**
 * This method is called when a exit signal is send
 */
static void onExit() {
    isRunning = FALSE;
}

static char* onGetPassword(rfbClient* client) {
    if (!cachedPass) {
        return NULL;
    }

    return strdup(cachedPass);
}

int main(int argc,char** argv) {    
    SDL_Event e;
    SDL_DisplayMode mode;
    int status;
    shader_data *data;
    cachedPass = NULL;
    
    int w = WINDOW_WIDTH, h = WINDOW_HEIGHT;
    int i;
    char *host = NULL;
    
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-pass") && (i + 1) < argc) {
            cachedPass = strdup(argv[i + 1]);
            i++;
        }
        else if (!strcmp(argv[i], "-size") && (i + 2) < argc) {           
            w = atoi(argv[i + 1]);
            h = atoi(argv[i + 2]);                        
            i+=2;
        }
        else if (!strcmp(argv[i], "-host") && (i + 1) < argc) {
            host = strdup(argv[i + 1]);
            i++;
        }        
    }    
    
    /* Initialize test framework */
    state = SDLTest_CommonCreateState(argv, SDL_INIT_VIDEO);
    if (!state) {
        rfbClientErr("SDL could not initialize! SDL_Error: %d\n", SDL_GetError());
        return 1;
    }
    
    /* Set OpenGL parameters */
    state->window_flags |= SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE /*| SDL_WINDOW_BORDERLESS*/;
    state->gl_red_size = 5;
    state->gl_green_size = 5;
    state->gl_blue_size = 5;
    state->gl_depth_size = 24;
    state->gl_major_version = 2;
    state->gl_minor_version = 0;
    state->gl_profile_mask = SDL_GL_CONTEXT_PROFILE_ES;
    state->gl_accelerated=1;
    state->window_w = w;
    state->window_h = h;
    
    if (!SDLTest_CommonInit(state)) {
        quit(2);
        return 0;
    }
    
        
    eventRedraw = SDL_RegisterEvents(1);
    if (eventRedraw < 0) {
        rfbClientErr("SDL could not register event! SDL_Error: %d\n", SDL_GetError());
        quit(2);
        return 1;
    }
    

    /* Create OpenGL ES contexts */
    context = SDL_GL_CreateContext(state->windows[0]);
    if (!context) {
        rfbClientLog("SDL_GL_CreateContext(): %s\n", SDL_GetError());
        quit(2);
    }

    /* Important: call this *after* creating the context */
    if (LoadContext(&ctx) < 0) {
        rfbClientLog("Could not load GLES2 functions\n");
        quit(2);
        return 0;
    }

    if (state->render_flags & SDL_RENDERER_PRESENTVSYNC) {
        SDL_GL_SetSwapInterval(1);
    } else {
        SDL_GL_SetSwapInterval(0);
    }

    SDL_GetCurrentDisplayMode(0, &mode);
    status = SDL_GL_MakeCurrent(state->windows[0], context);
    if (status) {
        rfbClientLog("SDL_GL_MakeCurrent(): %s\n", SDL_GetError());
        /* Continue for next window */
        quit(2);
        return 0;
    }
    
    SDL_GL_GetDrawableSize(state->windows[0], &w, &h);
    ctx.glViewport(0, 0, w, h);

    data = &datas;

    /* Shader Initialization */
    process_shader(&data->shader_vert, _shader_vert_src, GL_VERTEX_SHADER);
    process_shader(&data->shader_frag, _shader_frag_src, GL_FRAGMENT_SHADER);

    /* Create shader_program (ready to attach shaders) */
    data->shader_program = GL_CHECK(ctx.glCreateProgram());

    /* Attach shaders and link shader_program */
    GL_CHECK(ctx.glAttachShader(data->shader_program, data->shader_vert));
    GL_CHECK(ctx.glAttachShader(data->shader_program, data->shader_frag));
    GL_CHECK(ctx.glLinkProgram(data->shader_program));

    /* Get attribute locations of non-fixed attributes like color and texture coordinates. */
    data->attr_position = GL_CHECK(ctx.glGetAttribLocation(data->shader_program, "av4position"));

    /* Get uniform locations */
    data->attr_mvp = GL_CHECK(ctx.glGetUniformLocation(data->shader_program, "mvp"));
    data->attr_tex = GL_CHECK(ctx.glGetUniformLocation(data->shader_program, "tex"));    
    data->attr_texture = GL_CHECK(ctx.glGetUniformLocation(data->shader_program, "texture"));
    GL_CHECK(ctx.glUseProgram(data->shader_program));

    /* Enable attributes for position, color and texture coordinates etc. */
    GL_CHECK(ctx.glEnableVertexAttribArray(data->attr_position));

    /* Populate attributes for position, color and texture coordinates etc. */
    GL_CHECK(ctx.glVertexAttribPointer(data->attr_position, 3, GL_FLOAT, GL_FALSE, 0, _vertices));
    
    //GL_CHECK(ctx.glEnable(GL_TEXTURE_2D));
    GL_CHECK(ctx.glGenTextures(1, &data->texture));
    GL_CHECK(ctx.glActiveTexture(GL_TEXTURE0));
    GL_CHECK(ctx.glBindTexture(GL_TEXTURE_2D, data->texture));
    GL_CHECK(ctx.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GL_CHECK(ctx.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    //GL_CHECK(ctx.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    //GL_CHECK(ctx.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GL_CHECK(ctx.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL_CHECK(ctx.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    
    SDL_InitSubSystem( SDL_INIT_GAMECONTROLLER );

    
    //format = SDL_AllocFormat(SDL_GetWindowPixelFormat(state->windows[0]));
    //format = SDL_AllocFormat(SDL_PIXELFORMAT_RGB24);
    realWidth = w;
    realHeight = h;

    atexit(onExit);
    signal(SIGINT, exit);

    /* 16-bit: cl=rfbGetClient(5,3,2); */
    cl = rfbGetClient(8,3,4);
    cl->MallocFrameBuffer = onResize;
    cl->canHandleNewFBSize = TRUE;
    cl->GotFrameBufferUpdate = onUpdate;
    cl->listenPort = LISTEN_PORT_OFFSET;
    cl->listen6Port = LISTEN_PORT_OFFSET;
    cl->GetPassword = onGetPassword;

    if (host) {
        argc = 2;
        argv[1] = host;
    } else {
        argc = 0;
    }
    
    if(!rfbInitClient(cl, &argc,argv)) {
        cl = NULL;
        isRunning = FALSE;
    }
    
    while(isRunning) {
        if(SDL_PollEvent(&e)) {
            if(!handleSDLEvent(cl, &e)) {
                break;
            }
        }
        else {
            int i = WaitForMessage(cl, 500);
            if (i < 0) {
                isRunning = FALSE;
                break;
            } else if (i) {
                if(!HandleRFBServerMessage(cl)) {
                    isRunning = FALSE;
                    break;
                }
            }
        }
    }
    
    SDL_QuitSubSystem( SDL_INIT_GAMECONTROLLER );

    exit(0);
    return 0;
}