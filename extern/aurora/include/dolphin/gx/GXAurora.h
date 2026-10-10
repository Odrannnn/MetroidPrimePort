#ifndef DOLPHIN_GXAURORA_H
#define DOLPHIN_GXAURORA_H

#include <dolphin/types.h>

#if __cplusplus
extern "C" {
#endif

//
// Subcommands for GX_AURORA.
//

/**
 * Sets the actual render viewport in native framebuffer coordinates.
 * Must be followed by six f32 values: left, top, width, height, nearz, farz.
 */
#define GX_AURORA_LOAD_VIEWPORT_RENDER 0x0001

/**
 * Sets the actual render scissor in native framebuffer coordinates.
 * Must be followed by four u32 values: left, top, width, height.
 */
#define GX_AURORA_LOAD_SCISSOR_RENDER 0x0002

/**
 * Loads a full 4x4 projection matrix, bypassing GXSetProjection's 6-parameter
 * hardware encoding. Must be followed by sixteen f32 values in row-major order.
 */
#define GX_AURORA_LOAD_PROJECTION_FULL 0x0003

/**
 * Aurora equivalent of CP_REG_ARRAYBASE_ID: sets the base address and size of a vertex array.
 * This command must be followed by a 64-bit memory address, 32-bit size, and 1-byte little-endian flag.
 * The index of the vertex array is given by the lowest 4 bits of the command ID,
 * e.g. writing GX_AURORA_LOAD_ARRAYBASE + 5 will set the vertex array for the sixth vertex attribute.
 * To set strides, use the normal CP_REG_ARRAYSTRIDE_ID register.
 */
#define GX_AURORA_LOAD_ARRAYBASE 0x0010

/**
 * Pushes a debug group to the backend graphics API. These may show in debugging tools such as RenderDoc.
 * Must be followed by a u16 string length and that many UTF-8 characters (no null terminator required).
 * It is considered an error to have unpopped debug groups at the end of the frame. They will be automatically cleared.
 */
#define GX_AURORA_DEBUG_GROUP_PUSH 0x0020

/**
 * Pops a previously pushed debug group.
 * Followed by nothing.
 */
#define GX_AURORA_DEBUG_GROUP_POP 0x0021

/**
 * Sends a debug marker to the backend graphics API.
 * Must be followed by a u16 string length and that many UTF-8 characters (no null terminator required).
 */
#define GX_AURORA_DEBUG_MARKER_INSERT 0x0022

/**
 * Records a draw-sync token. Must be followed by a u16 token. The token becomes
 * readable via GXReadDrawSync once the FIFO processor has passed this command,
 * mirroring hardware ordering so callers can fence data referenced by draws,
 * such as reused vertex workspaces.
 */
#define GX_AURORA_SET_DRAW_SYNC 0x0023

#define GX_AURORA_LOAD_TEXOBJ 0x0030

#define GX_AURORA_LOAD_TLUT 0x0031

#define GX_AURORA_DESTROY_TEXOBJ 0x0032

#define GX_AURORA_DESTROY_TLUT 0x0033

#define GX_AURORA_DESTROY_COPY_TEX 0x0034

#define GX_AURORA_LOAD_COPY_SRC 0x0035

#define GX_AURORA_LOAD_COPY_DST 0x0036

#define GX_AURORA_LOAD_COPY_DEST 0x0037

#define GX_AURORA_REQUEST_DEPTH_SNAPSHOT 0x0038

#define GX_AURORA_BEGIN_OFFSCREEN 0x0039

#define GX_AURORA_END_OFFSCREEN 0x003A

/**
 * Draw primitives with the vertex count derived from a byte length, as written by
 * GXBegin(prim, fmt, GX_AUTO). Must be followed by a u8 draw opcode (vtxfmt|prim),
 * a u32 vertex data byte length, then that many bytes of vertex data. The byte length
 * must be a whole multiple of the current vertex size or zero (no draw).
 */
#define GX_AURORA_DRAW_SIZED 0x0040

/**
 * Draw pre-merged triangles with a prebuilt index buffer, as written by the display
 * list optimizer (aurora::gx::dl::optimize). Must be followed by a u8 draw opcode
 * (vtxfmt | GX_TRIANGLES), a u16 vertex count, a u32 index count, that many u16
 * indices, then vertex count * vertex size bytes of packed vertex data. Index data
 * is always host-endian regardless of stream endianness.
 */
#define GX_AURORA_DRAW_INDEXED 0x0041

/**
 * Sets a base index added to every indexed fetch from one vertex array, so 16-bit
 * indices can address arrays of more than 65536 elements a window at a time.
 * Must be followed by a u8 attribute index (attr - GX_VA_POS) and a u32 base index.
 */
#define GX_AURORA_LOAD_ARRAY_BASE_INDEX 0x0042

/**
 * Port extension: switches the fragment shader's colour output to a PBR evaluation of
 * texture maps 0-3 (base colour, occlusion/roughness/metal, two-channel normal, emissive)
 * lit by the GX lights of colour channel 0. The TEV stages still run and supply alpha.
 * Must be followed by a u8 (0 = off). Stays in effect until changed.
 */
#define GX_AURORA_SET_PBR 0x0043

/**
 * Port extension: copies the EFB (GXSetTexCopySrc rectangle) into one face of the PBR
 * environment probe, a cube map the PBR path reflects, and clears the EFB like a
 * clearing GXCopyTex. Must be followed by a u8 face index (+X, -X, +Y, -Y, +Z, -Z).
 */
#define GX_AURORA_COPY_PROBE_FACE 0x0044

/**
 * Port extension: the view-to-probe rotation for PBR draws, as three vec4f columns (the
 * probe-space images of view X, Y and Z). The first column's w is the probe's weight:
 * 0 keeps the light-derived stand-in environment, 1 uses the probe, and 2 and 3
 * are diagnostics that draw every PBR surface as a perfect mirror of the probe, or as a
 * window onto it.
 */
#define GX_AURORA_SET_PBR_PROBE 0x0045

/**
 * Port extension: per-material constants for PBR draws, as six vec4f. The first is a
 * multiplier on the emissive map (rgb; 1 leaves it as sampled) and, in w, the threshold of
 * a height-blended alpha. The second is a backlight weight (rgb, linear): a rim of the
 * surface's base colour times that weight, scaled by the light reaching the surface, on the
 * edges facing away from the viewer; zero is off; its w is the shading mode. The third's x
 * is the edge width of a second layer's blend (0: none), its y the kind of a special
 * surface and z that kind's strength; the fourth is the scale and offset of each layer's
 * height, the fifth the kind's parameters and the sixth world up in view space (see
 * GXSetPBRMaterial). Stays in effect until changed.
 */
#define GX_AURORA_SET_PBR_MATERIAL 0x0046

/**
 * Port extension: room cubes, prefiltered HDR cube maps for the PBR path to reflect in
 * place of the probe. CREATE takes an id (not 0), the edge, the mip count and a pointer
 * to a heap block the command owns (see GXCreatePBRCube); DESTROY takes the id. SET
 * selects the cube of the following PBR draws and takes four floats (see GXSetPBRCube);
 * id 0 goes back to the probe.
 */
#define GX_AURORA_CREATE_PBR_CUBE 0x0047
#define GX_AURORA_DESTROY_PBR_CUBE 0x0048
#define GX_AURORA_SET_PBR_CUBE 0x0049

/**
 * Port extension: the baked ambient light of the following PBR draws, which the
 * luminance of the GX ambient colour then scales (a first w of 1) or which is used as it is
 * (2). Six vectors of four floats (see GXSetPBRAmbient); a first w of 0 goes back to the GX
 * ambient. Stays in effect until changed.
 */
#define GX_AURORA_SET_PBR_AMBIENT 0x004A

/**
 * Port extension: ambient volumes, a room's baked ambient light as 3D textures, which a
 * PBR draw samples at every pixel instead of taking one value for the whole model. CREATE
 * takes an id, the three sizes and a pointer to a heap block the command owns (see
 * GXCreatePBRVolume); DESTROY takes the id. SET selects the volume of the following PBR
 * draws and takes six vectors of four floats (see GXSetPBRVolume); id 0, or a fourth w of
 * 0, goes back to GX_AURORA_SET_PBR_AMBIENT.
 */
#define GX_AURORA_CREATE_PBR_VOLUME 0x004B
#define GX_AURORA_DESTROY_PBR_VOLUME 0x004C
#define GX_AURORA_SET_PBR_VOLUME 0x004D

/**
 * Port extension: baked lightmaps, a 2D-array texture (L0 and L1 x/y/z) which PBR draws read
 * through a vertex attribute's UV (see GXCreatePBRLightmap). CREATE takes the id, the width,
 * height, layer count, the format and a pointer to a heap block the command owns; DESTROY
 * takes the id. SET selects the lightmap of the following PBR draws: the id, four floats
 * (the rect) and nine (the axes, row by row); id 0 turns it off. Stays in effect until changed.
 */
#define GX_AURORA_CREATE_PBR_LIGHTMAP 0x0064
#define GX_AURORA_DESTROY_PBR_LIGHTMAP 0x0065
#define GX_AURORA_SET_PBR_LIGHTMAP 0x0066
// The vertex attribute holding the lightmap UV (see GXSetPBRLightmapAttr). Payload: u8 GXAttr.
#define GX_AURORA_SET_PBR_LIGHTMAP_ATTR 0x0067

/**
 * Port extension: the tone curve of the following PBR draws, as three vec4f (see
 * GXSetPBRTone). A slope of 0 (x of the second) goes back to the built-in highlight
 * roll-off. Stays in effect until changed.
 */
#define GX_AURORA_SET_PBR_TONE 0x004E

/**
 * Port extension: a table for the PBR environment specular's scale and bias in place of
 * the analytic fit (see GXSetPBRBrdfLut). Payload: u64 a heap std::vector<u8> the command
 * owns, empty to go back to the fit. Stays in effect until changed.
 */
#define GX_AURORA_SET_PBR_BRDF_LUT 0x0054

// Distance-field texturing for the following draws: every texture sample is read as
// a signed distance (red, edge at 0.5) and becomes coverage, one screen pixel wide:
// rgb = inside the shape, a = inside the shape grown out to `edge`.
// Payload:
//   u8 edge (distance of the outer edge x 255; 128 = the shape itself, 0 = off)
#define GX_AURORA_SET_SDF 0x004F

// Port: how the following draws sample a converted Remastered HUD picture (GXSetHudSample).
// Payload:
//   u8  mode (0 none; 1 the sampled alpha is squared, as Remastered's UI shaders do;
//             2 + n the UI_Interference shader with a picture of kHudInterferenceRows[n] rows)
//   f32 x, y (the shader's DYIN: x = 1 - the HUD's fade-in alpha, y = a random 0..1 per frame)
#define GX_AURORA_SET_HUD_SAMPLE 0x0068

// Names the following draws for diagnostics (see GXSetDrawTag); changes no state.
// Payload:
//   u32 asset id, u32 model index, u32 material
#define GX_AURORA_SET_DRAW_TAG 0x0050

// Lights the following PBR draws leave out (see GXSetPBRLightSkip). Stays in effect until
// changed.
// Payload:
//   u32 mask (bit n: GX_LIGHTn)
#define GX_AURORA_SET_PBR_LIGHT_SKIP 0x0051

// Scales the diffuse colour and the F0 of the following PBR draws (see GXSetPBRLightScale);
// both 1 is neutral, and their alpha's fade (1, 0 is neutral). Stays in effect until changed.
// Payload:
//   f32 diffuse, f32 f0, f32 alpha, u32 alphaReplaces
#define GX_AURORA_SET_PBR_LIGHT_SCALE 0x0053

// Port extension: Remastered's bloom, colour grade and frame average over the EFB as drawn so
// far (see GXPortPostProcess). Queued, so the game thread does not wait for the FIFO to be
// processed before it can record it.
// Payload:
//   32 u32: aurora::gfx::bloom::Params, word for word
#define GX_AURORA_PORT_POST_PROCESS 0x0052

// Replaces one light's colour, position and attenuation in the following PBR draws with a
// Remastered HDR light's (see GXSetPBRLightHdr); other draws keep the GX light. Stays in effect
// until changed.
// Payload:
//   u32 light (its GX_LIGHTn bit), f32 r, g, b (linear), f32 view-space x, y, z,
//   f32 inner radius, f32 outer radius (0 = off), u32 falloff (0 none, 1 linear, 2 quadratic,
//   3 1 - smoothstep)
#define GX_AURORA_SET_PBR_LIGHT_HDR 0x0056

// Multiplies the baked light of the following PBR draws (see GXSetPBRBakedLightModulation).
// Stays in effect until changed; 1, 1, 1 is neutral.
// Payload:
//   f32 r, g, b (linear)
#define GX_AURORA_SET_PBR_BAKED_LIGHT_MODULATION 0x0057

// Remastered's character backlight of the following PBR draws (see GXSetPBRBacklight).
// Stays in effect until changed.
// Payload:
//   f32 height plane x, y, z, w (view space), f32 back colour direction x, y, z (view space),
//   f32 back strength, f32 top strength
#define GX_AURORA_SET_PBR_BACKLIGHT 0x0058

// The constants of a PBR kind 14 material (Remastered's BoundaryShield; see GXSetPBRShield).
// Stays in effect until changed.
// Payload:
//   8 x (f32 x, y, z, w)
#define GX_AURORA_SET_PBR_SHIELD 0x005D

// Port extension: Remastered's volumetric fog over the EFB as drawn so far (see
// GXPortVolumetricFog). Queued like GX_AURORA_PORT_POST_PROCESS.
// Payload:
//   132 u32: aurora::gfx::volfog::Params, word for word
#define GX_AURORA_PORT_VOLUMETRIC_FOG 0x0059

// Port extension: the draws after GX_AURORA_PORT_VOLUMETRIC_FOG fog themselves through its
// froxels until this (see GXPortVolumetricFogEnd).
// Payload: none
#define GX_AURORA_PORT_VOLUMETRIC_FOG_END 0x005A

// Port extension: draws the following opaque surfaces in two passes (see GXPortSetDepthPrepass).
// Payload:
//   u8 pass (0 = off, 1 = depth only, 2 = shade where the depth is equal)
#define GX_AURORA_PORT_DEPTH_PREPASS 0x0055

// Port extension: the serial of the following draws (see GXPortSetDrawSerial). Stays in effect until
// changed; 0 is none. Shown as a colour by the "drawid" mode.
// Payload:
//   u32 serial (24 bits)
#define GX_AURORA_PORT_DRAW_SERIAL 0x005B

// Port extension: the "drawid" debug view (see GXPortSetDrawIdMode): every draw is its serial as a flat
// colour, with no blend, fog or post-processing.
// Payload:
//   u8 on
#define GX_AURORA_PORT_DRAW_ID_MODE 0x005C

// Port extension: data kept on the GPU across frames (GXPortRetainResident).
// RETAIN payload: u64 the game's pointer, u64 a heap std::vector<u8> copy the processor takes.
// RELEASE payload: u64 the pointer.
// CALL_DL payload: u64 the pointer of a retained display list, u32 its size; drawn as
// GXCallDisplayList would draw it.
#define GX_AURORA_RESIDENT_RETAIN 0x0060
#define GX_AURORA_RESIDENT_RELEASE 0x0061
#define GX_AURORA_RESIDENT_CALL_DL 0x0062

// Port extension: the sun's shadow (see GXPortSetShadowCaster, GXPortSetShadowFrame and
// GXPortRenderShadowMap).
// CASTER payload: u8 on. Stays in effect until changed.
// FRAME payload: 19 f32: world -> view (3 rows of 4), the sun's direction (world, the way its light
// travels), the radius around the camera the map covers (<= 0: no sun), its colour (rgb).
// RENDER payload: none.
#define GX_AURORA_PORT_SHADOW_CASTER 0x005E
#define GX_AURORA_PORT_SHADOW_FRAME 0x005F
#define GX_AURORA_PORT_SHADOW_RENDER 0x0063
// Port extension: the room's Remastered point and spot lights (GXPortSetRoomLights).
// Payload: u32 count, then count records of 16 f32 (see GXPortSetRoomLights). Lasts until the
// end of the frame.
#define GX_AURORA_PORT_ROOM_LIGHTS 0x0069
// Port extension: the draws that follow are particles (GXPortSetParticleFog). Payload: u8 on.
#define GX_AURORA_PORT_PARTICLE_FOG 0x006A
// Port extension: Remastered's X-ray visor post passes (GXPortXRayPass). Payload: 48 u32 words.
#define GX_AURORA_PORT_XRAY 0x006B
// Port extension: one pass of Remastered's thermal visor post (GXPortThermalPass). Payload: 20 u32 words.
#define GX_AURORA_PORT_THERMAL 0x006C
// Port extension: the world draws that follow go to the RGBA16Float scene target (GXPortSceneHdr) until
// the post pass (GXPortPostProcess) resolves it to the EFB. Payload: u8 on.
#define GX_AURORA_PORT_SCENE_HDR 0x006D

#define GX2_SET_POLYGON_OFFSET 0x1000


/*
 * Debug marker stuff
 */

/**
 * Pushes a debug group to the backend graphics API. These may show in debugging tools such as RenderDoc.
 * It is considered an error to have unpopped debug groups at the end of the frame. They will be automatically cleared.
 */
void GXPushDebugGroup(const char* label);

/**
 * Pop a debug group previously pushed via GXPushDebugGroup().
 */
void GXPopDebugGroup();

/**
 * Sends a debug marker to the backend graphics API. These may show in debugging tools such as RenderDoc.
 */
void GXInsertDebugMarker(const char* label);

typedef enum _AuroraViewportPolicy {
  AURORA_VIEWPORT_FIT = 0,     // Preserve logical aspect in the content framebuffer
  AURORA_VIEWPORT_STRETCH = 1, // Match content framebuffer aspect to the native surface
  AURORA_VIEWPORT_NATIVE = 2,  // Use active framebuffer pixels directly
} AuroraViewportPolicy;

/**
 * Configures content framebuffer sizing and how GXSetViewport/GXSetScissor parameters are applied to rendering.
 * When AURORA_VIEWPORT_NATIVE is used, GXSetTexCopySrc/GXSetTexCopyDst will use native framebuffer resolution.
 */
void AuroraSetViewportPolicy(AuroraViewportPolicy policy);

/**
 * Retrieves the current content framebuffer size.
 */
void AuroraGetRenderSize(u32* width, u32* height);

/**
 * Flush pending GX state and wait for FIFO processing without signaling a draw-done callback.
 */
void AuroraGXSync(void);

/**
 * Sets the actual render viewport in native framebuffer coordinates.
 * Overrides the automatically scaled values set by the logical GXSetViewport.
 */
void GXSetViewportRender(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz);

/**
 * Sets the actual render scissor in native framebuffer coordinates.
 * Overrides the automatically scaled values set by the logical GXSetScissor.
 */
void GXSetScissorRender(u32 left, u32 top, u32 wd, u32 ht);

void GX2SetPolygonOffset(f32 mFrontOffset, f32 mFrontScale, f32 mBackOffset, f32 mBackScale, f32 mClamp);

/**
 * Load an arbitrary 4x4 projection matrix, avoiding the 6-parameter hardware encoding.
 */
void GXSetProjectionFull(const void* mtx);

/**
 * Create an offscreen framebuffer and switch rendering to it.
 * All subsequent GX rendering will target this framebuffer until GXRestoreFrameBuffer() is called.
 * Use GXCopyTex to resolve the offscreen content into a texture.
 */
void GXCreateFrameBuffer(u32 width, u32 height);

/**
 * Restore rendering to the main EFB framebuffer.
 * Must be called after GXCreateFrameBuffer() to resume normal rendering.
 */
void GXRestoreFrameBuffer(void);

#if __cplusplus
}
#endif

#endif
