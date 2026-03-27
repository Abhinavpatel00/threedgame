/*
 * Easing Cubic Bézier – single‑header C99 library
 * ------------------------------------------------
 * This header condenses the original C++ implementation found in
 * /EasingCubicBezier into a C99‑friendly, drop‑in library.  It takes a 2‑D
 * cubic Bézier curve (x(t), y(t)), analytically inverts x(t) and evaluates
 * y(t) at that parameter, giving a fast easing function that maps an input
 * x in [x0, x3] to an eased output y.
 *
 *  How it works (high level)
 *  -------------------------
 *  1. The user supplies Bézier control points P0 = (x0, y0) … P3 = (x3, y3)
 *     that describe the desired easing shape.  The x‑coordinates must be
 *     monotonic for the mapping x→y to be single‑valued (the usual CSS
 *     cubic‑bezier constraint).
 *  2. We rewrite x(t) and y(t) as cubic polynomials in t on [0,1].
 *     x(t) = ax t^3 + bx t^2 + cx t + dx  (same for y).
 *  3. Given an input x*, we need t such that x(t) = x*.  This means solving
 *     ax t^3 + bx t^2 + cx t + (dx – x*) = 0.  The code transforms this
 *     into the depressed cubic u^3 + p u + q = 0 and picks one of several
 *     closed‑form solutions depending on p, q and the discriminant:
 *       - Linear / quadratic fallbacks when the leading terms vanish
 *       - Pure cubic with p = 0 (single cbrt)
 *       - Three real roots (trigonometric form, cos)
 *       - One real root (hyperbolic form, sinh / cosh)
 *  4. All factors that do not depend on x* are pre‑computed and stored in an
 *     `ecb_curve` structure.  Runtime evaluation boils down to a handful of
 *     math calls and Horner evaluation of y(t).
 *
 *  Usage (minimal)
 *  ---------------
 *      #define ECB_IMPLEMENTATION
 *      #include "easing_cubic_bezier.h"
 *
 *      double px[4] = {0.0, 0.42, 0.58, 1.0};  // CSS "ease"
 *      double py[4] = {0.0, 0.0, 1.0, 1.0};
 *      ecb_curve curve;
 *      ecb_make_from_bezier(&curve, px, py, 0.0); // 0.0 → use default ε
 *
 *      double y = ecb_evaluate(&curve, 0.25); // x = 0.25 → eased y
 *
 *  Use cases
 *  ---------
 *  - UI & motion design (CSS cubic-bezier, SVG, game tweens)
 *  - Parametric time‑remapping in audio/video tools
 *  - Normalized curves for physics/servo inputs where a monotone x→y map
 *    is required.
 *
 *  Design goals
 *  ------------
 *  - Header‑only, pure C99 (no dynamic allocation, no global state).
 *  - Works with both `double` and `float` helpers; uses only libc math.
 *  - Optional fast approximations for acos/ cbrt via ECB_FAST_MATH.
 *
 *  License: MIT (matches the original project).
 */

/* MIT License
 *
 * Copyright (c) 2025 Lukasz Izdebski
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef EASING_CUBIC_BEZIER_H
#define EASING_CUBIC_BEZIER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <math.h>
#include <float.h>
#include <stdbool.h>
#include <stddef.h>

/* -------------------------------------------------------------
 * Public API types
 * -----------------------------------------------------------*/

typedef enum ecb_type
{
    ECB_TYPE_NONE   = 0,
    ECB_TYPE_P3     = 1, /* Degenerate: x(t) is linear (only c, d ≠ 0)        */
    ECB_TYPE_X2     = 2, /* Degenerate: x(t) is quadratic                    */
    ECB_TYPE_X3P0   = 3, /* Cubic with p = 0 (single cbrt solution)          */
    ECB_TYPE_X3COS  = 4, /* Cubic, three real roots (trigonometric form)     */
    ECB_TYPE_X3SINH = 5, /* Cubic, one real root, p > 0  (hyperbolic sinh)   */
    ECB_TYPE_X3COSH = 6  /* Cubic, one real root, p < 0  (hyperbolic cosh)   */
} ecb_type;

typedef struct ecb_curve
{
    /* Pre‑factored coefficients for y(t) reconstruction. */
    ecb_type type; /* which analytic branch to use */
    double   A, B, C, D; /* Horner coefficients for y after substitution */
    double   L, K;       /* affine transform applied to input x: φ = L*x + K */
} ecb_curve;

typedef struct ecb_curvef
{
    ecb_type type;
    float    A, B, C, D;
    float    L, K;
} ecb_curvef;

/* Default numeric tolerance: 10× machine ε (matches original library). */
#define ECB_DEFAULT_EPSILON   (DBL_EPSILON * 10.0)
#define ECB_DEFAULT_EPSILON_F (FLT_EPSILON * 10.0f)

/* -------------------------------------------------------------
 * Public API – creation
 * -----------------------------------------------------------*/

/* Build curve from Bézier control points (double precision). */
void ecb_make_from_bezier(ecb_curve* out,
                          const double px[4], const double py[4],
                          double epsilon /* 0 → use default */);

/* Convenience: explicit coordinates. */
void ecb_make_from_points(ecb_curve* out,
                          double x0, double y0, double x1, double y1,
                          double x2, double y2, double x3, double y3,
                          double epsilon /* 0 → use default */);

/* Build curve directly from cubic polynomials ax t^3 + bx t^2 + cx t + dx
 * and ay t^3 + by t^2 + cy t + dy.  Coefficients are in index order
 * poly[0]=a ... poly[3]=d. */
void ecb_make_from_polynomial(ecb_curve* out,
                              const double poly_x[4], const double poly_y[4],
                              double epsilon /* 0 → use default */);

/* Float variants mirror the double API but store results in ecb_curvef. */
void ecb_make_from_bezier_f(ecb_curvef* out,
                            const float px[4], const float py[4],
                            float epsilon /* 0 → use default */);

void ecb_make_from_points_f(ecb_curvef* out,
                            float x0, float y0, float x1, float y1,
                            float x2, float y2, float x3, float y3,
                            float epsilon /* 0 → use default */);

void ecb_make_from_polynomial_f(ecb_curvef* out,
                                const float poly_x[4], const float poly_y[4],
                                float epsilon /* 0 → use default */);

/* -------------------------------------------------------------
 * Public API – evaluation
 * -----------------------------------------------------------*/

/* Evaluate eased output y for an input x.  Inputs outside [x0, x3] are
 * extrapolated linearly by the underlying cubic solution. */
double ecb_evaluate(const ecb_curve* curve, double x);

float  ecb_evaluate_f(const ecb_curvef* curve, float x);

/* Inspect the affine inner transform φ(x) = L*x + K used in the cubic
 * solution.  Mostly useful for debugging or visualization. */
static inline double ecb_inner(const ecb_curve* c, double x)
{
    return c->L * x + c->K;
}

static inline float ecb_inner_f(const ecb_curvef* c, float x)
{
    return c->L * x + c->K;
}

/* -------------------------------------------------------------
 * Implementation (header‑only)
 * -----------------------------------------------------------*/

#ifdef ECB_IMPLEMENTATION

/* ---------- Utility helpers (shared) ---------- */

static inline double ecb_clamp_d(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float ecb_clamp_f(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline double ecb_sign_d(double v)
{
    return (v > 0.0) - (v < 0.0);
}

static inline float ecb_sign_f(float v)
{
    return (float)((v > 0.0f) - (v < 0.0f));
}

#ifndef ECB_FMA
#define ECB_FMA(a,b,c) ((a)*(b) + (c))
#endif

#ifdef ECB_FAST_MATH
/* Slightly faster cbrt/acos approximations (mirrors original fast paths). */
static inline double ecb_fast_cbrt_d(double x)
{ return x == 0.0 ? 0.0 : copysign(1.0, x) * exp(log(fabs(x)) / 3.0); }
static inline float ecb_fast_cbrt_f(float x)
{ return x == 0.0f ? 0.0f : copysignf(1.0f, x) * expf(logf(fabsf(x)) / 3.0f); }

static inline double ecb_fast_asinh_d(double x)
{ return log(x + sqrt(x*x + 1.0)); }
static inline float ecb_fast_asinh_f(float x)
{ return logf(x + sqrtf(x*x + 1.0f)); }

static inline double ecb_fast_acosh_d(double x)
{ return log(x + sqrt(x*x - 1.0)); }
static inline float ecb_fast_acosh_f(float x)
{ return logf(x + sqrtf(x*x - 1.0f)); }

/* Max error ~4.5e-5 for double; matches source library. */
static inline double ecb_fast_acos_d(double x)
{
    const double A = -0.021641405;
    const double B = +0.077981383;
    const double C = -0.213301322;
    const double half_pi = 1.57079632679489661923; /* pi/2 */
    const double s = copysign(1.0, x);
    const double ax = x * s;
    const double arg = sqrt(fmax(0.0, 1.0 - ax));
    return ((((A*ax + B)*ax + C)*ax + half_pi)*arg - half_pi)*s + half_pi;
}

static inline float ecb_fast_acos_f(float x)
{
    const float A = -0.021641405f;
    const float B = +0.077981383f;
    const float C = -0.213301322f;
    const float half_pi = 1.57079632679f;
    const float s = copysignf(1.0f, x);
    const float ax = x * s;
    const float arg = sqrtf(fmaxf(0.0f, 1.0f - ax));
    return ((((A*ax + B)*ax + C)*ax + half_pi)*arg - half_pi)*s + half_pi;
}
#endif /* ECB_FAST_MATH */

/* ---------- Core math: double precision ---------- */

#ifdef ECB_FAST_MATH
#define ECB_CBRT_D(x)   ecb_fast_cbrt_d(x)
#define ECB_ACOS_D(x)   ecb_fast_acos_d(x)
#define ECB_ASINH_D(x)  ecb_fast_asinh_d(x)
#define ECB_ACOSH_D(x)  ecb_fast_acosh_d(x)
#else
#define ECB_CBRT_D(x)   cbrt(x)
#define ECB_ACOS_D(x)   acos(x)
#define ECB_ASINH_D(x)  asinh(x)
#define ECB_ACOSH_D(x)  acosh(x)
#endif

static inline void ecb_calculate_polynomial_d(const double p[4], double x0, double x3, double out[4])
{
    /* Expand Bernstein basis into power basis and normalize over [x0, x3]. */
    const double m0[4] = { -1.0, 3.0, -3.0, 1.0 };
    const double m1[4] = { 3.0 * x3, -3.0 * (2.0 * x3 + x0), 3.0 * (2.0 * x0 + x3), -3.0 * x0 };
    const double m2[4] = { -3.0 * x3 * x3, 3.0 * x3 * (2.0 * x0 + x3), -3.0 * x0 * (2.0 * x3 + x0), 3.0 * x0 * x0 };
    const double m3[4] = { x3 * x3 * x3, -3.0 * x0 * x3 * x3, 3.0 * x0 * x0 * x3, -x0 * x0 * x0 };
    const double scale = 1.0 / ((x3 - x0) * (x3 - x0) * (x3 - x0));
    out[0] = (m0[0]*p[0] + m0[1]*p[1] + m0[2]*p[2] + m0[3]*p[3]) * scale;
    out[1] = (m1[0]*p[0] + m1[1]*p[1] + m1[2]*p[2] + m1[3]*p[3]) * scale;
    out[2] = (m2[0]*p[0] + m2[1]*p[1] + m2[2]*p[2] + m2[3]*p[3]) * scale;
    out[3] = (m3[0]*p[0] + m3[1]*p[1] + m3[2]*p[2] + m3[3]*p[3]) * scale;
}

static inline ecb_type ecb_calculate_type_d(const double poly_x[4], double eps)
{
    /* Cardano preparation: x(t) = a t^3 + b t^2 + c t + d.
       After the Tschirnhaus substitution t = u - b/(3a), the depressed form is
       u^3 + p u + q = 0.  The sign/magnitude of p (and discriminant Δ) tells us
       which closed-form root to use. */
    const double b_a = poly_x[1] / poly_x[0];
    const double c_a = poly_x[2] / poly_x[0];
    const double d_a = poly_x[3] / poly_x[0];
    const double p = c_a - b_a * b_a / 3.0;
    const double b_over_3a = poly_x[1] / (3.0 * poly_x[0]);
    const double q = (2.0 * b_over_3a * b_over_3a - c_a) * b_over_3a + d_a;

    if (fabs(poly_x[0]) < eps && fabs(poly_x[1]) < eps) return ECB_TYPE_P3;
    if (fabs(poly_x[0]) < eps) return ECB_TYPE_X2;
    if (fabs(p) < eps)        return ECB_TYPE_X3P0;
    if (poly_x[0] < 0.0)      return ECB_TYPE_X3COS;
    if (p > 0.0)              return ECB_TYPE_X3SINH;
    if (p < 0.0)              return ECB_TYPE_X3COSH;
    return ECB_TYPE_NONE;
}

static inline void ecb_make_from_polynomial_d_core(ecb_curve* out, const double poly_x[4], const double poly_y[4], double eps)
{
    if (eps == 0.0) eps = ECB_DEFAULT_EPSILON;

    /* Recompute p, q because they feed both the type classifier and the
       pre-factorization constants below. */
    const double b_a = poly_x[1] / poly_x[0];
    const double c_a = poly_x[2] / poly_x[0];
    const double d_a = poly_x[3] / poly_x[0];
    const double p = c_a - b_a * b_a / 3.0;
    const double b_over_3a = poly_x[1] / (3.0 * poly_x[0]);
    const double q = (2.0 * b_over_3a * b_over_3a - c_a) * b_over_3a + d_a;
    const double sign_p = ecb_sign_d(p);
    const double sign_b = ecb_sign_d(poly_x[1]);

    out->type = ecb_calculate_type_d(poly_x, eps);
    double A = 1.0, B = 1.0, extra = 1.0;

    switch (out->type)
    {
    case ECB_TYPE_P3:
        /* Linear case: x(t) = c t + d → t = (x - d)/c.
           A, B scale/shift the y(t) polynomial accordingly. */
        A = 1.0 / poly_x[2];
        B = -(poly_x[3] / poly_x[2]);
        out->L = 1.0;
        out->K = 0.0;
        break;
    case ECB_TYPE_X2:
        /* Quadratic case: x(t) = b t^2 + c t + d.
           We solve t = sign(b)*sqrt((x - d)/b + (c/2b)^2) - c/2b. */
        A = sign_b;
        B = -(poly_x[2] / (2.0 * poly_x[1]));
        out->L = 1.0 / poly_x[1];
        out->K = B*B - poly_x[3] / poly_x[1];
        break;
    case ECB_TYPE_X3P0:
        /* p = 0 → u^3 + q = 0 ⇒ u = cbrt(-q). */
        B = -b_over_3a;
        out->L = 1.0 / poly_x[0];
        out->K = -q;
        break;
    case ECB_TYPE_X3COSH:
        extra = sign_b; /* flips branch to cosh when sign differs */
        /* fallthrough */
    case ECB_TYPE_X3COS:
        /* fallthrough */
    case ECB_TYPE_X3SINH:
        /* General cubic: u = 2*sqrt(|p|/3) * solution, where the solution is
           cos(·), sinh(·), or cosh(·) depending on the sign of p and Δ. */
        A = -2.0 * sign_p * extra * sqrt(fabs(p) / 3.0);
        B = -b_over_3a;
        out->L = 3.0 * sign_p / (p * poly_x[0] * A);
        out->K = 3.0 * sign_p * (-q / p / A);
        break;
    default:
        out->type = ECB_TYPE_NONE;
        out->A = out->B = out->C = out->D = out->L = out->K = 0.0;
        return;
    }

    /* Precompute coefficients for y(t) after substituting the analytic root. */
    out->A = poly_y[0] * A * A * A;
    out->B = (3.0 * poly_y[0] * B + poly_y[1]) * A * A;
    out->C = ((3.0 * poly_y[0] * B + 2.0 * poly_y[1]) * B + poly_y[2]) * A;
    out->D = (((poly_y[0] * B + poly_y[1]) * B + poly_y[2]) * B) + poly_y[3];
}

static inline double ecb_evaluate_d_core(const ecb_curve* c, double x)
{
    /* At runtime we only need to evaluate the branch-specific closed form
       for φ (which encodes the root u of the depressed cubic) and then plug
       it into Horner-form y(t). */
    const double one_third = 1.0 / 3.0;
    const double two_third_pi = -2.0 / 3.0 * 3.14159265358979323846;
    double phi = c->L * x + c->K;

    switch (c->type)
    {
    case ECB_TYPE_P3:
        break;
    case ECB_TYPE_X2:
        phi = phi > 0.0 ? sqrt(phi) : 0.0;
        break;
    case ECB_TYPE_X3P0:
        phi = ECB_CBRT_D(phi);
        break;
    case ECB_TYPE_X3COS:
        phi = cos( ECB_ACOS_D(ecb_clamp_d(phi, -1.0, 1.0)) * one_third + two_third_pi );
        break;
    case ECB_TYPE_X3SINH:
        phi = sinh( ECB_ASINH_D(phi) * one_third );
        break;
    case ECB_TYPE_X3COSH:
        phi = (phi >= 1.0)
              ? cosh( ECB_ACOSH_D(phi) * one_third )
              : cos( ECB_ACOS_D(phi < -1.0 ? -1.0 : phi) * one_third );
        break;
    default:
        return 0.0;
    }

    return ((c->A * phi + c->B) * phi + c->C) * phi + c->D;
}

void ecb_make_from_bezier(ecb_curve* out, const double px[4], const double py[4], double epsilon)
{
    double poly_x[4], poly_y[4];
    ecb_calculate_polynomial_d(px, px[0], px[3], poly_x);
    ecb_calculate_polynomial_d(py, px[0], px[3], poly_y);
    ecb_make_from_polynomial_d_core(out, poly_x, poly_y, epsilon == 0.0 ? ECB_DEFAULT_EPSILON : epsilon);
}

void ecb_make_from_points(ecb_curve* out,
                          double x0, double y0, double x1, double y1,
                          double x2, double y2, double x3, double y3,
                          double epsilon)
{
    double px[4] = { x0, x1, x2, x3 };
    double py[4] = { y0, y1, y2, y3 };
    ecb_make_from_bezier(out, px, py, epsilon);
}

void ecb_make_from_polynomial(ecb_curve* out, const double poly_x[4], const double poly_y[4], double epsilon)
{
    ecb_make_from_polynomial_d_core(out, poly_x, poly_y, epsilon == 0.0 ? ECB_DEFAULT_EPSILON : epsilon);
}

double ecb_evaluate(const ecb_curve* curve, double x)
{
    return ecb_evaluate_d_core(curve, x);
}

/* ---------- Core math: float precision ---------- */

#ifdef ECB_FAST_MATH
#define ECB_CBRT_F(x)   ecb_fast_cbrt_f(x)
#define ECB_ACOS_F(x)   ecb_fast_acos_f(x)
#define ECB_ASINH_F(x)  ecb_fast_asinh_f(x)
#define ECB_ACOSH_F(x)  ecb_fast_acosh_f(x)
#else
#define ECB_CBRT_F(x)   cbrtf(x)
#define ECB_ACOS_F(x)   acosf(x)
#define ECB_ASINH_F(x)  asinhf(x)
#define ECB_ACOSH_F(x)  acoshf(x)
#endif

static inline void ecb_calculate_polynomial_f(const float p[4], float x0, float x3, float out[4])
{
    const float m0[4] = { -1.0f, 3.0f, -3.0f, 1.0f };
    const float m1[4] = { 3.0f * x3, -3.0f * (2.0f * x3 + x0), 3.0f * (2.0f * x0 + x3), -3.0f * x0 };
    const float m2[4] = { -3.0f * x3 * x3, 3.0f * x3 * (2.0f * x0 + x3), -3.0f * x0 * (2.0f * x3 + x0), 3.0f * x0 * x0 };
    const float m3[4] = { x3 * x3 * x3, -3.0f * x0 * x3 * x3, 3.0f * x0 * x0 * x3, -x0 * x0 * x0 };
    const float scale = 1.0f / ((x3 - x0) * (x3 - x0) * (x3 - x0));
    out[0] = (m0[0]*p[0] + m0[1]*p[1] + m0[2]*p[2] + m0[3]*p[3]) * scale;
    out[1] = (m1[0]*p[0] + m1[1]*p[1] + m1[2]*p[2] + m1[3]*p[3]) * scale;
    out[2] = (m2[0]*p[0] + m2[1]*p[1] + m2[2]*p[2] + m2[3]*p[3]) * scale;
    out[3] = (m3[0]*p[0] + m3[1]*p[1] + m3[2]*p[2] + m3[3]*p[3]) * scale;
}

static inline ecb_type ecb_calculate_type_f(const float poly_x[4], float eps)
{
    const float b_a = poly_x[1] / poly_x[0];
    const float c_a = poly_x[2] / poly_x[0];
    const float d_a = poly_x[3] / poly_x[0];
    const float p = c_a - b_a * b_a / 3.0f;
    const float b_over_3a = poly_x[1] / (3.0f * poly_x[0]);
    const float q = (2.0f * b_over_3a * b_over_3a - c_a) * b_over_3a + d_a;

    if (fabsf(poly_x[0]) < eps && fabsf(poly_x[1]) < eps) return ECB_TYPE_P3;
    if (fabsf(poly_x[0]) < eps) return ECB_TYPE_X2;
    if (fabsf(p) < eps)        return ECB_TYPE_X3P0;
    if (poly_x[0] < 0.0f)      return ECB_TYPE_X3COS;
    if (p > 0.0f)              return ECB_TYPE_X3SINH;
    if (p < 0.0f)              return ECB_TYPE_X3COSH;
    return ECB_TYPE_NONE;
}

static inline void ecb_make_from_polynomial_f_core(ecb_curvef* out, const float poly_x[4], const float poly_y[4], float eps)
{
    if (eps == 0.0f) eps = ECB_DEFAULT_EPSILON_F;

    const float b_a = poly_x[1] / poly_x[0];
    const float c_a = poly_x[2] / poly_x[0];
    const float d_a = poly_x[3] / poly_x[0];
    const float p = c_a - b_a * b_a / 3.0f;
    const float b_over_3a = poly_x[1] / (3.0f * poly_x[0]);
    const float q = (2.0f * b_over_3a * b_over_3a - c_a) * b_over_3a + d_a;
    const float sign_p = ecb_sign_f(p);
    const float sign_b = ecb_sign_f(poly_x[1]);

    out->type = ecb_calculate_type_f(poly_x, eps);
    float A = 1.0f, B = 1.0f, extra = 1.0f;

    switch (out->type)
    {
    case ECB_TYPE_P3:
        A = 1.0f / poly_x[2];
        B = -(poly_x[3] / poly_x[2]);
        out->L = 1.0f;
        out->K = 0.0f;
        break;
    case ECB_TYPE_X2:
        A = sign_b;
        B = -(poly_x[2] / (2.0f * poly_x[1]));
        out->L = 1.0f / poly_x[1];
        out->K = B*B - poly_x[3] / poly_x[1];
        break;
    case ECB_TYPE_X3P0:
        B = -b_over_3a;
        out->L = 1.0f / poly_x[0];
        out->K = -q;
        break;
    case ECB_TYPE_X3COSH:
        extra = sign_b;
        /* fallthrough */
    case ECB_TYPE_X3COS:
        /* fallthrough */
    case ECB_TYPE_X3SINH:
        A = -2.0f * sign_p * extra * sqrtf(fabsf(p) / 3.0f);
        B = -b_over_3a;
        out->L = 3.0f * sign_p / (p * poly_x[0] * A);
        out->K = 3.0f * sign_p * (-q / p / A);
        break;
    default:
        out->type = ECB_TYPE_NONE;
        out->A = out->B = out->C = out->D = out->L = out->K = 0.0f;
        return;
    }

    out->A = poly_y[0] * A * A * A;
    out->B = (3.0f * poly_y[0] * B + poly_y[1]) * A * A;
    out->C = ((3.0f * poly_y[0] * B + 2.0f * poly_y[1]) * B + poly_y[2]) * A;
    out->D = (((poly_y[0] * B + poly_y[1]) * B + poly_y[2]) * B) + poly_y[3];
}

static inline float ecb_evaluate_f_core(const ecb_curvef* c, float x)
{
    const float one_third = 1.0f / 3.0f;
    const float two_third_pi = -2.0f / 3.0f * 3.14159265358979323846f;
    float phi = c->L * x + c->K;

    switch (c->type)
    {
    case ECB_TYPE_P3:
        break;
    case ECB_TYPE_X2:
        phi = phi > 0.0f ? sqrtf(phi) : 0.0f;
        break;
    case ECB_TYPE_X3P0:
        phi = ECB_CBRT_F(phi);
        break;
    case ECB_TYPE_X3COS:
        phi = cosf( ECB_ACOS_F(ecb_clamp_f(phi, -1.0f, 1.0f)) * one_third + two_third_pi );
        break;
    case ECB_TYPE_X3SINH:
        phi = sinhf( ECB_ASINH_F(phi) * one_third );
        break;
    case ECB_TYPE_X3COSH:
        phi = (phi >= 1.0f)
              ? coshf( ECB_ACOSH_F(phi) * one_third )
              : cosf( ECB_ACOS_F(phi < -1.0f ? -1.0f : phi) * one_third );
        break;
    default:
        return 0.0f;
    }

    return ((c->A * phi + c->B) * phi + c->C) * phi + c->D;
}

void ecb_make_from_bezier_f(ecb_curvef* out, const float px[4], const float py[4], float epsilon)
{
    float poly_x[4], poly_y[4];
    ecb_calculate_polynomial_f(px, px[0], px[3], poly_x);
    ecb_calculate_polynomial_f(py, px[0], px[3], poly_y);
    ecb_make_from_polynomial_f_core(out, poly_x, poly_y, epsilon == 0.0f ? ECB_DEFAULT_EPSILON_F : epsilon);
}

void ecb_make_from_points_f(ecb_curvef* out,
                            float x0, float y0, float x1, float y1,
                            float x2, float y2, float x3, float y3,
                            float epsilon)
{
    float px[4] = { x0, x1, x2, x3 };
    float py[4] = { y0, y1, y2, y3 };
    ecb_make_from_bezier_f(out, px, py, epsilon);
}

void ecb_make_from_polynomial_f(ecb_curvef* out, const float poly_x[4], const float poly_y[4], float epsilon)
{
    ecb_make_from_polynomial_f_core(out, poly_x, poly_y, epsilon == 0.0f ? ECB_DEFAULT_EPSILON_F : epsilon);
}

float ecb_evaluate_f(const ecb_curvef* curve, float x)
{
    return ecb_evaluate_f_core(curve, x);
}

#undef ECB_CBRT_D
#undef ECB_ACOS_D
#undef ECB_ASINH_D
#undef ECB_ACOSH_D
#undef ECB_CBRT_F
#undef ECB_ACOS_F
#undef ECB_ASINH_F
#undef ECB_ACOSH_F

#endif /* ECB_IMPLEMENTATION */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EASING_CUBIC_BEZIER_H */
