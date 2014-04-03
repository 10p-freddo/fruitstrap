IOS_SDK_VERSION = 7.1

IOS_CC = gcc -ObjC
DEVICE_SUPPORT = $(shell xcode-select --print-path)/Platforms/iPhoneOS.platform/DeviceSupport
IOS_SDK = $(shell xcode-select --print-path)/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS$(IOS_SDK_VERSION).sdk

all: clean ios-deploy

demo.app: demo Info.plist
	mkdir -p demo.app
	cp demo demo.app/
	cp Info.plist ResourceRules.plist demo.app/
	codesign -f -s "iPhone Developer" --entitlements Entitlements.plist demo.app

demo: demo.c
	$(IOS_CC) -g -arch armv7 -isysroot $(IOS_SDK) -framework CoreFoundation -o demo demo.c

ios-deploy: clean ios-deploy.c
	$(IOS_CC) -g -o ios-deploy -framework Foundation -framework CoreFoundation -framework MobileDevice -F/System/Library/PrivateFrameworks ios-deploy.c

symlink: 
	cd $(DEVICE_SUPPORT); ln -sfn "`find . -type d -maxdepth 1 -exec basename {} \; | tail -1`" Latest	
    
install: symlink ios-deploy
	mkdir -p $(prefix)/bin
	cp ios-deploy $(prefix)/bin

uninstall:
	rm $(prefix)/bin/ios-deploy

debug: all
	./ios-deploy --debug --bundle demo.app

clean:
	rm -rf *.app demo ios-deploy
