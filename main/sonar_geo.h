// main/sonar_geo.h
// 地点校验用的大圆距离。纯 C,由主机测试覆盖。
//
// 高德的模糊查询会返回同名异地 POI(例如"模力社区"可能命中北京的同名邮局),
// 所以候选必须用坐标校验,不能只看第一个结果。
#pragma once

double sonar_geo_distance_m(double lat1, double lon1, double lat2, double lon2);
