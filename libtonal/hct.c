/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2021-2025 Google LLC (Material Color Utilities, of which this is a port to C)
 * Copyright 2026 pulpoff <pashkovsky@gmail.com>
 *
 * HCT: sRGB to and from CAM16 hue and chroma with L* tone, and the contrast ratio of two tones. See tonal.h. */
#include "tonal-private.h"

#include <stdlib.h>
#include <string.h>

/* ---- sRGB, XYZ, L*a*b* ------------------------------------------------------------------------------------------ */

static const double srgb_to_xyz[3][3] = {
  { 0.41233895, 0.35762064, 0.18051042 },
  { 0.2126, 0.7152, 0.0722 },
  { 0.01932141, 0.11916382, 0.95034478 },
};

static const double xyz_to_srgb[3][3] = {
  { 3.2413774792388685, -1.5376652402851851, -0.49885366846268053 },
  { -0.9691452513005321, 1.8758853451067872, 0.04156585616912061 },
  { 0.05562093689691305, -0.20395524564742123, 1.0571799111220335 },
};

static const double white_point_d65[3] = { 95.047, 100.0, 108.883 };

static guint32
argb_from_rgb (int red, int green, int blue)
{
  return 0xff000000u | ((guint32) (red & 255) << 16) | ((guint32) (green & 255) << 8) | (guint32) (blue & 255);
}

/* an sRGB component 0..255 as linear light, 0..100 */
double
tonal_linearized (int component)
{
  double normalized = component / 255.0;

  if (normalized <= 0.040449936)
    return normalized / 12.92 * 100.0;
  return pow ((normalized + 0.055) / 1.055, 2.4) * 100.0;
}

/* linear light 0..100 as an sRGB component, rounded and clamped to 0..255 */
int
tonal_delinearized (double linear)
{
  double normalized = linear / 100.0;
  double delinearized;

  if (normalized <= 0.0031308)
    delinearized = normalized * 12.92;
  else
    delinearized = 1.055 * pow (normalized, 1.0 / 2.4) - 0.055;
  return (int) tonal_clamp (0, 255, tonal_round (delinearized * 255.0));
}

static double
lab_f (double t)
{
  const double e = 216.0 / 24389.0, kappa = 24389.0 / 27.0;

  return t > e ? pow (t, 1.0 / 3.0) : (kappa * t + 16) / 116;
}

static double
lab_invf (double ft)
{
  const double e = 216.0 / 24389.0, kappa = 24389.0 / 27.0;
  double ft3 = ft * ft * ft;

  return ft3 > e ? ft3 : (116 * ft - 16) / kappa;
}

double
tonal_y_from_lstar (double lstar)
{
  return 100.0 * lab_invf ((lstar + 16.0) / 116.0);
}

double
tonal_lstar_from_y (double y)
{
  return lab_f (y / 100.0) * 116.0 - 16.0;
}

static void
xyz_from_argb (guint32 argb, double xyz[3])
{
  double r = tonal_linearized ((argb >> 16) & 255), g = tonal_linearized ((argb >> 8) & 255);
  double b = tonal_linearized (argb & 255);

  for (int i = 0; i < 3; i++)
    xyz[i] = r * srgb_to_xyz[i][0] + g * srgb_to_xyz[i][1] + b * srgb_to_xyz[i][2];
}

static guint32
argb_from_xyz (double x, double y, double z)
{
  const double (*m)[3] = xyz_to_srgb;

  return argb_from_rgb (tonal_delinearized (m[0][0] * x + m[0][1] * y + m[0][2] * z),
                        tonal_delinearized (m[1][0] * x + m[1][1] * y + m[1][2] * z),
                        tonal_delinearized (m[2][0] * x + m[2][1] * y + m[2][2] * z));
}

double
tonal_lstar_from_argb (guint32 argb)
{
  double xyz[3];

  xyz_from_argb (argb, xyz);
  return 116.0 * lab_f (xyz[1] / 100.0) - 16.0;
}

guint32
tonal_argb_from_lstar (double lstar)
{
  int component = tonal_delinearized (tonal_y_from_lstar (lstar));

  return argb_from_rgb (component, component, component);
}

void
tonal_lab_from_argb (guint32 argb, double lab[3])
{
  double r = tonal_linearized ((argb >> 16) & 255), g = tonal_linearized ((argb >> 8) & 255);
  double b = tonal_linearized (argb & 255);
  const double (*m)[3] = srgb_to_xyz;
  double x = m[0][0] * r + m[0][1] * g + m[0][2] * b;
  double y = m[1][0] * r + m[1][1] * g + m[1][2] * b;
  double z = m[2][0] * r + m[2][1] * g + m[2][2] * b;
  double fx = lab_f (x / white_point_d65[0]), fy = lab_f (y / white_point_d65[1]), fz = lab_f (z / white_point_d65[2]);

  lab[0] = 116.0 * fy - 16;
  lab[1] = 500.0 * (fx - fy);
  lab[2] = 200.0 * (fy - fz);
}

guint32
tonal_argb_from_lab (const double lab[3])
{
  double fy = (lab[0] + 16.0) / 116.0;
  double fx = lab[1] / 500.0 + fy;
  double fz = fy - lab[2] / 200.0;

  return argb_from_xyz (lab_invf (fx) * white_point_d65[0], lab_invf (fy) * white_point_d65[1],
                        lab_invf (fz) * white_point_d65[2]);
}

/* ---- CAM16 under the standard viewing conditions --------------------------------------------------------------- */

static const double xyz_to_cam16rgb[3][3] = {
  { 0.401288, 0.650173, -0.051461 },
  { -0.250268, 1.204414, 0.045854 },
  { -0.002079, 0.048952, 0.953127 },
};

/* the viewing conditions every HCT colour is seen in: sRGB's white point, an adapting luminance of 200 / pi times
 * the luminance of L* 50, a background of L* 50, an average surround, the illuminant not discounted */
typedef struct {
  double aw, nbb, ncb, c, nc, n, rgb_d[3], fl, fl_root, z;
} ViewingConditions;

static const ViewingConditions *
viewing_conditions (void)
{
  static ViewingConditions vc;
  static gsize done;

  if (g_once_init_enter (&done))
    {
      const double *xyz = white_point_d65;
      const double (*m)[3] = xyz_to_cam16rgb;
      double adapting_luminance = 200.0 / G_PI * tonal_y_from_lstar (50.0) / 100.0;
      double background_lstar = 50.0, surround = 2.0;
      double r_w = xyz[0] * m[0][0] + xyz[1] * m[0][1] + xyz[2] * m[0][2];
      double g_w = xyz[0] * m[1][0] + xyz[1] * m[1][1] + xyz[2] * m[1][2];
      double b_w = xyz[0] * m[2][0] + xyz[1] * m[2][1] + xyz[2] * m[2][2];
      double f = 0.8 + surround / 10.0;
      double c = f >= 0.9 ? tonal_lerp (0.59, 0.69, (f - 0.9) * 10.0) : tonal_lerp (0.525, 0.59, (f - 0.8) * 10.0);
      double d = f * (1.0 - (1.0 / 3.6) * exp ((-adapting_luminance - 42.0) / 92.0));
      double k, k4, k4f, rgb_a_factors[3], rgb_a[3];

      d = tonal_clamp (0.0, 1.0, d);
      vc.nc = f;
      vc.c = c;
      vc.rgb_d[0] = d * (100.0 / r_w) + 1.0 - d;
      vc.rgb_d[1] = d * (100.0 / g_w) + 1.0 - d;
      vc.rgb_d[2] = d * (100.0 / b_w) + 1.0 - d;
      k = 1.0 / (5.0 * adapting_luminance + 1.0);
      k4 = k * k * k * k;
      k4f = 1.0 - k4;
      vc.fl = k4 * adapting_luminance + 0.1 * k4f * k4f * cbrt (5.0 * adapting_luminance);
      vc.n = tonal_y_from_lstar (background_lstar) / white_point_d65[1];
      vc.z = 1.48 + sqrt (vc.n);
      vc.nbb = 0.725 / pow (vc.n, 0.2);
      vc.ncb = vc.nbb;
      rgb_a_factors[0] = pow (vc.fl * vc.rgb_d[0] * r_w / 100.0, 0.42);
      rgb_a_factors[1] = pow (vc.fl * vc.rgb_d[1] * g_w / 100.0, 0.42);
      rgb_a_factors[2] = pow (vc.fl * vc.rgb_d[2] * b_w / 100.0, 0.42);
      for (int i = 0; i < 3; i++)
        rgb_a[i] = 400.0 * rgb_a_factors[i] / (rgb_a_factors[i] + 27.13);
      vc.aw = (2.0 * rgb_a[0] + rgb_a[1] + 0.05 * rgb_a[2]) * vc.nbb;
      vc.fl_root = pow (vc.fl, 0.25);
      g_once_init_leave (&done, 1);
    }
  return &vc;
}

static double
signum (double x)
{
  return x < 0 ? -1.0 : x > 0 ? 1.0 : 0.0;
}

/* CAM16 hue and chroma of an sRGB colour */
static void
cam16_from_argb (guint32 argb, double *hue_out, double *chroma_out)
{
  const ViewingConditions *vc = viewing_conditions ();
  const double (*m)[3] = xyz_to_cam16rgb;
  double red_l = tonal_linearized ((argb >> 16) & 255), green_l = tonal_linearized ((argb >> 8) & 255);
  double blue_l = tonal_linearized (argb & 255);
  double x = 0.41233895 * red_l + 0.35762064 * green_l + 0.18051042 * blue_l;
  double y = 0.2126 * red_l + 0.7152 * green_l + 0.0722 * blue_l;
  double z = 0.01932141 * red_l + 0.11916382 * green_l + 0.95034478 * blue_l;
  double r_t = x * m[0][0] + y * m[0][1] + z * m[0][2];
  double g_t = x * m[1][0] + y * m[1][1] + z * m[1][2];
  double b_t = x * m[2][0] + y * m[2][1] + z * m[2][2];
  double r_d = vc->rgb_d[0] * r_t, g_d = vc->rgb_d[1] * g_t, b_d = vc->rgb_d[2] * b_t;
  double r_af = pow (vc->fl * fabs (r_d) / 100.0, 0.42);
  double g_af = pow (vc->fl * fabs (g_d) / 100.0, 0.42);
  double b_af = pow (vc->fl * fabs (b_d) / 100.0, 0.42);
  double r_a = signum (r_d) * 400.0 * r_af / (r_af + 27.13);
  double g_a = signum (g_d) * 400.0 * g_af / (g_af + 27.13);
  double b_a = signum (b_d) * 400.0 * b_af / (b_af + 27.13);
  double a = (11.0 * r_a + -12.0 * g_a + b_a) / 11.0;
  double b = (r_a + g_a - 2.0 * b_a) / 9.0;
  double u = (20.0 * r_a + 20.0 * g_a + 21.0 * b_a) / 20.0;
  double p2 = (40.0 * r_a + 20.0 * g_a + b_a) / 20.0;
  double hue = tonal_sanitize_degrees (atan2 (b, a) * (180.0 / G_PI));
  double ac = p2 * vc->nbb;
  double j = 100.0 * pow (ac / vc->aw, vc->c * vc->z);
  double hue_prime = hue < 20.14 ? hue + 360 : hue;
  double e_hue = 0.25 * (cos (hue_prime * (G_PI / 180.0) + 2.0) + 3.8);
  double p1 = 50000.0 / 13.0 * e_hue * vc->nc * vc->ncb;
  double t = p1 * hypot (a, b) / (u + 0.305);
  double alpha = pow (1.64 - pow (0.29, vc->n), 0.73) * pow (t, 0.9);

  *hue_out = hue;
  *chroma_out = alpha * sqrt (j / 100.0);
}

TonalHct
tonal_hct_from_argb (guint32 argb)
{
  TonalHct hct;

  cam16_from_argb (argb, &hct.hue, &hct.chroma);
  hct.tone = tonal_lstar_from_argb (argb);
  return hct;
}

/* ---- the solver: hue, chroma and tone to sRGB ------------------------------------------------------------------ *
 *
 * First Newton's method on CAM16's lightness J for the exact colour; when that falls outside sRGB, a bisection
 * along the edge of the sRGB cube at the tone's luminance for the colour of that hue with the most chroma. */

static const double scaled_discount_from_linrgb[3][3] = {
  { 0.001200833568784504, 0.002389694492170889, 0.0002795742885861124 },
  { 0.0005891086651375999, 0.0029785502573438758, 0.0003270666104008398 },
  { 0.00010146692491640572, 0.0005364214359186694, 0.0032979401770712076 },
};

static const double linrgb_from_scaled_discount[3][3] = {
  { 1373.2198709594231, -1100.4251190754821, -7.278681089101213 },
  { -271.815969077903, 559.6580465940733, -32.46047482791194 },
  { 1.9622899599665666, -57.173814538844006, 308.7233197812385 },
};

static const double y_from_linrgb[3] = { 0.2126, 0.7152, 0.0722 };

/* the linear values at which an sRGB component steps from one 8-bit value to the next */
static const double critical_planes[255] = {
  0.015176349177441876, 0.045529047532325624, 0.07588174588720938, 0.10623444424209313,
  0.13658714259697685, 0.16693984095186062, 0.19729253930674434, 0.2276452376616281,
  0.2579979360165119, 0.28835063437139563, 0.3188300904430532, 0.350925934958123,
  0.3848314933096426, 0.42057480301049466, 0.458183274052838, 0.4976837250274023,
  0.5391024159806381, 0.5824650784040898, 0.6277969426914107, 0.6751227633498623,
  0.7244668422128921, 0.775853049866786, 0.829304845476233, 0.8848452951698498,
  0.942497089126609, 1.0022825574869039, 1.0642236851973577, 1.1283421258858297,
  1.1946592148522128, 1.2631959812511864, 1.3339731595349034, 1.407011200216447,
  1.4823302800086415, 1.5599503113873272, 1.6398909516233677, 1.7221716113234105,
  1.8068114625156377, 1.8938294463134073, 1.9832442801866852, 2.075074464868551,
  2.1693382909216234, 2.2660538449872063, 2.36523901573795, 2.4669114995532007,
  2.5710888059345764, 2.6777882626779785, 2.7870270208169257, 2.898822059350997,
  3.0131901897720907, 3.1301480604002863, 3.2497121605402226, 3.3718988244681087,
  3.4967242352587946, 3.624204428461639, 3.754355295633311, 3.887192587735158,
  4.022731918402185, 4.160988767090289, 4.301978482107941, 4.445716283538092,
  4.592217266055746, 4.741496401646282, 4.893568542229298, 5.048448422192488,
  5.20615066083972, 5.3666897647573375, 5.5300801301023865, 5.696336044816294,
  5.865471690767354, 6.037501145825082, 6.212438385869475, 6.390297286737924,
  6.571091626112461, 6.7548350853498045, 6.941541251256611, 7.131223617812143,
  7.323895587840543, 7.5195704746346665, 7.7182615035334345, 7.919981813454504,
  8.124744458384042, 8.332562408825165, 8.543448553206703, 8.757415699253682,
  8.974476575321063, 9.194643831691977, 9.417930041841839, 9.644347703669503,
  9.873909240696694, 10.106627003236781, 10.342513269534024, 10.58158024687427,
  10.8238400726681, 11.069304815507364, 11.317986476196008, 11.569896988756009,
  11.825048221409341, 12.083451977536606, 12.345119996613247, 12.610063955123938,
  12.878295467455942, 13.149826086772048, 13.42466730586372, 13.702830557985108,
  13.984327217668513, 14.269168601521828, 14.55736596900856, 14.848930523210871,
  15.143873411576273, 15.44220572664832, 15.743938506781891, 16.04908273684337,
  16.35764934889634, 16.66964922287304, 16.985093187232053, 17.30399201960269,
  17.62635644741625, 17.95219714852476, 18.281524751807332, 18.614349837764564,
  18.95068293910138, 19.290534541298456, 19.633915083172692, 19.98083495742689,
  20.331304511189067, 20.685334046541502, 21.042933821039977, 21.404114048223256,
  21.76888489811322, 22.137256497705877, 22.50923893145328, 22.884842241736916,
  23.264076429332462, 23.6469514538663, 24.033477234264016, 24.42366364919083,
  24.817520537484558, 25.21505769858089, 25.61628489293138, 26.021211842414342,
  26.429848230738664, 26.842203703840827, 27.258287870275353, 27.678110301598522,
  28.10168053274597, 28.529008062403893, 28.96010235337422, 29.39497283293396,
  29.83362889318845, 30.276079891419332, 30.722335150426627, 31.172403958865512,
  31.62629557157785, 32.08401920991837, 32.54558406207592, 33.010999283389665,
  33.4802739966603, 33.953417292456834, 34.430438229418264, 34.911345834551085,
  35.39614910352207, 35.88485700094671, 36.37747846067349, 36.87402238606382,
  37.37449765026789, 37.87891309649659, 38.38727753828926, 38.89959975977785,
  39.41588851594697, 39.93615253289054, 40.460400508064545, 40.98864111053629,
  41.520882981230194, 42.05713473317016, 42.597404951718396, 43.141702194811224,
  43.6900349931913, 44.24241185063697, 44.798841244188324, 45.35933162437017,
  45.92389141541209, 46.49252901546552, 47.065252796817916, 47.64207110610409,
  48.22299226451468, 48.808024568002054, 49.3971762874833, 49.9904556690408,
  50.587870934119984, 51.189430279724725, 51.79514187861014, 52.40501387947288,
  53.0190544071392, 53.637271562750364, 54.259673423945976, 54.88626804504493,
  55.517063457223934, 56.15206766869424, 56.79128866487574, 57.43473440856916,
  58.08241284012621, 58.734331877617365, 59.39049941699807, 60.05092333227251,
  60.715611475655585, 61.38457167773311, 62.057811747619894, 62.7353394731159,
  63.417162620860914, 64.10328893648692, 64.79372614476921, 65.48848194977529,
  66.18756403501224, 66.89098006357258, 67.59873767827808, 68.31084450182222,
  69.02730813691093, 69.74813616640164, 70.47333615344107, 71.20291564160104,
  71.93688215501312, 72.67524319850172, 73.41800625771542, 74.16517879925733,
  74.9167682708136, 75.67278210128072, 76.43322770089146, 77.1981124613393,
  77.96744375590167, 78.74122893956174, 79.51947534912904, 80.30219030335869,
  81.08938110306934, 81.88105503125999, 82.67721935322541, 83.4778813166706,
  84.28304815182372, 85.09272707154808, 85.90692527145302, 86.72564993000343,
  87.54890820862819, 88.3767072518277, 89.2090541872801, 90.04595612594655,
  90.88742016217518, 91.73345337380438, 92.58406282226491, 93.43925555268066,
  94.29903859396902, 95.16341895893969, 96.03240364439274, 96.9059996312159,
  97.78421388448044, 98.6670533535366, 99.55452497210776,
};

static void
matrix_multiply (const double row[3], const double (*m)[3], double out[3])
{
  double a = row[0] * m[0][0] + row[1] * m[0][1] + row[2] * m[0][2];
  double b = row[0] * m[1][0] + row[1] * m[1][1] + row[2] * m[1][2];
  double c = row[0] * m[2][0] + row[1] * m[2][1] + row[2] * m[2][2];

  out[0] = a;
  out[1] = b;
  out[2] = c;
}

static double
sanitize_radians (double angle)
{
  return fmod (angle + G_PI * 8, G_PI * 2);
}

static double
true_delinearized (double component)
{
  double normalized = component / 100.0;
  double delinearized;

  if (normalized <= 0.0031308)
    delinearized = normalized * 12.92;
  else
    delinearized = 1.055 * pow (normalized, 1.0 / 2.4) - 0.055;
  return delinearized * 255.0;
}

static double
chromatic_adaptation (double component)
{
  double af = pow (fabs (component), 0.42);

  return signum (component) * 400.0 * af / (af + 27.13);
}

static double
hue_of (const double linrgb[3])
{
  double scaled_discount[3];
  double r_a, g_a, b_a;

  matrix_multiply (linrgb, scaled_discount_from_linrgb, scaled_discount);
  r_a = chromatic_adaptation (scaled_discount[0]);
  g_a = chromatic_adaptation (scaled_discount[1]);
  b_a = chromatic_adaptation (scaled_discount[2]);
  return atan2 ((r_a + g_a - 2.0 * b_a) / 9.0, (11.0 * r_a + -12.0 * g_a + b_a) / 11.0);
}

static gboolean
are_in_cyclic_order (double a, double b, double c)
{
  return sanitize_radians (b - a) < sanitize_radians (c - a);
}

static void
lerp_point (const double source[3], double t, const double target[3], double out[3])
{
  double p[3];

  for (int i = 0; i < 3; i++)
    p[i] = source[i] + (target[i] - source[i]) * t;
  memcpy (out, p, sizeof p);
}

static void
set_coordinate (const double source[3], double coordinate, const double target[3], int axis, double out[3])
{
  double t = (coordinate - source[axis]) / (target[axis] - source[axis]);

  lerp_point (source, t, target, out);
}

static gboolean
is_bounded (double x)
{
  return 0.0 <= x && x <= 100.0;
}

/* the nth of the 12 edges of the RGB cube crossed by the plane of luminance y; FALSE when it is not crossed */
static gboolean
nth_vertex (double y, int n, double out[3])
{
  double k_r = y_from_linrgb[0], k_g = y_from_linrgb[1], k_b = y_from_linrgb[2];
  double coord_a = n % 4 <= 1 ? 0.0 : 100.0;
  double coord_b = n % 2 == 0 ? 0.0 : 100.0;

  if (n < 4)
    {
      double g = coord_a, b = coord_b, r = (y - g * k_g - b * k_b) / k_r;

      out[0] = r, out[1] = g, out[2] = b;
      return is_bounded (r);
    }
  else if (n < 8)
    {
      double b = coord_a, r = coord_b, g = (y - r * k_r - b * k_b) / k_g;

      out[0] = r, out[1] = g, out[2] = b;
      return is_bounded (g);
    }
  else
    {
      double r = coord_a, g = coord_b, b = (y - r * k_r - g * k_g) / k_b;

      out[0] = r, out[1] = g, out[2] = b;
      return is_bounded (b);
    }
}

static void
bisect_to_segment (double y, double target_hue, double left[3], double right[3])
{
  double left_hue = 0.0, right_hue = 0.0;
  gboolean initialized = FALSE, uncut = TRUE;

  for (int i = 0; i < 3; i++)
    left[i] = right[i] = -1.0;
  for (int n = 0; n < 12; n++)
    {
      double mid[3], mid_hue;

      if (!nth_vertex (y, n, mid))
        continue;
      mid_hue = hue_of (mid);
      if (!initialized)
        {
          memcpy (left, mid, sizeof mid);
          memcpy (right, mid, sizeof mid);
          left_hue = right_hue = mid_hue;
          initialized = TRUE;
          continue;
        }
      if (uncut || are_in_cyclic_order (left_hue, mid_hue, right_hue))
        {
          uncut = FALSE;
          if (are_in_cyclic_order (left_hue, target_hue, mid_hue))
            {
              memcpy (right, mid, sizeof mid);
              right_hue = mid_hue;
            }
          else
            {
              memcpy (left, mid, sizeof mid);
              left_hue = mid_hue;
            }
        }
    }
}

static int
critical_plane_below (double x)
{
  return (int) floor (x - 0.5);
}

static int
critical_plane_above (double x)
{
  return (int) ceil (x - 0.5);
}

static void
bisect_to_limit (double y, double target_hue, double out[3])
{
  double left[3], right[3], left_hue;

  bisect_to_segment (y, target_hue, left, right);
  left_hue = hue_of (left);
  for (int axis = 0; axis < 3; axis++)
    {
      int l_plane, r_plane;

      if (left[axis] == right[axis])
        continue;
      if (left[axis] < right[axis])
        {
          l_plane = critical_plane_below (true_delinearized (left[axis]));
          r_plane = critical_plane_above (true_delinearized (right[axis]));
        }
      else
        {
          l_plane = critical_plane_above (true_delinearized (left[axis]));
          r_plane = critical_plane_below (true_delinearized (right[axis]));
        }
      for (int i = 0; i < 8; i++)
        {
          int m_plane;
          double mid[3], mid_hue;

          if (abs (r_plane - l_plane) <= 1)
            break;
          m_plane = (int) floor ((l_plane + r_plane) / 2.0);
          set_coordinate (left, critical_planes[m_plane], right, axis, mid);
          mid_hue = hue_of (mid);
          if (are_in_cyclic_order (left_hue, target_hue, mid_hue))
            {
              memcpy (right, mid, sizeof mid);
              r_plane = m_plane;
            }
          else
            {
              memcpy (left, mid, sizeof mid);
              left_hue = mid_hue;
              l_plane = m_plane;
            }
        }
    }
  for (int i = 0; i < 3; i++)
    out[i] = (left[i] + right[i]) / 2;
}

static double
inverse_chromatic_adaptation (double adapted)
{
  double adapted_abs = fabs (adapted);
  double base = MAX (0, 27.13 * adapted_abs / (400.0 - adapted_abs));

  return signum (adapted) * pow (base, 1.0 / 0.42);
}

static guint32
argb_from_linrgb (const double linrgb[3])
{
  return argb_from_rgb (tonal_delinearized (linrgb[0]), tonal_delinearized (linrgb[1]),
                        tonal_delinearized (linrgb[2]));
}

/* the exact colour, or 0 when it is not in sRGB */
static guint32
find_result_by_j (double hue_radians, double chroma, double y)
{
  const ViewingConditions *vc = viewing_conditions ();
  double j = sqrt (y) * 11.0;
  double t_inner_coeff = 1 / pow (1.64 - pow (0.29, vc->n), 0.73);
  double e_hue = 0.25 * (cos (hue_radians + 2.0) + 3.8);
  double p1 = e_hue * (50000.0 / 13.0) * vc->nc * vc->ncb;
  double h_sin = sin (hue_radians), h_cos = cos (hue_radians);

  for (int round = 0; round < 5; round++)
    {
      double j_normalized = j / 100.0;
      double alpha = chroma == 0.0 || j == 0.0 ? 0.0 : chroma / sqrt (j_normalized);
      double t = pow (alpha * t_inner_coeff, 1.0 / 0.9);
      double ac = vc->aw * pow (j_normalized, 1.0 / vc->c / vc->z);
      double p2 = ac / vc->nbb;
      double gamma = 23.0 * (p2 + 0.305) * t / (23.0 * p1 + 11 * t * h_cos + 108.0 * t * h_sin);
      double a = gamma * h_cos, b = gamma * h_sin;
      double scaled[3] = {
        inverse_chromatic_adaptation ((460.0 * p2 + 451.0 * a + 288.0 * b) / 1403.0),
        inverse_chromatic_adaptation ((460.0 * p2 - 891.0 * a - 261.0 * b) / 1403.0),
        inverse_chromatic_adaptation ((460.0 * p2 - 220.0 * a - 6300.0 * b) / 1403.0),
      };
      double linrgb[3], fnj;

      matrix_multiply (scaled, linrgb_from_scaled_discount, linrgb);
      if (linrgb[0] < 0 || linrgb[1] < 0 || linrgb[2] < 0)
        return 0;
      fnj = y_from_linrgb[0] * linrgb[0] + y_from_linrgb[1] * linrgb[1] + y_from_linrgb[2] * linrgb[2];
      if (fnj <= 0)
        return 0;
      if (round == 4 || fabs (fnj - y) < 0.002)
        {
          if (linrgb[0] > 100.01 || linrgb[1] > 100.01 || linrgb[2] > 100.01)
            return 0;
          return argb_from_linrgb (linrgb);
        }
      j = j - (fnj - y) * j / (2 * fnj);
    }
  return 0;
}

guint32
tonal_hct_to_argb (double hue, double chroma, double tone)
{
  double hue_radians, y, linrgb[3];
  guint32 exact;

  if (chroma < 0.0001 || tone < 0.0001 || tone > 99.9999)
    return tonal_argb_from_lstar (tone);
  hue = tonal_sanitize_degrees (hue);
  hue_radians = hue / 180 * G_PI;
  y = tonal_y_from_lstar (tone);
  exact = find_result_by_j (hue_radians, chroma, y);
  if (exact != 0)
    return exact;
  bisect_to_limit (y, hue_radians, linrgb);
  return argb_from_linrgb (linrgb);
}

/* ---- contrast ----------------------------------------------------------------------------------------------- */

#define CONTRAST_RATIO_EPSILON 0.04
#define LUMINANCE_GAMUT_MAP_TOLERANCE 0.4

static double
ratio_of_ys (double y1, double y2)
{
  double lighter = MAX (y1, y2);
  double darker = lighter == y2 ? y1 : y2;

  return (lighter + 5.0) / (darker + 5.0);
}

double
tonal_contrast_ratio (double tone_a, double tone_b)
{
  return ratio_of_ys (tonal_y_from_lstar (tone_a), tonal_y_from_lstar (tone_b));
}

/* the darkest tone lighter than `tone` that reaches `ratio`, or -1 */
double
tonal_contrast_lighter (double tone, double ratio)
{
  double dark_y, light_y, real, value;

  if (tone < 0.0 || tone > 100.0)
    return -1.0;
  dark_y = tonal_y_from_lstar (tone);
  light_y = ratio * (dark_y + 5.0) - 5.0;
  if (light_y < 0.0 || light_y > 100.0)
    return -1.0;
  real = ratio_of_ys (light_y, dark_y);
  if (real < ratio && fabs (real - ratio) > CONTRAST_RATIO_EPSILON)
    return -1.0;
  value = tonal_lstar_from_y (light_y) + LUMINANCE_GAMUT_MAP_TOLERANCE;
  return value < 0 || value > 100 ? -1.0 : value;
}

/* the lightest tone darker than `tone` that reaches `ratio`, or -1 */
double
tonal_contrast_darker (double tone, double ratio)
{
  double light_y, dark_y, real, value;

  if (tone < 0.0 || tone > 100.0)
    return -1.0;
  light_y = tonal_y_from_lstar (tone);
  dark_y = (light_y + 5.0) / ratio - 5.0;
  if (dark_y < 0.0 || dark_y > 100.0)
    return -1.0;
  real = ratio_of_ys (light_y, dark_y);
  if (real < ratio && fabs (real - ratio) > CONTRAST_RATIO_EPSILON)
    return -1.0;
  value = tonal_lstar_from_y (dark_y) - LUMINANCE_GAMUT_MAP_TOLERANCE;
  return value < 0 || value > 100 ? -1.0 : value;
}

double
tonal_contrast_lighter_unsafe (double tone, double ratio)
{
  double safe = tonal_contrast_lighter (tone, ratio);

  return safe < 0.0 ? 100.0 : safe;
}

double
tonal_contrast_darker_unsafe (double tone, double ratio)
{
  return MAX (0.0, tonal_contrast_darker (tone, ratio));
}
