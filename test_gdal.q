/ test_gdal.q — run with:  q test_gdal.q   (from the qgeos folder)
\l gdal.q
\l geos.q

pass:0; fail:0;
chk:{[name;got;want] $[got~want; pass+:1; [fail+:1; -1 "FAIL ",name,": got ",(-3!got)," want ",-3!want]]};
near:{[name;got;want;tol] chk[name; all tol>abs got-want; 1b]};
err:{[f;x] @[f;x;{x}]};
tmp:"/tmp/qgdal_test_",string .z.i;
system "rm -rf ",tmp," && mkdir -p ",tmp;
out:{hsym `$tmp,"/",x};

/ ---------- read ----------
p:.gdal.read[`:fixtures/places.geojson;`];
chk["layers"; .gdal.layers `:fixtures/places.geojson; enlist `places];
chk["read string path"; .gdal.read["fixtures/places.geojson";`places]; p];
chk["read by index"; .gdal.read[`:fixtures/places.geojson;0]; p];
chk["columns"; cols p; `name`pop`big`score`open`founded`seen`geom];
chk["types"; exec t from meta delete geom from p; "Cijfbdp"];
chk["strings"; p`name; ("Bryan";"College Station";"Nowhere")];
chk["int + null"; p`pop; 86000 120000 0Ni];
chk["int64 + null"; p`big; 5000000000 6000000000 0N];
chk["real + null"; p`score; 1.5 2.25 0n];
chk["bool (null->0b)"; p`open; 100b];
chk["date + null"; p`founded; (1855.01.01;1938.10.19;0Nd)];
chk["datetime as UTC timestamp"; p`seen; (2024.01.15D10:30:00;2024.02.01D00:00:00;0Np)];
chk["geometry"; .geos.towkt p`geom; ("POINT (-96.37 30.67)";"POINT (-96.33 30.63)";"")];

i:.gdal.info[`:fixtures/places.geojson;`];
chk["info count"; i`count; 3];
chk["info crs"; i`crs; "EPSG:4326"];
chk["info geomtype"; i`geomtype; `Point];
chk["info extent"; i`extent; -96.37 30.63 -96.33 30.67];
chk["info fields"; exec kind from i`fields; `String`Integer`Integer64`Real`Boolean`Date`DateTime];

/ ---------- write -> read round trips ----------
/ (date/timestamp lists are built with ( ; ; ) because peachq can't parse 2024.01.01 0Nd literals yet)
t:([] id:1 2 3; name:(enlist "a";"bb";"");  kind:`x`y`z; v:1.5 0n 3.25; n:10 0N 30;
      d:(2024.01.01;0Nd;1999.12.31); ts:(2024.03.04D05:06:07;0Np;2000.01.01D00:00:00);
      ok:101b; geom:.geos.fromwkt ("POINT(1 2)";"LINESTRING(0 0,1 1)";"POLYGON((0 0,1 0,1 1,0 0))"));
back:{[f] .gdal.read[out f;`]};

chk["write gpkg count"; .gdal.write[out "t.gpkg";`things;t;"EPSG:4326"]; 3];
g:back "t.gpkg";
chk["gpkg layer name"; .gdal.layers out "t.gpkg"; enlist `things];
chk["gpkg crs"; .gdal.info[out "t.gpkg";`]`crs; "EPSG:4326"];
chk["gpkg values"; delete geom, kind from g; delete geom, kind from t];
chk["gpkg symbols come back as strings"; g`kind; string `x`y`z];
chk["gpkg geometry"; .geos.towkt g`geom; .geos.towkt t`geom];

chk["write fgb"; .gdal.write[out "t.fgb";`;t;"EPSG:4326"]; 3];
chk["fgb geometry"; .geos.towkt (back "t.fgb")`geom; .geos.towkt t`geom];
chk["fgb numbers"; (back "t.fgb")`v`n; t`v`n];

chk["write geojson"; .gdal.write[out "t.geojson";`;t;""]; 3];
chk["geojson values"; (back "t.geojson")`name`v`d; t`name`v`d];

pts:select from t where id=1;
chk["write shp (points only)"; .gdal.write[out "t.shp";`;pts;"EPSG:32614"]; 1];
chk["shp geometry"; .geos.towkt (back "t.shp")`geom; enlist "POINT (1 2)"];
chk["shp crs"; .gdal.info[out "t.shp";`]`crs; "EPSG:32614"];

chk["write without geometry (csv)"; .gdal.write[out "t.csv";`;select id, name from t;""]; 3];
chk["csv read"; (back "t.csv")`name; t`name];

/ ---------- CRS ----------
cs:.gdal.transform[p`geom;"EPSG:4326";"EPSG:3857"];
xy:{"F"$" " vs -1_7_.geos.towkt x};
near["to web mercator (gdaltransform reference)"; xy cs 1; -10723406.548116 3584790.25187363; 1e-6];
near["to UTM 14N (cs2cs reference)"; xy .gdal.transform[p[`geom]1;"EPSG:4326";"EPSG:32614"]; 755918.088401 3391637.257487; 1e-6];
near["round trip"; xy .gdal.transform[cs 1;"EPSG:3857";"EPSG:4326"]; -96.33 30.63; 1e-9];
chk["null passes through"; cs 2; `byte$()];
chk["proj string accepted"; type .gdal.transform[p[`geom]0;"+proj=longlat +datum=WGS84";"EPSG:3857"]; 4h];

/ ---------- raster ----------
r:.gdal.rinfo `:fixtures/grid.asc;
chk["rinfo size"; r`width`height`bands; 4 3 1];
chk["rinfo transform"; r`transform; 0 10 0 30 0 -10f];
chk["rinfo nodata"; r`nodata; enlist -9999f];
chk["rread"; .gdal.rread[`:fixtures/grid.asc;1]; 1 2 3 4 5 6 0n 8 9 10 11 12f];
chk["rsample"; .gdal.rsample[`:fixtures/grid.asc;1;.geos.point[5 15 25 35 45 5f;25 25 15 5 5 -5f]]; 1 2 0n 12 0n 0nf];
chk["rsample atom"; .gdal.rsample[`:fixtures/grid.asc;1;.geos.point[5f;5f]]; 9f];
system "gdal_translate -q -a_srs EPSG:32614 fixtures/grid.asc ",tmp,"/grid.tif";
chk["geotiff crs"; .gdal.rinfo[out "grid.tif"]`crs; "EPSG:32614"];
chk["geotiff read"; .gdal.rread[out "grid.tif";1]; .gdal.rread[`:fixtures/grid.asc;1]];

/ ---------- with qgeos ----------
z:.gdal.read[`:fixtures/zones.geojson;`];
j:.geos.sjoin[p`geom;z`geom;`within];
chk["sjoin on loaded files"; ([] place:p[`name] j 0; zone:z[`zone] j 1); ([] place:("Bryan";"College Station"); zone:("north";"south"))];
utm:.gdal.transform[z`geom;"EPSG:4326";"EPSG:32614"];
near["area after reprojection (km2)"; .geos.area[utm]%1e6; 477.5 477.5; 3];

/ ---------- errors ----------
chk["missing file"; (err[.gdal.layers;`:nope.gpkg]) like "gdal: open*"; 1b];
chk["bad layer"; err[.gdal.read[`:fixtures/places.geojson];`nope]; "layer"];
chk["bad crs"; (err[.gdal.transform[p`geom;"EPSG:999999"];"EPSG:4326"]) like "gdal: transform: bad crs*"; 1b];
chk["refuses overwrite"; err[.gdal.write[out "t.gpkg";`;t];""]; "gdal: write: file exists"];
chk["unknown extension"; (err[.gdal.write[out "t.xyz";`;t];""]) like "gdal: write: unknown extension*"; 1b];
chk["bad band"; err[.gdal.rread[`:fixtures/grid.asc];2]; "band"];
chk["unsupported column type"; err[.gdal.write[out "u.gpkg";`;([] a:(1 2;3 4))];""]; "gdal: write: unsupported column type"];
chk["failed write leaves no file"; () ~ key out "u.gpkg"; 1b];

system "rm -rf ",tmp;
-1 "qgdal ",.gdal.version[]," — ",string[pass]," passed, ",string[fail]," failed";
exit $[fail;1;0]
