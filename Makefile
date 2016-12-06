#IOS_CC = /Developer/Platforms/iPhoneOS.platform/Developer/usr/bin/gcc
IOS_CC= /Applications/Xcode.app//Contents/Developer/Platforms/iPhoneOS.platform/Developer/usr/bin/gcc


all: demo.app fruitstrap

demo.app: demo Info.plist
	mkdir -p demo.app
	cp demo demo.app/
	cp Info.plist ResourceRules.plist demo.app/
	codesign -f -s "iPhone Developer: Miguel de Icaza (QY8A78Y78J)" --entitlements Entitlements.plist demo.app

demo: demo.c
	$(IOS_CC) -arch armv7 -isysroot /Applications/Xcode.app//Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS5.0.sdk -framework CoreFoundation -o demo demo.c
#/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS5.0.sdk -framework CoreFoundation -o demo demo.c

fruitstrap: fruitstrap.c
	gcc -o fruitstrap -framework CoreFoundation -framework MobileDevice -F/System/Library/PrivateFrameworks fruitstrap.c

install: all
	./fruitstrap demo.app

debug: all
	./fruitstrap -d demo.app

clean:
	rm -rf *.app demo fruitstrap
