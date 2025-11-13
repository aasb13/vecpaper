#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <regex.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "argparse.h"

// Globals
char debug = 0;
double FRAME_TIME;
char *screenset;
int cache_length;
GLuint passthrough_program = 0;
GLuint cache_tex = 0;
struct cached_frame {
    unsigned char *jpeg_data;
    size_t jpeg_size;
};
struct cached_frame *frame_cache = NULL;

struct wl_state;
struct display_output;

struct display_output *target_display = NULL; // NULL initially
int output_count = 0;
struct wl_display *display;
struct wl_compositor *compositor;
struct wl_surface *surface;
struct wl_registry *registry;
struct zwlr_layer_shell_v1 *layer_shell;
struct zwlr_layer_surface_v1 *layer_surface;
struct wl_egl_window *egl_win;
GLuint shader_program;
double global_time = 0.0;

struct wl_list outputs; 

EGLDisplay egl_display;
EGLContext egl_context;
EGLSurface egl_surface;
EGLConfig egl_config;
GLuint vbo = 0;

// Static variables
GLfloat VERTS[] = {-1, -1, 1, -1, -1, 1, 1, 1};

// Sets to current time after start (for debug prints)
static clock_t start_time;

// Prototype for debug print
void debprintf(const char *format, ...);

// Vertex shader will be constant for now
static const char *vertex_shader_src =
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "  uv = (pos + 1.0) * 0.5;\n"
    "  gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

// Shader used for caching
static const char *passthrough_fragment_src =
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(tex, uv);\n"
    "}\n";

// Structures
struct wl_state {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    char *monitor; // User selected output
    int surface_layer;
};

struct monitor_geom {
    int x, y;          // Top-left corner in global desktop coordinates
};

struct display_output {
    uint32_t wl_name;
    struct wl_output *wl_output;
    char *name;
    char *identifier;

    struct wl_state *state;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_egl_window *egl_window;
    EGLSurface *egl_surface;

    uint32_t width, height;
    uint32_t scale;

    struct wl_list link;

    struct wl_callback *frame_callback;

    // hyprland geometry
    struct monitor_geom hyprland_monitor_geom;
};

static void cleanup_display_output(struct display_output *output) {
    if (output->frame_callback) {
        wl_callback_destroy(output->frame_callback);
        output->frame_callback = NULL;
    }
    if (output->layer_surface) {
        zwlr_layer_surface_v1_destroy(output->layer_surface);
        output->layer_surface = NULL;
    }
    if (output->surface) {
        wl_surface_destroy(output->surface);
        output->surface = NULL;
    }
    if (output->wl_output) {
        wl_output_destroy(output->wl_output);
        output->wl_output = NULL;
    }
    free(output->name);
    free(output->identifier);
}

// Clean everything before exiting
static void cleanup(void)
{
    debprintf("Cleaning up resources\n");

    if (egl_display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(egl_display, egl_surface);
        if (egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(egl_display, egl_context);
        eglTerminate(egl_display);
    }

    struct display_output *output, *tmp;
    wl_list_for_each_safe(output, tmp, &outputs, link) {
        cleanup_display_output(output);
        wl_list_remove(&output->link);
        free(output);
    }

    if (vbo)
        glDeleteBuffers(1, &vbo);

    if (layer_surface)
        zwlr_layer_surface_v1_destroy(layer_surface);

    if (surface)
        wl_surface_destroy(surface);

    if (layer_shell)
        zwlr_layer_shell_v1_destroy(layer_shell);

    if (registry)
        wl_registry_destroy(registry);

    if (egl_win)
        wl_egl_window_destroy(egl_win);

    if (cache_length > 0) {
        for (int i = 0; i < cache_length; i++) {
            free(frame_cache[i].jpeg_data);
        }
        free(frame_cache);
        glDeleteTextures(1, &cache_tex);
        if (passthrough_program) glDeleteProgram(passthrough_program);
    }

    if (display)
        wl_display_disconnect(display);

    debprintf("Cleanup complete\n");
}

// Read file into a string
static char *read_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        perror("File does not exist");
        return NULL;
    }

    // Ensure it's a regular file, not a directory or something else
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "Path exists but is not a regular file: %s\n", path);
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("Cannot open file");
        return NULL;
    }

    long len = st.st_size;
    char *buf = malloc(len + 1);
    if (!buf) {
        perror("malloc");
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, len, f) != (size_t)len) {
        perror("fread");
        free(buf);
        fclose(f);
        return NULL;
    }

    buf[len] = '\0'; // null-terminate
    fclose(f);
    return buf;
}

// Debug printing
void debprintf(const char *format, ...)
{
    if (!debug)
    {
        return;
    }
    va_list args;
    va_start(args, format);

    // Elapsed time in seconds
    double elapsed = (double)(clock() - start_time) / CLOCKS_PER_SEC;

    printf("[%.4f s] ", elapsed);

    vprintf(format, args);
    va_end(args);
}

// JPEG error handling
struct jpeg_error_mgr_jmp {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};
typedef struct jpeg_error_mgr_jmp *jpeg_error_mgr_jmp_ptr;

static void jpeg_error_exit_jmp(j_common_ptr cinfo) {
    jpeg_error_mgr_jmp_ptr myerr = (jpeg_error_mgr_jmp_ptr)cinfo->err;
    longjmp(myerr->setjmp_buffer, 1);
}

// Compress RGBA into JPEG in memory
static unsigned char *compress_jpeg(unsigned char *rgba, int w, int h, int quality, size_t *out_size) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr_jmp jerr = {0};  // Zero-init

    // Zero-init cinfo
    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit_jmp;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_compress(&cinfo);
        return NULL;
    }

    jpeg_create_compress(&cinfo);

    unsigned char *outbuffer = NULL;
    unsigned long outsize = 0;
    jpeg_mem_dest(&cinfo, &outbuffer, &outsize);

    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    (void)jpeg_start_compress(&cinfo, TRUE);  // Cast to void

    JSAMPROW row_pointer[1];
    int row_stride = w * 3;
    unsigned char *rgb_row = malloc(row_stride);  // One row RGB
    if (!rgb_row) {
        jpeg_destroy_compress(&cinfo);
        return NULL;
    }

    while (cinfo.next_scanline < cinfo.image_height) {
        int y = cinfo.next_scanline;

        // Copy RGBA row into RGB (drop alpha)
        for (int x = 0; x < w; x++) {
            int rgba_idx = (y * w + x) * 4;
            int rgb_idx = x * 3;
            rgb_row[rgb_idx + 0] = rgba[rgba_idx + 0];  // R
            rgb_row[rgb_idx + 1] = rgba[rgba_idx + 1];  // G
            rgb_row[rgb_idx + 2] = rgba[rgba_idx + 2];  // B
        }

        row_pointer[0] = rgb_row;
        if (jpeg_write_scanlines(&cinfo, row_pointer, 1) != 1) {
            free(rgb_row);
            jpeg_destroy_compress(&cinfo);
            return NULL;
        }
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(rgb_row);

    *out_size = outsize;
    return outbuffer;
}

// Decompress JPEG into RGBA
static unsigned char *decompress_jpeg(unsigned char *jpeg_data, size_t jpeg_size, int w, int h) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr_jmp jerr = {0};  // Zero-init error struct

    // Zero-init cinfo to avoid garbage
    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit_jmp;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return NULL;  // Error - jump out
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);
    (void)jpeg_read_header(&cinfo, TRUE);  // Cast to void to ignore the warnings
    (void)jpeg_start_decompress(&cinfo);

    if (cinfo.output_width != (unsigned int)w || cinfo.output_height != (unsigned int)h) {
        debprintf("JPEG size mismatch: expected %dx%d, got %dx%d\n", w, h, cinfo.output_width, cinfo.output_height);
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    unsigned char *rgba = malloc((size_t)w * h * 4);  // RGBA output
    if (!rgba) {
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    // RGB temp buffer for one row (libjpeg outputs RGB)
    unsigned char *rgb_row = malloc((size_t)w * 3);
    if (!rgb_row) {
        free(rgba);
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    JSAMPROW row_pointer[1];  // Array of pointers for libjpeg
    row_pointer[0] = rgb_row;  // Point to valid RGB buffer

    // int row_stride = cinfo.output_width * cinfo.output_components;  // 3 for RGB

    // Loop: read each row, copy to RGBA
    while (cinfo.output_scanline < cinfo.output_height) {
        // READ: Fill rgb_row via row_pointer
        if (jpeg_read_scanlines(&cinfo, row_pointer, 1) != 1) {
            debprintf("Failed to read scanline %d\n", cinfo.output_scanline);
            free(rgba);
            free(rgb_row);
            jpeg_finish_decompress(&cinfo);
            jpeg_destroy_decompress(&cinfo);
            return NULL;
        }

        // Current row index (scanline starts at 0)
        int row_idx = cinfo.output_scanline - 1;  // After read, its incremented

        // Copy RGB into RGBA (add alpha=255)
        for (int x = 0; x < w; x++) {
            int rgb_offset = x * 3;
            int rgba_offset = (row_idx * w + x) * 4;
            rgba[rgba_offset + 0] = rgb_row[rgb_offset + 0];  // R
            rgba[rgba_offset + 1] = rgb_row[rgb_offset + 1];  // G
            rgba[rgba_offset + 2] = rgb_row[rgb_offset + 2];  // B
            rgba[rgba_offset + 3] = 255;  // A
        }
    }

    free(rgb_row);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return rgba;
}

// = Wayland callbacks section =

// Wayland callbacks prototypes
static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surf, uint32_t serial, uint32_t w, uint32_t h);
static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surf);
static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {}; // NOP

static void output_done(void *data, struct wl_output *wl_output);
static void output_scale(void *data, struct wl_output *wl_output, int32_t scale) {
    struct display_output *output = data;
    output->scale = scale;
}
static void output_name(void *data, struct wl_output *wl_output, const char *name) {
    (void)wl_output;

    struct display_output *output = data;
    output->name = strdup(name);
}
static void output_description(void *data, struct wl_output *wl_output, const char *description) {
    (void)wl_output;

    struct display_output *output = data;

    char *paren = strrchr(description, '(');
    if (paren) {
        size_t length = paren - description;
        output->identifier = calloc(length, sizeof(char));
        if (!output->identifier) {
            debprintf("Failed to allocate output identifier\n");
            return;
        }
        strncpy(output->identifier, description, length);
        output->identifier[length - 1] = '\0'; // Null terminate
    } else {
        output->identifier = strdup(description);
    }
}
static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, 
    int32_t subpixel, const char *make, const char *model, int32_t transform) {}; // NOP
static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    struct display_output *output = data;
    // Only store the current mode
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        output->width = width;
        output->height = height;
    }
};

// Wayland callbacks structures
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

// Wayland callbacks functions
static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surf,
                                    uint32_t serial, uint32_t w, uint32_t h)
{
    zwlr_layer_surface_v1_ack_configure(surf, serial);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surf)
{
    debprintf("Layer surface closed\n");
    wl_display_disconnect(display);
    cleanup();
    exit(0);
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    }
    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
    {
        layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    }

    struct wl_state *state = data;
    if (strcmp(interface, wl_output_interface.name) == 0) {
        struct display_output *output = calloc(1, sizeof(struct display_output));
        output->scale = 1; // Default to no scaling
        output->identifier = NULL;
        output->state = state;
        output->wl_name = name;
        output->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 4);
        wl_output_add_listener(output->wl_output, &output_listener, output);
        debprintf("Added output listener\n");
        wl_list_insert(&outputs, &output->link);
        debprintf("Inserted display into wl list\n");
    }
}

static void output_done(void *data, struct wl_output *wl_output) {
    (void)wl_output;

    struct display_output *output = data;

    debprintf("Output ID %u → Name: '%s', Identifier: '%s'\n",
              output->wl_name, output->name, output->identifier);

    if(screenset == NULL || strcmp(output->name, screenset) == 0){
        target_display = data;
        target_display->wl_output = wl_output;
        debprintf("Set target display to %s\n", output->name);
    }
}

static void init_egl(struct wl_display *dpy, struct wl_surface *surf)
{
    EGLint major, minor, n;
    egl_display = eglGetDisplay((EGLNativeDisplayType)dpy);
    eglInitialize(egl_display, &major, &minor);

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_SAMPLE_BUFFERS, 0,
        EGL_SAMPLES, 0,
        EGL_NONE};

    eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &n);

    static const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, ctx_attribs);

    if (egl_context == EGL_NO_CONTEXT)
    {
        fprintf(stderr, "Failed to create EGL context\n");
        exit(1);
    }
    debprintf("Created EGL context\n");

    egl_win = wl_egl_window_create(surf, target_display->width, target_display->height);
    egl_surface = eglCreateWindowSurface(egl_display, egl_config, egl_win, NULL);
    debprintf("Created EGL surface\n");

    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    glViewport(0, 0, target_display->width, target_display->height);
}

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    // Check for glsl compilation errors
    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE)
    {
        GLint log_len;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        char *log = malloc(log_len);
        glGetShaderInfoLog(shader, log_len, NULL, log);
        fprintf(stderr, "Shader compilation failed:\n%s\n", log);
        free(log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static GLuint compile_gl_program(char *fragment_shader_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    if (!vs)
    {
        fprintf(stderr, "Vertex shader compilation failed\n");
        exit(1);
    }
    debprintf("Compiled vertex shader\n");

    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);
    if (!fs)
    {
        fprintf(stderr, "Fragment shader compilation failed\n");
        glDeleteShader(vs);
        exit(1);
    }
    debprintf("Compiled fragment shader\n");

    free(fragment_shader_src);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    debprintf("Attached vertex + fragment shaders\n");
    glLinkProgram(prog);

    GLint status;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (status != GL_TRUE)
    {
        GLint log_len;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &log_len);
        char *log = malloc(log_len);
        glGetProgramInfoLog(prog, log_len, NULL, log);
        fprintf(stderr, "Program link failed:\n%s\n", log);
        free(log);
        glDeleteProgram(prog);
        exit(1);
    }
    debprintf("Linked shader program\n");

    // Deleting shaders from memory after linking them
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

void replace_all(char **src, const char *oldStr, const char *newStr) {
    char *pos, *tmp;
    int len_old = strlen(oldStr);
    int len_new = strlen(newStr);
    int count = 0;

    // Count occurrences
    for (pos = *src; (pos = strstr(pos, oldStr)) != NULL; pos += len_old) {
        count++;
    }

    if (count == 0) return;

    // Allocate new string
    tmp = malloc(strlen(*src) + count * (len_new - len_old) + 1);
    if (!tmp) return;

    char *dst = tmp;
    char *p = *src;

    while ((pos = strstr(p, oldStr)) != NULL) {
        size_t n = pos - p;
        memcpy(dst, p, n);
        dst += n;
        memcpy(dst, newStr, len_new);
        dst += len_new;
        p = pos + len_old;
    }
    strcpy(dst, p);

    free(*src);
    *src = tmp;
}

// Converter for shadertoy-type shaders to shaders that are suitable
// Should be converted differently in the future instead of just replacing and
// adding stuff
char* convert_shadertoy(const char *shader_src) {
    char *shader = strdup(shader_src);
    if (!shader) return NULL;

    // Replace Shadertoy variables
    int has_fragcoord_xy = (strstr(shader, "gl_FragCoord.xy") != NULL);

    replace_all(&shader, "iResolution", "resolution");
    replace_all(&shader, "iTime", "time");
    replace_all(&shader, "iMouse", "mouse");
    
    if (has_fragcoord_xy) {
        // If gl_FragCoord.xy already exists, just replace fragCoord with gl_FragCoord
        replace_all(&shader, "fragCoord", "gl_FragCoord");
    } else {
        // Otherwise, replace fragCoord with gl_FragCoord.xy
        replace_all(&shader, "fragCoord", "gl_FragCoord.xy");
    }

    replace_all(&shader, "fragColor", "gl_FragColor");

    // Replace mainImage with main
    char *pos = strstr(shader, "void mainImage");
    if (pos) {
        char *brace = strchr(pos, '{');
        if (brace) {
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "void main()");
            memcpy(pos, tmp, strlen(tmp));
            memmove(pos + strlen(tmp), brace, strlen(brace) + 1);
        }
    }

    // GLSL version and uniforms (must be prepended!)
    const char *version_line = "#version 330 core\n";
    const char *uniforms =
        "uniform vec2 center;\n"
        "uniform vec2 resolution;\n"
        "uniform float time;\n"
        "uniform vec2 mouse;\n"
        "uniform float pulse1;\n"
        "uniform float pulse2;\n"
        "uniform float pulse3;\n\n";

    // Allocate final shader
    size_t total_len = strlen(version_line) + strlen(uniforms) + strlen(shader) + 1;
    char *final_shader = malloc(total_len);
    if (!final_shader) {
        free(shader);
        return NULL;
    }

    // Prepend version + uniforms before the shader
    strcpy(final_shader, version_line);
    strcat(final_shader, uniforms);
    strcat(final_shader, shader);

    free(shader);
    return final_shader;
}

// Hyprland mouse only for now
void get_monitor_geometry(struct display_output *output) {
    FILE *fp = popen("hyprctl monitors", "r");
    if (!fp) {
        output->hyprland_monitor_geom.x = 0;
        output->hyprland_monitor_geom.y = 0;
        return;
    }

    char line[512];
    regex_t regex;
    regcomp(&regex, ".*\\((ID \\d+)\\):\\s+(\\d+)x(\\d+)@(-?\\d+)x(-?\\d+).*", REG_EXTENDED);

    while (fgets(line, sizeof(line), fp)) {
        regmatch_t matches[6];
        if (regexec(&regex, line, 6, matches, 0) == 0) {
            // Extract monitor name
            char mon_name[64];
            int len = matches[1].rm_eo - matches[1].rm_so;
            snprintf(mon_name, len + 1, "%.*s", len, line + matches[1].rm_so);

            // Extract geometry
            int width  = atoi(line + matches[2].rm_so);
            int height = atoi(line + matches[3].rm_so);
            int x      = atoi(line + matches[4].rm_so);
            int y      = atoi(line + matches[5].rm_so);

            // Compare monitor name with target_display
            if (strstr(line, output->name)) {
                output->hyprland_monitor_geom.x = x;
                output->hyprland_monitor_geom.y = y;
                break;
            }
        }
    }

    regfree(&regex);
    pclose(fp);
}

void hyprctl_get_cursor_pos(int *cx, int *cy) {
    FILE *fp = popen("hyprctl cursorpos", "r");
    if (!fp) {
        *cx = *cy = 0;
        return;
    }

    char buf[128];
    if (fgets(buf, sizeof(buf), fp)) {
        sscanf(buf, "%d, %d", cx, cy);
    } else {
        *cx = *cy = 0;
    }

    pclose(fp);
}

void handle_sigint(int sig)
{
    (void)sig;
    cleanup();
    exit(0);
}

int main(int argc, const char **argv)
{
    signal(SIGINT, handle_sigint);
    
    bool running_hyprland = false; // Using hyprctl for cursor position (for now), so must check if its hyprland or not before trying hyprctl

    char *fragment_shader_file = NULL;
    screenset = NULL;
    char* convertfile = NULL;
    bool runtimeconvertfile = false;
    int fps = 60;

    int cache_seconds = 0;
    int cache_quality = 75;

    struct argparse_option options[] = {
        OPT_HELP(),
        OPT_STRING('c', "convert", &convertfile, "Convert a file with a shadertoy shader and exit. (WARNING: the file will be overwritten. If you don't want it to be overwritten, please consider runtime convert option)"),
        OPT_BOOLEAN('r', "rt-convert", &runtimeconvertfile, "Convert the shader file with shadertoy shader in runtime without modifying the file"),
        OPT_STRING('s', "shader", &fragment_shader_file, "Path to the fragment shader"),
        OPT_STRING(0, "monitor", &screenset, "A monitor to which the shader will be rendered"), // Should be '*' or MONITOR1,MONITOR2 when multimonitor setups will be supported
        OPT_BOOLEAN('d', "debug", &debug, "Option to get debug outputs", 0, 0),
        OPT_INTEGER('f', "fps", &fps, "Frames per second"),
        OPT_INTEGER(0, "cache", &cache_seconds, "Amount of seconds for caching (looping). Useful when you dont want to compute the shader over and over."),
        OPT_INTEGER(0, "cache-quality", &cache_quality, "Caching quality (JPEG compression quality) 10-100 (default 75)"),
        OPT_END(),
    };
    struct argparse argparse;
    argparse_init(&argparse, options, NULL, 0);
    argparse_parse(&argparse, argc, argv);

    debprintf("Running in debug mode\n");

    const char *hypr = getenv("HYPRLAND_INSTANCE_SIGNATURE");

    if (hypr) {
        debprintf("Running hyprland\n");
        running_hyprland = true;
    } else {
        debprintf("Not running under Hyprland.\n");
    }

    if(convertfile && runtimeconvertfile){
        fprintf(stderr, "The file should be either converted at runtime or with file modifiying. Specify either convert or either runtimeconvert file, not both\n");
        exit(1);
    }

    if (convertfile != NULL) {
        // Read the input shader
        char *shader_src = read_file(convertfile);
        if (!shader_src) {
            fprintf(stderr, "Failed to read %s\n", convertfile);
            exit(1);
        }

        // Convert the shader
        char *converted_shader = convert_shadertoy(shader_src);
        free(shader_src); // free original buffer
        if (!converted_shader) {
            fprintf(stderr, "Failed to convert shader.\n");
            exit(1);
        }

        // Overwrite the file with converted shader
        FILE *f = fopen(convertfile, "w");
        if (!f) {
            fprintf(stderr, "Failed to open %s for writing\n", convertfile);
            free(converted_shader);
            exit(1);
        }
        fputs(converted_shader, f);
        fclose(f);
        free(converted_shader);

        printf("Shader converted and saved to %s\n", convertfile);

        exit(0); // Exit after overwriting
    }

    if(fragment_shader_file == NULL){
        fprintf(stderr, "Shader file was not specified\n");
        exit(1);
    }

    debprintf("Reading %s\n", fragment_shader_file);
    char *fragment_shader_src = read_file(fragment_shader_file);
    if (!fragment_shader_src)
    {
        fprintf(stderr, "Failed to read %s\n", fragment_shader_file);
        return 1;
    }
    
    if(runtimeconvertfile){
        debprintf("Converting shadertoy shader in runtime");
        char *converted_shader = convert_shadertoy(fragment_shader_src);
        free(fragment_shader_src);
        fragment_shader_src = converted_shader; // Use converted shader for compilation
    }

    if(screenset == NULL){
        printf("No monitor specified, will be picking the last one\n");
    }
    if(fps<=1){ // If fps is equal to one, it messes up nanosleep because of how FRAME_TIME is made, so discard 1 fps
        fprintf(stderr, "Invalid value for fps, it should be bigger than 1\n");
        exit(1);
    }
    
    FRAME_TIME = 1.0 / (double)fps;
    if(cache_seconds > 0){
        debprintf("%d cache seconds\n", cache_seconds);
        cache_length = fps * cache_seconds;
        debprintf("%d cache length\n", cache_length);
    }
    wl_list_init(&outputs);
    struct wl_state state = {0};
    state.monitor = screenset ? strdup(screenset) : strdup("*"); // default to all
    state.surface_layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
    display = wl_display_connect(NULL);
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    wl_display_roundtrip(display); // First roundtrip to get registry
    wl_display_roundtrip(display); // Second roundtrip to get output listener give monitors
    surface = wl_compositor_create_surface(compositor);
    // Setting empty input region to be passthrough
    struct wl_region *empty_region = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(surface, empty_region);
    wl_region_destroy(empty_region);
    if(target_display == NULL){
        fprintf(stderr, "The target monitor could not be found\n");
        exit(1);
    }

    int w = target_display->width; // Should be changed after multimonitor will be supported
    int h = target_display->height;

    // Allocate the frame cache into ram
    if (cache_length > 0) {
        debprintf("Giving memory to compressed frame cache (JPEG)\n");
        frame_cache = calloc(cache_length, sizeof(struct cached_frame));
    }

    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, target_display->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "vecpaper");
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);
    zwlr_layer_surface_v1_set_size(layer_surface, target_display->width, target_display->height);
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
    wl_surface_commit(surface);
    init_egl(display, surface);
    shader_program = compile_gl_program(fragment_shader_src);
    glUseProgram(shader_program);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(VERTS), VERTS, GL_STATIC_DRAW);
    GLint pos_loc = glGetAttribLocation(shader_program, "pos");
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
    GLuint t_loc = glGetUniformLocation(shader_program, "time");
    if (t_loc == -1)
    {
        fprintf(stderr, "Warning: 'time' uniform not found. Perhaps it is unused?\n");
    }
    GLuint res_loc = glGetUniformLocation(shader_program, "resolution");
    if (res_loc == -1)
    {
        fprintf(stderr, "Warning: 'resolution' uniform not found. Perhaps it is unused?\n");
    }
    else
    {
        glUniform2f(res_loc, target_display->width, target_display->height);
    }

    GLuint mouse_loc = glGetUniformLocation(shader_program, "mouse");
    if (mouse_loc == -1) {
        fprintf(stderr, "Warning: 'mouse' uniform not found. Perhaps it is unused?\n");
    }

    if (running_hyprland) {
        get_monitor_geometry(target_display); // Get monitor offsets for Hyprland
    }

    float mouse_x, mouse_y;

    mouse_x = (float)(target_display->width / 2);
    mouse_y = (float)(target_display->height / 2);

    glUniform2f(mouse_loc, mouse_x, mouse_y); // Setting initial position

    debprintf("Resolution: %dx%d\n", target_display->width, target_display->height);
    int current_frame = 0;

    // Main render loop
    // The problem to make this multimonitor is to render only 1 frame and mirror it to each monitor, instead of rendering it each time for each monitor
    // On the other hand if we render it for each monitor, then we shouldn't be caring about framerate or resolution being the same
    while (wl_display_dispatch_pending(display) != -1)
    {
        if(cache_length > 0 && current_frame == cache_length){
            debprintf("Finished caching frames\n");
            break; // We already cached all frames, stop render loop and start the cache loop instead
        }

        // Set uniforms
        glUniform1f(t_loc, (float)global_time); // Time

        if (running_hyprland && cache_length <= 0) {
            int cursor_x, cursor_y;
            hyprctl_get_cursor_pos(&cursor_x, &cursor_y);
            mouse_x = cursor_x - target_display->hyprland_monitor_geom.x;
            mouse_y = cursor_y - target_display->hyprland_monitor_geom.y;


            // debprintf("%f %f\n", mouse_x, mouse_y);

            glUniform2f(mouse_loc, mouse_x, mouse_y);
        }

        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if(cache_length>0){
            unsigned char *raw = malloc(w * h * 4); // Put the full frame in ram for now
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw);

            size_t jpeg_size;
            unsigned char *jpeg = compress_jpeg(raw, w, h, cache_quality, &jpeg_size);
            free(raw);

            if (!jpeg) {
                fprintf(stderr, "JPEG compression failed\n");
                cleanup();
                exit(1);
            }

            frame_cache[current_frame].jpeg_data = jpeg;
            frame_cache[current_frame].jpeg_size = jpeg_size;

            debprintf("Cached frame %d: %zu bytes (compressed)\n", current_frame, jpeg_size);
        }
        GLenum err = glGetError();
        if (err != GL_NO_ERROR)
        {
            fprintf(stderr, "OpenGL error: 0x%x\n", err);
            cleanup();
            exit(1);
        }
        eglSwapBuffers(egl_display, egl_surface);
        wl_display_flush(display);
        struct timespec ts = {0, (long)(FRAME_TIME * 1e9)};
        nanosleep(&ts, NULL);
        global_time += FRAME_TIME; // That may cause time drifting because we dont sync with time spent on rendering
        current_frame++;
    }

    // Cache playback
    if(cache_length > 0){
        int frame_idx = 0;
        // Create texture for playback
        glGenTextures(1, &cache_tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, cache_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Compile passthrough program
        passthrough_program = compile_gl_program(strdup(passthrough_fragment_src));
        glUseProgram(passthrough_program);

        // Rebind vertex attribs for new program
        pos_loc = glGetAttribLocation(passthrough_program, "pos");
        glEnableVertexAttribArray(pos_loc);
        glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);

        // Bind texture uniform
        GLint tex_loc = glGetUniformLocation(passthrough_program, "tex");
        if (tex_loc != -1) glUniform1i(tex_loc, 0);

        debprintf("Entering cache render loop (passthrough shader)\n");

        while (wl_display_dispatch_pending(display) != -1) {
            // Upload current cached frame
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, cache_tex);
            
            // Decompress on the fly
            unsigned char *rgba = decompress_jpeg(
                frame_cache[frame_idx].jpeg_data,
                frame_cache[frame_idx].jpeg_size,
                w, h
            );

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, cache_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

            free(rgba);  // free immediately

            glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            GLenum err = glGetError();
            if (err != GL_NO_ERROR)
            {
                fprintf(stderr, "OpenGL error in cache loop: 0x%x\n", err);
                cleanup();
                exit(1);
            }

            eglSwapBuffers(egl_display, egl_surface);
            wl_display_flush(display);
            frame_idx = (frame_idx + 1) % cache_length;
            struct timespec ts = {0, (long)(FRAME_TIME * 1e9)};
            nanosleep(&ts, NULL);
        }
    }
    wl_display_disconnect(display);
    return 0;
}