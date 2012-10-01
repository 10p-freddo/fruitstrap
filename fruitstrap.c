//TODO: don't copy/mount DeveloperDiskImage.dmg if it's already done - Xcode checks this somehow

#import <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <signal.h>
#include <getopt.h>
#include <pwd.h>
#include "MobileDevice.h"

#define FDVENDOR_PATH  "/tmp/fruitstrap-remote-debugserver"
#define PREP_CMDS_PATH "/tmp/fruitstrap-gdb-prep-cmds"
#define GDB_SHELL      "--arch armv7f -x " PREP_CMDS_PATH

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

typedef struct am_device * AMDeviceRef;
int AMDeviceSecureTransferPath(int zero, AMDeviceRef device, CFURLRef url, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceSecureInstallApplication(int zero, AMDeviceRef device, CFURLRef url, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceMountImage(AMDeviceRef device, CFStringRef image, CFDictionaryRef options, void *callback, int cbarg);
int AMDeviceLookupApplications(AMDeviceRef device, int zero, CFDictionaryRef* result);

bool found_device = false, debug = false, verbose = false, unbuffered = false;
char *app_path = NULL;
char *device_id = NULL;
char *args = NULL;
char *gdb_args = "";
int timeout = 0;
CFStringRef last_path = NULL;
service_conn_t gdbfd;
int child_pid = 0;

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

CFStringRef copy_long_shot_disk_image_path() {
    FILE *fpipe = NULL;
    char *command = "find `xcode-select --print-path` -name DeveloperDiskImage.dmg | tail -n 1";

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

const char *get_home() {
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd *pwd = getpwuid(getuid());
        home = pwd->pw_dir;
    }
    return home;
}

CFStringRef copy_xcode_path_for(CFStringRef search) {
    CFStringRef xcodeDevPath = copy_xcode_dev_path();
    CFStringRef path;
    bool found = false;
    const char* home = get_home();

    
    // Try using xcode-select --print-path
    if (!found) {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/%@"), xcodeDevPath, search);
        found = path_exists(path);
    }
    // If not look in the default xcode location (xcode-select is sometimes wrong)
    if (!found) {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("/Applications/Xcode.app/Contents/Developer/%@"), search);
        found = path_exists(path);
    }
    // If not look in the users home directory, Xcode can store device support stuff there
    if (!found) {
        path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s/Library/Developer/Xcode/%@"), home, search);
        found = path_exists(path);
    }
    
    CFRelease(xcodeDevPath);

    if (found) {
        return path;
    } else {
        CFRelease(path);
        return NULL;
    }
}

CFStringRef copy_device_support_path(AMDeviceRef device) {
    CFStringRef version = AMDeviceCopyValue(device, 0, CFSTR("ProductVersion"));
    CFStringRef build = AMDeviceCopyValue(device, 0, CFSTR("BuildVersion"));
    CFStringRef path = NULL;

    if (path == NULL) {
        path = copy_xcode_path_for(CFStringCreateWithFormat(NULL, NULL, CFSTR("iOS DeviceSupport/%@ (%@)"), version, build));
    }
    if (path == NULL) {
        path = copy_xcode_path_for(CFStringCreateWithFormat(NULL, NULL, CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/%@ (%@)"), version, build));
    }
    if (path == NULL) {
        path = copy_xcode_path_for(CFStringCreateWithFormat(NULL, NULL, CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/%@"), version));
    }
    if (path == NULL) {
        path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/Latest"));
    }
    
    CFRelease(version);
    CFRelease(build);

    if (path == NULL)
    {
        printf("[ !! ] Unable to locate DeviceSupport directory.\n[ !! ] This probably means you don't have Xcode installed, you will need to launch the app manually and logging output will not be shown!\n");
        exit(1);
    }

    return path;
}

CFStringRef copy_developer_disk_image_path(AMDeviceRef device) {
    CFStringRef version = AMDeviceCopyValue(device, 0, CFSTR("ProductVersion"));
    CFStringRef build = AMDeviceCopyValue(device, 0, CFSTR("BuildVersion"));
    CFStringRef path = NULL;

    if (path == NULL) {
        path = copy_xcode_path_for(CFStringCreateWithFormat(NULL, NULL, CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/%@ (%@)/DeveloperDiskImage.dmg"), version, build));
    }
    if (path == NULL) {
        path = copy_xcode_path_for(CFStringCreateWithFormat(NULL, NULL, CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/%@/DeveloperDiskImage.dmg"), version));
    }
    if (path == NULL) {
        path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/DeviceSupport/Latest/DeveloperDiskImage.dmg"));
    }
    
    CFRelease(version);
    CFRelease(build);
    
    if (path == NULL) {
        // Sometimes Latest seems to be missing in Xcode, in that case use find and hope for the best
        path = copy_long_shot_disk_image_path();
        if (CFStringGetLength(path) < 5) {
            path = NULL;
        }
    }

    if (path == NULL)
    {
        printf("[ !! ] Unable to locate DeveloperDiskImage.dmg.\n[ !! ] This probably means you don't have Xcode installed, you will need to launch the app manually and logging output will not be shown!\n");
        exit(1);
    }

    return path;
}

void mount_callback(CFDictionaryRef dict, int arg) {
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));

    if (CFEqual(status, CFSTR("LookingUpImage"))) {
        printf("[  0%%] Looking up developer disk image\n");
    } else if (CFEqual(status, CFSTR("CopyingImage"))) {
        printf("[ 30%%] Copying DeveloperDiskImage.dmg to device\n");
    } else if (CFEqual(status, CFSTR("MountingImage"))) {
        printf("[ 90%%] Mounting developer disk image\n");
    }
}

void mount_developer_image(AMDeviceRef device) {
    CFStringRef ds_path = copy_device_support_path(device);
    CFStringRef image_path = copy_developer_disk_image_path(device);
    CFStringRef sig_path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@.signature"), image_path);
    CFRelease(ds_path);

    if (verbose) {
        printf("Device support path: ");
        fflush(stdout);
        CFShow(ds_path);
        printf("Developer disk image: ");
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
        printf("[ 95%%] Developer disk image mounted successfully\n");
    } else if (result == 0xe8000076 /* already mounted */) {
        printf("[ 95%%] Developer disk image already mounted\n");
    } else {
        printf("[ !! ] Unable to mount developer disk image. (%x)\n", result);
        exit(1);
    }

    CFRelease(image_path);
    CFRelease(options);
}

mach_error_t transfer_callback(CFDictionaryRef dict, int arg) {
    int percent;
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));
    CFNumberGetValue(CFDictionaryGetValue(dict, CFSTR("PercentComplete")), kCFNumberSInt32Type, &percent);

    if (CFEqual(status, CFSTR("CopyingFile"))) {
        CFStringRef path = CFDictionaryGetValue(dict, CFSTR("Path"));

        if ((last_path == NULL || !CFEqual(path, last_path)) && !CFStringHasSuffix(path, CFSTR(".ipa"))) {
            printf("[%3d%%] Copying %s to device\n", percent / 2, CFStringGetCStringPtr(path, kCFStringEncodingMacRoman));
        }

        if (last_path != NULL) {
            CFRelease(last_path);
        }
        last_path = CFStringCreateCopy(NULL, path);
    }

    return 0;
}

mach_error_t install_callback(CFDictionaryRef dict, int arg) {
    int percent;
    CFStringRef status = CFDictionaryGetValue(dict, CFSTR("Status"));
    CFNumberGetValue(CFDictionaryGetValue(dict, CFSTR("PercentComplete")), kCFNumberSInt32Type, &percent);

    printf("[%3d%%] %s\n", (percent / 2) + 50, CFStringGetCStringPtr(status, kCFStringEncodingMacRoman));
    return 0;
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

void killed(int signum) {
    killpg(child_pid, SIGTERM);
    _exit(0);
}

void gdb_ready_handler(int signum)
{
    _exit(0);
}

void handle_device(AMDeviceRef device) {
    if (found_device) return; // handle one device only

    CFStringRef found_device_id = AMDeviceCopyDeviceIdentifier(device);

    if (device_id != NULL) {
        if(strcmp(device_id, CFStringGetCStringPtr(found_device_id, CFStringGetSystemEncoding())) == 0) {
            found_device = true;
        } else {
            return;
        }
    } else {
        found_device = true;
    }

    CFRetain(device); // don't know if this is necessary?

    printf("[  0%%] Found device (%s), beginning install\n", CFStringGetCStringPtr(found_device_id, CFStringGetSystemEncoding()));

    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    assert(AMDeviceValidatePairing(device) == 0);
    assert(AMDeviceStartSession(device) == 0);

    CFStringRef path = CFStringCreateWithCString(NULL, app_path, kCFStringEncodingASCII);
    CFURLRef relative_url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
    CFURLRef url = CFURLCopyAbsoluteURL(relative_url);

    CFRelease(relative_url);

    service_conn_t afcFd;
    assert(AMDeviceStartService(device, CFSTR("com.apple.afc"), &afcFd, NULL) == 0);
    assert(AMDeviceStopSession(device) == 0);
    assert(AMDeviceDisconnect(device) == 0);
    assert(AMDeviceTransferApplication(afcFd, path, NULL, transfer_callback, NULL) == 0);

    close(afcFd);

    CFStringRef keys[] = { CFSTR("PackageType") };
    CFStringRef values[] = { CFSTR("Developer") };
    CFDictionaryRef options = CFDictionaryCreate(NULL, (const void **)&keys, (const void **)&values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    assert(AMDeviceValidatePairing(device) == 0);
    assert(AMDeviceStartSession(device) == 0);

    service_conn_t installFd;
    assert(AMDeviceStartService(device, CFSTR("com.apple.mobile.installation_proxy"), &installFd, NULL) == 0);

    assert(AMDeviceStopSession(device) == 0);
    assert(AMDeviceDisconnect(device) == 0);

    mach_error_t result = AMDeviceInstallApplication(installFd, path, options, install_callback, NULL);
    if (result != 0)
    {
       printf("AMDeviceInstallApplication failed: %d\n", result);
        exit(1);
    }

    close(installFd);

    CFRelease(path);
    CFRelease(options);

    printf("[100%%] Installed package %s\n", app_path);

    if (!debug) exit(0); // no debug phase

    AMDeviceConnect(device);
    assert(AMDeviceIsPaired(device));
    assert(AMDeviceValidatePairing(device) == 0);
    assert(AMDeviceStartSession(device) == 0);

    printf("------ Debug phase ------\n");

    mount_developer_image(device);      // put debugserver on the device
    start_remote_debug_server(device);  // start debugserver
    write_gdb_prep_cmds(device, url);   // dump the necessary gdb commands into a file

    CFRelease(url);

    printf("[100%%] Connecting to remote debug server\n");
    printf("-------------------------\n");

    signal(SIGHUP, gdb_ready_handler);

    pid_t parent = getpid();
    int pid = fork();
    if (pid == 0) {
        CFStringRef path = copy_xcode_path_for(CFSTR("Platforms/iPhoneOS.platform/Developer/usr/libexec/gdb/gdb-arm-apple-darwin"));
        if (path == NULL) {
             printf("[ !! ] Unable to locate GDB.\n");
             exit(1);
        } else {
            CFStringRef gdb_cmd = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ %@ %s"), path, CFSTR(GDB_SHELL), gdb_args);
 
            // Convert CFStringRef to char* for system call
            const char *char_gdb_cmd = CFStringGetCStringPtr(gdb_cmd, kCFStringEncodingMacRoman);

            system(char_gdb_cmd);      // launch gdb
        }
        kill(parent, SIGHUP);  // "No. I am your father."
        _exit(0);
    }

    child_pid = pid;
    setpgid(pid, 0); // Set process group of child to child's pid
    signal(SIGINT, killed);
    signal(SIGTERM, killed);
}

void device_callback(struct am_device_notification_callback_info *info, void *arg) {
    switch (info->msg) {
        case ADNCI_MSG_CONNECTED:
            handle_device(info->dev);
        default:
            break;
    }
}

void timeout_callback(CFRunLoopTimerRef timer, void *info) {
    if (!found_device) {
        printf("Timed out waiting for device.\n");
        exit(1);
    }
}

void usage(const char* app) {
    printf("usage: %s [-d/--debug] [-i/--id device_id] -b/--bundle bundle.app [-a/--args arguments] [-t/--timeout timeout(seconds)] [-u/--unbuffered] [-g/--gdbargs gdbarguments]\n", app);
}

int main(int argc, char *argv[]) {
    static struct option longopts[] = {
        { "debug", no_argument, NULL, 'd' },
        { "id", required_argument, NULL, 'i' },
        { "bundle", required_argument, NULL, 'b' },
        { "args", required_argument, NULL, 'a' },
        { "verbose", no_argument, NULL, 'v' },
        { "timeout", required_argument, NULL, 't' },
        { "unbuffered", no_argument, NULL, 'u' },
        { "gbdbargs", required_argument, NULL, 'g' },
        { NULL, 0, NULL, 0 },
    };
    char ch;

    while ((ch = getopt_long(argc, argv, "dvi:b:a:t:u:g:", longopts, NULL)) != -1)
    {
        switch (ch) {
        case 'd':
            debug = 1;
            break;
        case 'i':
            device_id = optarg;
            break;
        case 'b':
            app_path = optarg;
            break;
        case 'a':
            args = optarg;
            break;
        case 'v':
            verbose = 1;
            break;
        case 't':
            timeout = atoi(optarg);
            break;
        case 'u':
            unbuffered = 1;
            break;
        case 'g':
            gdb_args = optarg;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (!app_path) {
        usage(argv[0]);
        exit(0);
    }

    if (unbuffered) setbuf(stdout, NULL);

    printf("------ Install phase ------\n");

    assert(access(app_path, F_OK) == 0);

    AMDSetLogLevel(5); // otherwise syslog gets flooded with crap
    if (timeout > 0)
    {
        CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + timeout, 0, 0, 0, timeout_callback, NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
        printf("[....] Waiting up to %d seconds for iOS device to be connected\n", timeout);
    }
    else
    {
        printf("[....] Waiting for iOS device to be connected\n");
    }

    struct am_device_notification *notify;
    AMDeviceNotificationSubscribe(&device_callback, 0, 0, NULL, &notify);
    CFRunLoopRun();
}
