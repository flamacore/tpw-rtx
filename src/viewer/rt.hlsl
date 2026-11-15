// Path-traced lighting for the reconstructed SimTheme Park scene (DXR 1.1 inline ray queries).
// Per pixel: primary ray, soft sun shadow (+ GGX sun highlight), one torch light, one diffuse bounce with sun + torch at
// the bounce, then temporal accumulation reprojected with exact world positions.
// Output is DEMODULATED: accum.rgb = diffuse irradiance, accum.a = sun specular (scalar, times sun colour later), the
// albedo lives in the G-buffer. The denoiser can then blur lighting without blurring textures; the composite multiplies.
// Compiled offline: dxc -T cs_6_5 -E main rt.hlsl -Fo rt.cso
//
// Pseudo-PBR: shading normals from the texture's luminance (bump), roughness from texture brightness (ponytail: a
// guess, the 1999 art has no material data).

struct Vtx   { float3 p; uint c; float2 uv; };
struct Light { float3 pos; float radius; float3 color; float pad; };

cbuffer C : register(b0)
{
    row_major float4x4 invViewProj;   // NDC -> world (current)
    row_major float4x4 viewProj;      // world -> clip (current, unjittered; matches the raster passes)
    row_major float4x4 prevViewProj;  // previous frame, unjittered
    float3 camPos;      uint frame;
    float3 sunDir;      uint numLights;
    float3 sunColor;    float pixelSpread;    // radians per pixel (ray cone)
    float3 skyZenith;   uint opaqueTris;
    float3 skyHorizon;  uint alphaGeom;       // geometry index holding the alpha-tested triangles
    uint2 dim;          float2 jitter;        // jitter in pixels (0 unless the TAA-jitter setting is on)
    uint resetHistory;  float normalStr; float specStr; float gloss;
    float tintMix;      float shadeMix;  float bounce;  float historyMax;
    float sunSoft;      uint rrMode;     float2 pad1;   // rrMode: write DLSS Ray Reconstruction inputs instead of accumulating
};

RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<Vtx>   verts   : register(t1);   // 3 per triangle (no index buffer), opaque triangles first, then alpha-tested
StructuredBuffer<uint2> mats    : register(t3);   // per triangle: x = texId | alphaTest << 16, y = alphaRef
StructuredBuffer<Light> lights  : register(t4);
Texture2D gTex[] : register(t0, space1);
SamplerState samp : register(s0);

RWTexture2D<float4> accumOut  : register(u0);   // rgb diffuse irradiance, a = sun specular
RWTexture2D<float4> accumPrev : register(u1);
RWTexture2D<uint4>  gbufOut   : register(u2);   // x octahedral normal (2x16), y albedo (RGB8, sqrt), z view depth, w history length (float bits)
RWTexture2D<uint4>  gbufPrev  : register(u3);
RWTexture2D<float>  depthOut  : register(u4);   // NDC depth for compositing raster passes (and DLSS-RR "HW depth")
// DLSS Ray Reconstruction guides (render resolution), written only in rrMode
RWTexture2D<float4> rrColor     : register(u5);   // noisy per-frame radiance, linear HDR
RWTexture2D<float4> rrAlbedo    : register(u6);   // diffuse albedo
RWTexture2D<float4> rrSpecAlb   : register(u7);   // specular albedo (EnvBRDF)
RWTexture2D<float4> rrNormRough : register(u8);   // world normal xyz, linear roughness w
RWTexture2D<float2> rrMotion    : register(u9);   // previous - current position, render pixels

// ---------------------------------------------------------------- utils
uint pcg(inout uint s) { s = s * 747796405u + 2891336453u; uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u; return (w >> 22u) ^ w; }
float rnd(inout uint s) { return (pcg(s) & 0xffffff) / 16777216.0; }
float3 unpackColor(uint c) { return float3((c >> 16) & 255, (c >> 8) & 255, c & 255) / 255.0; }
float3 toLinear(float3 c) { return pow(max(c, 0), 2.2); }
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
uint triIndex(uint geom, uint prim) { return geom == alphaGeom ? opaqueTris + prim : prim; }

uint packNormal(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 o = n.z >= 0 ? n.xy : (1 - abs(n.yx)) * (n.xy >= 0 ? 1 : -1);
    uint2 q = uint2(saturate(o * 0.5 + 0.5) * 65535 + 0.5);
    return q.x | q.y << 16;
}
float3 unpackNormal(uint p)
{
    float2 o = float2(p & 0xffff, p >> 16) / 65535 * 2 - 1;
    float3 n = float3(o, 1 - abs(o.x) - abs(o.y));
    float t = saturate(-n.z);
    n.xy += n.xy >= 0 ? -t : t;
    return normalize(n);
}
uint packAlbedo(float3 a) { uint3 q = uint3(sqrt(saturate(a)) * 255 + 0.5); return q.r | q.g << 8 | q.b << 16; }
uint3 albedoBytes(uint p) { return uint3(p & 255, (p >> 8) & 255, (p >> 16) & 255); }

float3 cosineSample(float3 n, inout uint s)
{
    float r1 = rnd(s), r2 = rnd(s), phi = 6.2831853 * r1, sr = sqrt(r2);
    float3 t = abs(n.y) < 0.99 ? normalize(cross(n, float3(0, 1, 0))) : normalize(cross(n, float3(1, 0, 0)));
    float3 b = cross(n, t);
    return normalize(t * cos(phi) * sr + b * sin(phi) * sr + n * sqrt(1 - r2));
}

float3 sky(float3 d)
{
    float3 c = lerp(skyHorizon, skyZenith, saturate(d.y));
    return c + sunColor * 4 * pow(saturate(dot(d, sunDir)), 400);   // small sun glow
}

// ---------------------------------------------------------------- tracing with alpha test
bool alphaPass(uint tri, float2 bary)
{
    uint2 m = mats[tri];
    if (!(m.x & 0x10000)) return true;
    Vtx a = verts[tri * 3], b = verts[tri * 3 + 1], c = verts[tri * 3 + 2];
    float2 uv = a.uv * (1 - bary.x - bary.y) + b.uv * bary.x + c.uv * bary.y;
    float alpha = gTex[NonUniformResourceIndex(m.x & 0xffff)].SampleLevel(samp, uv, 0).a;
    return alpha * 255.0 >= max(m.y, 1u);
}

struct Hit { bool hit; float t; uint tri; float2 bary; };

Hit trace(float3 o, float3 d, float tmax, bool anyHit)
{
    RayQuery<RAY_FLAG_NONE> q;
    RayDesc r; r.Origin = o; r.TMin = 0.0; r.Direction = d; r.TMax = tmax;
    q.TraceRayInline(scene, anyHit ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : RAY_FLAG_NONE, 0xff, r);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            alphaPass(triIndex(q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex()), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    }
    Hit h; h.hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT; h.t = 0; h.tri = 0; h.bary = 0;
    if (h.hit) { h.t = q.CommittedRayT(); h.tri = triIndex(q.CommittedGeometryIndex(), q.CommittedPrimitiveIndex()); h.bary = q.CommittedTriangleBarycentrics(); }
    return h;
}

bool occluded(float3 o, float3 d, float tmax) { return trace(o, d, tmax, true).hit; }

// ---------------------------------------------------------------- surfaces and lighting
struct Surf { float3 p, n, gn, albedo; float rough; };

Surf surface(Hit h, float3 o, float3 d, float coneWidth, bool detail)
{
    Vtx a = verts[h.tri * 3], b = verts[h.tri * 3 + 1], c = verts[h.tri * 3 + 2];
    float w = 1 - h.bary.x - h.bary.y;
    Surf s;
    s.p = o + d * h.t;
    float3 dp1 = b.p - a.p, dp2 = c.p - a.p, cr = cross(dp1, dp2);
    s.gn = normalize(cr);
    if (dot(s.gn, d) > 0) s.gn = -s.gn;
    s.n = s.gn;
    float2 uv = a.uv * w + b.uv * h.bary.x + c.uv * h.bary.y;
    float3 col = unpackColor(a.c) * w + unpackColor(b.c) * h.bary.x + unpackColor(c.c) * h.bary.y;
    uint texId = mats[h.tri].x & 0xffff, tw, th, levels;
    Texture2D tex = gTex[NonUniformResourceIndex(texId)];
    tex.GetDimensions(0, tw, th, levels);
    float2 e1 = b.uv - a.uv, e2 = c.uv - a.uv;
    float det = e1.x * e2.y - e1.y * e2.x;
    float uvArea = abs(det) * tw * th, worldArea = max(length(cr), 1e-8);
    float lod = 0.5 * log2(max(uvArea / worldArea, 1e-8)) + log2(max(coneWidth / max(abs(dot(s.gn, d)), 0.25), 1e-8));
    // -1.25 bias: the isotropic cone estimate over-blurs ground seen at grazing angles (no anisotropic filtering here)
    float lv = clamp(lod - 1.25, 0, levels - 1);
    float3 t = tex.SampleLevel(samp, uv, lv).rgb;
    float maxc = max(max(col.r, col.g), max(col.b, 1e-3));
    float3 tint = col / maxc;   // the vertex colour's hue without its baked lighting
    s.albedo = toLinear(t) * toLinear(lerp(1, tint, tintMix)) * toLinear(lerp(1, maxc, shadeMix));
    float lt = luma(t);
    s.rough = lerp(0.9, 0.25, saturate(lt * lt * gloss));
    if (detail && normalStr > 0 && texId && abs(det) > 1e-12) {
        // bump from luminance: height differences one texel (at this mip) along u and v, applied along dP/du, dP/dv
        float2 texel = exp2(lv) / float2(tw, th);
        float hu = luma(tex.SampleLevel(samp, uv + float2(texel.x, 0), lv).rgb) - lt;
        float hv = luma(tex.SampleLevel(samp, uv + float2(0, texel.y), lv).rgb) - lt;
        float3 T = (dp1 * e2.y - dp2 * e1.y) / det, B = (dp2 * e1.x - dp1 * e2.x) / det;
        T = normalize(T - s.gn * dot(s.gn, T) + 1e-9); B = normalize(B - s.gn * dot(s.gn, B) + 1e-9);
        s.n = normalize(s.gn - 2.0 * normalStr * (hu * T + hv * B));
    }
    return s;
}

float ggx(float3 n, float3 v, float3 l, float r)   // sun highlight: D * G * F / (4 n.v), n.l folded in
{
    float3 hv = normalize(v + l);
    float nh = saturate(dot(n, hv)), nv = saturate(dot(n, v)) + 1e-3, nl = saturate(dot(n, l));
    float a = r * r, a2 = a * a, dd = nh * nh * (a2 - 1) + 1, k = a * 0.5;
    float D = a2 / (3.14159 * dd * dd);
    float G = nl / (nl * (1 - k) + k) * nv / (nv * (1 - k) + k);
    float F = 0.04 + 0.96 * pow(1 - saturate(dot(hv, v)), 5);
    return D * G * F / (4 * nv);
}

// Specular albedo for DLSS-RR, from the RR integration guide appendix (Ray Tracing Gems ch. 32)
// This software contains source code provided by NVIDIA Corporation (EnvBRDFApprox2 below).
float3 EnvBRDFApprox2(float3 SpecularColor, float alpha, float NoV)
{
    NoV = abs(NoV);
    float4 X = float4(1, NoV, NoV * NoV, NoV * NoV * NoV);
    float4 Y = float4(1, alpha, alpha * alpha, alpha * alpha * alpha);
    float2x2 M1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f);
    float3x3 M2 = float3x3(1.f, 2.92338f, 59.4188f, 20.3225f, -27.0302f, 222.592f, 121.563f, 626.13f, 316.627f);
    float2x2 M3 = float2x2(0.0365463f, 3.32707, 9.0632f, -9.04756);
    float3x3 M4 = float3x3(1.f, 3.59685f, -1.36772f, 9.04401f, -16.3174f, 9.22949f, 5.56589f, 19.7886f, -20.2123f);
    float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
    float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
    bias *= saturate(SpecularColor.g * 50);
    return mad(SpecularColor, max(0, scale), max(0, bias));
}

struct Direct { float3 diffuse; float spec; };

Direct direct(Surf s, float3 v, inout uint seed, bool wantSpec)
{
    Direct r; r.diffuse = 0; r.spec = 0;
    float3 o = s.p + s.gn * 0.02;
    float3 sd = normalize(sunDir + (float3(rnd(seed), rnd(seed), rnd(seed)) - 0.5) * 0.035 * sunSoft);
    float ndl = dot(s.n, sd);
    if (ndl > 0 && dot(s.gn, sd) > -0.05 && !occluded(o, sd, 1e4)) {
        r.diffuse += sunColor * ndl;
        if (wantSpec) r.spec = specStr * ggx(s.n, v, sd, s.rough);
    }
    if (numLights) {
        uint li = min(uint(rnd(seed) * numLights), numLights - 1);
        Light l = lights[li];
        float3 lv = l.pos - s.p;
        float dist = length(lv);
        lv /= max(dist, 1e-4);
        float nd = dot(s.n, lv), att = saturate(1 - dist / l.radius);
        if (nd > 0 && att > 0 && !occluded(o, lv, dist - 0.05)) r.diffuse += l.color * nd * att * att * numLights;
    }
    return r;
}

[numthreads(8, 8, 1)]
void main(uint2 px : SV_DispatchThreadID)
{
    if (any(px >= dim)) return;
    uint seed = (px.y * dim.x + px.x) * 9781u + frame * 6271u;
    pcg(seed);
    float2 ndc = ((px + 0.5 + jitter) / dim) * float2(2, -2) + float2(-1, 1);
    float4 far = mul(float4(ndc, 1, 1), invViewProj);
    float3 dir = normalize(far.xyz / far.w - camPos);

    Hit h = trace(camPos, dir, 1e5, false);
    float3 irr, n = float3(0, 1, 0), albedo = 1;
    float spec = 0, viewZ = 1e9, ndcZ = 1, rough = 0;
    float3 P = camPos + dir * 1e5;
    if (!h.hit) irr = sky(dir);
    else {
        Surf s = surface(h, camPos, dir, pixelSpread * h.t, true);
        P = s.p; n = s.n; albedo = s.albedo; rough = s.rough;
        Direct L = direct(s, -dir, seed, true);
        spec = L.spec;
        float3 bd = cosineSample(s.n, seed);
        if (dot(bd, s.gn) < 0) bd = reflect(bd, s.gn);   // bumped normal may point the bounce into the surface
        Hit h2 = trace(s.p + s.gn * 0.02, bd, 1e4, false);
        float3 Li;
        if (h2.hit) {
            Surf s2 = surface(h2, s.p, bd, pixelSpread * h.t + 0.02 * h2.t, false);
            Li = s2.albedo * (direct(s2, -bd, seed, false).diffuse + 0.5 * lerp(skyHorizon, skyZenith, 0.5));
        } else Li = sky(bd);
        irr = L.diffuse + bounce * Li;
        float4 clip = mul(float4(P, 1), viewProj);
        ndcZ = saturate(clip.z / clip.w); viewZ = clip.w;
    }

    // motion vectors (previous - current, render pixels): DLSS-RR and the DLSS 5 pass both read them
    float4 cc = mul(float4(P, 1), viewProj), pc = mul(float4(P, 1), prevViewProj);
    float2 cur = (cc.xy / cc.w * float2(0.5, -0.5) + 0.5) * dim, prv = (pc.xy / pc.w * float2(0.5, -0.5) + 0.5) * dim;
    rrMotion[px] = pc.w > 0 ? prv - cur : 0;

    if (rrMode) {   // DLSS-RR does the denoising, accumulation, anti-aliasing and upscaling: hand it the raw sample
        rrColor[px] = float4(h.hit ? albedo * irr + sunColor * spec : irr, 1);
        rrAlbedo[px] = float4(h.hit ? albedo : 0.5, 1);
        rrSpecAlb[px] = float4(h.hit ? EnvBRDFApprox2(min(0.04 * specStr, 1).xxx, rough * rough, dot(n, -dir)) : 0, 1);
        rrNormRough[px] = h.hit ? float4(n, rough) : 0;
        depthOut[px] = ndcZ;
        return;
    }

    // temporal accumulation, reprojected through the previous camera. Only the camera has motion vectors, so
    // moving things (visitors, ride cars) are caught by their surface colour changing under the pixel.
    float4 acc = float4(irr, spec);
    float cnt = 1;
    uint alb = packAlbedo(albedo);
    if (h.hit && !resetHistory) {
        float4 pc = mul(float4(P, 1), prevViewProj);
        float2 pn = pc.xy / pc.w;
        int2 q = int2((pn * float2(0.5, -0.5) + 0.5) * dim);
        if (pc.w > 0 && all(q >= 0) && all(q < int2(dim))) {
            uint4 g = gbufPrev[q];
            int3 da = abs(int3(albedoBytes(g.y)) - int3(albedoBytes(alb)));
            if (abs(asfloat(g.z) - pc.w) < 0.03 * pc.w && dot(unpackNormal(g.x), n) > 0.8 && max(da.x, max(da.y, da.z)) < 28) {
                cnt = min(asfloat(g.w) + 1, historyMax);
                acc = lerp(accumPrev[q], acc, 1 / cnt);
            }
        }
    }
    accumOut[px] = acc;
    gbufOut[px] = uint4(packNormal(n), alb, asuint(viewZ), asuint(cnt));
    depthOut[px] = ndcZ;
}
