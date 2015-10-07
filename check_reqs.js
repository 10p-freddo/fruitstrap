#!/usr/bin/env node

var shell = require('shelljs');
var util = require('util');
var os = require('os');

var XCODEBUILD_MIN_VERSION = '6.4';
var XCODEBUILD_NOT_FOUND_MESSAGE =
    'Please install Xcode version ' + XCODEBUILD_MIN_VERSION + ' or greater from App Store';
	
var tool = 'xcodebuild'
var tool_command = shell.which(tool);
if (!tool_command) {
    console.log(tool + ' : ' + XCODEBUILD_NOT_FOUND_MESSAGE);
	process.exit(1);
}

shell.exec(tool + ' -version', { silent: true }, function(code, output) {
	if (code === 0) {
		var arr = output.match(/^Xcode (\d+\.\d+)/);
		var ver = arr[1];

		if (os.release() >= '15.0.0' && ver < '7.0') {
			console.log(util.format('You need at least Xcode 7.0 on OS X 10.11 El Capitan (you have version %s)', ver));
			process.exit(1);
		}
		
		if (ver < XCODEBUILD_MIN_VERSION) {
		    console.log(util.format('%s : %s. (you have version %s)', tool, XCODEBUILD_NOT_FOUND_MESSAGE, ver));
		}
	} 
});