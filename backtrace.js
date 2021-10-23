#!/bin/env node
var spawn = require('child_process').execFileSync;
var fs = require('fs');

if (process.argv.length < 3) {
    console.log(`Usage: node ${process.argv[1]} <backtrace-string>`);
    process.exit(1);
}
let btString = process.argv[2];
bt = btString.split(" ");
if (bt[0] === "Backtrace:") {
    bt.shift();
}
let elfs = fs.readdirSync('./build/').filter(fn => fn.endsWith('.elf'));
if (elfs.length < 1) {
    console.error("No .elf file found in ./build directory");
    process.exit(1);
}
if (elfs.length > 1) {
    console.error("More than one .elf files found in ./build directory:", JSON.stringify(elfs));
    process.exit(1);
}

console.log(`======================== Backtrace (./build/${elfs[0]}) =============================`);
for (let line of bt) {
    spawn("xtensa-esp32-elf-addr2line", ["-pfiaC", "-e", "build/" + elfs[0], line], {'stdio': 'inherit'});
}

