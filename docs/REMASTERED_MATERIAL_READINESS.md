# Particle material readiness review (effects 111)

Static evidence from the shipped Remastered binary and assets. Addresses are
`main.raw` offsets. This is a draw/output-path audit, not an observation of
Remastered framebuffer pixels. Visibility still depends on emission, lifetime,
SIZE, camera, depth and fog. Research outputs are retained locally in
`build/s-elem-20261025/`.

## Null material is not the same as no TEXR/MTIN

`LoadGeneratorDesc` **0x256880** reads a GUID at GPSM+5, then the primitive word
at +21. `CreateGPSM` **0x256b7c**, `_END` branch, forwards that header GUID and
the MTIN GUID to `InitRenderState` **0x288718** (virtual slot +0x30).

The static-state constructor **0x28bfc4** starts with `state+8 = 0`. Its branch
is (names substituted for decompiler arguments):

```c
if (MTIN == nil) {
    if (headerMTRL != MTIN) { NMaterialFormat::build_tag(headerMTRL, ...); /* load state+8 */ }
} else {
    NMaterialInstanceFormat::build_tag(MTIN, ...); /* load state+0x10 */
}
```

Scene-node construction **0x2afe40** initializes `node+0x140 = 0`, then in
the non-custom branch:

```c
if (*(long *)(state + 8) != 0)
    *(undefined8 *)(node + 0x140) = *(undefined8 *)(*(long *)(state + 8) + 0x10);
BuildParticleMaterialInstance(...);
```

`UpdateCustomMaterialReadyToRender` **0x2b0ad4** returns ready when there is no
MATI token. That return does **not** manufacture a material resource. A loaded
MATI instead populates `node+0x148` and `node+0x140`.

`UpdateRenderDataForContext` **0x2b0cac** clears `node+0xd0`, then dispatches:

```c
switch (*(uint *)(state + 4)) {
case 1: UpdateRenderData_Sprite(...); break;
case 2: UpdateRenderData_PlanarSquareModel(...); break;
/* model/swoosh/electric cases omitted */
}
```

It does not assign a default material. The decisive draw guard is in
`RenderSpriteEntries` **0x2b13a8**:

```c
if ((*(long *)(node + 0x140) != 0) &&
    (instance = *(long **)(node + 0x80), instance != 0) &&
    ((instance->virtual_0x10() & 1) != 0) &&
    (*(int *)(node + 0xd0) != 0)) {
    /* BindGraphicsState, material binding, indexed instanced draw */
}
```

Thus **nil header MTRL AND absent/nil MTIN skips the sprite draw, even with
TEXR**. Non-null header MTRL without TEXR/MTIN is a different case.
`BuildParticleMaterialInstance` **0x2b0268** skips `AddTextureToken` when
`state+0x30 == 0`, but still selects `NMaterialParticle::get_sprite_function_desc()`
and builds instance data. It uses the header's MTRL, not a white/black fallback
texture. The no-texture fragment has no sampler; it produces COLR multiplied
by the supplied gain/alpha factors and fog interpolation.

All six base MTRLs used by the 23 affected non-MATI nodes have identical folded
no-texture fragment `001_0` (SHA-256 prefix `01a37482c557e5fa`):
`09de6ad1`, `2fb61b47`, `7553241e`, `d29482c3`, `d9fa12e5`, `f446674d`.
For EmptyMuzzle this is MTRL `09de6ad1-900a-47ca-924b-e4b590b1871a`
(shader key `d16ade09`); alpha is `clamp(v1.y * v0.w, 0, 1)`.

## Nil BCLR: explicit black-array binding, independent blend/depth

MATI factory **0x1fdcb0** selects loader **0x1fdd20**. The loader reads a
texture parameter's GUID and usage, then explicitly branches:

```c
if (textureGuid == skInvalidGuidBase) {
    switch (parameterType) {
    case 5: texture = cache.GetBlackTexture(); break;
    case 6: texture = cache.GetBlackTextureArray(); break;
    case 7: texture = cache.GetBlackCubeMap(); break;
    case 8: texture = cache.GetBlackTextureVolume(); break;
    }
    if (texture != 0) builder.AddTextureRaw(parameterId, texture);
} else {
    /* build texture tag and AddTextureToken */
}
```

The relevant BCLR entries are type **6** and nil. Cache ctor **0x123c14**
constructs the array (cache+0x70, getter **0x12b014**) from RGBA8
`0xff000000` little-endian: **(0,0,0,255)**. Shader **642dd72b** samples it;
RGB is texture RGB × vertex RGB × gain, alpha is texture alpha × vertex alpha,
with discard for alpha <= 0. Nil BCLR therefore means black with the authored
alpha, not white, not transparency, and not disabled drawing.

`BindGraphicsState` **0x2b10e4** first takes the generator's PBDM row (normal
draw override = -1), then overrides it only when:

```c
if (node->materialInstance != 0 && node->materialInstance->blend != -1)
    blend = SBlendModeState(node->materialInstance->blend);
```

The field is resource+0x24, serialized MATI LE i32 **0x61**. Independently:
depth compare is LEQUAL if static flag 0x80, ALWAYS otherwise; write is
`(flags >> 6) & 1`. Those flags come from generator ZTES/ZBUF, not BCLR.

The five requested-area MATIs `ff09f1e8`, `e2b00b0d`, `f22ae333`,
`92fbe692`, `5afd5316` each contain **blend -1**, **type-6 nil BCLR**.
Their relevant roots/stub children omit PBDM/ZTES/ZBUF: defaults are
**PBDM 0, SrcAlpha/InvSrcAlpha, LEQUAL, depth writes off**. These defaults
are positive constructor evidence: SRender **0x28f4cc** zeros +0xa2/+0xa3
(description PBDM+0x533); SSystem **0x28cbd4** initializes flags to 0x4882
(ZTES bit1=1, ZBUF bit0=0). Blend table initializer **0x28a600** gives
PBDM 0 alpha blending, PBDM 2 SrcAlpha/One additive.

## Requested asset identities and outcomes

The request's names and IDs do not match the importer mapping. These are the
actual mappings; conclusions include both interpretations of B0F9DBE6.

| Retail ID / source | Remastered | Port after review |
|---|---|---|
| D3053354 / b2f2c408 PlasmaCharge | Root and root/0 are primitive 0: no own geometry despite alpha-blended nil-BCLR MATIs ff09f1e8/e2b00b0d. Other children provide the charge geometry. | PPRM 0 roots; children remain enabled. No black root quad. |
| 53861B29 / 95964af3 `07_Over_Stonehenge` | Primitive-0 root; child 810da7ed has nil-BCLR MATI 92fbe692, alpha blend, MAXP 0. No own or child sprite pixels. | Same primitive dispatch and zero capacity. |
| 8D7BBFB2 / d682fa42 PlasmaMuzzle | Primitive-0 root with f22ae333; same MAXP-0 child. No muzzle sprite pixels. | Same. |
| B0F9DBE6 / legacy 10000000-0000-f000-f000-0000b0f9dbe6 EmptyMuzzle | Primitive-1, header MTRL 09de6ad1, no TEXR/MTIN; COLR (1,1,1,0). Eligible untextured sprite, zero alpha, no depth write: no contribution. | Eligible untextured COLR sprite with alpha zero, no depth write. |
| B0F9DBE6 / f494900e PlasmaAuxMuzzle (mapped replacement) | Primitive-0 root with 5afd5316; primitive-1 child uses header MTRL 7553241e, COLR alpha zero. No sprite contribution. | PPRM-0 root and zero-alpha child. |

## What the 41 eligibility changes mean

Re-run: `remastered_effect_tool census <romfs> <outdir>`. The TSV now includes
the parsed base MTRL and whether MTIN contains a nonnil GUID. All 5,011 GPSMs
parse. The 41 counts remain unchanged: **23 base-MTRL nodes, 18 MATI nodes,
zero null-resource nodes**. All 18 MATIs have type-6 nil BCLR and blend -1.

The following partitions apply to their authored normal emission path, not
arbitrary caller-forced particles or guaranteed visible screen coverage:

| Count | Behavior |
|---:|---|
| 16 | Untextured COLR sprites, including variable colors/sizes and both alpha/additive blend. |
| 14 | MAXP=0: no sprite particles. |
| 2 | Constant COLR alpha=0 (EmptyMuzzle and PlasmaAuxMuzzle child): no color/depth contribution. |
| 2 | GRTE=0 (12c0d553 root and ea81f0d7 root): no rate-driven emission. |
| 5 | Nil-BCLR, PBDM=2, depth writes off: additive black contributes no color. |
| 2 | Nil-BCLR, PBDM=0, nonzero capacity/rate: **alpha-blended black**, not additive. |

The two alpha-black cases are **44744b2c-db29-4cfd-8502-cf6e8b52b2ff root**
and **74375058-4291-4846-aa81-cc603d206488 root**, both MATI
**e760697f-c212-4aae-ac15-eba4e0b706a7**, SIZE .1, MAXP 128, GRTE 1, PSLT 1.
They must remain black alpha sprites; a blanket placeholder suppression is wrong.
The five additive-black roots are **1f9ef401, 62b018ec, 78d8a8e9,
9c77478c, f27d01bc**. The full per-node GUID/property audit is retained in
`build/s-elem-20261025/material-41.txt` and `material-41-table.txt`.

Per-node index (GUID prefix is unique within this audit; PBDM 0=alpha,
2=additive; MATI blend is -1 in every black row):

| Source / node | Material output | PBDM | Authored constraint |
|---|---|---:|---|
| 00aadc3b root | COLR | 2 | |
| legacy 572c7dc6 root/3 | COLR | 2 | |
| legacy b071be30 root | COLR | 2 | |
| legacy b0f9dbe6 root | COLR | 0 | alpha 0 |
| 12c0d553 root | COLR | 0 | GRTE 0 |
| 1338e07e root | COLR | 0 | |
| 1338e07e root/0 | COLR | 0 | |
| 1338e07e root/1 | COLR | 0 | |
| 1b9a0e4a root | COLR | 2 | ZBUF 1 |
| 1dede3e7 root/0 | COLR | 2 | |
| 1dede3e7 root/1 | COLR | 2 | |
| 1dede3e7 root/5 | COLR | 2 | |
| 1dede3e7 root/6 | COLR | 0 | |
| 1f9ef401 root | black | 2 | |
| 2e30f081 root/0 | black | 0 | MAXP 0 |
| 335247ff root | COLR | 0 | MAXP 0 |
| 335247ff root/0 | COLR | 2 | MAXP 0 |
| 3d8d5854 root/4 | COLR | 0 | MAXP 0 |
| 3d8d5854 root/8 | black | 0 | MAXP 0 |
| 3d8d5854 root/11 | black | 0 | MAXP 0 |
| 3d8d5854 root/14 | black | 0 | MAXP 0 |
| 3d8d5854 root/17 | black | 0 | MAXP 0 |
| 3d8d5854 root/20 | black | 0 | MAXP 0 |
| 3ec1f3bd root | COLR | 0 | SIZE .001 |
| 44744b2c root | black | 0 | alpha-black sprite |
| 573ab754 root | COLR | 0 | pulsed emission |
| 62b018ec root | black | 2 | SIZE .001 |
| 69aa5867 root/0 | black | 0 | MAXP 0 |
| 74375058 root | black | 0 | alpha-black sprite |
| 772a20c1 root | COLR | 0 | |
| 78d8a8e9 root | black | 2 | |
| 955fde05 root | COLR | 2 | SIZE .0001–.00015 |
| 9592d63e root | COLR | 2 | ZBUF 1 |
| 95964af3 root/0 | black | 0 | MAXP 0 |
| 9c77478c root | black | 2 | |
| 9d5bc064 root/0 | black | 0 | MAXP 0 |
| c214f6c8 root | COLR | 2 | MAXP 0 |
| d682fa42 root/0 | black | 0 | MAXP 0 |
| ea81f0d7 root | black | 0 | GRTE 0, ZTES 0 |
| f27d01bc root | black | 2 | |
| f494900e root/0 | COLR | 0 | alpha 0 |

Depth is LEQUAL/write-off throughout this table except the three explicit
ZBUF/ZTES overrides above. Empty cells are not claims of constant visibility;
the raw audit records the actual COLR, SIZE and emission expressions.

## Port change and regression coverage

The parser retains GPSM+5 as `EffectNode::baseMaterial`. For primitives 1/2,
nil base MTRL plus absent/nil MTIN emits **PPRM 0**. TEXR presence does not
change that decision. SIZE remains converted/evaluated; children and lights
are not suppressed. Non-null materials, including nil-BCLR ones, keep the
existing black texture, authored blend, depth and primitive behavior.

Regression matrix covers primitives 1/2 × base-MTRL presence × TEXR presence
× absent/nil/nonnil MTIN, checking header retention, dispatch bytes, and
unchanged active SIZE. Import stage remains **111**.

## Reproduce a plasma shot (existing commands only)

From this worktree, with the current stage-111 Remastered import loaded:

```sh
python3 tools/mprig.py doctor
python3 tools/mprig.py start material-plasma --build port-elem --room "landing site"
python3 tools/mprig.py cmd material-plasma 'give PlasmaBeam 1' 'give ChargeBeam 1' 'beam plasma' 'wait 90' status
python3 tools/mprig.py shot material-plasma build/s-elem-20261025/plasma-idle.png
python3 tools/mprig.py shot material-plasma build/s-elem-20261025/plasma-shot.png 'press a 1' 'wait 1'
python3 tools/mprig.py shot material-plasma build/s-elem-20261025/plasma-charge.png 'press a 120' 'wait 90'
python3 tools/mprig.py cmd material-plasma 'press a 1' 'wait 30' status
python3 tools/mprig.py stop --all material-plasma
```

Check `doctor` for stale import and the run's `game.log` for the loaded mod.
Expect the charge's child geometry, but no opaque black root/muzzle card.
This sequence is supplied for the user; no rig run was performed in this review.
