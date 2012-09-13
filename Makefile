IOS_CC = xcrun -sdk iphoneos clang

all: demo.app fruitstrap

demo.app: demo Info.plist
	mkdir -p demo.app
	cp demo demo.app/
	cp Info.plist ResourceRules.plist demo.app/
	codesign -f -s "iPhone Developer" --entitlements Entitlements.plist demo.app

demo: demo.c
	$(IOS_CC) -isysroot `xcode-select -print-path`/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS5.1.sdk -arch armv7 -framework CoreFoundation -o demo demo.c

fruitstrap: fruitstrap.c
	clang -o fruitstrap -framework CoreFoundation -framework MobileDevice -F/System/Library/PrivateFrameworks fruitstrap.c

install: all
	./fruitstrap demo.app

debug: all
	./fruitstrap -d demo.app

clean:
	rm -rf *.app demo fruitstrap
