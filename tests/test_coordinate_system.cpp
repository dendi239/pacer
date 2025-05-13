#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>

TEST_CASE("London stays in London, high or low", "[london]") {
  pacer::GPSSample london{
      .lat = 51.5074,
      .lon = -0.1278,
      .altitude = 0,
      .full_speed = 0,
      .ground_speed = 0,
  };

  auto cs = pacer::CoordinateSystem(london);

  auto local = cs.Local(london);
  assert(std::abs(local.Norm()) < 1e-6);

  auto global = cs.Global(local);
  CHECK_THAT(global.lat, Catch::Matchers::WithinRelMatcher(london.lat, 1e-6));
  CHECK_THAT(global.lon, Catch::Matchers::WithinRelMatcher(london.lon, 1e-6));
  CHECK_THAT(global.altitude,
             Catch::Matchers::WithinRelMatcher(london.altitude, 1e-6));

  for (auto alt : {-100, 10, 100}) {
    auto jump = london;
    jump.altitude = alt;
    auto local_jump = cs.Local(jump);
    auto global_jump = cs.Global(local_jump);
    CHECK_THAT(jump.lat,
               Catch::Matchers::WithinRelMatcher(global_jump.lat, 1e-6));
    CHECK_THAT(jump.lon,
               Catch::Matchers::WithinRelMatcher(global_jump.lon, 1e-6));
    CHECK_THAT(jump.altitude,
               Catch::Matchers::WithinRelMatcher(global_jump.altitude, 1e-6));
  }
}
