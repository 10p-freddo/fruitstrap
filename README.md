ios-deploy
==========
Install and debug iOS apps without using Xcode. Designed to work on un-jailbroken devices.

## Requirements

* Mac OS X. Tested on 10.11 El Capitan and iOS 9.0
* You need to have a valid iOS Development certificate installed.
* Xcode 6.4 or greater should be installed

## Roadmap

See our [milestones](https://github.com/phonegap/ios-deploy/milestones).

Significant changes: 

    1.8.0 will use an Xcode project instead of a Makefile (to prepare for 2.0.0)
    2.0.0 will break out the commands into their own files, and create ios-deploy-lib for node.js use

## Installation
=======

ios-deploy installation is made simple using the node.js package manager.  If you use [Homebrew](http://brew.sh/), install [node.js](https://nodejs.org):

```
brew install node
```

Now install ios-deploy with the [node.js](https://nodejs.org) package manager:

```
npm install -g ios-deploy
```

To build from source:

```
xcodebuild
```

This will build `ios-deploy` into the `build/Release` folder.


### OS X 10.11 El Capitan

If you are *not* using a node version manager like [nvm](https://github.com/creationix/nvm) or [n](https://github.com/tj/n), you may have to do either of these two things below when under El Capitan:

1. Add the `--unsafe-perm` flag  when installing ios-deploy
2. Ensure the `nobody` user has write access to `/usr/local/lib/node_modules/ios-deploy/ios-deploy`

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
      -9, --uninstall_only         uninstall the app ONLY. Use only with -1 <bundle_id> 
      -1, --bundle_id <bundle id>  specify bundle id for list, upload, and uninstall_only
      -l, --list                   list files
      -o, --upload <file>          upload file
      -w, --download               download app tree
      -2, --to <target pathname>   use together with up/download file/tree. specify target
      -D, --mkdir <dir>            make directory on device
      -R, --rm <path>              remove file or directory on device (directories must be empty)
      -V, --version                print the executable version
      -e, --exists                 check if the app with given bundle_id is installed or not
      -B, --list_bundle_id         list bundle_id

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

    // check whether an app by bundle id exists on the device (check return code `echo $?`)
    ios-deploy --exists --bundle_id com.apple.mobilemail

    // Download the Documents directory of the app *only*
    ios-deploy --download=/Documents --bundle_id my.app.id --to ./my_download_location
    
    // List ids and names of connected devices
    ios-deploy -c
    
    // Uninstall an app
    ios-deploy --uninstall_only --bundle_id my.bundle.id
    
    // list all bundle ids of all apps on your device
    ios-deploy --list_bundle_id

## Demo

The included demo.app represents the minimum required to get code running on iOS.

* `make demo.app` will generate the demo.app executable. If it doesn't compile, modify `IOS_SDK_VERSION` in the Makefile.
* `make debug` will install demo.app and launch a LLDB session.

## Notes

* With some modifications, it may be possible to use this without Xcode installed; however, you would need a copy of the relevant DeveloperDiskImage.dmg (included with Xcode). lldb would also run slower as symbols would be downloaded from the device on-the-fly.
