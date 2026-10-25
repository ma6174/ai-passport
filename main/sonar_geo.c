// main/sonar_geo.c
#include "sonar_geo.h"

#include <math.h>

#define EARTH_RADIUS_M 6371000.0
#define DEG2RAD (3.14159265358979323846 / 180.0)

double sonar_geo_distance_m(double lat1, double lon1, double lat2, double lon2) {
    double dlat = (lat2 - lat1) * DEG2RAD;
    double dlon = (lon2 - lon1) * DEG2RAD;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * DEG2RAD) * cos(lat2 * DEG2RAD) * sin(dlon / 2) * sin(dlon / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return EARTH_RADIUS_M * c;
}
