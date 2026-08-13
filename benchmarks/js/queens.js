let count = 0;
function absn(n) { return n < 0 ? -n : n; }
function ok(c, prev, dist) { return c !== prev && absn(c - prev) !== dist; }
function q8(c0,c1,c2,c3,c4,c5,c6){let c=0;while(c<8){if(ok(c,c6,1)&&ok(c,c5,2)&&ok(c,c4,3)&&ok(c,c3,4)&&ok(c,c2,5)&&ok(c,c1,6)&&ok(c,c0,7))count+=1;c+=1;}}
function q7(c0,c1,c2,c3,c4,c5){let c=0;while(c<8){if(ok(c,c5,1)&&ok(c,c4,2)&&ok(c,c3,3)&&ok(c,c2,4)&&ok(c,c1,5)&&ok(c,c0,6))q8(c0,c1,c2,c3,c4,c5,c);c+=1;}}
function q6(c0,c1,c2,c3,c4){let c=0;while(c<8){if(ok(c,c4,1)&&ok(c,c3,2)&&ok(c,c2,3)&&ok(c,c1,4)&&ok(c,c0,5))q7(c0,c1,c2,c3,c4,c);c+=1;}}
function q5(c0,c1,c2,c3){let c=0;while(c<8){if(ok(c,c3,1)&&ok(c,c2,2)&&ok(c,c1,3)&&ok(c,c0,4))q6(c0,c1,c2,c3,c);c+=1;}}
function q4(c0,c1,c2){let c=0;while(c<8){if(ok(c,c2,1)&&ok(c,c1,2)&&ok(c,c0,3))q5(c0,c1,c2,c);c+=1;}}
function q3(c0,c1){let c=0;while(c<8){if(ok(c,c1,1)&&ok(c,c0,2))q4(c0,c1,c);c+=1;}}
function q2(c0){let c=0;while(c<8){if(ok(c,c0,1))q3(c0,c);c+=1;}}
function q1(){let c=0;while(c<8){q2(c);c+=1;}}
const start = process.hrtime.bigint();
q1();
const elapsed = Number(process.hrtime.bigint() - start) / 1e9;
console.log(count);
console.log(elapsed);
