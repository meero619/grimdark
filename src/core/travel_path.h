#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <queue>
#include <limits>

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
inline std::vector<size_t> shortest_route(const std::vector<Point>& nodes,
    const std::vector<std::vector<size_t>>& edges, size_t start, const std::vector<bool>& goals) {
  if (start >= nodes.size() || edges.size() != nodes.size() || goals.size() != nodes.size()) return {};
  using Entry = std::pair<float,size_t>;
  std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
  std::vector<float> costs(nodes.size(),std::numeric_limits<float>::infinity());
  std::vector<size_t> previous(nodes.size(),nodes.size());
  costs[start]=0; queue.push({0,start});
  while (!queue.empty()) {
    auto [cost,n]=queue.top(); queue.pop();
    if (cost != costs[n]) continue;
    if (goals[n]) {
      std::vector<size_t> result;
      for (size_t k=n;k!=nodes.size();k=previous[k]) result.push_back(k);
      std::reverse(result.begin(),result.end()); return result;
    }
    for (size_t next:edges[n]) {
      if (next>=nodes.size() || !finite(nodes[next])) continue;
      float value=cost+std::max(0.01f,distance(nodes[n],nodes[next]));
      if (value<costs[next]) { costs[next]=value; previous[next]=n; queue.push({value,next}); }
    }
  }
  return {};
}
}
