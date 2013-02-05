XCODE_PATH = $(shell xcode-select --print-path)
IOS_CC = $(XCODE_PATH)/Platforms/iPhoneOS.platform/Developer/usr/bin/gcc
SDK_PATH = $(shell find $(XCODE_PATH)/Platforms/iPhoneOS.platform -name "iPhoneOS*sdk" | sort -rn | head -1)

all: demo.app fruitstrap

demo.app: demo Info.plist
	mkdir -p demo.app
	cp demo demo.app/
	cp Info.plist ResourceRules.plist demo.app/
	codesign -f -s "iPhone Developer" --entitlements Entitlements.plist demo.app

demo: demo.c
	echo $(SDK_PATH)
	$(IOS_CC) -arch armv7 -isysroot $(SDK_PATH) -framework CoreFoundation -o demo demo.c

fruitstrap: fruitstrap.c
	gcc -o fruitstrap -framework CoreFoundation -framework MobileDevice -F/System/Library/PrivateFrameworks fruitstrap.c

install: all
	./fruitstrap -b demo.app

debug: all
	./fruitstrap -d -b demo.app

clean:
	rm -rf *.app demo fruitstrap