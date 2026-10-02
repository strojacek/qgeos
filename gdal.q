/ gdal.q — load qgdal.so and bind its functions into the .gdal namespace.
/ Set .gdal.lib before loading this file to use another path (default: ./qgdal).

.gdal.lib:@[value;`.gdal.lib;`:./qgdal];

.gdal.version:  .gdal.lib 2:(`qgdal_version;1);
.gdal.layers:   .gdal.lib 2:(`qgdal_layers;1);
.gdal.info:     .gdal.lib 2:(`qgdal_info;2);
.gdal.read:     .gdal.lib 2:(`qgdal_read;2);
.gdal.write:    .gdal.lib 2:(`qgdal_write;4);
.gdal.transform:.gdal.lib 2:(`qgdal_transform;3);
.gdal.rinfo:    .gdal.lib 2:(`qgdal_rinfo;1);
.gdal.rread:    .gdal.lib 2:(`qgdal_rread;2);
.gdal.rsample:  .gdal.lib 2:(`qgdal_rsample;3);
