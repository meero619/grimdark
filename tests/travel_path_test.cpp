#include "doctest/doctest.h"
#include "core/travel_path.h"
#include <limits>
using namespace gd::core::travel_path;
TEST_CASE("Travel refuses missing, partial and wrong-floor routes") {
  CHECK_FALSE(reaches({}, {10,0,10}));
  CHECK_FALSE(reaches({{0,0,0},{3,0,3}}, {10,0,10}));
  CHECK_FALSE(reaches({{0,0,0},{10,10,10}}, {10,0,10}));
  CHECK(reaches({{0,0,0},{9,0,10}}, {10,0,10}));
  CHECK_FALSE(reaches({{std::numeric_limits<float>::quiet_NaN(),0,0},{10,0,10}}, {10,0,10}));
}
TEST_CASE("Travel follows detours and stops after no forward progress") {
  std::vector<Point> route{{0,0,10},{10,0,10},{10,0,0}};
  CHECK(remaining({0,0,0},route,0) == doctest::Approx(30));
  CHECK(remaining({0,0,5},route,0) == doctest::Approx(25));
  Progress p; p.reset(0,30);
  CHECK_FALSE(p.stalled(3,25));
  CHECK_FALSE(p.stalled(6,24));
  CHECK(p.stalled(11,24));
}
TEST_CASE("Travel lookahead never passes the next corner") {
  auto p = toward({0,0,0},{0,2,10},1.5f);
  CHECK(p.z == doctest::Approx(1.5));
  CHECK(p.y == doctest::Approx(0.3));
  p = toward({0,0,0},{0,0,1},1.5f);
  CHECK(p.z == doctest::Approx(1));
}
TEST_CASE("Regional travel follows connections and refuses disconnected destinations") {
  std::vector<Point> nodes{{0,0,0},{0,0,5},{5,0,5},{5,0,0},{100,0,100}};
  std::vector<std::vector<size_t>> edges{{1},{0,2},{1,3},{2},{}};
  auto route=shortest_route(nodes,edges,0,{false,false,false,true,false});
  CHECK(route==std::vector<size_t>{0,1,2,3});
  CHECK(shortest_route(nodes,edges,0,{false,false,false,false,true}).empty());
  CHECK(shortest_route(nodes,edges,0,{true,false,false,false,false})==std::vector<size_t>{0});
}
