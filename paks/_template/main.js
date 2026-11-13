// UniFrog app pak entry script (standalone JS2300 mode).
//
// A pak is one folder under /unifrog_data/apps with an app.ini beside this
// file, so adding a tool is editing files on the SD card, not rebuilding the
// firmware. See docs/app-paks.md for the full API list.
//
// Standalone scripts draw with the JS2300.video helpers and read input with
// JS2300.input.poll(); short tools that only need to report a result can log,
// write a file, and raise a toast instead, like this skeleton does.

var REPORT_PATH = "/media/mmcblk0/unifrog_data/logs/reports/my-tool.txt";

function main() {
  var started = JS2300.now();
  var report;

  report = "My Tool\n";
  report += "ms=" + (JS2300.now() - started) + "\n";

  try {
    JS2300.fs.writeText(REPORT_PATH, report);
  } catch (writeErr) {
    JS2300.log("my-tool write failed");
  }
  try {
    JS2300.system.action("toast:My Tool done");
  } catch (toastErr) {}
  JS2300.log("my-tool done");
  return 0;
}

main();
