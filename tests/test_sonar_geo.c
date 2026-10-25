// tests/test_sonar_geo.c
#include <assert.h>

#include "sonar_geo.h"

int main(void) {
    // 同一个点
    assert(sonar_geo_distance_m(31.184698, 121.617324, 31.184698, 121.617324) < 1.0);
    // 张江三点之间:模力社区 <-> 人工智能岛 约 0.8 km
    double d = sonar_geo_distance_m(31.184698, 121.617324, 31.184820, 121.609497);
    assert(d > 700 && d < 900);
    // 张江 <-> 北京:约 1100 km,用来拒绝同名异地 POI
    d = sonar_geo_distance_m(31.184698, 121.617324, 39.90957, 116.29533);
    assert(d > 1000000 && d < 1300000);
    return 0;
}
