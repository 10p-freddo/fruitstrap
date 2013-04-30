IOS_CC = clang
IOS_SDK = /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS6.1.sdk

all: demo.app ios-deploy

demo.app: demo Info.plist
	mkdir -p demo.app
	cp demo demo.app/
	cp Info.plist ResourceRules.plist demo.app/
	codesign -f -s "iPhone Developer" --entitlements Entitlements.plist demo.app

demo: demo.c
	$(IOS_CC) -arch armv7 -isysroot $(IOS_SDK) -framework CoreFoundation -o demo demo.c

ios-deploy: ios-deploy.c
	gcc -o ios-deploy -framework CoreFoundation -framework MobileDevice -F/System/Library/PrivateFrameworks ios-deploy.c

install: all
	./ios-deploy demo.app

debug: all
	./ios-deploy -d demo.app

clean:
	rm -rf *.app demo ios-deploy