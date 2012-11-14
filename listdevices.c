//TODO: don't copy/mount DeveloperDiskImage.dmg if it's already done - Xcode checks this somehow

#import <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <stdio.h>
#include <signal.h>
#include <getopt.h>
#include "MobileDevice.h"

#define FDVENDOR_PATH  "/tmp/fruitstrap-remote-debugserver"
#define PREP_CMDS_PATH "/tmp/fruitstrap-gdb-prep-cmds"
#define GDB_SHELL      "`xcode-select -print-path`/Platforms/iPhoneOS.platform/Developer/usr/libexec/gdb/gdb-arm-apple-darwin --arch armv7f -i mi -q -x " PREP_CMDS_PATH

#define PRINT(...) if (!quiet) { printf(__VA_ARGS__); fflush(stdout); }

// approximation of what Xcode does:
#define GDB_PREP_CMDS CFSTR("set mi-show-protections off\n\
    set auto-raise-load-levels 1\n\
    set shlib-path-substitutions /usr \"{ds_path}/Symbols/usr\" /System \"{ds_path}/Symbols/System\" \"{device_container}\" \"{disk_container}\" \"/private{device_container}\" \"{disk_container}\" /Developer \"{ds_path}/Symbols/Developer\"\n\
    set remote max-packet-size 1024\n\
    set sharedlibrary check-uuids on\n\
    set env NSUnbufferedIO YES\n\
    set minimal-signal-handling 1\n\
    set sharedlibrary load-rules \\\".*\\\" \\\".*\\\" container\n\
    set inferior-auto-start-dyld 0\n\
    file \"{disk_app}\"\n\
    set remote executable-directory {device_app}\n\
    set remote noack-mode 1\n\
    set trust-readonly-sections 1\n\
    target remote-mobile " FDVENDOR_PATH "\n\
    mem 0x1000 0x3fffffff cache\n\
    mem 0x40000000 0xffffffff none\n\
    mem 0x00000000 0x0fff none\n\
    run {args}\n\
    set minimal-signal-handling 0\n\
    set inferior-auto-start-cfm off\n\
    set sharedLibrary load-rules dyld \".*libobjc.*\" all dyld \".*CoreFoundation.*\" all dyld \".*Foundation.*\" all dyld \".*libSystem.*\" all dyld \".*AppKit.*\" all dyld \".*PBGDBIntrospectionSupport.*\" all dyld \".*/usr/lib/dyld.*\" all dyld \".*CarbonDataFormatters.*\" all dyld \".*libauto.*\" all dyld \".*CFDataFormatters.*\" all dyld \"/System/Library/Frameworks\\\\\\\\|/System/Library/PrivateFrameworks\\\\\\\\|/usr/lib\" extern dyld \".*\" all exec \".*\" all\n\
    sharedlibrary apply-load-rules all\n\
    set inferior-auto-start-dyld 1\n\
    continue\n\
    quit")

typedef enum {
    OP_NONE,

    OP_INSTALL,
    OP_UNINSTALL,
    OP_LIST_DEVICES,
    OP_UPLOAD_FILE,
    OP_LIST_FILES

} operation_t;

typedef struct am_device * AMDeviceRef;
int AMDeviceSecureTransferPath(int zero, AMDeviceRef device, CFURLRef url, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceSecureInstallApplication(int zero, AMDeviceRef device, CFURLRef url, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceMountImage(AMDeviceRef device, CFStringRef image, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceLookupApplications(AMDeviceRef device, int zero, CFDictionaryRef* result);

bool found_device = false, debug = false, verbose = false, quiet = false;
char *app_path = NULL;
char *device_id = NULL;
char *doc_file_path = NULL;
char *target_filename = NULL;
char *bundle_id = NULL;
char *args = NULL;
int timeout = 0;
operation_t operation = OP_INSTALL;
CFStringRef last_path = NULL;
service_conn_t gdbfd;

Boolean path_exists(CFTypeRef path) {
    if (CFGetTypeID(path) == CFStringGetTypeID()) {
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, true);
        Boolean result = CFURLResourceIsReachable(url, NULL);
        CFRelease(url);
        return result;
    } else if (CFGetTypeID(path) == CFURLGetTypeID()) {
        return CFURLResourceIsReachable(path, NULL);
    } else {
        return false;
    }
}

CFStringRef copy_xcode_dev_path() {
	FILE *fpipe = NULL;
	char *command = "xcode-select -print-path";

	if (!(fpipe = (FILE *)popen(command, "r")))
	{
		perror("Error encountered while opening pipe");
		exit(EXIT_FAILURE);
	}

	char buffer[256] = { '\0' };

	fgets(buffer, sizeof(buffer), fpipe);
	pclose(fpipe);

	strtok(buffer, "\n");
	return CFStringCreateWithCString(NULL, buffer, kCFStringEncodingUTF8);
}

CFStringRef copy_device_support_path(AMDeviceRef device) {
    CFStringRef version = AMDeviceCopyValue(device, 0, CFSTR("ProductVersion"));
    CFStringRef build = AMDeviceCopyValue(device, 0, CFSTR("BuildVersion"));
    const char* home = getenv("HOME");
    CFStringRef path;
    bool found = false;

	CFStringRef xcodeDevPath = copy_xcode_dev_path();

	path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s/Library/Developer/Xcode/iOS DeviceSupport/%@ (%@)"), home, version, build);
	found = path_exists(path);

	if (!found)
	{
		path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s/Library/Developer/Xcode/iOS DeviceSupport/%@"), home, version);
		found = path_exists(path);
	}
	if (!found)
	{
		path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s/Library/Developer/Xcode/iOS DeviceSupport/Latest"), home);
		found = path_exists(path);
	}
	if (!found)
	{
		path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/Platforms/iPhoneOS.platform/DeviceSupport/%@ (%@)"), xcodeDevPath, version, build);
		found = path_exists(path);
	}
	if (!found)
	{
		path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/Platforms/iPhoneOS.platform/DeviceSupport/%@"), xcodeDevPath, version);
		found = path_exists(path);
	}
	if (!found)
	{
		path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/Platforms/iPhoneOS.platform/DeviceSupport/Latest"), xcodeDevPath);
		found = path_exists(path);
	}

	CFRelease(version);
	CFRelease(build);
	CFRelease(xcodeDevPath);

	if (!found)
	{
		PRINT("[ !! ] Unable to locate DeviceSupport directory.\n");
		CFRelease(path);
		exit(EXIT_FAILURE);
	}

	return path;
}

CFStringRef copy_developer_disk_image_path(AMDeviceRef device) {
    CFStringRef version = AMDeviceCopyValue(device, 0, CFSTR("ProductVersion"));
    CFStringRef build = AMDeviceCopyValue(device, 0, CFSTR("BuildVersion"));
    const char *home = getenv("HOME");
    CFStringRef path;
    bool found = false;

	CFStringRef xcodeDevPath = copy_xcode_dev_path();

	path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s/Library/Developer/Xcode/iOS DeviceSupport/%@ (%@)/DeveloperDiskImage.dmg"), home, version, build);
	found = path_exists(path);

	if (!found)
	{
		path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s/Library/Developer/Xcode/iOS DeviceSupport/%@/DeveloperDiskImage.dmg"), home, version);
		found = path_exists(path);
	}
	if (!found)
	{
		path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s/Library/Developer/Xcode/iOS DeviceSupport/Latest/DeveloperDiskImage.dmg"), home);
		found = path_exists(path);
	}
	if (!found)
	{
		path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/Platforms/iPhoneOS.platform/DeviceSupport/%@ (%@)/DeveloperDiskImage.dmg"), xcodeDevPath, version, build);
		found = path_exists(path);
	}
	if (!found)
	{
		path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/Platforms/iPhoneOS.platform/DeviceSupport/%@/DeveloperDiskImage.dmg"), xcodeDevPath, version);
		found = path_exists(path);
	}
	if (!found)
	{
		path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/Platforms/iPhoneOS.platform/DeviceSupport/Latest/DeveloperDiskImage.dmg"), xcodeDevPath);
		found = path_exists(path);
	}

	CFRelease(version);
	CFRelease(build);
	CFRelease(xcodeDevPath);

    if (!found)
	{
		PRINT("[ !! ] Unable to locate DeviceSupport directory containing DeveloperDiskImage.dmg.\n");

		CFIndex pathLength = CFStringGetLength(path);
		char *buffer = calloc(pathLength + 1, sizeof(char));
		Boolean success = CFStringGetCString(path, buffer, pathLength + 1, kCFStringEncodingUTF8);
		CFRelease(path);

		if (success) PRINT("[ !! ] Last path checked: %s\n", buffer);
		exit(EXIT_FAILURE);
	}

	return path;
}

void mount_callback(CFDictionaryRef dict, int arg) {
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));

    if (CFEqual(status, CFSTR("LookingUpImage"))) {
        PRINT("[  0%%] Looking up developer disk image\n");
    } else if (CFEqual(status, CFSTR("CopyingImage"))) {
        PRINT("[ 30%%] Copying DeveloperDiskImage.dmg to device\n");
    } else if (CFEqual(status, CFSTR("MountingImage"))) {
        PRINT("[ 90%%] Mounting developer disk image\n");
    }
}

void mount_developer_image(AMDeviceRef device) {
    CFStringRef ds_path = copy_device_support_path(device);
    CFStringRef image_path = copy_developer_disk_image_path(device);
    CFStringRef sig_path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@.signature"), image_path);
    CFRelease(ds_path);

    if (verbose) {
        PRINT("Device support path: ");
        fflush(stdout);
        CFShow(ds_path);
        PRINT("Developer disk image: ");
        fflush(stdout);
        CFShow(image_path);
    }

    FILE* sig = fopen(CFStringGetCStringPtr(sig_path, kCFStringEncodingMacRoman), "rb");
    void *sig_buf = malloc(128);
    assert(fread(sig_buf, 1, 128, sig) == 128);
    fclose(sig);
    CFDataRef sig_data = CFDataCreateWithBytesNoCopy(NULL, sig_buf, 128, NULL);
    CFRelease(sig_path);

    CFTypeRef keys[] = { CFSTR("ImageSignature"), CFSTR("ImageType") };
    CFTypeRef values[] = { sig_data, CFSTR("Developer") };
    CFDictionaryRef options = CFDictionaryCreate(NULL, (const void **)&keys, (const void **)&values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(sig_data);

    int result = AMDeviceMountImage(device, image_path, options, &mount_callback, 0);
    if (result == 0) {
        PRINT("[ 95%%] Developer disk image mounted successfully\n");
    } else if (result == 0xe8000076 /* already mounted */) {
        PRINT("[ 95%%] Developer disk image already mounted\n");
    } else {
        PRINT("[ !! ] Unable to mount developer disk image. (%x)\n", result);
        exit(EXIT_FAILURE);
    }

    CFRelease(image_path);
    CFRelease(options);
}

void transfer_callback(CFDictionaryRef dict, int arg) {
    int percent;
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));
    CFNumberGetValue(CFDictionaryGetValue(dict, CFSTR("PercentComplete")), kCFNumberSInt32Type, &percent);

    if (CFEqual(status, CFSTR("CopyingFile"))) {
        CFStringRef path = CFDictionaryGetValue(dict, CFSTR("Path"));

        if ((last_path == NULL || !CFEqual(path, last_path)) && !CFStringHasSuffix(path, CFSTR(".ipa"))) {
            PRINT("[%3d%%] Copying %s to device\n", percent / 2, CFStringGetCStringPtr(path, kCFStringEncodingMacRoman));
        }

        if (last_path != NULL) {
            CFRelease(last_path);
        }
        last_path = CFStringCreateCopy(NULL, path);
    }
}

void operation_callback(CFDictionaryRef dict, int arg) {
    int percent;
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));
    CFNumberGetValue(CFDictionaryGetValue(dict, CFSTR("PercentComplete")), kCFNumberSInt32Type, &percent);
    PRINT("[%3d%%] %s\n", (percent / 2) + 50, CFStringGetCStringPtr(status, kCFStringEncodingMacRoman));
}

void fdvendor_callback(CFSocketRef s, CFSocketCallBackType callbackType, CFDataRef address, const void *data, void *info) {
    CFSocketNativeHandle socket = (CFSocketNativeHandle)(*((CFSocketNativeHandle *)data));

    struct msghdr message;
    struct iovec iov[1];
    struct cmsghdr *control_message = NULL;
    char ctrl_buf[CMSG_SPACE(sizeof(int))];
    char dummy_data[1];

    memset(&message, 0, sizeof(struct msghdr));
    memset(ctrl_buf, 0, CMSG_SPACE(sizeof(int)));

    dummy_data[0] = ' ';
    iov[0].iov_base = dummy_data;
    iov[0].iov_len = sizeof(dummy_data);

    message.msg_name = NULL;
    message.msg_namelen = 0;
    message.msg_iov = iov;
    message.msg_iovlen = 1;
    message.msg_controllen = CMSG_SPACE(sizeof(int));
    message.msg_control = ctrl_buf;

    control_message = CMSG_FIRSTHDR(&message);
    control_message->cmsg_level = SOL_SOCKET;
    control_message->cmsg_type = SCM_RIGHTS;
    control_message->cmsg_len = CMSG_LEN(sizeof(int));

    *((int *) CMSG_DATA(control_message)) = gdbfd;

    sendmsg(socket, &message, 0);
    CFSocketInvalidate(s);
    CFRelease(s);
}

CFURLRef copy_device_app_url(AMDeviceRef device, CFStringRef identifier) {
    CFDictionaryRef result;
    assert(AMDeviceLookupApplications(device, 0, &result) == 0);

    CFDictionaryRef app_dict = CFDictionaryGetValue(result, identifier);
    assert(app_dict != NULL);

    CFStringRef app_path = CFDictionaryGetValue(app_dict, CFSTR("Path"));
    assert(app_path != NULL);

    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, app_path, kCFURLPOSIXPathStyle, true);
    CFRelease(result);
    return url;
}

CFStringRef copy_disk_app_identifier(CFURLRef disk_app_url) {
    CFURLRef plist_url = CFURLCreateCopyAppendingPathComponent(NULL, disk_app_url, CFSTR("Info.plist"), false);
    CFReadStreamRef plist_stream = CFReadStreamCreateWithFile(NULL, plist_url);
    CFReadStreamOpen(plist_stream);
    CFPropertyListRef plist = CFPropertyListCreateWithStream(NULL, plist_stream, 0, kCFPropertyListImmutable, NULL, NULL);
    CFStringRef bundle_identifier = CFRetain(CFDictionaryGetValue(plist, CFSTR("CFBundleIdentifier")));
    CFReadStreamClose(plist_stream);

    CFRelease(plist_url);
    CFRelease(plist_stream);
    CFRelease(plist);

    return bundle_identifier;
}

void write_gdb_prep_cmds(AMDeviceRef device, CFURLRef disk_app_url) {
    CFMutableStringRef cmds = CFStringCreateMutableCopy(NULL, 0, GDB_PREP_CMDS);
    CFRange range = { 0, CFStringGetLength(cmds) };

    CFStringRef ds_path = copy_device_support_path(device);
    CFStringFindAndReplace(cmds, CFSTR("{ds_path}"), ds_path, range, 0);
    range.length = CFStringGetLength(cmds);

    if (args) {
        CFStringRef cf_args = CFStringCreateWithCString(NULL, args, kCFStringEncodingASCII);
        CFStringFindAndReplace(cmds, CFSTR("{args}"), cf_args, range, 0);
        CFRelease(cf_args);
    } else {
        CFStringFindAndReplace(cmds, CFSTR(" {args}"), CFSTR(""), range, 0);
    }
    range.length = CFStringGetLength(cmds);

    CFStringRef bundle_identifier = copy_disk_app_identifier(disk_app_url);
    CFURLRef device_app_url = copy_device_app_url(device, bundle_identifier);
    CFStringRef device_app_path = CFURLCopyFileSystemPath(device_app_url, kCFURLPOSIXPathStyle);
    CFStringFindAndReplace(cmds, CFSTR("{device_app}"), device_app_path, range, 0);
    range.length = CFStringGetLength(cmds);

    CFStringRef disk_app_path = CFURLCopyFileSystemPath(disk_app_url, kCFURLPOSIXPathStyle);
    CFStringFindAndReplace(cmds, CFSTR("{disk_app}"), disk_app_path, range, 0);
    range.length = CFStringGetLength(cmds);

    CFURLRef device_container_url = CFURLCreateCopyDeletingLastPathComponent(NULL, device_app_url);
    CFStringRef device_container_path = CFURLCopyFileSystemPath(device_container_url, kCFURLPOSIXPathStyle);
    CFMutableStringRef dcp_noprivate = CFStringCreateMutableCopy(NULL, 0, device_container_path);
    range.length = CFStringGetLength(dcp_noprivate);
    CFStringFindAndReplace(dcp_noprivate, CFSTR("/private/var/"), CFSTR("/var/"), range, 0);
    range.length = CFStringGetLength(cmds);
    CFStringFindAndReplace(cmds, CFSTR("{device_container}"), dcp_noprivate, range, 0);
    range.length = CFStringGetLength(cmds);

    CFURLRef disk_container_url = CFURLCreateCopyDeletingLastPathComponent(NULL, disk_app_url);
    CFStringRef disk_container_path = CFURLCopyFileSystemPath(disk_container_url, kCFURLPOSIXPathStyle);
    CFStringFindAndReplace(cmds, CFSTR("{disk_container}"), disk_container_path, range, 0);

    CFDataRef cmds_data = CFStringCreateExternalRepresentation(NULL, cmds, kCFStringEncodingASCII, 0);
    FILE *out = fopen(PREP_CMDS_PATH, "w");
    fwrite(CFDataGetBytePtr(cmds_data), CFDataGetLength(cmds_data), 1, out);
    fclose(out);

    CFRelease(cmds);
    if (ds_path != NULL) CFRelease(ds_path);
    CFRelease(bundle_identifier);
    CFRelease(device_app_url);
    CFRelease(device_app_path);
    CFRelease(disk_app_path);
    CFRelease(device_container_url);
    CFRelease(device_container_path);
    CFRelease(dcp_noprivate);
    CFRelease(disk_container_url);
    CFRelease(disk_container_path);
    CFRelease(cmds_data);
}

void start_remote_debug_server(AMDeviceRef device) {
    assert(AMDeviceStartService(device, CFSTR("com.apple.debugserver"), &gdbfd, NULL) == 0);

    CFSocketRef fdvendor = CFSocketCreate(NULL, AF_UNIX, 0, 0, kCFSocketAcceptCallBack, &fdvendor_callback, NULL);

    int yes = 1;
    setsockopt(CFSocketGetNative(fdvendor), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, FDVENDOR_PATH);
    address.sun_len = SUN_LEN(&address);
    CFDataRef address_data = CFDataCreate(NULL, (const UInt8 *)&address, sizeof(address));

    unlink(FDVENDOR_PATH);

    CFSocketSetAddress(fdvendor, address_data);
    CFRelease(address_data);
    CFRunLoopAddSource(CFRunLoopGetMain(), CFSocketCreateRunLoopSource(NULL, fdvendor, 0), kCFRunLoopCommonModes);
}

void gdb_ready_handler(int signum)
{
	_exit(EXIT_SUCCESS);
}

void read_dir(service_conn_t afcFd, afc_connection* afc_conn_p, const char* dir)
{
    char *dir_ent;

    afc_connection afc_conn;
    if (!afc_conn_p) {
        afc_conn_p = &afc_conn;
        AFCConnectionOpen(afcFd, 0, &afc_conn_p);

    }

    printf("%s\n", dir);
    fflush(stdout);

    afc_dictionary afc_dict;
    afc_dictionary* afc_dict_p = &afc_dict;
    AFCFileInfoOpen(afc_conn_p, dir, &afc_dict_p);

    afc_directory afc_dir;
    afc_directory* afc_dir_p = &afc_dir;
    afc_error_t err = AFCDirectoryOpen(afc_conn_p, dir, &afc_dir_p);

    if (err != 0)
    {
        // Couldn't open dir - was probably a file
        return;
    }

    while(true) {
        err = AFCDirectoryRead(afc_conn_p, afc_dir_p, &dir_ent);

        if (!dir_ent)
            break;

        if (strcmp(dir_ent, ".") == 0 || strcmp(dir_ent, "..") == 0)
            continue;

        char* dir_joined = malloc(strlen(dir) + strlen(dir_ent) + 2);
        strcpy(dir_joined, dir);
        if (dir_joined[strlen(dir)-1] != '/')
            strcat(dir_joined, "/");
        strcat(dir_joined, dir_ent);
        read_dir(afcFd, afc_conn_p, dir_joined);
        free(dir_joined);
    }

    AFCDirectoryClose(afc_conn_p, afc_dir_p);
}

service_conn_t start_afc_service(AMDeviceRef device) {
    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    assert(AMDeviceValidatePairing(device) == 0);
    assert(AMDeviceStartSession(device) == 0);

    service_conn_t afcFd;
    assert(AMDeviceStartService(device, AMSVC_AFC, &afcFd, NULL) == 0);

    assert(AMDeviceStopSession(device) == 0);
    assert(AMDeviceDisconnect(device) == 0);
    return afcFd;
}

service_conn_t start_install_proxy_service(AMDeviceRef device) {
    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    assert(AMDeviceValidatePairing(device) == 0);
    assert(AMDeviceStartSession(device) == 0);

    service_conn_t installFd;
    assert(AMDeviceStartService(device, CFSTR("com.apple.mobile.installation_proxy"), &installFd, NULL) == 0);

    assert(AMDeviceStopSession(device) == 0);
    assert(AMDeviceDisconnect(device) == 0);

    return installFd;
}

// Used to send files to app-specific sandbox (Documents dir)
service_conn_t start_house_arrest_service(AMDeviceRef device) {
    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    assert(AMDeviceValidatePairing(device) == 0);
    assert(AMDeviceStartSession(device) == 0);

    service_conn_t houseFd;

    CFStringRef cf_bundle_id = CFStringCreateWithCString(NULL, bundle_id, kCFStringEncodingASCII);
    if (AMDeviceStartHouseArrestService(device, cf_bundle_id, 0, &houseFd, 0) != 0)
    {
        PRINT("Unable to find bundle with id: %s\n", bundle_id);
        exit(1);
    }

    assert(AMDeviceStopSession(device) == 0);
    assert(AMDeviceDisconnect(device) == 0);
    CFRelease(cf_bundle_id);

    return houseFd;
}

void install_app(AMDeviceRef device) {
    service_conn_t afcFd = start_afc_service(device);

    CFStringRef path = CFStringCreateWithCString(NULL, app_path, kCFStringEncodingASCII);

    assert(AMDeviceTransferApplication(afcFd, path, NULL, transfer_callback, NULL) == 0);
    close(afcFd);

    service_conn_t installFd = start_install_proxy_service(device);

    CFStringRef keys[] = { CFSTR("PackageType") };
    CFStringRef values[] = { CFSTR("Developer") };
    CFDictionaryRef options = CFDictionaryCreate(NULL, (const void **)&keys, (const void **)&values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    mach_error_t result = AMDeviceInstallApplication (installFd, path, options, operation_callback, NULL);
    if (result != 0)
    {
        PRINT("AMDeviceInstallApplication failed: %d\n", result);
        exit(1);
    }

    close(installFd);
    CFRelease(path);
    CFRelease(options);
}

void uninstall_app(AMDeviceRef device) {
    CFStringRef path = CFStringCreateWithCString(NULL, bundle_id, kCFStringEncodingASCII);

    service_conn_t installFd = start_install_proxy_service(device);

    mach_error_t result = AMDeviceUninstallApplication (installFd, path, NULL, operation_callback, NULL);
    if (result != 0)
    {
        PRINT("AMDeviceUninstallApplication failed: %d\n", result);
        exit(1);
    }

    close(installFd);
    CFRelease(path);
}

char* get_filename_from_path(char* path)
{
    char *ptr = path + strlen(path);
    while (ptr > path)
    {
        if (*ptr == '/')
            break;
        --ptr;
    }
    if (ptr+1 >= path+strlen(path))
        return NULL;
    if (ptr == path)
        return ptr;
    return ptr+1;
}

void* read_file_to_memory(char * path, size_t* file_size)
{
    struct stat buf;
    int err = stat(path, &buf);
    if (err < 0)
    {
        return NULL;
    }

    *file_size = buf.st_size;
    FILE* fd = fopen(path, "r");
    char* content = malloc(*file_size);
    if (fread(content, *file_size, 1, fd) != 1)
    {
        fclose(fd);
        return NULL;
    }
    fclose(fd);
    return content;
}

void list_files(AMDeviceRef device)
{
    service_conn_t houseFd = start_house_arrest_service(device);

    afc_connection afc_conn;
    afc_connection* afc_conn_p = &afc_conn;
    AFCConnectionOpen(houseFd, 0, &afc_conn_p);

    read_dir(houseFd, afc_conn_p, "/");
}

void upload_file(AMDeviceRef device) {
    service_conn_t houseFd = start_house_arrest_service(device);

    afc_file_ref file_ref;

    afc_connection afc_conn;
    afc_connection* afc_conn_p = &afc_conn;
    AFCConnectionOpen(houseFd, 0, &afc_conn_p);

    //        read_dir(houseFd, NULL, "/");

    if (!target_filename)
    {
        target_filename = get_filename_from_path(doc_file_path);
    }
    char *target_path = malloc(sizeof("/Documents/") + strlen(target_filename) + 1);
    strcat(target_path, "/Documents/");
    strcat(target_path, target_filename);

    size_t file_size;
    void* file_content = read_file_to_memory(doc_file_path, &file_size);

    if (!file_content)
    {
        PRINT("Could not open file: %s\n", doc_file_path);
        exit(-1);
    }

    assert(AFCFileRefOpen(afc_conn_p, target_path, 3, &file_ref) == 0);
    assert(AFCFileRefWrite(afc_conn_p, file_ref, file_content, file_size) == 0);
    assert(AFCFileRefClose(afc_conn_p, file_ref) == 0);
    assert(AFCConnectionClose(afc_conn_p) == 0);

    free(target_path);
    free(file_content);
}

void do_debug(AMDeviceRef device) {
    CFStringRef path = CFStringCreateWithCString(NULL, app_path, kCFStringEncodingASCII);

    CFURLRef relative_url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
    CFURLRef url = CFURLCopyAbsoluteURL(relative_url);

    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    assert(AMDeviceValidatePairing(device) == 0);
    assert(AMDeviceStartSession(device) == 0);

    PRINT("------ Debug phase ------\n");

    mount_developer_image(device);      // put debugserver on the device
    start_remote_debug_server(device);  // start debugserver
    write_gdb_prep_cmds(device, url);   // dump the necessary gdb commands into a file

    CFRelease(path);
    CFRelease(relative_url);
    CFRelease(url);

    PRINT("[100%%] Connecting to remote debug server\n");
    PRINT("-------------------------\n");

    signal(SIGHUP, gdb_ready_handler);

    pid_t parent = getpid();
    int pid = fork();
    if (pid == 0) {
        system(GDB_SHELL);      // launch gdb
        kill(parent, SIGHUP);  // "No. I am your father."
        _exit(EXIT_SUCCESS);
    }
}

void handle_device(AMDeviceRef device) {
    if (found_device) return; // handle one device only

    CFStringRef found_device_id = AMDeviceCopyDeviceIdentifier(device);

    PRINT ("found device id\n");
    if (device_id != NULL) {
        if(strcmp(device_id, CFStringGetCStringPtr(found_device_id, CFStringGetSystemEncoding())) == 0) {
            found_device = true;
        } else {
            return;
        }
    } else {
        if (operation == OP_LIST_DEVICES) {
            printf ("%s\n", CFStringGetCStringPtr(found_device_id, CFStringGetSystemEncoding()));
            fflush(stdout);
            return;
        }
        found_device = true;
    }

    if (operation == OP_INSTALL) {
        PRINT("[  0%%] Found device (%s), beginning install\n", CFStringGetCStringPtr(found_device_id, CFStringGetSystemEncoding()));

        install_app(device);

        PRINT("[100%%] Installed package %s\n", app_path);

        if (debug)
            do_debug(device);

    } else if (operation == OP_UNINSTALL) {
        PRINT("[  0%%] Found device (%s), beginning uninstall\n", CFStringGetCStringPtr(found_device_id, CFStringGetSystemEncoding()));

        uninstall_app(device);

        PRINT("[100%%] uninstalled package %s\n", bundle_id);

    } else if (operation == OP_UPLOAD_FILE) {
        PRINT("[  0%%] Found device (%s), sending file\n", CFStringGetCStringPtr(found_device_id, CFStringGetSystemEncoding()));

        upload_file(device);

        PRINT("[100%%] file sent %s\n", doc_file_path);

    } else if (operation == OP_LIST_FILES) {
        PRINT("[  0%%] Found device (%s), listing / ...\n", CFStringGetCStringPtr(found_device_id, CFStringGetSystemEncoding()));

        list_files(device);

        PRINT("[100%%] done.\n");
    }
    exit(0);
}

void device_callback(struct am_device_notification_callback_info *info, void *arg) {
    switch (info->msg) {
        case ADNCI_MSG_CONNECTED:
			if( info->dev->lockdown_conn ) {
				handle_device(info->dev);
			}
        default:
            break;
    }
}

void timeout_callback(CFRunLoopTimerRef timer, void *info) {
    if (!found_device) {
        PRINT("Timed out waiting for device.\n");
        exit(EXIT_FAILURE);
    }
}

void usage(const char* app) {
    printf ("usage: %s [-q/--quiet] [-t/--timeout timeout(seconds)] [-v/--verbose] <command> [<args>] \n\n", app);
    printf ("Commands available:\n");
    printf ("   install    [--id=device_id] --bundle=bundle.app [--debug] [--args=arguments] \n");
    printf ("    * Install the specified app with optional arguments to the specified device, or all\n");
    printf ("      attached devices if none are specified. \n\n");
    printf ("   uninstall  [--id=device_id] --bundle-id=<bundle id> \n");
    printf ("    * Removes the specified bundle identifier (eg com.foo.MyApp) from the specified device,\n");
    printf ("      or all attached devices if none are specified. \n\n");
    printf ("   upload     [--id=device_id] --bundle-id=<bundle id> --file=filename [--target=filename]\n");
    printf ("    * Uploads a file to the documents directory of the app specified with the bundle \n");
    printf ("      identifier (eg com.foo.MyApp) to the specified device, or all attached devices if\n");
    printf ("      none are specified. \n\n");
    printf ("   list-files [--id=device_id] --bundle-id=<bundle id> \n");
    printf ("    * Lists the the files in the app-specific sandbox  specified with the bundle \n");
    printf ("      identifier (eg com.foo.MyApp) on the specified device, or all attached devices if\n");
    printf ("      none are specified. \n\n");
    printf ("   list-devices  \n");
    printf ("    * List all attached devices. \n\n");
    fflush(stdout);
}

bool args_are_valid() {
    return (operation == OP_INSTALL && app_path) ||
    (operation == OP_UNINSTALL && bundle_id) ||
    (operation == OP_UPLOAD_FILE && bundle_id && doc_file_path) ||
    (operation == OP_LIST_FILES && bundle_id) ||
    (operation == OP_LIST_DEVICES);
}

int main(int argc, char *argv[]) {
    static struct option global_longopts[]= {
        { "quiet", no_argument, NULL, 'q' },
        { "verbose", no_argument, NULL, 'v' },
        { "timeout", required_argument, NULL, 't' },

        { "id", required_argument, NULL, 'i' },
        { "bundle", required_argument, NULL, 'b' },
        { "file", required_argument, NULL, 'f' },
        { "target", required_argument, NULL, 1 },
        { "bundle-id", required_argument, NULL, 0 },

        { "debug", no_argument, NULL, 'd' },
        { "args", required_argument, NULL, 'a' },

        { NULL, 0, NULL, 0 },
    };

    char ch;
    while ((ch = getopt_long(argc, argv, "qvi:b:f:da:t:", global_longopts, NULL)) != -1)
    {
        switch (ch) {
            case 0:
                bundle_id = optarg;
                break;
            case 'q':
                quiet = 1;
                break;
            case 'v':
                verbose = 1;
                break;
            case 'd':
                debug = 1;
                break;
            case 't':
                timeout = atoi(optarg);
                break;
            case 'b':
                app_path = optarg;
                break;
            case 'f':
                doc_file_path = optarg;
                break;
            case 1:
                target_filename = optarg;
                break;
            case 'a':
                args = optarg;
                break;
            case 'i':
                device_id = optarg;
                break;

            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        usage(argv [0]);
        exit(EXIT_SUCCESS);
    }

    operation = OP_NONE;
    if (strcmp (argv [optind], "install") == 0) {
        operation = OP_INSTALL;
    } else if (strcmp (argv [optind], "uninstall") == 0) {
        operation = OP_UNINSTALL;
    } else if (strcmp (argv [optind], "list-devices") == 0) {
        operation = OP_LIST_DEVICES;
    } else if (strcmp (argv [optind], "upload") == 0) {
        operation = OP_UPLOAD_FILE;
    } else if (strcmp (argv [optind], "list-files") == 0) {
        operation = OP_LIST_FILES;
    } else {
        usage (argv [0]);
        exit (0);
    }

    if (!args_are_valid()) {
        usage(argv[0]);
        exit(0);
    }

    if (operation == OP_INSTALL)
        assert(access(app_path, F_OK) == 0);

    AMDSetLogLevel(1+4+2+8+16+32+64+128); // otherwise syslog gets flooded with crap
    if (timeout > 0)
    {
        CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + timeout, 0, 0, 0, timeout_callback, NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
        PRINT("[....] Waiting up to %d seconds for iOS device to be connected\n", timeout);
    }
    else
    {
        PRINT("[....] Waiting for iOS device to be connected\n");
    }

    struct am_device_notification *notify;
    AMDeviceNotificationSubscribe(&device_callback, 0, 0, NULL, &notify);

    CFRunLoopRun();
}

