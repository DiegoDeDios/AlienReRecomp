/*
 * Windows-specific support (compiled only for Windows).
 *
 * - Console: the release is a GUI program (no console window). When it is
 *   started from a terminal it attaches to that console so command-line
 *   options and logs work as on other systems; otherwise log output goes
 *   to alien.log in the data folder, for bug reports.
 * - HLSL: the Direct3D 12 backend needs DXBC shaders. The generated HLSL
 *   (shaders_gen.h) is compiled at start-up with d3dcompiler_47.dll, which
 *   ships with Windows 10 and 11.
 */
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../render/render.h"
#include "../runtime/psx.h"

void win32_setup_console(const char *data_dir) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        return;
    }
    char path[1100];
    SDL_CreateDirectory(data_dir);
    snprintf(path, sizeof path, "%s/alien.log", data_dir);
    if (freopen(path, "w", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
    freopen(path, "a", stdout);
}

static pD3DCompile g_d3dcompile;

/* d3dcompiler_47.dll is present and compiles shader model 5.1 with
 * register spaces (checked once with a tiny shader; some replacements, e.g.
 * Wine's, do not), so Direct3D 12 can be offered */
bool render_hlsl_available(void) {
    static bool tried, ok;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        if (m)
            g_d3dcompile = (pD3DCompile)(void *)GetProcAddress(m, "D3DCompile");
        static const char test[] =
            "cbuffer U : register(b0, space1) { float4 v; };\n"
            "float4 main(float4 p : TEXCOORD0) : SV_Position { return p * v; }\n";
        ID3DBlob *code = NULL, *errors = NULL;
        ok = g_d3dcompile && SUCCEEDED(g_d3dcompile(test, sizeof test - 1, "test.hlsl", NULL, NULL,
                                                    "main", "vs_5_1", 0, 0, &code, &errors));
        if (code)
            ID3D10Blob_Release(code);
        if (errors)
            ID3D10Blob_Release(errors);
        if (!ok)
            LOGW("render", "no usable d3dcompiler_47.dll (shader model 5.1): Direct3D 12 disabled");
    }
    return ok;
}

void *render_compile_hlsl(const char *src, const char *profile, size_t *size) {
    if (!render_hlsl_available()) {
        SDL_SetError("no usable d3dcompiler_47.dll");
        return NULL;
    }
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = g_d3dcompile(src, strlen(src), "shader.hlsl", NULL, NULL, "main", profile,
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        SDL_SetError("HLSL (%s): %s", profile,
                     errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "compile failed");
        if (errors)
            ID3D10Blob_Release(errors);
        if (code)
            ID3D10Blob_Release(code);
        return NULL;
    }
    if (errors)
        ID3D10Blob_Release(errors);
    *size = ID3D10Blob_GetBufferSize(code);
    void *out = SDL_malloc(*size);
    if (out)
        memcpy(out, ID3D10Blob_GetBufferPointer(code), *size);
    ID3D10Blob_Release(code);
    return out;
}
#endif
