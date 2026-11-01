#ifndef ARC_PAIR_HLSLI
#define ARC_PAIR_HLSLI

// Compensated arc lengths as stored by the segmented scan: float2(hi, lo), value hi + lo with
// |lo| <= half an ulp of hi. Differences of two large, close arcs keep the precision of the result.
// `precise` keeps the compiler from folding the error terms away. For a pair, hi > 0 exactly when
// hi + lo > 0.

// a + b exactly (TwoSum).
float2 PairFromSum(float a, float b)
{
  precise float s = a + b;
  precise float bv = s - a;
  precise float e = (a - (s - bv)) + (b - bv);
  return float2(s, e);
}

// Veltkamp split into two 12-bit halves.
float2 PairSplit(float a)
{
  precise float c = 4097.0 * a;
  precise float hi = c - (c - a);
  precise float lo = a - hi;
  return float2(hi, lo);
}

// a * b exactly (Dekker's TwoProduct).
float2 PairFromProduct(float a, float b)
{
  precise float p = a * b;
  float2 as = PairSplit(a);
  float2 bs = PairSplit(b);
  precise float e = (((as.x * bs.x - p) + as.x * bs.y) + as.y * bs.x) + as.y * bs.y;
  return float2(p, e);
}

float2 PairAdd(float2 a, float b)
{
  float2 s = PairFromSum(a.x, b);
  precise float e = s.y + a.y;
  precise float hi = s.x + e;
  precise float lo = e - (hi - s.x);
  return float2(hi, lo);
}

// a - b rounded to a float.
float PairDiff(float2 a, float2 b)
{
  float2 s = PairFromSum(a.x, -b.x);
  precise float d = s.x + (s.y + (a.y - b.y));
  return d;
}

bool PairLessEqual(float2 a, float2 b)
{
  return (a.x < b.x) || (a.x == b.x && a.y <= b.y);
}

// floor(arc / spacing), exact while it is below 2^24. spacing > 0, arc >= 0. The first guess can be off
// by more than one when the spacing is below an ulp of hi.
float PairGridIndex(float2 arc, float spacing)
{
  float k = floor(arc.x / spacing);
  k += floor(PairDiff(arc, PairFromProduct(k, spacing)) / spacing);
  float remainder = PairDiff(arc, PairFromProduct(k, spacing));
  if (remainder < 0.0) k -= 1.0;
  else if (remainder >= spacing) k += 1.0;
  return k;
}

// Grid points k * spacing (k = 0, 1, ...) in [0, arc]; 0 when the arc or the spacing is 0.
uint PairGridCount(float2 arc, float spacing)
{
  return (arc.x > 0.0 && spacing > 0.0) ? (uint)PairGridIndex(arc, spacing) + 1u : 0u;
}

#endif
