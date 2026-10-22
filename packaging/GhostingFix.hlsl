// BeamNG-VRS ghosting fix: stereo-aligned local exposure and bloom.
//
// Launch-BeamNG-VRS.ps1 inserts this file into a copy of the game's own
// shaders/common/postfx/hdr/combinePass.cs.hlsl and writes the result to the BeamNG user
// folder. It only changes where the combine pass reads its shared textures.
//
// Why: in BeamNG 0.40, the local exposure textures (blurred log-luminance and bilateral
// grid) and bloom are built from one VR eye and then reused for the other eye. In the
// other eye, the first eye's picture shows through as dark or bright "ghost" shapes,
// offset sideways.
//
// How: the two eyes see almost the same picture, shifted sideways by the eye spacing and
// the lens FOV. The fix compares this eye's own picture with the shared blurred-luminance
// texture at 32 probe points, for 48 candidate sideways shifts, and keeps the best match.
// It then reads local exposure and bloom at the shifted position. Three cases:
//   - The eye the textures were built from measures a shift of about 0, so it renders
//     like the stock game.
//   - So does the desktop.
//   - So does a future game version that builds the textures per eye.
// Each 8x8 thread group measures the shift itself, since the combine pass has no buffer
// to share results. Every group reads the same inputs, so every group finds the same
// shift (no seams).

#define VRS_STEREO_PROBES_X 8
#define VRS_STEREO_PROBES_Y 4
#define VRS_STEREO_PROBES (VRS_STEREO_PROBES_X * VRS_STEREO_PROBES_Y)
#define VRS_STEREO_CANDIDATES 48      // threads 0..47 of the 64 in a group test one shift each
#define VRS_STEREO_STEP (1.0f / 150.0f) // UV per candidate: shifts from -0.16 to +0.153
#define VRS_STEREO_PRIOR 0.01f        // small cost per step, so flat scenes keep shift 0
#define VRS_STEREO_MISMATCH 2.0f      // stops; caps one probe's cost (near objects disagree)
// Probes in a row are VRS_STEREO_PROBE_STEPS shifts apart, so neighbouring probes share
// shifted positions: each row of the shared texture is read once, at
// (VRS_STEREO_PROBES_X - 1) * VRS_STEREO_PROBE_STEPS + VRS_STEREO_CANDIDATES points.
#define VRS_STEREO_PROBE_STEPS 14
#define VRS_STEREO_ROW ((VRS_STEREO_PROBES_X - 1) * VRS_STEREO_PROBE_STEPS + VRS_STEREO_CANDIDATES)
// 1 = read bloom at the same shifted position (bloom comes from the same eye as local
// exposure); 0 = read it in place.
#define VRS_STEREO_SHIFT_BLOOM 1
// 1 = draw the measured shift as a bar at the top of the image.
#define VRS_STEREO_DEBUG 0

groupshared float vrsProbeLogLum[VRS_STEREO_PROBES];
groupshared float vrsSharedLogLum[VRS_STEREO_PROBES_Y * VRS_STEREO_ROW];
groupshared float vrsCandidateCost[VRS_STEREO_CANDIDATES];
groupshared float vrsShiftShared;
static float2 vrsLocalExposureShift = float2(0.0f, 0.0f);
static float2 vrsBloomShift = float2(0.0f, 0.0f);

// Probe p of a row sits at shared-row index p * VRS_STEREO_PROBE_STEPS + VRS_STEREO_CANDIDATES / 2.
float vrsRowU(uint index) {
    const float centre = 0.5f * ((VRS_STEREO_PROBES_X - 1) * VRS_STEREO_PROBE_STEPS + VRS_STEREO_CANDIDATES);
    return 0.5f + (float(index) - centre) * VRS_STEREO_STEP;
}

float vrsRowV(uint row) {
    return 0.25f + 0.5f * (float(row) + 0.5f) / VRS_STEREO_PROBES_Y;
}

float2 vrsProbeUv(uint probe) {
    const uint column = probe % VRS_STEREO_PROBES_X;
    return float2(vrsRowU(column * VRS_STEREO_PROBE_STEPS + VRS_STEREO_CANDIDATES / 2), vrsRowV(probe / VRS_STEREO_PROBES_X));
}

float vrsLogLum(float3 rgb) {
    return log2(max(dot(max(rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)), 1e-6f));
}

// This eye's log-luminance blurred about as much as the game's localExposureBlurredLogLum
// (quarter resolution plus a 9-tap blur): a 1-2-1 weighted 3x3 grid of bilinear taps
// 8 pixels apart.
float vrsBlurredLogLum(float2 uv) {
    uint2 size;
    backbuffer.GetDimensions(size.x, size.y);
    const float2 texel = 8.0f / float2(size);
    float sum = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float weight = float((2 - abs(x)) * (2 - abs(y)));
            sum += weight * vrsLogLum(backbuffer.SampleLevel(samplerLinear, uv + float2(x, y) * texel, 0).rgb);
        }
    }
    return sum / 16.0f;
}

// Called by every thread of the group, before any thread returns.
void vrsEstimateEyeShift(uint thread) {
#ifdef BNG_COCONUT
    // 1. This eye's picture at the probe points, and the shared texture along the probe
    // rows (every position any candidate shift needs).
    if (thread < VRS_STEREO_PROBES) vrsProbeLogLum[thread] = vrsBlurredLogLum(vrsProbeUv(thread));
    for (uint i = thread; i < VRS_STEREO_PROBES_Y * VRS_STEREO_ROW; i += 64) {
        const float2 uv = saturate(float2(vrsRowU(i % VRS_STEREO_ROW), vrsRowV(i / VRS_STEREO_ROW)));
        vrsSharedLogLum[i] = localExposureBlurredLogLum.SampleLevel(samplerLinear, uv, 0).r;
    }
    GroupMemoryBarrierWithGroupSync();

    float probeMean = 0.0f;
    for (uint p = 0; p < VRS_STEREO_PROBES; ++p) probeMean += vrsProbeLogLum[p];
    probeMean /= VRS_STEREO_PROBES;

    // 2. One candidate shift per thread. Means are removed so a global exposure
    // difference between the two images does not matter.
    if (thread < VRS_STEREO_CANDIDATES) {
        float sharedMean = 0.0f;
        [unroll] for (uint p = 0; p < VRS_STEREO_PROBES; ++p) {
            sharedMean += vrsSharedLogLum[(p / VRS_STEREO_PROBES_X) * VRS_STEREO_ROW + (p % VRS_STEREO_PROBES_X) * VRS_STEREO_PROBE_STEPS + thread];
        }
        sharedMean /= VRS_STEREO_PROBES;
        float cost = VRS_STEREO_PRIOR * abs(float(thread) - VRS_STEREO_CANDIDATES / 2);
        [unroll] for (uint q = 0; q < VRS_STEREO_PROBES; ++q) {
            const float sharedValue = vrsSharedLogLum[(q / VRS_STEREO_PROBES_X) * VRS_STEREO_ROW + (q % VRS_STEREO_PROBES_X) * VRS_STEREO_PROBE_STEPS + thread];
            const float difference = (vrsProbeLogLum[q] - probeMean) - (sharedValue - sharedMean);
            cost += min(abs(difference), VRS_STEREO_MISMATCH);
        }
        vrsCandidateCost[thread] = cost;
    }
    GroupMemoryBarrierWithGroupSync();

    // 3. Best candidate, refined between steps with a parabola so the shift moves smoothly.
    if (thread == 0) {
        uint best = 0;
        for (uint c = 1; c < VRS_STEREO_CANDIDATES; ++c) {
            if (vrsCandidateCost[c] < vrsCandidateCost[best]) best = c;
        }
        float fraction = 0.0f;
        if (best > 0 && best + 1 < VRS_STEREO_CANDIDATES) {
            const float left = vrsCandidateCost[best - 1];
            const float centre = vrsCandidateCost[best];
            const float right = vrsCandidateCost[best + 1];
            const float curvature = left - 2.0f * centre + right;
            if (curvature > 1e-6f) fraction = clamp(0.5f * (left - right) / curvature, -0.5f, 0.5f);
        }
        vrsShiftShared = (float(best) - VRS_STEREO_CANDIDATES / 2 + fraction) * VRS_STEREO_STEP;
    }
    GroupMemoryBarrierWithGroupSync();
    vrsLocalExposureShift = float2(vrsShiftShared, 0.0f);

    // 4. Bloom is built in the same once-per-frame chain, so it gets the same shift.
#if VRS_STEREO_SHIFT_BLOOM
    vrsBloomShift = vrsLocalExposureShift;
#endif
#endif
}

// Debug bar: its length from the centre shows the measured shift (full width = 0.16 UV).
float3 vrsDebugOverlay(float3 color, float2 uv) {
#if VRS_STEREO_DEBUG
    if (uv.y < 0.02f) {
        const float end = 0.5f + vrsLocalExposureShift.x / 0.32f;
        if ((uv.x - 0.5f) * (uv.x - end) <= 0.0f) {
            return float3(0.0f, 1.0f, 0.0f);
        }
    }
#endif
    return color;
}
// ---- end of BeamNG-VRS ghosting fix ----

