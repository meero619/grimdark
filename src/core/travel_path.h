#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace gd::core::travel_path {
struct Point { float x = 0, y = 0, z = 0; };
inline bool finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
inline float distance(Point a, Point b) { return std::hypot(a.x - b.x, a.z - b.z); }
// A snapped/partial corridor must not be reported as arrival at an unrelated destination.
inline bool reaches(const std::vector<Point>& path, Point goal, float tolerance = 4.0f) {
  if (path.empty() || !finite(goal)) return false;
  for (auto p : path) if (!finite(p)) return false;
  return distance(path.back(), goal) <= tolerance && std::abs(path.back().y - goal.y) < 5.0f;
}
inline Point toward(Point from, Point to, float length) {
  float d = distance(from, to);
  float t = d > 0.001f ? std::min(1.0f, length / d) : 1.0f;
  return {from.x + t * (to.x - from.x), from.y + t * (to.y - from.y), from.z + t * (to.z - from.z)};
}
// Measure forward progress by distance remaining along the corridor, not straight-line distance to
// the destination: a correct detour can initially move away from the goal.
inline float remaining(Point me, const std::vector<Point>& path, size_t corner) {
  if (corner >= path.size()) return 0;
  float d = distance(me, path[corner]);
  for (size_t i = corner + 1; i < path.size(); ++i) d += distance(path[i-1], path[i]);
  return d;
}
class Progress {
 public:
  void reset(double now, float remaining) { last_ = now; best_ = remaining; }
  bool stalled(double now, float remaining) {
    if (remaining < best_ - 0.3f) { best_ = remaining; last_ = now; }
    return now - last_ > 4.0;
  }
 private:
  double last_ = 0;
  float best_ = 0;
};
}
