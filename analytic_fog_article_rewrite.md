# Analytic Fog Rendering With Volumetric Primitives (Detailed Rewrite)

## 1. Motivation

Most real-time fog implementations use one of these two models:

1. Global exponential distance fog.
2. Ray-marched heterogeneous media.

Distance fog is cheap but uniform and scene-wide.
Ray marching is expressive but can alias, is sample-count dependent, and can be expensive.

The analytic approach sits in the middle:

- Restrict fog to bounded primitives (spheres/boxes/etc.).
- Use closed-form antiderivatives for chosen density kernels.
- Evaluate exact optical depth over a finite ray segment.

This gives stable, non-aliased fog for specific families of density functions.

---

## 2. Optical Depth and Transmittance

Let:

- $L_i$: incident radiance.
- $L_o$: outgoing radiance after absorption.
- $T$: transmittance.
- $\tau$: optical depth.

Definitions:

$$
T = \frac{L_o}{L_i}, \qquad T = e^{-\tau}
$$

If density is constant $\rho$ across segment length $\ell$:

$$
\tau = \rho\ell
$$

If density varies spatially, we integrate along the segment inside the medium:

$$
\tau = \int_{t_a}^{t_b} f(p(t))\,dt
$$

where $p(t)$ is the ray position and $f$ is density at that point.

---

## 3. Ray Setup for Radial Densities

A view ray is:

$$
p(t) = r_o + t r_d
$$

For radial kernels centered at the origin, density depends on $\|p\|$ or $\|p\|^2$.

Key quadratic form:

$$
\|r_o + tr_d\|^2 = at^2 + 2bt + c
$$

with:

$$
a = r_d\cdot r_d,\qquad b = r_d\cdot r_o,\qquad c = r_o\cdot r_o
$$

So every radial kernel reduces to a 1D polynomial or polynomial+sqrt in $t$.

---

## 4. Practical Transform Trick

To place primitives anywhere with scale/rotation, transform ray into primitive-local space:

$$
r'_o + tr'_d = M(r_o + tr_d) = Mr_o + tMr_d
$$

Then evaluate kernel in canonical local coordinates.

This keeps formulas reusable and only changes local ray coefficients $(a,b,c)$.

---

## 5. Interval Evaluation

If $F(t)$ is antiderivative of cross-section density $\bar f(t)$:

$$
\tau = F(t_b) - F(t_a)
$$

Numerical stability tip used in practice:

- Shift interval so lower bound is zero when possible.
- Keep logarithm arguments positive with epsilon in linear/spiky formulas.

---

## 6. Radial Kernels and Closed Forms

### 6.1 Linear Kernel

Kernel:

$$
f(p) = 1 - \|p\|
$$

Cross-section:

$$
\bar f(t) = 1 - \sqrt{at^2 + 2bt + c}
$$

Useful substitutions:

$$
u = t + \frac{b}{a},\qquad v = \frac{\sqrt{b^2-ac}}{a}
$$

Antiderivative form:

$$
F(t) = t - \frac{\sqrt{a}}{2}\left(u\sqrt{u^2-v^2} - v^2\ln\left|u+\sqrt{u^2-v^2}\right|\right)
$$

---

### 6.2 Quadratic Kernel

Kernel:

$$
f(p) = 1 - \|p\|^2
$$

Cross-section:

$$
\bar f(t) = 1-(at^2+2bt+c) = -at^2-2bt-c+1
$$

Antiderivative:

$$
F(t)= -\frac{a}{3}t^3 - bt^2 - (c-1)t
$$

(Equivalent polynomial forms are fine if algebraically identical.)

---

### 6.3 Quartic Kernel

Kernel:

$$
f(p) = (1-\|p\|^2)^2
$$

Cross-section expansion:

$$
\bar f(t)=a^2t^4 + 4abt^3 + 2(2b^2+ac-a)t^2 + 4(bc-b)t + c^2-2c+1
$$

Antiderivative:

$$
F(t)=\frac{a^2}{5}t^5 + abt^4 + \frac{4b^2+2ac-2a}{3}t^3 + 2(bc-b)t^2 + (c^2-2c+1)t
$$

This kernel is very practical because:

- It is compactly supported inside unit sphere.
- It is smooth at boundaries.
- It is fully polynomial after substitution.

---

### 6.4 Spiky Kernel

Kernel:

$$
f(p)=(1-\|p\|)^2
$$

Cross-section:

$$
\bar f(t)=(1-\sqrt{at^2+2bt+c})^2
$$

Antiderivative decomposes into polynomial terms plus the same sqrt/log structure used by linear.

---

### 6.5 Gaussian Kernel

Kernel:

$$
f(p)=e^{-\|p\|^2}
$$

Cross-section:

$$
\bar f(t)=e^{-at^2-2bt-c}
$$

Closed form uses error function:

$$
\operatorname{erf}(z)=\frac{2}{\sqrt\pi}\int_0^z e^{-x^2}dx
$$

Antiderivative:

$$
F(t)=\frac{\sqrt\pi}{2\sqrt a}e^{\frac{b^2}{a}-c}\operatorname{erf}\left(\frac{at+b}{\sqrt a}\right)
$$

Gaussian is not compactly supported, so practical rendering usually truncates with a finite primitive radius.

---

## 7. Shading and Composition

Pure absorption model:

$$
C_{out}=C_{scene}T
$$

Common artistic extension (used in many engines):

$$
C_{out}=C_{scene}T + C_{fog}(1-T)
$$

with $T=e^{-\tau}$ and optional primitive-level lighting hacks.

---

## 8. Engine Integration Notes (This Repository)

The renderer implementation in this workspace now includes an analytic fog compute pass:

1. Source: HDR color + depth.
2. Output: intermediate HDR target (fogged HDR).
3. Consumer: postprocess pass reads fogged HDR.

Pipeline order in frame:

1. Main scene (HDR + bloom source + depth).
2. Toon outline.
3. Analytic fog compute pass.
4. DoF prepare + postprocess + SMAA + swapchain blit.

Implementation specifics:

- Descriptor model remains bindless-first via shared bindings in common_bindless.
- Camera data comes from GlobalData UBO, not push constants.
- Fog pass push constants are 16-byte aligned.
- Fog uses quartic radial kernel over multiple overlapping spheres to approximate heterogeneous patches.

---

## 9. Numerical Robustness Checklist

1. Clamp integration interval to visible segment: $[\max(0,t_0),\min(t_{scene},t_1)]$.
2. Return zero if interval is empty.
3. Clamp final $\tau$ to $\ge 0$.
4. Use epsilon guards for divisions/log terms.
5. Avoid reconstructing world position for invalid depth values.

---

## 10. Minimal Pseudocode

```cpp
for each pixel:
  sceneColor = HDR[pixel]
  depth = Depth[pixel]
  if fog disabled or depth invalid:
    out = sceneColor
    continue

  worldPos = reconstructWorld(uv, depth, invViewProj)
  rayOrigin = cameraPos
  rayDir = normalize(worldPos - cameraPos)
  tScene = length(worldPos - cameraPos)

  tau = 0
  for each fog sphere primitive:
    if ray intersects sphere over [tEnter, tExit]:
      tau += densityScale * (F_quartic(tExit) - F_quartic(tEnter))

  T = exp(-max(tau, 0))
  out = sceneColor * T + fogColor * (1 - T)
```

---

## 11. Why This Works Well in Real-Time

- No step-count artifacts from coarse marching.
- Fewer texture reads than volumetric 3D media for this class of effects.
- Predictable cost per primitive and pixel.
- Easy art direction by changing primitive placement/radii/kernel weights.

Tradeoff:

- Only densities with tractable antiderivatives are analytic.
- Complex light transport (multiple scattering, shadowed in-scattering) still requires approximations.

---

## 12. Suggested Next Extensions

1. Primitive lists in SSBO instead of hardcoded few primitives.
2. Optional boolean operations (union/subtract/intersection) between analytic volumes.
3. Primitive-local anisotropic lighting term.
4. Temporal blue-noise dither for subtle band reduction.
5. Hybrid mode: analytic near fog + coarse ray-march far participating media.
