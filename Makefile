IOS_CC = xcrun -sdk iphoneos clang
IOS_MIN_OS = 5.1
IOS_SDK = 6.0
OSX_MIN = 10.7

CERT="iPhone Developer"

all: demo.app fruitstrap

demo.app: demo Info.plist
	mkdir -p demo.app
	cp demo demo.app/
	cp Info.plist ResourceRules.plist demo.app/
	codesign -f -s $(CERT) --entitlements Entitlements.plist demo.app

demo: demo.c
	$(IOS_CC) -isysroot `xcode-select -print-path`/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS$(IOS_SDK).sdk -mios-version-min=$(IOS_MIN_OS) -arch armv7 -framework CoreFoundation -o demo demo.c

fruitstrap: fruitstrap.c
	clang -o fruitstrap -mmacosx-version-min=$(OSX_MIN) -framework CoreFoundation -framework MobileDevice -F/System/Library/PrivateFrameworks fruitstrap.c

listdevices: listdevices.c
	gcc -g -o listdevices -framework CoreFoundation -framework MobileDevice -F/System/Library/PrivateFrameworks listdevices.c

install: all
	./fruitstrap -b demo.app

install_os: fruitstrap
	sudo mkdir -p /usr/local/bin
	sudo cp fruitstrap /usr/local/bin/fruitstrap

debug: all
	./fruitstrap -d -b demo.app

clean:
	rm -rf *.app demo fruitstrap
