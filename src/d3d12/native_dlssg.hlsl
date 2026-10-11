Texture2DArray<float4> SourceColor : register(t0);
Texture2DArray<float2> SourceMotion : register(t1);
Texture2DArray<float> SourceDepth : register(t2);
Texture2D<float4> Generated : register(t3);
ByteAddressBuffer DisableInterpolation : register(t4);
Texture2DArray<float4> CurrentFallback : register(t5);
Texture2D<float4> Packed : register(t6);
RWTexture2D<float4> Color : register(u0);
RWTexture2D<float2> Motion : register(u1);
RWTexture2D<float> Depth : register(u2);
// The camera fit's result for the CPU (FitNativeDlssGCamera).
RWBuffer<float> Fit : register(u3);

cbuffer Params : register(b0) {
    uint2 Extent;
    uint ColorSlice;
    uint GuideSlice;
    float4 OutputRect;
    float4 MotionRect;
    float4 DepthRect;
    float2 MotionScale;
    float2 JitterDelta;
    float4 Rotation;
    float4 SourceTangents;
    float4 TargetTangents;
    uint MotionSlice;
    uint EncodeSrgb;
    // The eye's size in the feature: Extent, or smaller when the feature runs
    // at a reduced resolution.
    uint2 FeatureExtent;
    // Where this eye sits in the feature's private textures, and the cell of
    // them this dispatch writes: the eye plus its share of any seam beside it.
    uint EyeX;
    uint CellX;
    uint CellWidth;
    uint CellHeight;
    float2 MotionNormal; // one over the feature's size
    float2 GuideScale; // guide texels per feature pixel
    // A reduced-resolution generated frame takes the real frame's detail where
    // the two differ by less than one over this, in display units, fading out
    // towards it. Zero restores none.
    float DetailFalloff;
    // How far the composed generated frame lies from B towards A: one half
    // for 2X, two thirds and one third for 3X.
    float TowardsA;
    uint DepthInverted; // nearer surfaces have larger depth values
    // The compose's output is shown from a camera of its own - the head's
    // pose when the generated frame is shown, which the layer submits it
    // with - and SyntheticRotation turns its rays into B's camera. It has
    // B's field of view and rectangle. CurrentFallback holds A in the compose.
    uint SyntheticCamera;
    float4 SyntheticRotation;
    // The camera's own motion from B to the aligned A, a rotation and a
    // translation taking B's camera space to A's, in the depth's units: the
    // previous pair's fit, or a test's. With CameraRotation.w zero the
    // vectors are written whole; otherwise less the camera's flow, which NGX
    // is then told to add back from its matrices. DepthToZA and DepthToZBC
    // linearise a depth d into the distance along the view,
    // DepthToZA / (d * DepthToZBC.x + DepthToZBC.y).
    float4 CameraRotation;
    float3 CameraMove;
    float DepthToZA;
    float2 DepthToZBC;
};

float3 rotate(float3 ray, float4 q) {
    return ray + 2 * cross(q.xyz, cross(q.xyz, ray) + q.w * ray);
}
float2 source_uv(float3 ray, float4 tangents) {
    return (ray.xy / max(-ray.z, 0.00001) - tangents.xz) / (tangents.yw - tangents.xz);
}
// DLSS-G takes display-ready colour. An sRGB view decodes on read, so encode
// before NGX sees it and decode again for the sRGB render target.
float3 encode_srgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float3 decode_srgb(float3 c) {
    c = saturate(c);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float4 display(float4 c) {
    return EncodeSrgb != 0 ? float4(encode_srgb(c.rgb), c.a) : c;
}
// The hardware sRGB decode is not an exact inverse of the formula above. An
// unresampled 8-bit texel goes back on its own code; only the resampled
// previous frame keeps the precision between codes.
float4 display_texel(float4 c) {
    return EncodeSrgb != 0 ? float4(round(encode_srgb(c.rgb) * 255.0) / 255.0, c.a) : c;
}
float4 bilinear(Texture2DArray<float4> t, float2 coordinate) {
    float2 c = clamp(coordinate, OutputRect.xy, OutputRect.xy + float2(Extent) - 1);
    int2 lo = int2(c), hi = min(lo + 1, int2(OutputRect.xy + float2(Extent) - 1));
    float2 f = frac(c);
    return lerp(lerp(t.Load(int4(lo, ColorSlice, 0)), t.Load(int4(hi.x, lo.y, ColorSlice, 0)), f.x),
                lerp(t.Load(int4(lo.x, hi.y, ColorSlice, 0)), t.Load(int4(hi, ColorSlice, 0)), f.x),
                f.y);
}
float4 sample_color(float2 coordinate) {
    return bilinear(SourceColor, coordinate);
}
// One eye's cell of a feature texture, sampled at a feature coordinate.
float4 bilinear_cell(Texture2D<float4> t, float2 coordinate) {
    float2 c = clamp(coordinate, float2(EyeX, 0), float2(EyeX, 0) + float2(FeatureExtent) - 1);
    int2 lo = int2(c), hi = min(lo + 1, int2(EyeX, 0) + int2(FeatureExtent) - 1);
    float2 f = frac(c);
    return lerp(lerp(t.Load(int3(lo, 0)), t.Load(int3(hi.x, lo.y, 0)), f.x),
                lerp(t.Load(int3(lo.x, hi.y, 0)), t.Load(int3(hi, 0)), f.x), f.y);
}
// Catmull-Rom weights for the four texels about a sample, from its fraction.
float4 catmull_rom(float f) {
    float f2 = f * f, f3 = f2 * f;
    return float4(-0.5 * f3 + f2 - 0.5 * f, 1.5 * f3 - 2.5 * f2 + 1,
                  -1.5 * f3 + 2 * f2 + 0.5 * f, 0.5 * f3 - 0.5 * f2);
}
// One eye's cell of a feature texture, sampled at a feature coordinate with a
// Catmull-Rom filter.
float4 cubic_cell(Texture2D<float4> t, float2 coordinate) {
    int2 lo = int2(EyeX, 0), hi = int2(EyeX, 0) + int2(FeatureExtent) - 1;
    float2 c = clamp(coordinate, float2(lo), float2(hi));
    int2 base = int2(floor(c));
    float2 f = c - float2(base);
    float4 wx = catmull_rom(f.x), wy = catmull_rom(f.y);
    float4 sum = 0;
    [unroll] for (int j = 0; j < 4; ++j) {
        float4 row = 0;
        [unroll] for (int i = 0; i < 4; ++i) {
            row += wx[i] * t.Load(int3(clamp(base + int2(i - 1, j - 1), lo, hi), 0));
        }
        sum += wy[j] * row;
    }
    return sum;
}
bool reduced() {
    return any(FeatureExtent != Extent);
}
// The output-pixel coordinate of a feature pixel's centre, within the eye.
float2 eye_position(uint2 p) {
    return (float2(p) + 0.5) * float2(Extent) / float2(FeatureExtent);
}

// Where the content at a point of B's view was in A, in B's camera, as an
// offset in B's UV. NGX dilates the vectors at depth edges itself, as it does
// for a game's own frame generation; doing it here measured no different and
// cost more. Jitter and MV_Scale follow the bridge's existing guide contract.
float2 towards_a(float2 uv) {
    int2 motion_lo = int2(MotionRect.xy);
    int2 motion_hi = max(motion_lo, int2(ceil(MotionRect.xy + MotionRect.zw)) - 1);
    int2 mp = clamp(int2(MotionRect.xy + uv * MotionRect.zw), motion_lo, motion_hi);
    float2 mv = SourceMotion.Load(int4(mp, MotionSlice, 0));
    float2 backward = mv * MotionScale + JitterDelta;
    float2 a_uv = uv + backward / float2(Extent);
    float3 a_ray = float3(lerp(SourceTangents.x, SourceTangents.y, a_uv.x),
                          lerp(SourceTangents.z, SourceTangents.w, a_uv.y), -1);
    float4 inverse_rotation = float4(-Rotation.xyz, Rotation.w);
    float3 b_ray = rotate(a_ray, inverse_rotation);
    return source_uv(b_ray, TargetTangents) - uv;
}

// The point of the nearest surface among a depth texel and its four
// neighbours, as NGX dilates motion at depth edges: an edge pixel moves with
// the surface in front. The full 3x3 measured the same and cost more.
float2 nearest_surface(float2 uv) {
    int2 lo = int2(DepthRect.xy);
    int2 hi = max(lo, int2(ceil(DepthRect.xy + DepthRect.zw)) - 1);
    int2 centre = clamp(int2(DepthRect.xy + uv * DepthRect.zw), lo, hi);
    int2 pick = centre;
    float best = SourceDepth.Load(int4(centre, GuideSlice, 0));
    const int2 neighbours[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    [unroll] for (int i = 0; i < 4; ++i) {
        int2 p = clamp(centre + neighbours[i], lo, hi);
        float d = SourceDepth.Load(int4(p, GuideSlice, 0));
        if (DepthInverted != 0 ? d > best : d < best) {
            best = d;
            pick = p;
        }
    }
    return (float2(pick) + 0.5 - DepthRect.xy) / DepthRect.zw;
}

// Both eyes can share one feature, side by side. Pixels of the cell outside
// the eye - the seam between eyes, or rows below a shorter eye - repeat the
// eye's nearest edge, so NGX sees each eye as if alone.
bool pack_cell(uint3 id, out uint2 cell, out uint2 p) {
    cell = uint2(CellX + id.x, id.y);
    p = uint2(clamp(int2(cell) - int2(EyeX, 0), int2(0, 0), int2(FeatureExtent) - 1));
    return id.x < CellWidth && id.y < CellHeight;
}
// The real frame at a feature pixel: the texel itself, or at a reduced
// resolution the bilinear average about its centre - for half size, exactly
// the four texels it covers.
float4 packed_color(Texture2DArray<float4> t, uint2 p) {
    float4 c;
    if (reduced()) {
        c = display(bilinear(t, OutputRect.xy + eye_position(p) - 0.5));
    } else {
        c = display_texel(t.Load(int4(int2(OutputRect.xy) + int2(p), ColorSlice, 0)));
    }
    return c;
}

// Reseeds the feature with A aligned into B's camera. A reset evaluation reads
// only colour - NGX's output does not change whatever motion and depth it is
// given - so the seed writes neither. Its rotated bilinear reads measure
// faster in wider groups than B's pack does.
[numthreads(16, 8, 1)] void SeedNativeDlssG(uint3 id : SV_DispatchThreadID) {
    uint2 cell, p;
    if (!pack_cell(id, cell, p)) {
        return;
    }
    float2 uv = (float2(p) + 0.5) / float2(FeatureExtent);
    float3 ray = float3(lerp(TargetTangents.x, TargetTangents.y, uv.x),
                        lerp(TargetTangents.z, TargetTangents.w, uv.y), -1);
    float3 previous_ray = rotate(ray, Rotation);
    float2 previous_uv = source_uv(previous_ray, SourceTangents);
    if (previous_ray.z >= -0.00001 || any(previous_uv < 0) || any(previous_uv > 1)) {
        Color[cell] = packed_color(CurrentFallback, p);
    } else {
        Color[cell] = display(sample_color(OutputRect.xy + previous_uv * float2(Extent) - 0.5));
    }
}

[numthreads(8, 8, 1)] void PackNativeDlssG(uint3 id : SV_DispatchThreadID) {
    uint2 cell, p;
    if (!pack_cell(id, cell, p)) {
        return;
    }
    Color[cell] = packed_color(SourceColor, p);
    // Motion and depth may sit on a coarser grid. The cell holding a guide
    // texel's centre writes it, sampling the game's guides at that centre.
    uint2 g = uint2((float2(cell) + 0.5) * GuideScale);
    if (any(uint2((float2(g) + 0.5) / GuideScale) != cell)) {
        return;
    }
    float2 q = clamp((float2(g) + 0.5) / GuideScale - float2(EyeX, 0), 0.5,
                     float2(FeatureExtent) - 0.5);
    float2 uv = q / float2(FeatureExtent);
    // A rotated edge can land exactly at UV 1. Fractional guide rectangles
    // also occur when the color viewport is smaller than the guide output.
    int2 depth_lo = int2(DepthRect.xy);
    int2 depth_hi = max(depth_lo, int2(ceil(DepthRect.xy + DepthRect.zw)) - 1);
    int2 dp = clamp(int2(DepthRect.xy + uv * DepthRect.zw), depth_lo, depth_hi);
    float2 motion = towards_a(uv);
    float depth = SourceDepth.Load(int4(dp, GuideSlice, 0));
    if (CameraRotation.w != 0) {
        // Where the camera's own motion alone would have taken this pixel's
        // surface: the vector less that is the object's own motion.
        float z = DepthToZA / max(depth * DepthToZBC.x + DepthToZBC.y, 1e-7);
        float3 b_point = float3(lerp(TargetTangents.x, TargetTangents.y, uv.x),
                                lerp(TargetTangents.z, TargetTangents.w, uv.y), -1) * z;
        float3 a_point = rotate(b_point, CameraRotation) + CameraMove;
        motion -= source_uv(a_point, TargetTangents) - uv;
    }
    // As a fraction of the whole feature, which may hold both eyes.
    Motion[g] = motion * float2(FeatureExtent) * MotionNormal;
    // Dilation chooses motion, while depth remains at the original pixel.
    Depth[g] = depth;
}

struct Vertex {
    float4 position : SV_Position;
};
Vertex NativeDlssGVS(uint id : SV_VertexID) {
    Vertex v;
    v.position = float4(id == 0 ? float2(-1, -1) : id == 1 ? float2(-1, 3) : float2(3, -1), 0, 1);
    return v;
}

// What an output pixel shows, as a point of B's eye (pixel centres at
// halves). Without a camera of its own, the pixel itself. With one, where its
// ray meets B's view, sampled there with no second resample; a ray that
// leaves B's view takes A where A saw it - the far side of a head turn - and
// B's nearest edge where neither did.
struct Shown {
    float2 x;
    float4 previous;
    bool from_previous;
};
Shown shown_at(float2 position) {
    Shown shown;
    shown.x = position - OutputRect.xy;
    shown.previous = float4(0, 0, 0, 1);
    shown.from_previous = false;
    if (SyntheticCamera != 0) {
        float2 uv = shown.x / float2(Extent);
        float3 ray = float3(lerp(TargetTangents.x, TargetTangents.y, uv.x),
                            lerp(TargetTangents.z, TargetTangents.w, uv.y), -1);
        float3 b_ray = rotate(ray, SyntheticRotation);
        float2 b_uv = source_uv(b_ray, TargetTangents);
        shown.x = clamp(b_uv * float2(Extent), 0.5, float2(Extent) - 0.5);
        if (b_ray.z >= -0.00001 || any(b_uv < 0) || any(b_uv > 1)) {
            float3 a_ray = rotate(b_ray, Rotation);
            float2 a_uv = source_uv(a_ray, SourceTangents);
            if (a_ray.z < -0.00001 && all(a_uv >= 0) && all(a_uv <= 1)) {
                shown.previous = bilinear(CurrentFallback, OutputRect.xy + a_uv * float2(Extent) - 0.5);
                shown.from_previous = true;
            }
        }
    }
    return shown;
}

float4 NativeDlssGPS(Vertex input) : SV_Target {
    Shown shown = shown_at(input.position.xy);
    int2 p = int2(OutputRect.xy + shown.x);
    float4 current = SourceColor.Load(int4(p, ColorSlice, 0));
    if (shown.from_previous) {
        return shown.previous;
    }
    if ((DisableInterpolation.Load(0) & 255u) != 0) {
        return SyntheticCamera != 0 ? bilinear(SourceColor, OutputRect.xy + shown.x - 0.5) : current;
    }
    // The private colour may hold two alpha bits; the real frame's alpha is exact.
    if (!reduced()) {
        float3 generated = SyntheticCamera != 0
            ? bilinear_cell(Generated, shown.x - 0.5 + float2(EyeX, 0)).rgb
            : Generated.Load(int3(p - int2(OutputRect.xy) + int2(EyeX, 0), 0)).rgb;
        // An sRGB target encodes only approximately. Decoding the nearest 8-bit
        // code, rather than a 10-bit value between two, keeps unmoved pixels exact.
        return float4(EncodeSrgb != 0 ? decode_srgb(round(generated * 255.0) / 255.0) : generated,
                      current.a);
    }
    // A reduced-resolution feature's frame is upsampled, losing the real
    // frames' detail above that resolution. The engine motion says where in B
    // each generated pixel's content is: B's texel there, less B as packed
    // there, is that detail, added back where the generated frame agrees with
    // the packed B. Where they disagree - an occlusion, or content the vectors
    // do not describe - the pixel stays as generated.
    float2 x = shown.x;
    // Catmull-Rom keeps the generated frame's edges sharper than bilinear,
    // which measured 7% less error for no measurable cost. The packed B it is
    // compared with stays bilinear: filtering both alike measured worse.
    float3 generated = cubic_cell(Generated, x * float2(FeatureExtent) / float2(Extent) - 0.5 +
                                                 float2(EyeX, 0)).rgb;
    float2 uv = x / float2(Extent);
    // The content at x came from y = x - TowardsA * motion(y); two steps of
    // that fixed point follow the motion field across most of an edge. The
    // second step takes the nearest surface's motion, which cut the error at
    // moving edges by a tenth; in the first step it made no difference.
    float2 y = uv - TowardsA * towards_a(uv);
    y = uv - TowardsA * towards_a(nearest_surface(saturate(y)));
    int2 texel = clamp(int2(y * float2(Extent)), int2(0, 0), int2(Extent) - 1);
    float2 centre = (float2(texel) + 0.5) * float2(FeatureExtent) / float2(Extent) - 0.5 +
                    float2(EyeX, 0);
    float3 low = bilinear_cell(Packed, centre).rgb;
    float3 high = display(SourceColor.Load(int4(int2(OutputRect.xy) + texel, ColorSlice, 0))).rgb;
    float difference = max(max(abs(generated.r - low.r), abs(generated.g - low.g)),
                           abs(generated.b - low.b));
    float weight = DetailFalloff > 0 ? saturate(1 - difference * DetailFalloff) : 0;
    float3 restored = generated + (high - low) * weight;
    return float4(EncodeSrgb != 0 ? decode_srgb(restored) : saturate(restored), current.a);
}

// Fits the camera's own motion between the two frames to the vectors just
// packed for B: a rotation and a translation taking B's camera space to the
// aligned A's, by Gauss-Newton on the reprojection error over a 32x32 grid
// of the eye, keeping the closer seven tenths of the points from the third
// sweep on (the rest moves on its own: the character the camera orbits).
// One group of 256 threads per eye, 4 points each, which it reads again
// each sweep. A single group runs alone on the GPU, so its work is kept
// small: a 64x64 grid and 16 sweeps cost 0.6 ms a pair at 3004x3004 and
// measured the same. The result goes to Fit for the CPU, which hands it to
// the next pair: the rotation as an axis-angle (0-2), the translation
// (3-5), the inliers (6), the points fitted (7), the median residual over
// the points in eye pixels (8), 1 for a fit made (9) and the inlier cut
// (10).
#ifdef XRFG_NATIVE_DLSSG_FIT
#define FIT_GRID 32
#define FIT_THREADS 256
#define FIT_PER_THREAD 4
#define FIT_SWEEPS 10
#define FIT_BINS 32
groupshared float g_sum[27][FIT_THREADS];
// The normal equations, solved by one thread: in group memory so no thread
// carries them in registers.
groupshared float g_m[6][7];
groupshared float g_x[6];
groupshared float g_cut;
groupshared uint g_hist[FIT_BINS];
groupshared uint g_valid;
groupshared uint g_inliers;

float3 fit_rotate(float3 v, float3 w) {
    float a = length(w);
    float3 k = w / max(a, 1e-9);
    float c = cos(a), s = sin(a);
    return v * c + cross(k, v) * s + k * dot(k, v) * (1 - c);
}
// Where x takes a point of B against where its vector says it was, in eye
// pixels.
float2 fit_residual(float x[6], float3 b, float2 target) {
    float3 p = fit_rotate(b, float3(x[0], x[1], x[2])) + float3(x[3], x[4], x[5]);
    return (source_uv(p, TargetTangents) - target) * float2(Extent);
}
// Residual bins doubling from a sixteenth of a pixel.
uint fit_bin(float r) {
    return uint(clamp(floor(log2(max(r, 1.0 / 16.0))) + 4.0, 0.0, FIT_BINS - 1.0));
}
float fit_bin_edge(uint bin) {
    return exp2(float(bin) - 3.0);
}
// The residual below which `keep` of the histogram's points lie, taken as
// evenly spread within the bin the count crosses in: a cut at a bin's edge
// alone kept up to a whole bin of the points the fit should drop.
float fit_percentile(uint keep) {
    uint seen = 0;
    [loop] for (uint bin = 0; bin < FIT_BINS; ++bin) {
        const uint count = g_hist[bin];
        if (seen + count >= keep) {
            const float lo = bin == 0 ? 0.0 : fit_bin_edge(bin - 1), hi = fit_bin_edge(bin);
            return lo + (hi - lo) * float(keep - seen) / max(float(count), 1.0);
        }
        seen += count;
    }
    return fit_bin_edge(FIT_BINS - 1);
}
// The point of the grid a thread fits, read from the packed guides: its place
// in B's camera and where its vector says it was, in the eye's UV. Points are
// read again each sweep rather than kept, which keeps the registers down.
bool fit_point(uint index, out float3 b, out float2 target) {
    const float2 uv = (float2(index % FIT_GRID, index / FIT_GRID) + 0.5) / FIT_GRID;
    const float2 guide_limit = float2(float(EyeX + FeatureExtent.x) * GuideScale.x,
                                      float(FeatureExtent.y) * GuideScale.y) - 1;
    const uint2 g = uint2(clamp(float2(float(EyeX) + uv.x * FeatureExtent.x,
                                       uv.y * FeatureExtent.y) * GuideScale, 0, guide_limit));
    const float2 m = Motion[g] / (float2(FeatureExtent) * MotionNormal);
    const float d = Depth[g];
    const float z = clamp(DepthToZA / max(d * DepthToZBC.x + DepthToZBC.y, 1e-9), 1e-4, 1e7);
    b = float3(lerp(TargetTangents.x, TargetTangents.y, uv.x),
               lerp(TargetTangents.z, TargetTangents.w, uv.y), -1) * z;
    target = uv + m;
    // The pack wrote this vector less the camera's flow of the pair before's
    // fit, for NGX. The fit measures the whole motion - the next pair takes
    // its result as the camera's motion entire - so that flow goes back in,
    // from the same depth and constants the pack took it out with. Fitting
    // the vectors as packed measured the motion left over instead, which
    // live alternated pair by pair between the whole turn and nothing.
    if (CameraRotation.w != 0) {
        const float zc = DepthToZA / max(d * DepthToZBC.x + DepthToZBC.y, 1e-7);
        const float3 b_point = float3(lerp(TargetTangents.x, TargetTangents.y, uv.x),
                                      lerp(TargetTangents.z, TargetTangents.w, uv.y), -1) * zc;
        const float3 a_point = rotate(b_point, CameraRotation) + CameraMove;
        target += source_uv(a_point, TargetTangents) - uv;
    }
    return all(isfinite(m)) && all(abs(m) <= 1) && isfinite(d);
}

[numthreads(FIT_THREADS, 1, 1)] void FitNativeDlssGCamera(uint3 id : SV_GroupThreadID) {
    const uint t = id.x;
    if (t == 0) {
        g_valid = 0;
        g_inliers = 0;
        g_cut = 1e30;
    }
    if (t < 6) g_x[t] = 0;
    GroupMemoryBarrierWithGroupSync();
    [loop] for (uint k0 = 0; k0 < FIT_PER_THREAD; ++k0) {
        float3 b;
        float2 target;
        if (fit_point(k0 * FIT_THREADS + t, b, target)) InterlockedAdd(g_valid, 1);
    }
    GroupMemoryBarrierWithGroupSync();
    const float step[6] = {1e-4, 1e-4, 1e-4, 1e-2, 1e-2, 1e-2};
    [loop] for (uint sweep = 0; sweep < FIT_SWEEPS; ++sweep) {
        float x[6];
        [unroll] for (uint c0 = 0; c0 < 6; ++c0) x[c0] = g_x[c0];
        const float cut = g_cut;
        if (t < FIT_BINS) g_hist[t] = 0;
        GroupMemoryBarrierWithGroupSync();
        float hs[21], bs[6];
        [unroll] for (uint q0 = 0; q0 < 21; ++q0) hs[q0] = 0;
        [unroll] for (uint a0 = 0; a0 < 6; ++a0) bs[a0] = 0;
        [loop] for (uint k = 0; k < FIT_PER_THREAD; ++k) {
            float3 b;
            float2 target;
            if (!fit_point(k * FIT_THREADS + t, b, target)) continue;
            const float2 r0 = fit_residual(x, b, target);
            const float norm = length(r0);
            InterlockedAdd(g_hist[fit_bin(norm)], 1);
            // From the third sweep, the closer seven tenths by the residuals
            // of the sweep before.
            if (sweep >= 2 && norm > cut) continue;
            float2 jac[6];
            [unroll] for (uint c = 0; c < 6; ++c) {
                float xs[6];
                [unroll] for (uint e = 0; e < 6; ++e) xs[e] = x[e];
                xs[c] += step[c];
                jac[c] = (fit_residual(xs, b, target) - r0) / step[c];
            }
            uint q = 0;
            [unroll] for (uint a = 0; a < 6; ++a) {
                bs[a] -= dot(jac[a], r0);
                [unroll] for (uint c1 = a; c1 < 6; ++c1) {
                    hs[q] += dot(jac[a], jac[c1]);
                    ++q;
                }
            }
        }
        [unroll] for (uint q2 = 0; q2 < 21; ++q2) g_sum[q2][t] = hs[q2];
        [unroll] for (uint a2 = 0; a2 < 6; ++a2) g_sum[21 + a2][t] = bs[a2];
        GroupMemoryBarrierWithGroupSync();
        if (t < 27) {
            float s = 0;
            [loop] for (uint i = 0; i < FIT_THREADS; ++i) s += g_sum[t][i];
            g_sum[t][0] = s;
        }
        GroupMemoryBarrierWithGroupSync();
        if (t == 0) {
            // The normal equations, damped a little against a view that pins
            // the translation poorly (sky), by elimination with pivoting.
            uint q3 = 0;
            [unroll] for (uint a3 = 0; a3 < 6; ++a3) {
                [unroll] for (uint c3 = a3; c3 < 6; ++c3) {
                    g_m[a3][c3] = g_sum[q3][0];
                    g_m[c3][a3] = g_sum[q3][0];
                    ++q3;
                }
                g_m[a3][6] = g_sum[21 + a3][0];
            }
            [unroll] for (uint d0 = 0; d0 < 6; ++d0) g_m[d0][d0] += 1e-4 * abs(g_m[d0][d0]) + 1e-12;
            bool solved = true;
            [unroll] for (uint c4 = 0; c4 < 6; ++c4) {
                uint pivot = c4;
                [unroll] for (uint r = c4 + 1; r < 6; ++r) if (abs(g_m[r][c4]) > abs(g_m[pivot][c4])) pivot = r;
                if (abs(g_m[pivot][c4]) < 1e-20) solved = false;
                if (pivot != c4) {
                    [unroll] for (uint e1 = 0; e1 < 7; ++e1) { float tmp = g_m[c4][e1]; g_m[c4][e1] = g_m[pivot][e1]; g_m[pivot][e1] = tmp; }
                }
                [unroll] for (uint r2 = c4 + 1; r2 < 6; ++r2) {
                    const float factor = g_m[r2][c4] / g_m[c4][c4];
                    [unroll] for (uint e2 = c4; e2 < 7; ++e2) g_m[r2][e2] -= factor * g_m[c4][e2];
                }
            }
            if (solved) {
                float dx[6];
                [unroll] for (int c5 = 5; c5 >= 0; --c5) {
                    float s = g_m[c5][6];
                    [unroll] for (int e3 = c5 + 1; e3 < 6; ++e3) s -= g_m[c5][e3] * dx[e3];
                    dx[c5] = s / g_m[c5][c5];
                }
                [unroll] for (uint c6 = 0; c6 < 6; ++c6) g_x[c6] += dx[c6];
            }
            if (sweep >= 1) g_cut = fit_percentile((g_valid * 7 + 9) / 10);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    // The inliers by the last sweep's cut, and the median residual over the
    // points, from the last sweep's histogram.
    {
        const float cut = g_cut;
        float x[6];
        [unroll] for (uint c7 = 0; c7 < 6; ++c7) x[c7] = g_x[c7];
        [loop] for (uint k2 = 0; k2 < FIT_PER_THREAD; ++k2) {
            float3 b;
            float2 target;
            if (!fit_point(k2 * FIT_THREADS + t, b, target)) continue;
            if (length(fit_residual(x, b, target)) <= cut) InterlockedAdd(g_inliers, 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (t == 0) {
        [unroll] for (uint c8 = 0; c8 < 6; ++c8) Fit[c8] = g_x[c8];
        Fit[6] = float(g_inliers);
        Fit[7] = float(g_valid);
        Fit[8] = fit_percentile((g_valid + 1) / 2);
        Fit[9] = 1;
        Fit[10] = g_cut;
    }
}
#endif
