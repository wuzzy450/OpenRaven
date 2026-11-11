#include <assert.h>
#include "../openraven/glsl_compat.h"
int main(void) {
    char core[] = "#version 150 core\nvec4 a=texture2D(s,uv); vec4 b=textureCube(c,d);\n// texture2D(s,uv)\n/* textureCube(c,d) */\nfloat texture2D_extra=1.;";
    size_t size = strlen(core);
    assert(raven_glsl_compat(core) == 2);
    assert(strlen(core) == size);
    assert(strstr(core, "texture  (s,uv)"));
    assert(strstr(core, "texture    (c,d)"));
    assert(strstr(core, "// texture2D(s,uv)"));
    assert(strstr(core, "/* textureCube(c,d) */"));
    assert(strstr(core, "texture2D_extra"));
    assert(raven_glsl_compat(core) == 0);
    char compat[] = "#version 150 compatibility\ntexture2D(s,uv)";
    char old[] = "#version 120\ntexture2D(s,uv)";
    char modern[] = "#version 460 core\ntexture(s,uv)";
    assert(!raven_glsl_compat(compat));
    assert(!raven_glsl_compat(old));
    assert(!raven_glsl_compat(modern));
    char whitespace[] = "  #version 460 core\ntexture2D \n (s,uv)";
    assert(raven_glsl_compat(whitespace) == 1);
    char identifiers[] = "#version 150 core\nmytexture2D(s,uv); texture2D_extra(s,uv); texture2DLod(s,uv,0);";
    assert(!raven_glsl_compat(identifiers));
    char comment[] = "#version 150 core\n/* texture2D(s,uv)";
    assert(!raven_glsl_compat(comment));
    char empty[] = "";
    assert(!raven_glsl_compat(empty));
    return 0;
}
