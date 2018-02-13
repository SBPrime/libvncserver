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


static SDLTest_CommonState *state;
static SDL_GLContext context;
static GLES2_Context ctx;

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

/* Call this instead of exit(), so we can clean up SDL: atexit() is evil. */
static void
quit(int rc)
{
    int i;

    SDL_GL_DeleteContext(context);
    SDLTest_CommonQuit(state);
    exit(rc);
}

#define GL_CHECK(x) \
        x; \
        { \
          GLenum glError = ctx.glGetError(); \
          if(glError != GL_NO_ERROR) { \
            SDL_Log("glGetError() = %i (0x%.8x) at line %i\n", glError, glError, __LINE__); \
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
        SDL_Log("Shader compilation failed: %s", buffer);fflush(stderr);
        quit(-1);
    }
}

/* 3D data. Vertex range -0.5..0.5 in all axes.
* Z -0.5 is near, 0.5 is far. */
const float _vertices[] =
{
    /* Front face. */
    /* Bottom left */
     0, 1, 0,
     1, 0, 0,
     0, 0, 0,

    /* Top right */
     0, 1, 0,
     1, 1, 0,
     1, 0, 0
};

const float _colors[] =
{
    /* Front face */
    /* Bottom left */
    1.0, 1.0, 1.0, /* red */
    0.0, 0.0, 0.0, /* blue */
    1.0, 0.0, 0.0, /* green */
    /* Top right */
    1.0, 1.0, 1.0, /* red */
    0.0, 0.0, 1.0, /* yellow */
    0.0, 0.0, 0.0 /* blue */
};

const char* _shader_vert_src = 
" attribute vec4 av4position; "
" attribute vec3 av3color; "
" uniform mat4 mvp; "
" varying vec3 vv3color; "
" void main() { "
"    vv3color = av3color; "
"    gl_Position = mvp * av4position; "
" } ";

const char* _shader_frag_src = 
" precision lowp float; "
" varying vec3 vv3color; "
" void main() { "
"    gl_FragColor = vec4(vv3color, 1.0); "
" } ";

typedef struct shader_data
{
    GLuint shader_program, shader_frag, shader_vert;

    GLint attr_position;
    GLint attr_color, attr_mvp;

} shader_data;

static void identity_matrix(float *r)
{
    r[0] = 1.0f;
    r[5] = 1.0f;
    r[10] = 1.0f;
    r[15] = 1.0f;
}

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


static void
Render(unsigned int width, unsigned int height, shader_data* data)
{
    float matrix[16];

    calc_matrix(
	100, 100,
	100, 100,
	width, height,
	matrix);

    GL_CHECK(ctx.glUniformMatrix4fv(data->attr_mvp, 1, GL_FALSE, matrix));
    GL_CHECK(ctx.glDrawArrays(GL_TRIANGLES, 0, 6));

    SDL_Delay(100);
}

int done;
shader_data datas;

void loop()
{
    SDL_Event event;
    int i;
    int status;

    /* Check for events */
    while (SDL_PollEvent(&event) && !done) {
        switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED:
            SDL_GameControllerOpen(event.cdevice.which);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
	case SDL_KEYDOWN:
	case SDL_KEYUP:
            done = 1;
            break;
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
                case SDL_WINDOWEVENT_RESIZED:
                    if (event.window.windowID == SDL_GetWindowID(state->windows[0])) {
                        int w, h;
                        status = SDL_GL_MakeCurrent(state->windows[0], context);
                        if (status) {
                            SDL_Log("SDL_GL_MakeCurrent(): %s\n", SDL_GetError());
                            break;
                        }
                        /* Change view port to the new window dimensions */
                        SDL_GL_GetDrawableSize(state->windows[0], &w, &h);
                        ctx.glViewport(0, 0, w, h);
                        state->window_w = event.window.data1;
                        state->window_h = event.window.data2;
                        /* Update window content */
                        Render(event.window.data1, event.window.data2, &datas);
                        SDL_GL_SwapWindow(state->windows[0]);
                        break;
                    }
                    break;
            }
        }
        SDLTest_CommonEvent(state, &event, &done);
    }
    if (!done) {
      status = SDL_GL_MakeCurrent(state->windows[0], context);
      if (status) {
          SDL_Log("SDL_GL_MakeCurrent(): %s\n", SDL_GetError());
      }
      else {
          Render(state->window_w, state->window_h, &datas);
          SDL_GL_SwapWindow(state->windows[0]);
      }
    }
}

int
main(int argc, char *argv[])
{
    int value;
    int i;
    SDL_DisplayMode mode;
    int status;
    shader_data *data;

    /* Initialize test framework */
    state = SDLTest_CommonCreateState(argv, SDL_INIT_VIDEO);
    if (!state) {
        return 1;
    }

    /* Set OpenGL parameters */
    state->window_flags |= SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS;
    state->gl_red_size = 5;
    state->gl_green_size = 5;
    state->gl_blue_size = 5;
    state->gl_depth_size = 24;
    state->gl_major_version = 2;
    state->gl_minor_version = 0;
    state->gl_profile_mask = SDL_GL_CONTEXT_PROFILE_ES;
    state->gl_accelerated=1;
    state->window_w = 800;
    state->window_h = 800;

    if (!SDLTest_CommonInit(state)) {
        quit(2);
        return 0;
    }

    /* Create OpenGL ES contexts */
    context = SDL_GL_CreateContext(state->windows[0]);
    if (!context) {
        SDL_Log("SDL_GL_CreateContext(): %s\n", SDL_GetError());
        quit(2);
    }

    /* Important: call this *after* creating the context */
    if (LoadContext(&ctx) < 0) {
        SDL_Log("Could not load GLES2 functions\n");
        quit(2);
        return 0;
    }



    if (state->render_flags & SDL_RENDERER_PRESENTVSYNC) {
        SDL_GL_SetSwapInterval(1);
    } else {
        SDL_GL_SetSwapInterval(0);
    }

    SDL_GetCurrentDisplayMode(0, &mode);

    /* Set rendering settings for each context */

    int w, h;
    status = SDL_GL_MakeCurrent(state->windows[0], context);
    if (status) {
        SDL_Log("SDL_GL_MakeCurrent(): %s\n", SDL_GetError());
        /* Continue for next window */
    }
    else {
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
        data->attr_color = GL_CHECK(ctx.glGetAttribLocation(data->shader_program, "av3color"));

        /* Get uniform locations */
        data->attr_mvp = GL_CHECK(ctx.glGetUniformLocation(data->shader_program, "mvp"));

        GL_CHECK(ctx.glUseProgram(data->shader_program));

        /* Enable attributes for position, color and texture coordinates etc. */
        GL_CHECK(ctx.glEnableVertexAttribArray(data->attr_position));
        GL_CHECK(ctx.glEnableVertexAttribArray(data->attr_color));

        /* Populate attributes for position, color and texture coordinates etc. */
        GL_CHECK(ctx.glVertexAttribPointer(data->attr_position, 3, GL_FLOAT, GL_FALSE, 0, _vertices));
        GL_CHECK(ctx.glVertexAttribPointer(data->attr_color, 3, GL_FLOAT, GL_FALSE, 0, _colors));
    }
    SDL_InitSubSystem( SDL_INIT_GAMECONTROLLER );

    /* Main render loop */
    done = 0;

    while (!done) {
        loop();
    }

    SDL_QuitSubSystem( SDL_INIT_GAMECONTROLLER );

    /* Print out some timing information */
    quit(0);
    return 0;
}
