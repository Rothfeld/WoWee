#version 450

// FXAA 3.11, quality preset 12 (Timothy Lottes). Reads the resolved scene
// colour and writes the smoothed result.
// Push constants: pixel size, sharpness, intoxication, glow, AA enable.
//
// A port of the reference rather than a paraphrase of it. The pass this
// replaces had the shape of FXAA and two of its numbers wrong. Its
// end-of-edge search compared each sample against lumaM minus the average of
// the two neighbours - a signed contrast, close to zero - where the reference
// subtracts the luma midway between the pixel and the neighbour across the
// edge, so the search ended on its first step nearly everywhere. The offset
// it then applied was dst/span where the reference has 0.5 - dst/span, so the
// middle of every edge got the full half-pixel blend and the stair-steps at
// the ends got none. Together that drew a half-pixel smear along every edge
// and removed no aliasing, which is what "FXAA makes everything blurry and
// does not help" looks like from the chair.

layout(set = 0, binding = 0) uniform sampler2D uScene;

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    vec2  rcpFrame;
    float sharpness;    // 0 = no sharpen, 2 = max (matches FSR2 RCAS range)
    float intoxication;
    float glow;
    float enableAA;
    float deathEffect;
    float netherWorld;  // 0..1 Nether World blend strength (ffxNetherWorld)
    float netherTime;   // elapsed seconds for ffxnetherblur rotation
} pc;

// The reference's default tuning. Subpix 0.75 is what it calls the default
// amount of filtering; 0.50 is the value it names as sharper, if this still
// reads soft on fine texture.
#define FXAA_EDGE_THRESHOLD      0.166   // local contrast below this is left alone
#define FXAA_EDGE_THRESHOLD_MIN  0.0833  // and darks below this, whatever their contrast
#define FXAA_SUBPIX              0.75    // sub-pixel aliasing removal, 0 = off
#define FXAA_SEARCH_STEPS        12

// Quality preset 12: fine steps next to the pixel, coarse ones far out along
// the edge. Eleven samples a side at most; the last step only positions.
const float kSearchStep[FXAA_SEARCH_STEPS] =
    float[](1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 compositeGlow(vec3 scene, vec2 uv) {
    if (pc.glow == 0.0) return scene;
    // Separable four-tap binomial Gaussian, evaluated as one 16-tap pass.
    // Normalized footprint keeps the effect stable when resolution changes.
    const float weights[4] = float[](0.125, 0.375, 0.375, 0.125);
    vec2 stepUV = vec2(7.0) / vec2(1280.0, 720.0);
    vec3 blurred = vec3(0.0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            blurred += texture(uScene, uv + (vec2(x, y) - 1.5) * stepUV).rgb
                     * weights[x] * weights[y];
    return scene + pc.glow * blurred * blurred;
}

vec3 compositeNetherWorld(vec3 scene, vec2 uv) {
    if (pc.netherWorld <= 0.0) return scene;
    // TBC 2.4.3 ffxnetherblur.bls + ffxnethercombine.bls:
    // 4-tap directional blur on a 256x192 target (2x2 pre-box approximates downsampling),
    // with per-region angle offset (vColor.r * 2 - 1) * 3.0 + c_fBaseAngle and
    // c_fSampleOffs = 0.008 in UV space (not aspect-corrected).
    float spatialPhase = 0.5 + 0.5 * sin(uv.x * 5.0 + uv.y * 3.0);
    float baseAngle = pc.netherTime * 1.6 + 0.5 * sin(pc.netherTime * 0.9);
    float angle = (spatialPhase * 2.0 - 1.0) * 3.0 + baseAngle;
    vec2 stepUV = vec2(cos(angle), sin(angle)) * 0.008;
    vec2 boxUV = vec2(0.00195313, 0.00260417);  // 0.5/256, 0.5/192 from ffxnetherblur.bls

    vec3 blur = vec3(0.0);
    for (int k = 0; k < 4; ++k) {
        vec2 c = uv + float(k) * stepUV;
        vec3 tap = 0.25 * (
            texture(uScene, c + vec2(-boxUV.x, -boxUV.y)).rgb +
            texture(uScene, c + vec2( boxUV.x, -boxUV.y)).rgb +
            texture(uScene, c + vec2(-boxUV.x,  boxUV.y)).rgb +
            texture(uScene, c + vec2( boxUV.x,  boxUV.y)).rgb
        );
        blur += tap;
    }
    blur *= 0.25;

    // Exact ffxnethercombine.bls formula with measured vColorMul = (0.60, 0.60, 0.77, 0.76).
    vec3 T = 0.5 * scene + blur;
    vec3 M = 0.5 * T + vec3(dot(T, vec3(0.16666649)));
    vec3 nether = M * vec3(0.60, 0.60, 0.77);
    return clamp(mix(scene, nether, 0.76 * pc.netherWorld), 0.0, 1.0);
}

vec3 compositeDeath(vec3 scene) {
    if (pc.deathEffect == 0.0) return scene;
    // Legacy death compositing: luminance plus cyan midtone lift. The blue
    // coefficient is 0.144, not the regular FXAA luminance coefficient.
    float gray = clamp(dot(scene, vec3(0.299, 0.587, 0.144)), 0.0, 1.0);
    float midtone = 4.0 * gray * (1.0 - gray);
    return min(vec3(gray) + (vec3(83.0, 147.0, 168.0) / 255.0) * midtone, vec3(1.0));
}

vec3 compositeDrunk(vec3 scene, vec2 uv) {
    float drunk = clamp(pc.intoxication, 0.0, 1.0);
    if (drunk <= 0.0) return scene;
    // Native 2.4.3 fullscreen drunk blur: smooth, wide, isotropic Gaussian blur
    // (~5-6 px sigma at 1280x720) with no sine-wave UV ripple or ghosting.
    // 7x7 bilinear-spaced taps at 2.2px step (normalized to 1280x720) cover +-6.6px
    // smoothly with Gaussian weights (sigma = 5.5px).
    const float w[7] = float[](0.0681, 0.1292, 0.1932, 0.2190, 0.1932, 0.1292, 0.0681);
    vec2 stepUV = vec2(2.2) / vec2(1280.0, 720.0);
    vec3 blurred = vec3(0.0);
    for (int y = 0; y < 7; ++y) {
        float wy = w[y];
        float oy = (float(y) - 3.0) * stepUV.y;
        for (int x = 0; x < 7; ++x) {
            float ox = (float(x) - 3.0) * stepUV.x;
            blurred += texture(uScene, uv + vec2(ox, oy)).rgb * (w[x] * wy);
        }
    }
    return mix(scene, blurred, drunk);
}

void main() {
    vec2 uv  = TexCoord;
    vec2 rcp = pc.rcpFrame;

    // --- Centre sample ---
    vec3 rgbM = texture(uScene, uv).rgb;
    if (pc.intoxication > 0.0) {
        vec3 drunkScene = compositeDrunk(rgbM, uv);
        outColor = vec4(compositeDeath(compositeNetherWorld(compositeGlow(drunkScene, uv), uv)), 1.0);
        return;
    }

    // --- Cardinal neighbours: is there an edge here at all ---
    float lumaM = luma(rgbM);
    float lumaN = luma(texture(uScene, uv + vec2( 0.0, -1.0) * rcp).rgb);
    float lumaS = luma(texture(uScene, uv + vec2( 0.0,  1.0) * rcp).rgb);
    float lumaE = luma(texture(uScene, uv + vec2( 1.0,  0.0) * rcp).rgb);
    float lumaW = luma(texture(uScene, uv + vec2(-1.0,  0.0) * rcp).rgb);

    float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float range    = rangeMax - rangeMin;

    if (pc.enableAA == 0.0 || range < max(FXAA_EDGE_THRESHOLD_MIN, rangeMax * FXAA_EDGE_THRESHOLD)) {
        outColor = vec4(compositeDeath(compositeNetherWorld(compositeGlow(rgbM, uv), uv)), 1.0);
        return;
    }

    float lumaNW = luma(texture(uScene, uv + vec2(-1.0, -1.0) * rcp).rgb);
    float lumaNE = luma(texture(uScene, uv + vec2( 1.0, -1.0) * rcp).rgb);
    float lumaSW = luma(texture(uScene, uv + vec2(-1.0,  1.0) * rcp).rgb);
    float lumaSE = luma(texture(uScene, uv + vec2( 1.0,  1.0) * rcp).rgb);

    float lumaNS   = lumaN + lumaS;
    float lumaWE   = lumaW + lumaE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float lumaSWSE = lumaSW + lumaSE;

    // --- Sub-pixel aliasing: how far the pixel sits from its 3x3 lowpass ---
    float subpixA = (lumaNS + lumaWE) * 2.0 + (lumaNWSW + lumaNESE);
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float subpixC = clamp(abs(subpixB) / range, 0.0, 1.0);
    float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;   // smoothstep
    float subpixH = subpixF * subpixF * FXAA_SUBPIX;

    // --- Edge orientation ---
    float edgeHorz = abs(-2.0 * lumaW + lumaNWSW)
                   + abs(-2.0 * lumaM + lumaNS) * 2.0
                   + abs(-2.0 * lumaE + lumaNESE);
    float edgeVert = abs(-2.0 * lumaS + lumaSWSE)
                   + abs(-2.0 * lumaM + lumaWE) * 2.0
                   + abs(-2.0 * lumaN + lumaNWNE);
    bool horzSpan = edgeHorz >= edgeVert;

    // The neighbour across the edge is the one with the larger gradient from
    // the pixel; lengthSign points at it.
    float luma1 = horzSpan ? lumaN : lumaW;
    float luma2 = horzSpan ? lumaS : lumaE;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;
    bool  pair1 = abs(gradient1) >= abs(gradient2);
    float gradientScaled = max(abs(gradient1), abs(gradient2)) * 0.25;
    float lengthSign = horzSpan ? rcp.y : rcp.x;
    if (pair1) lengthSign = -lengthSign;

    // The luma half-way between the pixel and that neighbour. The search walks
    // along the edge, half a pixel over onto the line between the two, and a
    // side is done where its sample leaves this midpoint by more than a
    // quarter of the gradient: the end of the edge on that side.
    float lumaMid     = ((pair1 ? luma1 : luma2) + lumaM) * 0.5;
    bool  lumaMLTZero = (lumaM - lumaMid) < 0.0;

    vec2 posB  = uv;
    vec2 offNP = horzSpan ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);
    if ( horzSpan) posB.y += lengthSign * 0.5;
    if (!horzSpan) posB.x += lengthSign * 0.5;

    vec2  posN = posB - offNP * kSearchStep[0];
    vec2  posP = posB + offNP * kSearchStep[0];
    float lumaEndN = luma(texture(uScene, posN).rgb) - lumaMid;
    float lumaEndP = luma(texture(uScene, posP).rgb) - lumaMid;
    bool  doneN = abs(lumaEndN) >= gradientScaled;
    bool  doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * kSearchStep[1];
    if (!doneP) posP += offNP * kSearchStep[1];
    for (int i = 2; i < FXAA_SEARCH_STEPS && !(doneN && doneP); ++i) {
        if (!doneN) lumaEndN = luma(texture(uScene, posN).rgb) - lumaMid;
        if (!doneP) lumaEndP = luma(texture(uScene, posP).rgb) - lumaMid;
        doneN = doneN || abs(lumaEndN) >= gradientScaled;
        doneP = doneP || abs(lumaEndP) >= gradientScaled;
        if (!doneN) posN -= offNP * kSearchStep[i];
        if (!doneP) posP += offNP * kSearchStep[i];
    }

    // --- Where along the span this pixel is, and how much to blend for it ---
    float dstN = horzSpan ? (uv.x - posN.x) : (uv.y - posN.y);
    float dstP = horzSpan ? (posP.x - uv.x) : (posP.y - uv.y);
    bool  directionN = dstN < dstP;
    float dst        = min(dstN, dstP);
    float spanLength = dstN + dstP;
    bool  goodSpanN  = (lumaEndN < 0.0) != lumaMLTZero;
    bool  goodSpanP  = (lumaEndP < 0.0) != lumaMLTZero;
    bool  goodSpan   = directionN ? goodSpanN : goodSpanP;

    // Half a pixel at the near end of the span, where the stair-step is,
    // falling to nothing at its middle, where the edge is already straight.
    float pixelOffset      = 0.5 - dst / spanLength;
    float pixelOffsetGood  = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetFinal = max(pixelOffsetGood, subpixH);

    vec2 finalUV = uv;
    if ( horzSpan) finalUV.y += pixelOffsetFinal * lengthSign;
    if (!horzSpan) finalUV.x += pixelOffsetFinal * lengthSign;

    vec3 fxaaResult = texture(uScene, finalUV).rgb;

    // Post-FXAA contrast-adaptive sharpening (unsharp mask), only when FSR2
    // hands over its sharpness: it restores what the upscale softened.
    if (pc.sharpness > 0.0) {
        vec2 r = pc.rcpFrame;
        vec3 blur = (texture(uScene, uv + vec2(-r.x, 0)).rgb
                   + texture(uScene, uv + vec2( r.x, 0)).rgb
                   + texture(uScene, uv + vec2(0, -r.y)).rgb
                   + texture(uScene, uv + vec2(0,  r.y)).rgb) * 0.25;
        // scale sharpness from [0,2] to a modest [0, 0.3] boost factor
        float s = pc.sharpness * 0.15;
        fxaaResult = clamp(fxaaResult + s * (fxaaResult - blur), 0.0, 1.0);
    }

    outColor = vec4(compositeDeath(compositeNetherWorld(compositeGlow(fxaaResult, uv), uv)), 1.0);
}
