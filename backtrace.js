var spawn = require('child_process').execFileSync;

let btString = process.argv[3];
bt = btString.split(" ");
if (bt[0] === "Backtrace:") {
    bt.shift();
}
console.log("======================== Backtrace =============================");
for (let line of bt) {
    spawn("xtensa-esp32-elf-addr2line", ["-pfiaC", "-e", "build/" + process.argv[2], line], {'stdio': 'inherit'});
}


