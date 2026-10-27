// =====================================================================================
// BeamNG-VRS ghosting fix: stereo-aligned local exposure and bloom
// =====================================================================================
//
// WHAT THIS FILE IS
//   Launch-BeamNG-VRS.ps1 inserts this file into a copy of the game's own
//   shaders/common/postfx/hdr/combinePass.cs.hlsl and writes the result to the BeamNG
//   user folder ([Renderer] GhostingFix). The patched shader only changes WHERE the
//   combine pass reads two shared textures; everything else stays the game's code.
//
// THE BUG (BeamNG 0.40)
//   The local exposure textures (blurred log-luminance and bilateral grid) and bloom
//   are built from one VR eye, then reused for the other eye. In that other eye, the
//   first eye's picture shows through as dark or bright "ghost" shapes, offset sideways.
//
// THE FIX
//   Both eyes see almost the same picture, shifted sideways by the eye spacing and the
//   lens FOV. So each eye:
//     1. samples its own picture at 32 probe points,
//     2. compares them with the shared blurred-luminance texture at 48 candidate
//        sideways shifts,
//     3. keeps the best match, and reads local exposure and bloom at that shift.
//
//   Cases where the measured shift is about 0, so the picture is the stock one:
//     - the eye the textures were built from,
//     - the desktop view,
//     - a future game version that builds the textures per eye.
//
// WHY EVERY 8x8 GROUP REPEATS THE MEASUREMENT
//   The combine pass has no buffer to share a result between thread groups. Every group
//   reads the same inputs with the same code, so every group finds the same shift and
//   no seams appear between blocks.
//
// KNOWN LIMIT
//   One shift is used for the whole picture. It matches the scenery; very near objects
//   (the cockpit) have more parallax and can still be slightly off.
// =====================================================================================


// ---- Tuning ---------------------------------------------------------------------------

// Probe grid: 8 columns x 4 rows, in the middle half of the image height.
#define VRS_STEREO_PROBES_X 8
#define VRS_STEREO_PROBES_Y 4
#define VRS_STEREO_PROBES (VRS_STEREO_PROBES_X * VRS_STEREO_PROBES_Y)

// Candidate shifts: threads 0..47 of the 64 in a group each test one shift.
// With a step of 1/150 of the image width they cover -0.16 .. +0.153.
#define VRS_STEREO_CANDIDATES 48
#define VRS_STEREO_STEP (1.0f / 150.0f)

// Small extra cost per step away from 0, so a flat scene (fog, night sky) keeps shift 0.
#define VRS_STEREO_PRIOR 0.01f

// Largest cost one probe can add, in stops. Near objects disagree with the scenery's
// shift; the cap stops them from outvoting the rest of the picture.
#define VRS_STEREO_MISMATCH 2.0f

// Probes in a row sit VRS_STEREO_PROBE_STEPS candidate steps apart. Neighbouring probes
// then need the same shifted positions, so each probe row of the shared texture is read
// once into group memory (VRS_STEREO_ROW samples) instead of once per candidate.
#define VRS_STEREO_PROBE_STEPS 14
#define VRS_STEREO_ROW ((VRS_STEREO_PROBES_X - 1) * VRS_STEREO_PROBE_STEPS + VRS_STEREO_CANDIDATES)

// 1 = read bloom at the same shift (bloom comes from the same eye as local exposure).
// 0 = read bloom in place.
#define VRS_STEREO_SHIFT_BLOOM 1

// 1 = draw the measured shift as a green bar at the top of each eye (for testing).
#define VRS_STEREO_DEBUG 0


// ---- State ----------------------------------------------------------------------------

// Group memory, filled cooperatively by the 64 threads of a group.
groupshared float vrsProbeLogLum[VRS_STEREO_PROBES];                   // this eye
groupshared float vrsSharedLogLum[VRS_STEREO_PROBES_Y * VRS_STEREO_ROW]; // shared texture
groupshared float vrsCandidateCost[VRS_STEREO_CANDIDATES];
groupshared float vrsShiftShared;

// Per-thread results, read by the patched lines of the game's combine pass.
static float2 vrsLocalExposureShift = float2(0.0f, 0.0f);
static float2 vrsBloomShift = float2(0.0f, 0.0f);


// ---- Probe positions ------------------------------------------------------------------

// Horizontal UV of sample `index` in a probe row. Index 0..VRS_STEREO_ROW-1 covers every
// probe at every candidate shift; the row is centred on the image.
float vrsRowU(uint index) {
    const float centre = 0.5f * ((VRS_STEREO_PROBES_X - 1) * VRS_STEREO_PROBE_STEPS + VRS_STEREO_CANDIDATES);
    return 0.5f + (float(index) - centre) * VRS_STEREO_STEP;
}

// Vertical UV of probe row `row`: spread over 0.25 .. 0.75 of the image height.
float vrsRowV(uint row) {
    return 0.25f + 0.5f * (float(row) + 0.5f) / VRS_STEREO_PROBES_Y;
}

// Unshifted position of probe `probe`. In its row it sits at sample index
// column * VRS_STEREO_PROBE_STEPS + VRS_STEREO_CANDIDATES / 2 (the shift-0 candidate).
float2 vrsProbeUv(uint probe) {
    const uint column = probe % VRS_STEREO_PROBES_X;
    const uint row = probe / VRS_STEREO_PROBES_X;

    return float2(vrsRowU(column * VRS_STEREO_PROBE_STEPS + VRS_STEREO_CANDIDATES / 2), vrsRowV(row));
}

// Index in vrsSharedLogLum of probe `probe` seen through candidate shift `candidate`.
uint vrsSharedIndex(uint probe, uint candidate) {
    const uint column = probe % VRS_STEREO_PROBES_X;
    const uint row = probe / VRS_STEREO_PROBES_X;

    return row * VRS_STEREO_ROW + column * VRS_STEREO_PROBE_STEPS + candidate;
}


// ---- Luminance --------------------------------------------------------------------------

float vrsLogLum(float3 rgb) {
    return log2(max(dot(max(rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)), 1e-6f));
}

// This eye's log-luminance, blurred about as much as the game's
// localExposureBlurredLogLum (quarter resolution, then a 9-tap blur):
// a 3x3 grid of bilinear taps 8 pixels apart, weighted 1-2-1 in each direction.
float vrsBlurredLogLum(float2 uv) {
    uint2 size;
    backbuffer.GetDimensions(size.x, size.y);
    const float2 texel = 8.0f / float2(size);

    float sum = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float weight = float((2 - abs(x)) * (2 - abs(y)));
            const float3 rgb = backbuffer.SampleLevel(samplerLinear, uv + float2(x, y) * texel, 0).rgb;
            sum += weight * vrsLogLum(rgb);
        }
    }

    return sum / 16.0f; // the weights add up to 16
}


// ---- Shift measurement -------------------------------------------------------------------

// Measures this eye's sideways shift against the shared textures and stores it in
// vrsLocalExposureShift / vrsBloomShift. It is called from main() by EVERY thread of the
// group, before any thread returns, because it uses group barriers.
//
// Only the BNG_COCONUT path of the combine pass uses local exposure; the old path is
// left alone.
void vrsEstimateEyeShift(uint thread) {
#ifdef BNG_COCONUT

    // Step 1: gather the inputs into group memory.
    //   - threads 0..31 sample this eye's picture at one probe each;
    //   - all 64 threads share the reads of the shared texture along the probe rows.
    if (thread < VRS_STEREO_PROBES) {
        vrsProbeLogLum[thread] = vrsBlurredLogLum(vrsProbeUv(thread));
    }

    for (uint i = thread; i < VRS_STEREO_PROBES_Y * VRS_STEREO_ROW; i += 64) {
        const float2 uv = saturate(float2(vrsRowU(i % VRS_STEREO_ROW), vrsRowV(i / VRS_STEREO_ROW)));
        vrsSharedLogLum[i] = localExposureBlurredLogLum.SampleLevel(samplerLinear, uv, 0).r;
    }

    GroupMemoryBarrierWithGroupSync();


    // Step 2: score one candidate shift per thread (threads 0..47).
    //   Both sides have their mean removed, so a global brightness difference between
    //   this eye and the shared texture does not matter; only the pattern has to match.
    float probeMean = 0.0f;
    for (uint p = 0; p < VRS_STEREO_PROBES; ++p) {
        probeMean += vrsProbeLogLum[p];
    }
    probeMean /= VRS_STEREO_PROBES;

    if (thread < VRS_STEREO_CANDIDATES) {
        float sharedMean = 0.0f;
        [unroll] for (uint p = 0; p < VRS_STEREO_PROBES; ++p) {
            sharedMean += vrsSharedLogLum[vrsSharedIndex(p, thread)];
        }
        sharedMean /= VRS_STEREO_PROBES;

        float cost = VRS_STEREO_PRIOR * abs(float(thread) - VRS_STEREO_CANDIDATES / 2);
        [unroll] for (uint q = 0; q < VRS_STEREO_PROBES; ++q) {
            const float mine = vrsProbeLogLum[q] - probeMean;
            const float theirs = vrsSharedLogLum[vrsSharedIndex(q, thread)] - sharedMean;
            cost += min(abs(mine - theirs), VRS_STEREO_MISMATCH);
        }

        vrsCandidateCost[thread] = cost;
    }

    GroupMemoryBarrierWithGroupSync();


    // Step 3: thread 0 picks the cheapest candidate, then fits a parabola through it and
    // its two neighbours. The fractional step keeps the shift moving smoothly instead of
    // jumping a whole step at a time as the head turns.
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
            if (curvature > 1e-6f) {
                fraction = clamp(0.5f * (left - right) / curvature, -0.5f, 0.5f);
            }
        }

        vrsShiftShared = (float(best) - VRS_STEREO_CANDIDATES / 2 + fraction) * VRS_STEREO_STEP;
    }

    GroupMemoryBarrierWithGroupSync();


    // Step 4: every thread takes the result. Bloom is built in the same once-per-frame
    // chain as local exposure, so it gets the same shift.
    vrsLocalExposureShift = float2(vrsShiftShared, 0.0f);

#if VRS_STEREO_SHIFT_BLOOM
    vrsBloomShift = vrsLocalExposureShift;
#endif

#endif // BNG_COCONUT
}


// ---- Debug overlay ---------------------------------------------------------------------

// With VRS_STEREO_DEBUG 1: a green bar along the top edge, starting at the centre. Its
// length and direction show the measured shift (half the width = 0.16 UV).
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

// ---- end of BeamNG-VRS ghosting fix -------------------------------------------------------

