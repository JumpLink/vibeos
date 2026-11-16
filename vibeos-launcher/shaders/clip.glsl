// Coverage of the rounded rect and the clip at this fragment
float rrect_coverage(vec2 local, vec4 shape)
{
  vec2 q = abs(local) - shape.xy + shape.z;
  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - shape.z;
  float w = max(fwidth(d), 1e-4);
  return clamp(0.5 - d / w, 0.0, 1.0);
}

float clip_coverage(vec4 clip)
{
  if (clip.z <= clip.x)
    return 1.0;
  vec2 fc = gl_FragCoord.xy;
  vec2 a = clamp(fc - clip.xy + 0.5, 0.0, 1.0);
  vec2 b = clamp(clip.zw - fc + 0.5, 0.0, 1.0);
  return a.x * a.y * b.x * b.y;
}
