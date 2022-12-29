const nodencl = require('./index.js');
let platforms = nodencl.getPlatformInfo();
console.log(JSON.stringify(platforms, null, 2));
