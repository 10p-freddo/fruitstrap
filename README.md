ios-deploy
==========
Install and debug iPhone apps without using Xcode. Designed to work on unjailbroken devices.

## Requirements

* Mac OS X. Tested on Snow Leopard only.
* You need to have a valid iPhone development certificate installed.
* Xcode must be installed, along with the SDK for your iOS version.

## Usage

	Usage: ./ios-deploy [OPTION]...
        -d, --debug                  launch the app in GDB after installation\n"
        -i, --id <device_id>         the id of the device to connect to\n"
        -c, --detect                 only detect if the device is connected\n"
        -b, --bundle <bundle.app>    the path to the app bundle to be installed\n"
        -a, --args <args>            command line arguments to pass to the app when launching it\n"
        -t, --timeout <timeout>      number of seconds to wait for a device to be connected\n"
        -u, --unbuffered             don't buffer stdout\n"
        -g, --gdbargs <args>         extra arguments to pass to GDB when starting the debugger\n"
        -x, --gdbexec <file>         GDB commands script file\n"
        -n, --nostart                do not start the app when debugging\n"
        -I, --noninteractive         start in non interactive mode (quit when app crashes or exits)\n"
        -L, --justlaunch             just launch the app and exit lldb\n"
        -v, --verbose                enable verbose output\n"
        -m, --noinstall              directly start debugging without app install (-d not required)\n"
        -p, --port <number>          port used for device, default: 12345 \n"
        -r, --uninstall              uninstall the app before install (do not use with -m; app cache and data are cleared) \n"
        -1, --bundle_id <bundle id>  specify bundle id for list and upload\n"
        -l, --list                   list files\n"
        -o, --upload <file>          upload file\n"
        -w, --download               download app tree\n"
        -2, --to <target pathname>   use together with up/download file/tree. specify target\n"
        -V, --version                print the executable version \n",
	  
## Demo

* The included demo.app represents the minimum required to get code running on iOS.
* `make install` will install demo.app to the device.
* `make debug` will install demo.app and launch a GDB session.

## Notes

* With some modifications, it may be possible to use this without Xcode installed; however, you would need a copy of the relevant DeveloperDiskImage.dmg (included with Xcode). GDB would also run slower as symbols would be downloaded from the device on-the-fly.


## Listing Device Ids

Device Ids are the UDIDs of the iOS devices. From the command line, you can list device ids [this way](http://javierhz.blogspot.com/2012/06/how-to-get-udid-of-iphone-using-shell.html):

        system_profiler SPUSBDataType | sed -n -e '/iPod/,/Serial/p' | sed -n -e '/iPad/,/Serial/p' -e '/iPhone/,/Serial/p' | grep "Serial Number:" | awk -F ": " '{print $2}'
