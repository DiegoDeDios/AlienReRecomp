/* One shader in every format the SDL GPU backends take (see
 * tools/compile_shaders.py, which generates shaders_gen.h). */
#pragma once
#include <stddef.h>

typedef struct ShaderBlob {
    const unsigned char *spirv; /* Vulkan, entry point "main" */
    size_t spirv_size;
    const char *hlsl;           /* Direct3D 12 (shader model 5.1), entry "main" */
    const char *msl;            /* Metal, entry point "main0" */
} ShaderBlob;
