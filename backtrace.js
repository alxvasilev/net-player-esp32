#!/bin/env node
var spawn = require('child_process').execFileSync;
var fs = require('fs');

if (process.argv.length < 3) {
    console.log(`Usage: node ${process.argv[1]} <stack-addr1> <stack-add2>....`);
    process.exit(1);
}
let addresses = [];
for (let i = 2; i < process.argv.length; i++) {
    let str = process.argv[i];
    let addrs = str.split(" ");
    for (addr of addrs) {
        if (!addr.startsWith("0x")) {
            if (addr.toLowerCase().startsWith("backtrace") && !addresses.length) {
                continue;
            }
            console.error(`Invalid address "${addr}"`);
            process.exit(1);
        }
        addresses.push(addr);
    }
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
let elfFile = elfs[0];
console.log(`================ Backtrace (./build/${elfFile}) ================`);
for (let addr of addresses) {
    spawn("xtensa-esp32-elf-addr2line", ["-pfiaC", "-e", "build/" + elfFile, addr], {'stdio': 'inherit'});
}
