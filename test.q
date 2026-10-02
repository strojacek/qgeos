/ test.q — run with:  q test.q
\l geos.q

pass:0; fail:0;
chk:{[name;got;want] $[got~want; pass+:1; [fail+:1; -1 "FAIL ",name,": got ",(-3!got)," want ",-3!want]]};

sq:.geos.fromwkt "POLYGON((0 0,10 0,10 10,0 10,0 0))";
pts:.geos.fromwkt ("POINT(5 5)";"POINT(15 5)";"POINT(0 0)";"POINT(1 9)");

/ round trip
chk["wkt round trip"; .geos.towkt .geos.fromwkt "POINT(1 2)"; "POINT (1 2)"];
chk["wkt list"; .geos.towkt pts 0 1; ("POINT (5 5)";"POINT (15 5)")];
chk["point atom"; .geos.towkt .geos.point[3.0;4.0]; "POINT (3 4)"];
chk["point vec"; .geos.towkt .geos.point[1 2f;3 4f]; ("POINT (1 3)";"POINT (2 4)")];
chk["point long broadcast"; .geos.towkt .geos.point[1 2;0]; ("POINT (1 0)";"POINT (2 0)")];

/ measures
chk["area"; .geos.area sq; 100f];
chk["length"; .geos.length sq; 40f];
chk["area list"; .geos.area (sq;pts 0); 100 0f];
chk["centroid"; .geos.towkt .geos.centroid sq; "POINT (5 5)"];
chk["envelope"; .geos.area .geos.envelope .geos.buffer[pts 0;1]; 4f];
chk["buffer area ~pi"; 0.05>abs 3.14159-.geos.area .geos.buffer[pts 0;1f]; 1b];

/ predicates: atom/list in both directions (prepared paths) and list/list
chk["contains atom-list"; .geos.contains[sq;pts]; 1001b];
chk["within list-atom";   .geos.within[pts;sq]; 1001b];
chk["intersects (boundary counts)"; .geos.intersects[sq;pts]; 1011b];
chk["within list-list"; .geos.within[pts;(sq;sq;sq;sq)]; 1001b];
chk["atom-atom"; .geos.contains[sq;pts 0]; 1b];

/ distance
chk["distance atom"; .geos.distance[pts 0;pts 1]; 10f];
chk["distance list"; .geos.distance[pts;sq]; 0 5 0 0f];

/ nulls: empty byte vector is a null geometry
nul:`byte$();
chk["null area"; null .geos.area nul; 1b];
chk["null pred"; .geos.contains[sq;(pts 0;nul)]; 10b];
chk["null wkt"; .geos.towkt nul; ""];

/ errors
chk["bad wkt"; @[.geos.fromwkt;"POINT(oops";{`err}]; `err];
chk["bad wkb"; @[.geos.area;0x0102;{`err}]; `err];
chk["length mismatch"; @[.geos.within[pts];(sq;sq);{x}]; "length"];
chk["type"; @[.geos.area;42;{x}]; "type"];

/ in a table
t:([] id:til 4; geom:pts);
chk["select where"; exec id from t where .geos.within[geom;sq]; 0 3];
chk["update"; exec d from update d:.geos.distance[geom;pts 0] from t; 0 10f,sqrt 50 32f];

/ spatial join: small exact case (two overlapping squares, points on the diagonal)
sq2:.geos.fromwkt ("POLYGON((0 0,10 0,10 10,0 10,0 0))";"POLYGON((5 5,15 5,15 15,5 15,5 5))");
dp:.geos.point[1 7 12 20f;1 7 12 20f];
chk["sjoin within"; .geos.sjoin[dp;sq2;`within]; (0 1 1 2;0 0 1 1)];
chk["sjoin contains"; .geos.sjoin[sq2;dp;`contains]; (0 0 1 1;0 1 1 2)];
chk["sjoin no hits"; .geos.sjoin[dp 3;sq2;`intersects]; (`long$();`long$())];
chk["nearest"; .geos.nearest[dp;sq2]; 0 0 1 1];
chk["nearest atom"; .geos.nearest[dp 3;dp 0 1 2]; 2];

/ spatial join vs brute force on random data
n:2000; m:50;
rp:.geos.point[n?100f;n?100f];
rc:{.geos.buffer[x;y]}'[.geos.point[m?100f;m?100f];1+m?10f];
/ order pairs by (li;ri) with one unique key, so this doesn't rely on iasc being stable
srt:{i:iasc x[1]+x[0]*1+max 0,x 1; (x[0;i];x[1;i])};
bw:{[f;l;r] p:{[f;l;r;j] w:where f[l;r j]; (w;count[w]#j)}[f;l;r] each til count r; srt (raze p[;0];raze p[;1])};
chk["sjoin within = brute"; .geos.sjoin[rp;rc;`within]; bw[.geos.within;rp;rc]];
chk["sjoin intersects = brute"; .geos.sjoin[rc;rc;`intersects]; bw[.geos.intersects;rc;rc]];
chk["sjoin contains = brute"; .geos.sjoin[rc;rp;`contains]; bw[.geos.contains;rc;rp]];
nn:.geos.nearest[rp;rc];
chk["nearest = brute (by distance)"; .geos.distance[rp;rc nn]; min each .geos.distance[;rc] each rp];

/ spatial join nulls and errors
chk["sjoin nulls skipped"; .geos.sjoin[(dp 0;nul;dp 1);(nul;sq2 0);`within]; (0 2;1 1)];
chk["nearest null"; .geos.nearest[(nul;dp 0);sq2]; 0N 0];
chk["nearest empty right"; .geos.nearest[dp;enlist nul]; 4#0N];
/ a bad blob mid-list must raise, not return a partial result (krr returns NULL)
chk["bad wkb in prepared pred"; @[.geos.within[(pts 0;0x0102)];sq;{x}] like "geos: *"; 1b];
chk["bad wkb in distance list"; @[.geos.distance[(pts 0;0x0102)];sq;{x}] like "geos: *"; 1b];
chk["bad wkb in sjoin"; @[.geos.sjoin[(pts 0;0x0102);sq2];`within;{x}] like "geos: *"; 1b];
chk["sjoin bad pred"; @[.geos.sjoin[dp;sq2];`touches;{x}]; "domain"];

-1 "qgeos ",.geos.version[]," — ",string[pass]," passed, ",string[fail]," failed";
exit $[fail;1;0]
