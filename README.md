ios-deploy
==========
Install and debug iOS apps without using Xcode. Designed to work on un-jailbroken devices.

## Requirements

* Mac OS X. Tested on 10.10 Yosemite and iOS 8.1
* You need to have a valid iOS development certificate installed.
* Xcode 6.1 should be installed

## Usage

    Usage: ios-deploy [OPTION]...
      -d, --debug                  launch the app in GDB after installation
      -i, --id <device_id>         the id of the device to connect to
      -c, --detect                 only detect if the device is connected
      -b, --bundle <bundle.app>    the path to the app bundle to be installed
      -a, --args <args>            command line arguments to pass to the app when launching it
      -t, --timeout <timeout>      number of seconds to wait for a device to be connected
      -u, --unbuffered             don't buffer stdout
      -n, --nostart                do not start the app when debugging
      -I, --noninteractive         start in non interactive mode (quit when app crashes or exits)
      -L, --justlaunch             just launch the app and exit lldb
      -v, --verbose                enable verbose output
      -m, --noinstall              directly start debugging without app install (-d not required)
      -p, --port <number>          port used for device, default: 12345 
      -r, --uninstall              uninstall the app before install (do not use with -m; app cache and data are cleared) 
      -1, --bundle_id <bundle id>  specify bundle id for list and upload
      -l, --list                   list files
      -o, --upload <file>          upload file
      -w, --download               download app tree
      -2, --to <target pathname>   use together with up/download file/tree. specify target
      -V, --version                print the executable version 

## Examples

The commands below assume that you have an app called `my.app` with bundle id `bundle.id`. Substitute where necessary.

    // deploy and debug your app to a connected device
    ios-deploy --debug --bundle my.app

    // deploy and launch your app to a connected device, but quit the debugger after
    ios-deploy --justlaunch --debug --bundle my.app

    // deploy and launch your app to a connected device, quit when app crashes or exits
    ios-deploy --noninteractive --debug --bundle my.app

    // Upload a file to your app's Documents folder
    ios-deploy --bundle_id 'bundle.id' --upload test.txt --to Documents/test.txt
    
    // Download your app's Documents, Library and tmp folders
    ios-deploy --bundle_id 'bundle.id' --download --to MyDestinationFolder

    // List the contents of your app's Documents, Library and tmp folders
    ios-deploy --bundle_id 'bundle.id' --list

    // deploy and debug your app to a connected device, uninstall the app first
    ios-deploy --uninstall --debug --bundle my.app
    
## Demo

* The included demo.app represents the minimum required to get code running on iOS.
* `make install` will install demo.app to the device.
* `make debug` will install demo.app and launch a GDB session.

## Notes

* With some modifications, it may be possible to use this without Xcode installed; however, you would need a copy of the relevant DeveloperDiskImage.dmg (included with Xcode). lldb would also run slower as symbols would be downloaded from the device on-the-fly.


## Listing Device Ids

Device Ids are the UDIDs of the iOS devices. From the command line, you can list device ids [this way](http://javierhz.blogspot.com/2012/06/how-to-get-udid-of-iphone-using-shell.html):

        system_profiler SPUSBDataType | sed -n -e '/iPod/,/Serial/p' | sed -n -e '/iPad/,/Serial/p' -e '/iPhone/,/Serial/p' | grep "Serial Number:" | awk -F ": " '{print $2}'
