/ geos.q — load qgeos.so and bind its functions into the .geos namespace.
/ Set .geos.lib before loading this file to use another path (default: ./qgeos).

.geos.lib:@[value;`.geos.lib;`:./qgeos];

.geos.version:   .geos.lib 2:(`qgeos_version;1);
.geos.fromwkt:   .geos.lib 2:(`qgeos_fromwkt;1);
.geos.towkt:     .geos.lib 2:(`qgeos_towkt;1);
.geos.point:     .geos.lib 2:(`qgeos_point;2);
.geos.area:      .geos.lib 2:(`qgeos_area;1);
.geos.length:    .geos.lib 2:(`qgeos_length;1);
.geos.centroid:  .geos.lib 2:(`qgeos_centroid;1);
.geos.envelope:  .geos.lib 2:(`qgeos_envelope;1);
.geos.hull:      .geos.lib 2:(`qgeos_hull;1);
.geos.buffer:    .geos.lib 2:(`qgeos_buffer;2);
.geos.distance:  .geos.lib 2:(`qgeos_distance;2);
.geos.intersects:.geos.lib 2:(`qgeos_intersects;2);
.geos.contains:  .geos.lib 2:(`qgeos_contains;2);
.geos.within:    .geos.lib 2:(`qgeos_within;2);

.geos.sjoin:     .geos.lib 2:(`qgeos_sjoin;3);
.geos.nearest:   .geos.lib 2:(`qgeos_nearest;2);
