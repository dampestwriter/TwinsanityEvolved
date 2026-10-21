// render/backends/gl/gl_shader_library.cpp -- GlShaderLibrary implementation.
#include "engine/render/backends/gl/gl_shader_library.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <cstdio>
#include <cstring>
#if PS2R_HOST_VITA
#include <regex>
#include <string>
#include <vector>
#endif

#include "engine/render/backends/gl/gl_shader_sources.h"
#include "engine/render/backends/gl/gl_uniform_cache.h"

namespace ps2r {
namespace render {

const char* GlShaderLibrary::VertexSource() { return GlShaderSources::Ps2Vertex(); }
const char* GlShaderLibrary::FragmentSource() { return GlShaderSources::Ps2Fragment(); }
const char* GlShaderLibrary::ScanoutVertexSource() { return GlShaderSources::ScanoutVertex(); }
const char* GlShaderLibrary::ScanoutFragmentSource() { return GlShaderSources::ScanoutFragment(); }
const char* GlShaderLibrary::ScanoutDitherFragmentSource() {
#if PS2R_HOST_VITA
    // the CT16 dither's matrix is a dynamically indexed uniform array, which Sony's compiler does not finish: the plain
    // scanout stands in for the retail-look present on the Vita
    return GlShaderSources::ScanoutFragment();
#else
    return GlShaderSources::ScanoutDitherFragment();
#endif
}

#if PS2R_HOST_VITA
namespace {
// vitaGL translates GLSL of the 1.x shape (attribute / varying / texture2D / gl_FragColor) into the Cg Sony's compiler
// takes; the library's sources are GLSL 4.1 core. The spellings that differ are rewritten here; the attribute
// locations the `layout` qualifiers named are bound by name before the link (LinkProgram).
// GLSL 4.1 built-ins with no 1.x spelling, as Cg's (Sony's compiler takes them through the translation), and overloads
// for the calls Cg will not resolve by itself: a vector clamped to scalars, integer min / max / clamp. Cg has no unsigned
// integers; the library's uint arithmetic stays within 31 bits, so int carries it.
const char* const kVitaPrelude = R"CG(
#define TE_INT_DIVIDE_IN_FLOAT 1
#define uint int
#define uvec2 ivec2
#define uvec3 ivec3
#define uvec4 ivec4
#define textureLod(s, c, l) tex2Dlod(s, float4(c, 0.0, l))
float te_clamp(float x, float a, float b) { return clamp(x, a, b); }
vec2 te_clamp(vec2 x, float a, float b) { return clamp(x, vec2(a), vec2(b)); }
vec3 te_clamp(vec3 x, float a, float b) { return clamp(x, vec3(a), vec3(b)); }
vec4 te_clamp(vec4 x, float a, float b) { return clamp(x, vec4(a), vec4(b)); }
vec2 te_clamp(vec2 x, vec2 a, vec2 b) { return clamp(x, a, b); }
vec3 te_clamp(vec3 x, vec3 a, vec3 b) { return clamp(x, a, b); }
vec4 te_clamp(vec4 x, vec4 a, vec4 b) { return clamp(x, a, b); }
int te_clamp(int x, int a, int b) { return x < a ? a : (x > b ? b : x); }
ivec2 te_clamp(ivec2 x, ivec2 a, ivec2 b) { return ivec2(te_clamp(x.x, a.x, b.x), te_clamp(x.y, a.y, b.y)); }
float te_min(float a, float b) { return min(a, b); }
vec2 te_min(vec2 a, float b) { return min(a, vec2(b)); }
vec3 te_min(vec3 a, float b) { return min(a, vec3(b)); }
vec4 te_min(vec4 a, float b) { return min(a, vec4(b)); }
vec2 te_min(vec2 a, vec2 b) { return min(a, b); }
vec3 te_min(vec3 a, vec3 b) { return min(a, b); }
vec4 te_min(vec4 a, vec4 b) { return min(a, b); }
int te_min(int a, int b) { return a < b ? a : b; }
float te_max(float a, float b) { return max(a, b); }
vec2 te_max(vec2 a, float b) { return max(a, vec2(b)); }
vec3 te_max(vec3 a, float b) { return max(a, vec3(b)); }
vec4 te_max(vec4 a, float b) { return max(a, vec4(b)); }
vec2 te_max(vec2 a, vec2 b) { return max(a, b); }
vec3 te_max(vec3 a, vec3 b) { return max(a, b); }
vec4 te_max(vec4 a, vec4 b) { return max(a, b); }
int te_max(int a, int b) { return a > b ? a : b; }
#define clamp te_clamp
#define min te_min
#define max te_max
)CG";

// The fragment profile has no texel fetch and no texture size query (tex2Dfetch / tex2Dsize): texelFetch(s, p, l)
// becomes a sample at the texel's centre and textureSize(s, l) the size the backend sets in te_size_<s> before each draw
// (GlDevice: the size of the texture bound on the sampler's unit). Only the PS2 program's samplers are read this way.
const char* const kVitaSamplerSizes[] = {"u_texture", "u_frame", "u_rt_texture", "u_clut", "u_idx_tex2", "u_zsrc_tex"};

// Every call `name(a, b, ...)` in s given to `make` as its argument texts (nested parentheses and commas respected).
std::string RewriteCalls(const std::string& s, const char* name, std::string (*make)(const std::vector<std::string>&)) {
    std::string out;
    const std::regex call(std::string("\\b") + name + "\\s*\\(");
    std::smatch m;
    std::string rest = s;
    while (std::regex_search(rest, m, call)) {
        out += rest.substr(0, static_cast<size_t>(m.position(0)));
        size_t i = static_cast<size_t>(m.position(0) + m.length(0));
        int depth = 1;
        std::vector<std::string> args(1);
        for (; i < rest.size() && depth > 0; ++i) {
            const char c = rest[i];
            if (c == '(') ++depth;
            if (c == ')' && --depth == 0) break;
            if (c == ',' && depth == 1) { args.emplace_back(); continue; }
            args.back() += c;
        }
        out += make(args);
        rest = rest.substr(i + 1);
    }
    return out + rest;
}

std::string Trim(const std::string& t) {
    const size_t b = t.find_first_not_of(" \t\n"), e = t.find_last_not_of(" \t\n");
    return b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
}

std::string VitaGlsl(uint32_t type, const char* source) {
    std::string s = source;
    s = std::regex_replace(s, std::regex(R"(#version\s+\d+\s*\w*)"), "");
    s = std::regex_replace(s, std::regex(R"(layout\s*\(\s*location\s*=\s*\d+\s*\)\s*in\s+)"), "attribute ");
    s = std::regex_replace(s, std::regex(R"(\bnoperspective\s+)"), "");
    if (type == GL_VERTEX_SHADER) {
        s = std::regex_replace(s, std::regex(R"((^|\n)(\s*)out\s+)"), "$1$2varying ");
    } else {
        s = std::regex_replace(s, std::regex(R"((^|\n)(\s*)in\s+)"), "$1$2varying ");
        s = std::regex_replace(s, std::regex(R"(\bout\s+vec4\s+frag_color\s*;)"), "");
        s = "#define frag_color gl_FragColor\n" + s;
    }
    s = std::regex_replace(s, std::regex(R"(\btexture\s*\()"), "texture2D(");
    s = std::regex_replace(s, std::regex(R"(\b((0x[0-9A-Fa-f]+)|([0-9]+))[uU]\b)"), "$1");   // no unsigned literals in Cg
    if (type == GL_FRAGMENT_SHADER) {
        s = RewriteCalls(s, "texelFetch", [](const std::vector<std::string>& a) {
            const std::string sampler = Trim(a[0]);
            return "texture2D(" + sampler + ", (vec2(" + Trim(a.size() > 1 ? a[1] : "") + ") + vec2(0.5)) / te_size_" + sampler + ")";
        });
        s = RewriteCalls(s, "textureSize", [](const std::vector<std::string>& a) {
            return "ivec2(te_size_" + Trim(a[0]) + ")";
        });
        // a fragment shader that writes no colour (the stencil passes, drawn with colour writes off) still gets one:
        // Sony's compiler does not finish a fragment program without an output
        if (s.find("frag_color") == std::string::npos && s.find("gl_FragColor") == std::string::npos)
            s = std::regex_replace(s, std::regex(R"(void\s+main\s*\(\s*\)\s*\{)"), "void main() { gl_FragColor = vec4(0.0);");
        std::string sizes;
        for (const char* name : kVitaSamplerSizes)
            if (s.find(std::string("te_size_") + name) != std::string::npos) sizes += std::string("uniform vec2 te_size_") + name + ";\n";
        s = sizes + s;
    }
    return std::string(kVitaPrelude) + s;
}
}  // namespace
#endif

uint32_t GlShaderLibrary::CompileShader(uint32_t type, const char* source) {
    GLuint shader = glCreateShader(type);
#if PS2R_HOST_VITA
    const std::string vita = VitaGlsl(type, source);
    source = vita.c_str();
#endif
    glShaderSource(shader, 1, &source, nullptr);
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif
    if (!ok) {
        char log[1024];
        log[0] = '\0';
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gl] shader compile error: %s\n", log);
#if PS2R_HOST_VITA
        // the source as handed to vitaGL, numbered, to match the compiler's line numbers
        int line_no = 1;
        for (const char* p = source; *p && line_no <= 12; ++line_no) {
            const char* e = std::strchr(p, '\n');
            const int n = e ? int(e - p) : int(std::strlen(p));
            std::fprintf(stderr, "[gl]   %2d| %.*s\n", line_no, n, p);
            p = e ? e + 1 : p + n;
        }
        std::fflush(stderr);
#endif
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

uint32_t GlShaderLibrary::LinkProgram(uint32_t vert, uint32_t frag) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
#if PS2R_HOST_VITA
    // the locations the GLSL 4.1 sources gave with layout(location = N) (VitaGlsl turns those into attributes)
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texcoord");
    glBindAttribLocation(program, 2, "a_color");
    glBindAttribLocation(program, 3, "a_fog");
    std::fflush(stderr);
#endif
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gl] program link error: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

bool GlShaderLibrary::Build(GlUniformCache& uniforms) {
    Destroy();
    GLuint vs = CompileShader(GL_VERTEX_SHADER, VertexSource());
    if (!vs) return false;
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, FragmentSource());
    if (!fs) { glDeleteShader(vs); return false; }
    ps2_program_ = LinkProgram(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ps2_program_) return false;
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif

    GLuint svs = CompileShader(GL_VERTEX_SHADER, ScanoutVertexSource());
    if (!svs) return false;
    GLuint sfs = CompileShader(GL_FRAGMENT_SHADER, ScanoutFragmentSource());
    if (!sfs) { glDeleteShader(svs); return false; }
    scanout_program_ = LinkProgram(svs, sfs);
    glDeleteShader(svs);
    glDeleteShader(sfs);
    if (!scanout_program_) return false;
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif
    scanout_tex_loc_ = glGetUniformLocation(scanout_program_, "u_texture");
    GLuint dvs = CompileShader(GL_VERTEX_SHADER, ScanoutVertexSource());
    if (!dvs) return false;
    GLuint dfs = CompileShader(GL_FRAGMENT_SHADER, ScanoutDitherFragmentSource());
    if (!dfs) { glDeleteShader(dvs); return false; }
    scanout_dither_program_ = LinkProgram(dvs, dfs);
    glDeleteShader(dvs);
    glDeleteShader(dfs);
    if (!scanout_dither_program_) return false;
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif
    scanout_dither_tex_loc_ = glGetUniformLocation(scanout_dither_program_, "u_texture");
    scanout_dither_size_loc_ = glGetUniformLocation(scanout_dither_program_, "u_source_size");
    scanout_dither_dimx_loc_ = glGetUniformLocation(scanout_dither_program_, "u_dimx");
    if (!BuildStencilPrograms()) return false;
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif

    LookupLocations();
    PrimeUniforms(uniforms);
#if PS2R_HOST_VITA
    std::fflush(stderr);
#endif
    return true;
}

// The stencil-composite programs: strip-count and invert fragments over the PS2
// vertex program (their sampler units bound once), the constant quad pair, and the
// opaque-alpha stencil mark over the constant vertex (its frame sampler on unit 0).
bool GlShaderLibrary::BuildStencilPrograms() {
    struct Prog { const char* vs; const char* fs; uint32_t* out; };
    const Prog progs[4] = {
        {VertexSource(), GlShaderSources::StencilStripFragment(), &strip_program_},
        {VertexSource(), GlShaderSources::StencilInvertFragment(), &invert_program_},
        {GlShaderSources::ConstVertex(), GlShaderSources::ConstFragment(), &const_program_},
        {GlShaderSources::ConstVertex(), GlShaderSources::OpaqueMaskFragment(), &opaque_mask_program_},
    };
    for (const Prog& p : progs) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, p.vs);
        if (!vs) return false;
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, p.fs);
        if (!fs) { glDeleteShader(vs); return false; }
        *p.out = LinkProgram(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (!*p.out) return false;
    }
    sloc_.strip_screen = glGetUniformLocation(strip_program_, "u_screen_size");
    sloc_.strip_zsrc_en = glGetUniformLocation(strip_program_, "u_zsrc_en");
    sloc_.strip_datm = glGetUniformLocation(strip_program_, "u_datm");
    sloc_.strip_zsrc_num = glGetUniformLocation(strip_program_, "u_zsrc_num");
    sloc_.strip_zsrc_den = glGetUniformLocation(strip_program_, "u_zsrc_den");
    glUseProgram(strip_program_);
    const GLint rt_s = glGetUniformLocation(strip_program_, "u_rt_texture");
    const GLint zs_s = glGetUniformLocation(strip_program_, "u_zsrc_tex");
    if (rt_s >= 0) glUniform1i(rt_s, 1);
    if (zs_s >= 0) glUniform1i(zs_s, 4);
    sloc_.invert_screen = glGetUniformLocation(invert_program_, "u_screen_size");
    sloc_.invert_fix = glGetUniformLocation(invert_program_, "u_fix_alpha");
    glUseProgram(invert_program_);
    const GLint rt_i = glGetUniformLocation(invert_program_, "u_rt_texture");
    if (rt_i >= 0) glUniform1i(rt_i, 1);
    sloc_.const_color = glGetUniformLocation(const_program_, "u_color");
    glUseProgram(opaque_mask_program_);
    const GLint frame_s = glGetUniformLocation(opaque_mask_program_, "u_frame");
    if (frame_s >= 0) glUniform1i(frame_s, 0);
    glUseProgram(0);
    return true;
}

void GlShaderLibrary::Destroy() {
    if (ps2_program_) { glDeleteProgram(ps2_program_); ps2_program_ = 0; }
    if (scanout_program_) { glDeleteProgram(scanout_program_); scanout_program_ = 0; }
    if (scanout_dither_program_) { glDeleteProgram(scanout_dither_program_); scanout_dither_program_ = 0; }
    if (strip_program_) { glDeleteProgram(strip_program_); strip_program_ = 0; }
    if (invert_program_) { glDeleteProgram(invert_program_); invert_program_ = 0; }
    if (const_program_) { glDeleteProgram(const_program_); const_program_ = 0; }
    if (opaque_mask_program_) { glDeleteProgram(opaque_mask_program_); opaque_mask_program_ = 0; }
    sloc_ = GlStencilLocations{};
    loc_ = GlUniformLocations{};
    scanout_tex_loc_ = -1;
    scanout_dither_tex_loc_ = scanout_dither_size_loc_ = scanout_dither_dimx_loc_ = -1;
}

void GlShaderLibrary::LookupLocations() {
    const GLuint p = ps2_program_;
    auto L = [p](const char* name) { return glGetUniformLocation(p, name); };
    loc_.screen_size = L("u_screen_size");
    loc_.use_texture = L("u_use_texture");
    loc_.tex_sampler = L("u_texture");
    loc_.blend_a = L("u_blend_a");
    loc_.blend_b = L("u_blend_b");
    loc_.blend_c = L("u_blend_c");
    loc_.blend_d = L("u_blend_d");
    loc_.fix_alpha = L("u_fix_alpha");
    loc_.blend_enabled = L("u_blend_enabled");
    loc_.tfx = L("u_tfx");
    loc_.tcc = L("u_tcc");
    loc_.fba = L("u_fba");
    loc_.colclamp = L("u_colclamp");
    loc_.scanmsk = L("u_scanmsk");
    loc_.zte = L("u_zte");
    loc_.ztst = L("u_ztst");
    loc_.ate = L("u_ate");
    loc_.atst = L("u_atst");
    loc_.aref = L("u_aref");
    loc_.afail = L("u_afail");
    loc_.date = L("u_date");
    loc_.datm = L("u_datm");
    loc_.fog_enabled = L("u_fog_enabled");
    loc_.fogcol = L("u_fogcol");
    loc_.tex_size = L("u_tex_size");
    loc_.uv_scale = L("u_uv_scale");
    loc_.uv_mode = L("u_uv_mode");
    loc_.sw_blend = L("u_sw_blend");
    loc_.rt_sampler = L("u_rt_texture");
    loc_.flip_v = L("u_flip_v");
    loc_.fbmsk16_en = L("u_fbmsk16_en");
    loc_.fbmsk16 = L("u_fbmsk16");
    loc_.depth_fmt = L("u_depth_fmt");
    loc_.depth_zoff = L("u_depth_zoff");
    loc_.idx_fmt = L("u_idx_fmt");
    loc_.idx_self = L("u_idx_self");
    loc_.draw_scale = L("u_draw_scale");
    loc_.clut_sampler = L("u_clut");
    loc_.idx_tex2_sampler = L("u_idx_tex2");
    loc_.fb16 = L("u_fb16");
    loc_.ct16q = L("u_ct16q");
    loc_.uv_native = L("u_uv_native");
    loc_.dthe = L("u_dthe");
    loc_.dimx_lo = L("u_dimx_lo");
    loc_.dimx_hi = L("u_dimx_hi");
    loc_.zsrc_en = L("u_zsrc_en");
    loc_.zsrc_sampler = L("u_zsrc_tex");
    loc_.zsrc_num = L("u_zsrc_num");
    loc_.zsrc_den = L("u_zsrc_den");
    loc_.clut_raw = L("u_clut_raw");
    loc_.mip_ctl = L("u_mip_ctl");
    loc_.mip_lodp = L("u_mip_lodp");
    loc_.tex_lod_hw = L("u_tex_lod_hw");
}

// A fresh program object has every uniform at its GL default; the value shadow is
// dropped BEFORE priming so it can never serve a pre-relink value. Units 2/3/4 must
// hold valid textures on every draw (macOS GL returns black for all fetches when a
// statically referenced sampler's unit is incomplete) -- the device binds its dummy
// texture there; the arms are primed OFF.
void GlShaderLibrary::PrimeUniforms(GlUniformCache& uniforms) {
    uniforms.Clear();
    glUseProgram(ps2_program_);
    uniforms.Set1i(loc_.tex_sampler, 0);
    uniforms.Set1i(loc_.rt_sampler, 1);
    uniforms.Set1i(loc_.clut_sampler, 2);
    uniforms.Set1i(loc_.idx_tex2_sampler, 3);
    uniforms.Set1i(loc_.zsrc_sampler, 4);
    uniforms.Set1i(loc_.zsrc_en, 0);
    uniforms.Set1i(loc_.zsrc_num, 1);
    uniforms.Set1i(loc_.zsrc_den, 1);
    uniforms.Set1i(loc_.fb16, 0);
    uniforms.Set1i(loc_.ct16q, 0);
    uniforms.Set1i(loc_.dthe, 0);
    uniforms.Set1i(loc_.clut_raw, 0);
    uniforms.Set1i(loc_.uv_mode, 0);
    uniforms.Set1i(loc_.tex_lod_hw, 0);
    uniforms.Set2f(loc_.uv_scale, 1.0f, 1.0f);
    glUseProgram(0);
}

}  // namespace render
}  // namespace ps2r
