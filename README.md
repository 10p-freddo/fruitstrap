fruitstrap
==========
Install and debug iPhone apps without using Xcode. Designed to work on unjailbroken devices.

## Requirements

* Mac OS X. Tested on Lion/Mountain Lion.
* You need to have a valid iPhone development certificate installed (or at least a correctly signed iOS app).
* Xcode must be installed, along with the SDK for your iOS version.

## Usage

* `fruitstrap [-d/--debug] [-i/--id device_id] -b/--bundle <app> [-a/--args arguments] [-t/--timeout timeout(seconds)] [-u/--unbuffered] [-g/--gdbargs gdb_arguments]`
* Optional `-d` flag launches a remote GDB session after the app has been installed.
* `<app>` must be an iPhone application bundle, *not* an IPA.
* Optional device id, useful when you have more than one iPhone/iPad connected to your computer
* `<arguments>` are passed as argv to the running app.
* `<gdb_arguments>` are passed to gdb.

## Demo

* The included demo.app represents the minimum required to get code running on iOS.
* `make install` will install demo.app to the device.
* `make debug` will install demo.app and launch a GDB session.

## Notes

* With some modifications, it may be possible to use this without Xcode installed; however, you would need a copy of the relevant DeveloperDiskImage.dmg (included with Xcode). GDB would also run slower as symbols would be downloaded from the device on-the-fly.
