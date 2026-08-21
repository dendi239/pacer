#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pacer/map-tiles/tile-math.hpp>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("LatLonToTileXY known values") {
  // Zoom 0: the whole world is one tile; any coordinate lands inside [0, 1).
  {
    auto [x, y] = pacer::LatLonToTileXY(0.0, 0.0, 0);
    REQUIRE_THAT(x, WithinAbs(0.5, 1e-9));
    REQUIRE_THAT(y, WithinAbs(0.5, 1e-9));
  }

  // Greenwich at zoom 1 sits on the seam between the two x tiles.
  {
    auto [x, y] = pacer::LatLonToTileXY(51.4779, 0.0, 1);
    REQUIRE_THAT(x, WithinAbs(1.0, 1e-9));
    REQUIRE(y < 1.0); // northern hemisphere is the top half
    REQUIRE(y > 0.0);
  }
}

TEST_CASE("TileXYToLatLon round-trips through LatLonToTileXY") {
  const double lats[] = {51.37600, -33.8688, 0.0, 68.9585};
  const double lons[] = {-0.36100, 151.2093, 0.0, 33.0827};

  for (int zoom : {3, 10, 19}) {
    for (double lat : lats) {
      for (double lon : lons) {
        auto [x, y] = pacer::LatLonToTileXY(lat, lon, zoom);
        auto [lat2, lon2] = pacer::TileXYToLatLon(zoom, x, y);
        REQUIRE_THAT(lat2, WithinAbs(lat, 1e-6));
        REQUIRE_THAT(lon2, WithinAbs(lon, 1e-6));
      }
    }
  }
}

TEST_CASE("Latitude outside Web-Mercator range is clamped, not NaN") {
  auto [x, y] = pacer::LatLonToTileXY(90.0, 0.0, 5);
  REQUIRE(x == x); // not NaN
  REQUIRE(y == y);
  // y sits at the top edge of the tile grid, give or take float error.
  REQUIRE_THAT(y, WithinAbs(0.0, 1e-6));
}

TEST_CASE("SatelliteTileUrl formats zoom/y/x") {
  REQUIRE(pacer::SatelliteTileUrl(19, 261921, 174208) ==
          "https://server.arcgisonline.com/ArcGIS/rest/services/"
          "World_Imagery/MapServer/tile/19/174208/261921");
}

TEST_CASE("MetersPerTilePixel halves with every zoom level") {
  double at_10 = pacer::MetersPerTilePixel(51.376, 10);
  double at_11 = pacer::MetersPerTilePixel(51.376, 11);
  REQUIRE_THAT(at_10 / at_11, WithinRel(2.0, 1e-9));

  // Equator, zoom 0: circumference / 256 ≈ 156543 m per pixel.
  REQUIRE_THAT(pacer::MetersPerTilePixel(0.0, 0), WithinRel(156543.03, 1e-4));
}
